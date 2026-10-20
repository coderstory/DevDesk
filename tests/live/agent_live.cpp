// tests/live/agent_live.cpp
// 智能体（module_agent）真机全场景验收驱动：进程内装配 AgentLoop + DirectLlmClient
// + 全部内置工具，用**当前激活 profile**（真 baseUrl / 真 key）发真 LLM 请求，逐项跑
// 场景矩阵 A-I 并输出 PASS/FAIL。
//
// 环境门（防生产误用，缺一即 exit 2）：
//   DEVDESK_AGENT_LIVE=1        显式开关（scripts/run-agent-live.sh 会设）
//
// 安全红线（与本文件结构绑死，改前先读）：
//   1. 不启动完整 app、不注册 Router：只装配 service 层 + 工具注册表。
//   2. 真 profile 只读：把用户数据根的 devdesk.db（含 -wal/-shm）**复制**到临时目录
//      后只读解析激活 profile 与明文 key（ProfileRepository::getActiveProfile /
//      loadApiKey，均为纯 DB 读），取完**立即删除副本**——key 全程只进内存与
//      Authorization 头，不落日志/文件/argv；本文件所有输出均不含 key 与请求头。
//   3. 业务数据全部落在 DEVDESK_DATA_ROOT 指向的临时数据根（进程内 setenv，用完删），
//      不碰用户真实库；不枚举/不清钥匙串（ClaudeService 用独立测试命名空间，且从不
//      调 cleanupOrphanKeychainEntries）。
//   4. 禁绑 127.0.0.1:8888（DevDesk 代理口）与 15721；G1 的拒绝连接目标用 127.0.0.1:1。
//
// 入口：bash scripts/run-agent-live.sh [场景名 ...]；全部场景通过 exit 0，否则 1。
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/database.h"
#include "core/event_bus.h"
#include "core/llm_profile_source.h"
#include "core/paths.h"
#include "core/tool.h"
#include "module_agent/agent_loop.h"
#include "module_agent/agent_store.h"
#include "module_agent/app_stats_tool.h"
#include "module_agent/direct_llm_client.h"
#include "module_agent/message_blocks.h"
#include "module_agent/scheduler.h"
#include "module_agent/trigger_config.h"
#include "module_claude_proxy/claude_service.h"
#include "module_claude_proxy/claude_tools.h"
#include "module_claude_proxy/profile_repository.h"
#include "module_claude_proxy/ccswitch_source.h"
#include "module_markdown/markdown_service.h"
#include "module_markdown/markdown_tools.h"
#include "module_markdown/md_note_git.h"
#include "module_settings/app_tools.h"
#include "module_todo/todo_service.h"
#include "module_todo/todo_tools.h"
#include "platform/http_client.h"
#include "platform/keychain.h"

#include <eui/app.h>

// app 运行时符号的本地替身：module_settings / module_todo 的静态库带着对
// app::setWindowEffect 等的未定义引用，其唯一生产定义在 glfw_app_main.cpp（只编译
// 进 app 目标，本驱动不链）。本驱动无 UI，全部 no-op / 回落默认值——与
// tests/test_modules/test_settings.cpp 注入 dsl_app_impl.h 是同一问题的轻量解。
namespace app {
void setClearColor(const core::Color&) {}
void setWindowEffect(core::platform::WindowEffect) {}
core::platform::WindowEffect activeWindowEffect() { return {}; }
void setTitleBarAppearance(const core::platform::TitleBarAppearance&) {}
float contentScale() { return 1.0f; }
} // namespace app

namespace {

namespace fs = std::filesystem;
using module::agent::AgentLoop;
using module::agent::AgentStore;
using module::agent::AgentTask;
using module::agent::DirectLlmClient;
using module::agent::Scheduler;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

template <typename Pred>
bool waitUntil(Pred&& pred, int timeoutMs, int pollMs = 300) {
    const int64_t deadline = nowMs() + timeoutMs;
    while (nowMs() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
    }
    return pred();
}

std::string hostOf(const std::string& url) {
    size_t begin = url.find("://");
    begin = (begin == std::string::npos) ? 0 : begin + 3;
    size_t end = url.find_first_of("/?#", begin);
    if (end == std::string::npos) end = url.size();
    return url.substr(begin, end - begin);
}

// ── HTTP 捕获缝：只记计数 / 状态码 / 请求体（body 无鉴权头，可安全留存断言）──
struct HttpCapture {
    std::mutex mu;
    int requests = 0;
    std::vector<int> statuses;
    std::string lastBody;

    module::agent::DirectLlmClient::HttpFn fn() {
        return [this](const platform::http::Request& req) {
            auto resp = platform::http::send(req);
            std::lock_guard<std::mutex> lock(mu);
            ++requests;
            statuses.push_back(resp.isOk() ? resp.value().statusCode : -1);
            lastBody = req.body;
            return resp;
        };
    }
};

// ── 真 profile 快照来源（取自用户数据根的只读副本；可被场景覆写造失败态）──
struct SnapshotSource : public core::LlmProfileSource {
    core::LlmDirectProfile profile;
    core::Result<core::LlmDirectProfile> currentProfile() override {
        return core::Result<core::LlmDirectProfile>::ok(profile);
    }
};

// 全局装配（一次构造，全部场景共用）
struct Ctx {
    std::unique_ptr<core::Database> appDb;
    std::unique_ptr<module::todo::TodoService> todo;
    std::unique_ptr<module::markdown::MarkdownService> md;
    std::unique_ptr<module::claude::ClaudeService> claude;
    std::unique_ptr<module::todo::TodoTools> todoTools;
    std::unique_ptr<module::markdown::MarkdownTools> mdTools;
    std::unique_ptr<module::claude::ClaudeTools> claudeTools;
    std::shared_ptr<core::ITool> readSettingsTool;
    std::shared_ptr<core::ITool> statsTool;
    HttpCapture capture;
    std::unique_ptr<SnapshotSource> src;      // 真 profile（多数场景）
    std::unique_ptr<DirectLlmClient> llm;
    std::unique_ptr<AgentStore> store;
    std::unique_ptr<AgentLoop> loop;
    std::unique_ptr<Scheduler> scheduler;
    std::vector<core::event::EventBus::Subscription> eventSubs;
    fs::path notesDir;
    std::string modelSlot = "default";
    std::string providerHost;   // 仅 host（脱敏展示用）
    std::string modelId;        // default 槽位解析出的 model（展示用）

    // 手动跑一个任务到终态并回读 run 行
    struct RunOut {
        bool started = false;
        int64_t runId = 0;
        std::string status;
        std::string error;
        int iterations = 0;
    };
    RunOut run(AgentLoop& l, const AgentTask& t) {
        std::atomic<bool> cancel{false};
        RunOut out;
        auto r = l.run(t, cancel);
        if (r.isFail()) {
            out.error = r.errorMessage();
            return out;
        }
        out.started = true;
        out.runId = r.value();
        auto runs = store->listRuns(t.id, 5);
        if (runs.isOk() && !runs.value().empty()) {
            const auto& row = runs.value().front();
            out.status = row.status;
            out.error = row.error.value_or("");
            out.iterations = row.iterations;
        }
        return out;
    }
    RunOut run(const AgentTask& t) { return run(*loop, t); }

    // run 的 tool_use 工具名集合（断言「工具真被 LLM 决策调起」的证据面）
    std::set<std::string> toolsUsed(int64_t runId) {
        std::set<std::string> names;
        auto msgs = store->listMessages(runId);
        if (msgs.isFail()) return names;
        for (const auto& m : msgs.value()) {
            for (const auto& b : module::agent::decodeMessageBlocks(m.contentJson)) {
                if (b.type == module::agent::MessageBlock::Type::ToolUse) {
                    names.insert(b.toolName);
                }
            }
        }
        return names;
    }
    bool sawToolError(int64_t runId) {
        auto msgs = store->listMessages(runId);
        if (msgs.isFail()) return false;
        for (const auto& m : msgs.value()) {
            for (const auto& b : module::agent::decodeMessageBlocks(m.contentJson)) {
                if (b.type == module::agent::MessageBlock::Type::ToolResult && b.isError) {
                    return true;
                }
            }
        }
        return false;
    }
    int64_t todoRowCount(const char* title) {
        auto prep = appDb->db().prepare(
            "SELECT COUNT(*) AS n FROM todo_tasks WHERE title = ?");
        if (prep.isFail()) return -1;
        if (prep.value().bind(1, title).isFail()) return -1;
        auto step = prep.value().step();
        if (step.isFail() || !step.value()) return -1;
        auto row = prep.value().currentRow();
        if (row.isFail()) return -1;
        return row.value().getInt64("n").valueOr(-1);
    }
};

// 场景：返回空串 = PASS；非空 = FAIL 证据行
using ScenarioFn = std::string (*)(Ctx&);

int gFail = 0;

void report(const char* name, const std::string& evidence) {
    if (evidence.empty()) {
        std::printf("  [PASS] %s\n", name);
    } else {
        std::printf("  [FAIL] %s\n         证据: %s\n", name, evidence.c_str());
        ++gFail;
    }
    std::fflush(stdout);
}

AgentTask makeTask(AgentStore& store, const char* name, const char* goal,
                   int maxIterations, bool allowDestructive,
                   const char* triggerType = "manual",
                   const std::string& triggerConfig = "",
                   const std::string& contextNote = "", int autoNote = 0) {
    AgentTask t;
    auto r = store.createTask(name, goal, triggerType, "default", maxIterations,
                              allowDestructive, triggerConfig, contextNote, autoNote);
    if (r.isOk()) t.id = r.value();
    t.name = name;
    t.goal = goal;
    t.maxIterations = maxIterations;
    t.allowDestructive = allowDestructive;
    t.modelSlot = "default";
    t.contextNote = contextNote;
    t.autoNote = autoNote != 0;
    return t;
}

// ── G1 错 baseUrl（连接拒绝）──
std::string scG1(Ctx& c) {
    SnapshotSource bad;
    bad.profile = c.src->profile;
    bad.profile.baseUrl = "http://127.0.0.1:1";
    DirectLlmClient llmBad(bad, c.capture.fn());
    AgentLoop l(llmBad, core::ToolRegistry::instance(), *c.store);
    auto t = makeTask(*c.store, "live-g1", "只回复 OK，不要调用任何工具。", 3, false);
    auto out = c.run(l, t);
    if (!out.started) return "run 未启动: " + out.error;
    if (out.status != "failed") return "status=" + out.status + " error=" + out.error;
    if (out.error.empty()) return "failed 但无错误信息";
    return {};
}

// ── G2 错 key（真上游 401/403）──
std::string scG2(Ctx& c) {
    SnapshotSource badKey;
    badKey.profile = c.src->profile;
    badKey.profile.apiKeyPlain = "sk-agent-live-invalid-key";
    DirectLlmClient llmBad(badKey, c.capture.fn());
    AgentLoop l(llmBad, core::ToolRegistry::instance(), *c.store);
    auto t = makeTask(*c.store, "live-g2", "只回复 OK，不要调用任何工具。", 3, false);
    auto out = c.run(l, t);
    if (!out.started) return "run 未启动: " + out.error;
    if (out.status != "failed") return "status=" + out.status + " error=" + out.error;
    if (out.error.find("上游 HTTP 4") == std::string::npos) {
        return "错误信息未带 4xx 状态码: " + out.error.substr(0, 120);
    }
    return {};
}

// ── H context note 自动追加进 system ──
std::string scH(Ctx& c) {
    const char* canary = "AGENT_LIVE_CANARY_9527";
    auto t1 = makeTask(*c.store, "live-h1", "只回复 OK，不要调用任何工具。", 3,
                       false, "manual", "", canary, /*autoNote=*/1);
    auto out1 = c.run(t1);
    if (out1.status != "completed") return "autoNote=1 status=" + out1.status;
    std::string body1;
    {
        std::lock_guard<std::mutex> lock(c.capture.mu);
        body1 = c.capture.lastBody;
    }
    if (body1.find(canary) == std::string::npos || body1.find("\"system\"") == std::string::npos) {
        return "autoNote=1 请求体未见 system 注入 canary";
    }
    auto t2 = makeTask(*c.store, "live-h2", "只回复 OK，不要调用任何工具。", 3,
                       false, "manual", "", canary, /*autoNote=*/0);
    auto out2 = c.run(t2);
    if (out2.status != "completed") return "autoNote=0 status=" + out2.status;
    std::string body2;
    {
        std::lock_guard<std::mutex> lock(c.capture.mu);
        body2 = c.capture.lastBody;
    }
    if (body2.find(canary) != std::string::npos) {
        return "autoNote=0 请求体不应含 canary 却出现了";
    }
    return {};
}

// ── B 单工具任务 ──
std::string scB(Ctx& c) {
    auto t = makeTask(*c.store, "live-b",
                      "调用 todo.list 查一下当前任务列表，然后一句话回复有几条任务。",
                      4, false);
    auto out = c.run(t);
    if (out.status != "completed") {
        return "status=" + out.status + " error=" + out.error.substr(0, 120);
    }
    auto used = c.toolsUsed(out.runId);
    if (used.count("todo.list") == 0) return "todo.list 未被调用，实际: " + [&] {
            std::string s;
            for (const auto& n : used) s += n + " ";
            return s;
        }();
    return {};
}

// ── A1 todo 6 工具 ──
std::string scA1(Ctx& c) {
    auto t = makeTask(*c.store, "live-a1",
                      "依次完成 7 步，每步调用一个工具：1) todo.create 新建任务标题「买牛奶」；"
                      "2) todo.create 再新建一个标题「留验任务」（这个之后不许删）；"
                      "3) todo.list 找出「买牛奶」的 id；4) todo.update 把它标题改成「买咖啡」；"
                      "5) todo.move 把「买咖啡」移到最后一列；6) todo.complete 完成「买咖啡」；"
                      "7) todo.delete 删除「买咖啡」。全部做完回复 DONE。",
                      16, /*allowDestructive=*/true);
    auto out = c.run(t);
    if (out.status != "completed") {
        return "status=" + out.status + " error=" + out.error.substr(0, 160);
    }
    auto used = c.toolsUsed(out.runId);
    const char* need[] = {"todo.list", "todo.create", "todo.update",
                          "todo.move", "todo.complete", "todo.delete"};
    std::string missing;
    for (const char* n : need) {
        if (used.count(n) == 0) missing += std::string(n) + " ";
    }
    if (!missing.empty()) return "缺工具调用: " + missing;
    int64_t keep = c.todoRowCount("留验任务");
    int64_t milk = c.todoRowCount("买牛奶");
    int64_t coffee = c.todoRowCount("买咖啡");
    if (keep != 1) return "留验任务行数=" + std::to_string(keep) + "（应为 1）";
    if (coffee != 0) return "买咖啡行数=" + std::to_string(coffee) + "（删除未生效）";
    if (milk != 0) return "买牛奶行数=" + std::to_string(milk) + "（改名未生效）";
    return {};
}

// ── A2 markdown 5 工具 ──
std::string scA2(Ctx& c) {
    auto t = makeTask(*c.store, "live-a2",
                      "依次：1) md.create 创建笔记 live-agent-keep.md 内容为「live 留验」；"
                      "2) md.create 创建笔记 live-agent-tmp.md 内容为「live 临时」；"
                      "3) md.read 读 live-agent-tmp.md；4) md.search 搜索关键词 live；"
                      "5) md.list_tags 列出全部标签；6) md.delete 删除 live-agent-tmp.md。"
                      "只许删 tmp，不许删 keep。全部做完回复 DONE。",
                      14, /*allowDestructive=*/true);
    auto out = c.run(t);
    if (out.status != "completed") {
        return "status=" + out.status + " error=" + out.error.substr(0, 160);
    }
    auto used = c.toolsUsed(out.runId);
    const char* need[] = {"md.create", "md.read", "md.search", "md.list_tags", "md.delete"};
    std::string missing;
    for (const char* n : need) {
        if (used.count(n) == 0) missing += std::string(n) + " ";
    }
    if (!missing.empty()) return "缺工具调用: " + missing;
    if (!fs::exists(c.notesDir / "live-agent-keep.md")) return "keep 文件未落盘";
    if (fs::exists(c.notesDir / "live-agent-tmp.md")) return "tmp 文件未被删除";
    return {};
}

// ── A3 claude 4 工具（对隔离库内的替身 profile 操作）──
std::string scA3(Ctx& c) {
    auto t = makeTask(*c.store, "live-a3",
                      "依次：1) claude.list_profiles 记下各 profile 的 id；"
                      "2) claude.recent_logs 最多取 3 条；"
                      "3) claude.test_connection 测 id 最大的那个 profile（预期会报错，属正常，继续）；"
                      "4) claude.switch_profile 切到 id 最大的那个；"
                      "5) claude.switch_profile 再切回 id 最小的那个。全部做完回复 DONE。",
                      12, /*allowDestructive=*/true);
    auto out = c.run(t);
    if (out.status != "completed") {
        return "status=" + out.status + " error=" + out.error.substr(0, 160);
    }
    auto used = c.toolsUsed(out.runId);
    const char* need[] = {"claude.list_profiles", "claude.recent_logs",
                          "claude.test_connection", "claude.switch_profile"};
    std::string missing;
    for (const char* n : need) {
        if (used.count(n) == 0) missing += std::string(n) + " ";
    }
    if (!missing.empty()) return "缺工具调用: " + missing;
    auto active = c.claude->getActiveProfile();
    if (active.isFail() || active.value().name != "live-a") {
        return "最终激活 profile 应为 live-a（切回未生效）";
    }
    return {};
}

// ── A4 app.read_settings + app.stats ──
std::string scA4(Ctx& c) {
    auto t = makeTask(*c.store, "live-a4",
                      "依次：1) 调用 app.read_settings 读取应用配置；"
                      "2) 调用 app.stats 读取统计。然后用一句话总结回复。",
                      6, false);
    auto out = c.run(t);
    if (out.status != "completed") {
        return "status=" + out.status + " error=" + out.error.substr(0, 160);
    }
    auto used = c.toolsUsed(out.runId);
    if (used.count("app.read_settings") == 0) return "app.read_settings 未被调用";
    if (used.count("app.stats") == 0) return "app.stats 未被调用";
    return {};
}

// ── G3 工具执行报错回喂（is_error 进 messages，run 不崩不挂）──
std::string scG3(Ctx& c) {
    auto t = makeTask(*c.store, "live-g3",
                      "调用 todo.update，把 id=999999 的任务标题改成「X」。"
                      "工具报错后不要重试，直接回复 EXPECTED_TOOL_ERROR。",
                      5, false);
    auto out = c.run(t);
    if (out.status != "completed") {
        return "status=" + out.status + " error=" + out.error.substr(0, 160);
    }
    if (!c.sawToolError(out.runId)) return "messages 里没有 is_error 的 tool_result";
    return {};
}

// ── G4 破坏性操作未授权 → run 直接 failed ──
std::string scG4(Ctx& c) {
    auto t = makeTask(*c.store, "live-g4",
                      "调用 todo.delete 删除 id=1 的任务。", 4, /*allowDestructive=*/false);
    auto out = c.run(t);
    if (out.status != "failed") return "status=" + out.status + "（应为 failed）";
    if (out.error.find("破坏性操作") == std::string::npos ||
        out.error.find("未授权") == std::string::npos) {
        return "错误信息不完整: " + out.error.substr(0, 120);
    }
    auto used = c.toolsUsed(out.runId);
    if (used.count("todo.delete") == 0) return "todo.delete 未进入决策";
    return {};
}

// ── E 墙钟上限截断（加速时钟缝 + 真 LLM；墙钟预算 = maxIterations×60s，
//    配置层面造不出「秒级」，故用 100× 加速时钟把「真截断」压进秒级）──
std::string scE(Ctx& c) {
    const int64_t start = nowMs();
    AgentLoop::NowMsFn accel = [start] {
        return start + (nowMs() - start) * 100;   // 1 真实秒 = 100 墙钟秒
    };
    AgentLoop l(*c.llm, core::ToolRegistry::instance(), *c.store, accel);
    auto t = makeTask(*c.store, "live-e",
                      "必须先调用 todo.list 工具查询任务，然后一句话报告任务数。",
                      1, false);
    auto out = c.run(l, t);
    if (out.status != "limited") {
        return "status=" + out.status + " error=" + out.error.substr(0, 120) +
               "（应为 limited 墙钟截断）";
    }
    if (out.error.find("墙钟") == std::string::npos) {
        return "错误信息不含墙钟: " + out.error.substr(0, 120);
    }
    return {};
}

// ── F 单一闸拒并发：run 进行中 startRunExclusive 必拒、Scheduler 入队不产生第二个 run ──
std::string scF(Ctx& c) {
    auto t = makeTask(*c.store, "live-f",
                      "依次调用 app.stats 和 md.list_tags，然后回复 OK。", 6, false);
    std::atomic<bool> cancel{false};
    std::thread runner([&] { c.loop->run(t, cancel); });
    std::string ev;
    if (!waitUntil([&] { return c.store->hasRunningRun().valueOr(false); }, 30000)) {
        ev = "30s 内未观察到 running run";
    } else {
        auto gate = c.store->startRunExclusive(t.id);
        if (gate.isOk()) {
            ev = "run 进行中 startRunExclusive 竟成功（runId=" +
                 std::to_string(gate.value()) + "）";
        } else {
            // 闸在 AgentLoop::run 内：常驻入队在 run 进行中派发 → 必被拒、不落 run 行
            c.scheduler->enqueueResident(t.id);
        }
    }
    runner.join();
    if (ev.empty()) {
        waitUntil([&] { return c.scheduler->residentQueueSize() == 0; }, 5000);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto runs = c.store->listRuns(t.id, 20);
        if (runs.isFail()) {
            ev = "listRuns 失败: " + runs.errorMessage();
        } else if (runs.value().size() != 1) {
            ev = "run 行数=" + std::to_string(runs.value().size()) +
                 "（应为 1，常驻入队不该产生第二个 run）";
        }
    }
    return ev;
}

// ── D 事件触发：冷却窗口内丢弃 / 窗口外真触发 ──
std::string scD(Ctx& c) {
    module::agent::TriggerConfig cfg;
    cfg.events = {"todo.created"};
    cfg.cooldownSec = 6;
    auto cfgJson = module::agent::encodeTriggerConfig(cfg);
    if (cfgJson.isFail()) return "encodeTriggerConfig 失败: " + cfgJson.errorMessage();
    auto t = makeTask(*c.store, "live-d", "只回复 EVT_OK，不要调用任何工具。", 3,
                      false, "event", cfgJson.value());
    auto runs0 = c.store->listRuns(t.id, 10);
    if (runs0.isFail()) return "listRuns 失败: " + runs0.errorMessage();
    const size_t base = runs0.value().size();

    core::event::bus().emit(core::event::kTodoCreated);   // 第 1 发：应触发
    if (!waitUntil([&] {
            auto r = c.store->listRuns(t.id, 10);
            return r.isOk() && r.value().size() > base;
        }, 20000)) {
        return "事件 1 未触发 run";
    }
    auto first = c.store->listRuns(t.id, 10);
    if (first.isOk() && !first.value().empty()) {
        const auto& row = first.value().front();
        waitUntil([&] {
            auto r = c.store->listRuns(t.id, 10);
            return r.isOk() && !r.value().empty() && r.value().front().id == row.id &&
                   r.value().front().status != "running";
        }, 60000);
    }

    core::event::bus().emit(core::event::kTodoCreated);   // 第 2 发：冷却窗口内 → 丢弃
    std::this_thread::sleep_for(std::chrono::seconds(2));
    if (c.scheduler->eventQueueSize() != 0) {
        return "冷却窗口内事件被排队（应直接丢弃）";
    }
    auto afterDrop = c.store->listRuns(t.id, 10);
    if (afterDrop.isOk() && afterDrop.value().size() != base + 1) {
        return "冷却窗口内事件多触发了 run：size=" +
               std::to_string(afterDrop.value().size());
    }

    std::this_thread::sleep_for(std::chrono::seconds(7));   // 越过 6s 冷却
    core::event::bus().emit(core::event::kTodoCreated);     // 第 3 发：应触发
    if (!waitUntil([&] {
            auto r = c.store->listRuns(t.id, 10);
            return r.isOk() && r.value().size() >= base + 2;
        }, 20000)) {
        return "冷却窗外事件未触发第二个 run";
    }
    auto enabled = c.store->setTaskEnabled(t.id, false);
    if (enabled.isFail()) return "停用任务失败: " + enabled.errorMessage();
    return {};
}

// ── C cron 触发：真等分钟翻转，run 真起 ──
std::string scC(Ctx& c) {
    module::agent::TriggerConfig cfg;
    cfg.cron = "* * * * *";   // 每分钟
    auto cfgJson = module::agent::encodeTriggerConfig(cfg);
    if (cfgJson.isFail()) return "encodeTriggerConfig 失败: " + cfgJson.errorMessage();
    auto t = makeTask(*c.store, "live-c", "只回复 CRON_OK，不要调用任何工具。", 3,
                      false, "schedule", cfgJson.value());
    bool fired = waitUntil([&] {
        auto r = c.store->listRuns(t.id, 10);
        return r.isOk() && !r.value().empty();
    }, 85000, 1000);   // 最长等一个分钟翻转 + tick 余量
    c.store->setTaskEnabled(t.id, false);   // 两条路径都立即停，防每分钟触发干扰后续场景
    if (!fired) return "85s 内 cron 未触发";
    auto row = c.store->listRuns(t.id, 10);
    if (row.isFail() || row.value().empty()) return "run 行丢失";
    const int64_t runId = row.value().front().id;
    bool done = waitUntil([&] {
        auto r = c.store->listRuns(t.id, 10);
        for (const auto& x : r.value()) {
            if (x.id == runId) return x.status != "running";
        }
        return true;
    }, 90000);
    if (!done) return "cron run 90s 未收尾";
    auto again = c.store->listRuns(t.id, 10);
    for (const auto& x : again.value()) {
        if (x.id == runId && x.status != "completed") {
            return "cron run status=" + x.status + " error=" +
                   x.error.value_or("").substr(0, 120);
        }
    }
    return {};
}

// ── I 运行汇总落库：agent_runs 的耗时 / 迭代 / token 字段真落值 ──
std::string scI(Ctx& c) {
    auto tasks = c.store->listTasks();
    if (tasks.isFail()) return "listTasks 失败: " + tasks.errorMessage();
    bool found = false;
    std::string ev = "无 completed run 可查";
    for (const auto& task : tasks.value()) {
        auto runs = c.store->listRuns(task.id, 20);
        if (runs.isFail()) continue;
        for (const auto& r : runs.value()) {
            if (r.status != "completed") continue;
            found = true;
            if (r.iterations <= 0) ev = "iterations=" + std::to_string(r.iterations);
            else if (!r.inputTokens.has_value() || *r.inputTokens <= 0) ev = "input_tokens 未落值";
            else if (!r.outputTokens.has_value() || *r.outputTokens <= 0) ev = "output_tokens 未落值";
            else if (!r.endedAt.has_value() || *r.endedAt < r.startedAt) ev = "ended_at < started_at";
            else ev.clear();
            if (ev.empty()) {
                auto sums = c.store->listTaskRunSummaries();
                if (sums.isFail() || sums.value().empty()) {
                    return "listTaskRunSummaries 为空";
                }
                bool any = false;
                for (const auto& s : sums.value()) {
                    if (s.taskId == task.id && s.okRuns > 0 && s.totalSeconds >= 0) any = true;
                }
                if (!any) return "汇总行缺少本任务的 okRuns";
                return {};
            }
            return ev;   // 第一个 completed run 就判，证据明确
        }
    }
    return found ? ev : "无 completed run 可查";
}

// ── 装配 ──
// 把用户数据根的 devdesk.db（含 -wal/-shm）复制到 dir，只读解析激活 profile，随即删副本。
// 返回 false = 无激活 profile / 读失败（驱动无法继续，属环境缺前置）。
bool loadRealProfile(const fs::path& stageDir, SnapshotSource& src,
                     std::string& host, std::string& model, std::string& err) {
    const fs::path realDb = core::paths::sqliteDbFile();   // 必须在 setenv DEVDESK_DATA_ROOT 之前取
    std::error_code ec;
    if (!fs::exists(realDb, ec)) {
        err = "真实库不存在: " + realDb.string();
        return false;
    }
    const fs::path copyDb = stageDir / "devdesk.db";
    fs::copy_file(realDb, copyDb, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        err = "复制主库失败: " + ec.message();
        return false;
    }
    for (const char* suffix : {"-wal", "-shm"}) {
        std::error_code ec2;
        fs::copy_file(realDb.string() + suffix, copyDb.string() + suffix,
                      fs::copy_options::overwrite_existing, ec2);
    }
    // 副本句柄析构即关库（SqliteDb 无显式 close）；凭据页文件在库关闭后再删
    {
        auto opened = core::data::SqliteDb::open(copyDb);
        if (opened.isFail()) {
            err = "打开副本库失败: " + opened.errorMessage();
            return false;
        }
        // keychainService 传独立测试命名空间：本对象生命周期内只调纯 DB 读
        //（getActiveProfile / loadApiKey），绝不调 cleanupOrphanKeychainEntries
        module::claude::ProfileRepository repo(opened.value(),
                                               "DevDesk-agent-live-test");
        auto active = repo.getActiveProfile();
        if (active.isFail()) {
            err = "取激活 profile 失败: " + active.errorMessage();
            return false;
        }
        auto key = repo.loadApiKey(active.value().id);
        std::string keyPlain;
        if (key.isOk()) keyPlain = key.value();
        if (keyPlain.empty()) {
            // DB 凭据列空 → 回落 cc-switch 只读源：
            // matchCcSwitchProviderByBaseUrl 按 host+path 匹配，key 仅在内存过一手
            const std::string ccId = module::claude::ccswitch::
                matchCcSwitchProviderByBaseUrl(active.value().baseUrl);
            if (!ccId.empty()) {
                auto lst = module::claude::ccswitch::listCcSwitchProviders();
                if (lst.status == module::claude::ccswitch::CcSwitchSourceStatus::Ok) {
                    for (const auto& p : lst.providers) {
                        if (p.id == ccId && !p.apiKey.empty()) {
                            keyPlain = p.apiKey;
                            break;
                        }
                    }
                }
            }
        }
        if (keyPlain.empty() && std::getenv("DEVDESK_AGENT_LIVE_ALLOW_KEYCHAIN") != nullptr) {
            // 仍空 → 该 profile 的 key 可能在 macOS Keychain（存量形态）。读单条
            // (服务=按真实数据根派生, 账户=claude:<id>) 可能弹授权对话框，须显式
            // DEVDESK_AGENT_LIVE_ALLOW_KEYCHAIN=1 才走；绝不枚举/清理钥匙串。
            const std::string realRoot = realDb.parent_path().string();
            const std::string services[] = {
                module::claude::keychainServiceForDataRoot(realRoot), "DevDesk"};
            for (const std::string& svc : services) {
                if (svc.empty()) continue;
                auto kc = platform::keychain::loadSecret(
                    svc, "claude:" + std::to_string(active.value().id));
                if (kc.isOk() && !kc.value().empty()) {
                    keyPlain = kc.value();
                    break;
                }
            }
        }
        if (keyPlain.empty()) {
            err = "读 API Key 失败：DB 凭据列空，cc-switch 无匹配 baseUrl 的供应商；"
                  "在 Claude 页重存一次 key（写入 DB 列）后重跑";
            return false;
        }
        src.profile.baseUrl = active.value().baseUrl;
        src.profile.apiKeyPlain = keyPlain;
        src.profile.authField = active.value().authField;
        src.profile.modelsConfigJson = active.value().modelsConfigJson;
        src.profile.defaultModel = active.value().model;
        src.profile.dialectOverride = active.value().dialectOverride;
        host = hostOf(active.value().baseUrl);
        model = active.value().model;
    }
    // 副本（含凭据页）立即销毁
    for (const char* suffix : {"", "-wal", "-shm"}) {
        std::error_code ec3;
        fs::remove(copyDb.string() + suffix, ec3);
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (std::getenv("DEVDESK_AGENT_LIVE") == nullptr ||
        std::strcmp(std::getenv("DEVDESK_AGENT_LIVE"), "1") != 0) {
        std::fprintf(stderr,
                     "缺 DEVDESK_AGENT_LIVE=1 环境门（防生产误用）。\n"
                     "显式入口：bash scripts/run-agent-live.sh\n");
        return 2;
    }

    // 临时目录（stage = 真 profile 只读副本的暂存；root = 隔离数据根）
    const std::string tag = std::to_string(nowMs());
    const fs::path stage = "/tmp/devdesk-agent-live-stage-" + tag;
    const fs::path root = "/tmp/devdesk-agent-live-root-" + tag;
    std::error_code ec;
    fs::create_directories(stage, ec);
    fs::create_directories(root, ec);

    SnapshotSource src;
    std::string host, model, profErr;
    if (!loadRealProfile(stage, src, host, model, profErr)) {
        std::fprintf(stderr, "真 profile 取用失败：%s\n（无激活 profile 属环境缺前置，"
                             "请在 Claude 页配置后再跑）\n", profErr.c_str());
        fs::remove_all(stage, ec);
        fs::remove_all(root, ec);
        return 2;
    }
    std::printf("== 智能体真机全场景验收 ==\n  provider host = %s\n  default 槽 model = %s\n"
                "  数据根（隔离） = %s\n", host.c_str(), model.c_str(), root.c_str());
    std::fflush(stdout);

    // 隔离数据根 + 应用库（进程内 setenv，exit 前还原删除）
    const char* savedRootRaw = std::getenv("DEVDESK_DATA_ROOT");
    const std::string savedRootEnv = savedRootRaw ? savedRootRaw : "";
    setenv("DEVDESK_DATA_ROOT", root.string().c_str(), 1);

    Ctx c;
    c.notesDir = root / "notes";
    module::markdown::NoteGitAutoCommit::setEnabledForTest(false);
    int exitCode = 0;
    {
        auto dbR = core::Database::open(root / "devdesk.db");
        if (dbR.isFail()) {
            std::fprintf(stderr, "临时库打开失败: %s\n", dbR.errorMessage().c_str());
            return 2;
        }
        c.appDb = std::make_unique<core::Database>(std::move(dbR.value()));
        core::registerAppDatabase(&c.appDb->db());

        c.todo = std::make_unique<module::todo::TodoService>(c.appDb->db());
        c.md = std::make_unique<module::markdown::MarkdownService>(c.appDb->db(),
                                                                   c.notesDir);
        c.claude = std::make_unique<module::claude::ClaudeService>(
            c.appDb->db(), "DevDesk-agent-live-test");
        // 替身 profile ×2（假 key）供 claude.* 工具操作；baseUrl 用真上游 →
        // claude.test_connection 是一次真请求（假 key → 4xx，正好喂 G 类失败语义）
        auto p1 = c.claude->createProfile("live-a", src.profile.baseUrl,
                                          "sk-agent-live-fake-a", model,
                                          /*activateIfNoneActive=*/true, 0, "bearer");
        auto p2 = c.claude->createProfile("live-b", src.profile.baseUrl,
                                          "sk-agent-live-fake-b", model,
                                          /*activateIfNoneActive=*/false, 0, "bearer");
        if (p1.isFail() || p2.isFail()) {
            std::fprintf(stderr, "替身 profile 建立失败: %s / %s\n",
                         p1.errorMessage().c_str(), p2.errorMessage().c_str());
            return 2;
        }
        // todo 列（工具的 todo.move 需要真实列 id）
        c.todo->createColumn("待办");
        c.todo->createColumn("进行中");
        c.todo->createColumn("已完成");

        // 工具注册（与各模块 onStart 同一入口）
        c.todoTools = std::make_unique<module::todo::TodoTools>(*c.todo);
        c.todoTools->registerAll();
        c.mdTools = std::make_unique<module::markdown::MarkdownTools>(*c.md);
        c.mdTools->registerAll();
        c.claudeTools = std::make_unique<module::claude::ClaudeTools>(*c.claude);
        c.claudeTools->registerAll();
        c.readSettingsTool = module::settings::makeAppReadSettingsTool(&c.appDb->db());
        core::ToolRegistry::instance().registerTool(c.readSettingsTool);
        c.statsTool = module::agent::makeAppStatsTool(c.appDb->db());
        core::ToolRegistry::instance().registerTool(c.statsTool);

        // AgentLoop + DirectLlmClient（真 HttpFn = platform::http，经捕获缝）+ Scheduler
        c.src = std::make_unique<SnapshotSource>();
        c.src->profile = src.profile;
        c.llm = std::make_unique<DirectLlmClient>(*c.src, c.capture.fn());
        c.store = std::make_unique<AgentStore>(c.appDb->db());
        c.loop = std::make_unique<AgentLoop>(*c.llm, core::ToolRegistry::instance(),
                                             *c.store);
        c.scheduler = std::make_unique<Scheduler>(*c.store, *c.loop);
        c.scheduler->start();
        // 事件接线与 AgentModule::ensureLlm 同构：bus 三事件 → Scheduler::onEvent
        for (const char* ev : {core::event::kTodoCreated, core::event::kTodoCompleted,
                               core::event::kMdSaved}) {
            c.eventSubs.push_back(core::event::bus().subscribe(
                ev, [&c](const std::string& name) { c.scheduler->onEvent(name); }));
        }

        const std::vector<std::pair<const char*, ScenarioFn>> scenarios = {
            {"G1_bad_baseurl", scG1},        {"G2_bad_key_401", scG2},
            {"H_context_note_system", scH},  {"B_single_tool", scB},
            {"A1_todo_6tools", scA1},        {"A2_md_5tools", scA2},
            {"A3_claude_4tools", scA3},      {"A4_app_2tools", scA4},
            {"G3_tool_error_feedback", scG3},{"G4_destructive_denied", scG4},
            {"E_wall_clock_limit", scE},     {"F_exclusive_gate", scF},
            {"D_event_trigger_cooldown", scD},{"C_cron_trigger", scC},
            {"I_run_summary_fields", scI},
        };
        // 场景过滤：argv 里出现的名字才跑（缺省全跑）
        std::set<std::string> filter;
        for (int i = 1; i < argc; ++i) filter.insert(argv[i]);

        int pass = 0;
        for (const auto& [name, fn] : scenarios) {
            if (!filter.empty() && filter.count(name) == 0) continue;
            std::printf("  -- %s\n", name);
            std::fflush(stdout);
            std::string ev;
            try {
                ev = fn(c);
            } catch (const std::exception& e) {
                ev = std::string("异常逃出场景: ") + e.what();
            } catch (...) {
                ev = "未知异常逃出场景";
            }
            report(name, ev);
            if (ev.empty()) ++pass;
        }
        std::printf("\n== 场景汇总：通过 %d / 失败 %d（共 %zu）==\n", pass, gFail,
                    filter.empty() ? scenarios.size() : filter.size());

        // 真实调用量（报告口径；key 永不出现在任何输出）
        {
            std::lock_guard<std::mutex> lock(c.capture.mu);
            std::printf("== 真实调用量：LLM/工具测试请求 %d 次（含失败探测）==\n",
                        c.capture.requests);
        }

        // 收尾：先停调度（cancel + join 在跑 run），再拆工具，再拆服务
        c.scheduler->stopAll();
        c.eventSubs.clear();
        c.todoTools->unregisterAll();
        c.mdTools->unregisterAll();
        c.claudeTools->unregisterAll();
        core::ToolRegistry::instance().unregisterTool("app.read_settings");
        core::ToolRegistry::instance().unregisterTool("app.stats");
        c.scheduler.reset();
        c.loop.reset();
        c.llm.reset();
        c.store.reset();
        c.claudeTools.reset();
        c.mdTools.reset();
        c.todoTools.reset();
        c.statsTool.reset();
        c.readSettingsTool.reset();
        c.claude.reset();
        c.md.reset();
        c.todo.reset();
        core::registerAppDatabase(nullptr);
        c.appDb.reset();
    }
    module::markdown::NoteGitAutoCommit::setEnabledForTest(true);
    if (savedRootEnv.empty()) {
        unsetenv("DEVDESK_DATA_ROOT");
    } else {
        setenv("DEVDESK_DATA_ROOT", savedRootEnv.c_str(), 1);
    }
    fs::remove_all(stage, ec);
    fs::remove_all(root, ec);
    exitCode = gFail == 0 ? 0 : 1;
    return exitCode;
}
