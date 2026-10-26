// agent_e2e_probe.cpp — 智能体五需求端到端 probe（真窗口 + 真调度 + 真工具 + 真网络）
//
// 五条流程在同一条真 AgentModule 装配链上跑通（scratch 库 + scratch 笔记目录，
// 不碰用户数据根；生产库只读一次激活 profile 的凭据列做密文搬运，全程不打印）：
//   F1 每天 17:00 查逾期 todo → 通知（daily 任务：当天首个调度周期派发一次）
//   F2 每天在【流水】新建 YYYY-MM-DD.md 内容「# 今日待办」（daily 任务）
//   F3 手动联网查今日热点 top10（hot.list 四源真出网；断网降级路径也算过但留证）
//   F4 闹钟 once_at 到点发通知 + 响过即停用（任务经真 UI 弹窗创建，once_at≈now+90s）
//   F5 激活 profile 连通检测（claude.test_connection 不传 id = 当前激活，真请求出网）
//
// 装配形态（与 agent_dialog_probe 同族）：真 AgentModule（Deps 注入 scratch 库 +
// 真 ClaudeService 直连 profile 源）+ 真跨模块工具注册（TodoTools / MarkdownTools /
// ClaudeTools 走各模块 onStart 同款 registerAll）+ 真 DirectLlmClient 出网
//（**绝不碰 8888 代理端口**——本会话推理链路走它）。通知断言用 spy Backend 包
// platform::notify（Receipt 契约），不断言系统横幅真弹出（那是真机 UAT）。
// daily 触发的派发由真 Scheduler tick 线程驱动（真实时钟，无注入）。
//
// 构图开关：--screenshot <png> = 任务创建弹窗（F4 填写完成态）；
//           --screenshot-seeded <png> = run 结果区（F3 详情弹窗完成态）。
//           未传 = 零开销（probe_harness 契约）。任一流程失败单独报，不连坐。
#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（链接需要，同 agent_dialog_probe）
#include "test_support.h"

#include "core/paths.h"
#include "core/time.h"
#include "module_agent/agent_module.h"
#include "module_agent/agent_store.h"
#include "module_markdown/markdown_service.h"
#include "module_markdown/markdown_tools.h"
#include "module_todo/todo_service.h"
#include "module_todo/todo_tools.h"
#include "claude_service.h"    // probes include dir 已含 src/module_claude_proxy
#include "claude_tools.h"
#include "platform/json.h"
#include "platform/notification.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <unistd.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// app:: 层实现随 dsl_app_impl.h 自给（探针不建应用级 UI，compose 直接驱动模块页）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_agent_e2e_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

namespace pj = platform::json;
using module::agent::AgentMessageRow;
using module::agent::AgentStore;
using module::agent::AgentTaskRow;

// ---------- 五流程目标文案（marker 是断言锚点，改成唯一短串） ----------
constexpr const char* kTodoMarker = "X7逾期样例任务";
constexpr const char* kAlarmMarker = "E2E闹钟Z9";

const char* kGoalF1 =
    "用 todo.list 工具查询逾期未完成的待办（overdue=true）。如有逾期任务，"
    "必须调用 app.notify 发系统通知：标题用「逾期待办提醒」，正文逐条列出逾期"
    "任务标题原文。查完即结束，不要做别的。";
const char* kGoalF2 =
    "用 md.create 工具在【流水】笔记本（即「流水」文件夹）下新建今日笔记，"
    "路径用「流水/<今天日期，格式 YYYY-MM-DD>.md」，正文第一行必须是"
    "「# 今日待办」。建完即结束。";
const char* kGoalF3 =
    "调用 hot.list 工具查询今日热点，然后输出热点 top10 清单，逐条标注来源。";
const char* kGoalF4 =
    "闹钟已到点。请立即调用 app.notify 发系统通知：标题用「闹钟提醒」，"
    "正文必须包含「E2E闹钟Z9」字样。发完即结束。";
const char* kGoalF5 =
    "调用 claude.test_connection 工具（不要传 id 参数，即测当前激活 Profile），"
    "然后报告返回的状态码。只调这一次。";

// ---------- 通知 spy：包 platform::notify::Backend，记录全部 send ----------
class SpyNotifyBackend final : public platform::notify::Backend {
public:
    core::Result<platform::notify::Status> send(
        const platform::notify::Notification& n) override {
        std::lock_guard<std::mutex> lock(mu_);
        sends_.push_back(n);
        return core::Result<platform::notify::Status>::ok(
            platform::notify::Status::Accepted);
    }
    platform::notify::Authorization authorizationStatus() override {
        return platform::notify::Authorization::Authorized;
    }
    void requestAuthorization() override {}
    bool anyContains(const std::string& needle) {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& s : sends_) {
            if (s.title.find(needle) != std::string::npos ||
                s.body.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
    int count() {
        std::lock_guard<std::mutex> lock(mu_);
        return static_cast<int>(sends_.size());
    }

private:
    std::mutex mu_;
    std::vector<platform::notify::Notification> sends_;
};

// ---------- 直连 profile 源：与 main.cpp 的 ClaudeDirectProfileSource 同映射 ----------
class ProbeProfileSource final : public core::LlmProfileSource {
public:
    explicit ProbeProfileSource(module::claude::ClaudeService& svc)
        : svc_(svc) {}
    core::Result<core::LlmDirectProfile> currentProfile() override {
        auto active = svc_.getActiveProfile();
        if (active.isFail()) {
            return core::Result<core::LlmDirectProfile>::fail(
                active.errorCode(), active.errorMessage());
        }
        const module::claude::Profile& p = active.value();
        auto key = svc_.loadApiKey(p.id);
        if (key.isFail()) {
            return core::Result<core::LlmDirectProfile>::fail(
                key.errorCode(), "读取 API Key 失败：" + key.errorMessage());
        }
        core::LlmDirectProfile out;
        out.baseUrl = p.baseUrl;
        out.apiKeyPlain = key.value();
        out.authField = p.authField;
        out.modelsConfigJson = p.modelsConfigJson;
        out.defaultModel = p.model;
        return core::Result<core::LlmDirectProfile>::ok(std::move(out));
    }

private:
    module::claude::ClaudeService& svc_;
};

// ---------- 生产凭据密文搬运（生产库只读一次；列值原样，绝不打印） ----------
struct ProdCredential {
    bool ok = false;
    std::string baseUrl, model, modelsConfig, authField, dialect;
    std::string keyEnc, keyCheck, keyHint;
};

ProdCredential readProdCredential() {
    ProdCredential out;
    const std::filesystem::path prodDb =
        core::paths::platformDefaultAppDataDir() / "devdesk.db";
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(prodDb.string().c_str(), &raw,
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        std::cerr << "agent_e2e_probe: 生产库打不开（READONLY）: "
                  << prodDb.string() << " → LLM 腿无凭据\n";
        if (raw != nullptr) sqlite3_close(raw);
        return out;
    }
    const char* sql =
        "SELECT base_url, model, models_config, auth_field, dialect_override, "
        "api_key_encrypted, api_key_check, api_key_hint "
        "FROM claude_profiles WHERE is_active=1 AND api_key_encrypted != '' "
        "LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        auto col = [&](int i) -> std::string {
            const unsigned char* t = sqlite3_column_text(stmt, i);
            return t != nullptr
                       ? std::string(reinterpret_cast<const char*>(t))
                       : std::string();
        };
        out.baseUrl = col(0);
        out.model = col(1);
        out.modelsConfig = col(2);
        out.authField = col(3);
        out.dialect = col(4);
        out.keyEnc = col(5);
        out.keyCheck = col(6);
        out.keyHint = col(7);
        out.ok = !out.baseUrl.empty() && !out.keyEnc.empty();
        std::cerr << "agent_e2e_probe: 生产凭据列已原样读出（不打印内容；"
                  << "baseUrl/keyEnc 列长度 " << out.baseUrl.size() << "/"
                  << out.keyEnc.size() << "）\n";
    } else {
        std::cerr << "agent_e2e_probe: 生产库无激活 profile 凭据（"
                  << prodDb.string() << "）→ LLM 腿不可用，各流程记未跑/败\n";
    }
    if (stmt != nullptr) sqlite3_finalize(stmt);
    sqlite3_close(raw);
    return out;
}

// scratch 库侧落行：createProfile 占位 → 凭据/配置列 SQL 原样覆盖（搬运唯一口）
bool seedScratchProfile(module::claude::ClaudeService& svc,
                        core::data::SqliteDb& db, const ProdCredential& cred) {
    auto created = svc.createProfile(
        "e2e-prod-alias", cred.baseUrl, "e2e-placeholder-never-sent",
        cred.model, /*activateIfNoneActive=*/true,
        cred.authField.empty() ? "both" : cred.authField);
    if (created.isFail()) {
        std::cerr << "agent_e2e_probe: scratch profile 创建失败: "
                  << created.errorMessage() << "\n";
        return false;
    }
    const int64_t id = created.value().id;
    auto prep = db.prepare(
        "UPDATE claude_profiles SET api_key_encrypted=?, api_key_check=?, "
        "api_key_hint=?, models_config=? WHERE id=?");
    if (prep.isFail()) {
        std::cerr << "agent_e2e_probe: 凭据覆盖 prepare 失败\n";
        return false;
    }
    auto& stmt = prep.value();
    stmt.bind(1, cred.keyEnc);
    stmt.bind(2, cred.keyCheck);
    stmt.bind(3, cred.keyHint);
    stmt.bind(4, cred.modelsConfig);
    stmt.bind(5, id);
    if (stmt.step().isFail()) {
        std::cerr << "agent_e2e_probe: 凭据覆盖 step 失败\n";
        return false;
    }
    return true;
}

// ---------- 小工具 ----------
std::string formatOnceDraft(int64_t epochSec) {
    std::tm tmLocal{};
    core::time::localTime(static_cast<std::time_t>(epochSec), &tmLocal);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
                  tmLocal.tm_year + 1900, tmLocal.tm_mon + 1, tmLocal.tm_mday,
                  tmLocal.tm_hour, tmLocal.tm_min);
    return buf;
}

std::string todayDateStr() {
    return formatOnceDraft(core::time::nowUnix()).substr(0, 10);
}

std::string readSmallFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<std::filesystem::path> recursiveFiles(
    const std::filesystem::path& root) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied,
             ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) out.push_back(it->path());
    }
    return out;
}

// 消息层工具面提取：tool_use 名单 + 全部 tool_result 的 content 文本
bool hasToolUse(const std::vector<AgentMessageRow>& msgs,
                const std::string& toolName) {
    for (const auto& m : msgs) {
        if (m.role != "assistant") continue;
        auto parsed = pj::parse(m.contentJson);
        if (parsed.isFail() || !parsed.value().isArray()) continue;
        const pj::JsonValue arr = parsed.value();
        for (int i = 0; i < arr.size(); ++i) {
            if (!arr[i].isObject()) continue;
            auto obj = arr[i].asObject();
            if (!obj.has("type") || !obj.get("type").isString()) continue;
            if (obj.get("type").asString() != "tool_use") continue;
            if (obj.has("name") && obj.get("name").isString() &&
                obj.get("name").asString() == toolName) {
                return true;
            }
        }
    }
    return false;
}

// tool_result.content 三态全收（AgentLoop::makeResult 落的是 text block 对象，
// 不是裸字符串——只按字符串读会恒空）：
//   字符串原文 / {"type":"text","text":..} 对象 / blocks 数组
void collectContentText(const pj::JsonValue& content,
                        std::vector<std::string>& out) {
    if (content.isString()) {
        out.push_back(content.asString());
        return;
    }
    if (content.isObject()) {
        auto obj = content.asObject();
        if (obj.has("text") && obj.get("text").isString()) {
            out.push_back(obj.get("text").asString());
        }
        return;
    }
    if (content.isArray()) {
        for (int i = 0; i < content.size(); ++i) {
            collectContentText(content[i], out);
        }
    }
}

std::vector<std::string> toolResultTexts(
    const std::vector<AgentMessageRow>& msgs) {
    std::vector<std::string> out;
    for (const auto& m : msgs) {
        if (m.role != "user") continue;
        auto parsed = pj::parse(m.contentJson);
        if (parsed.isFail() || !parsed.value().isArray()) continue;
        const pj::JsonValue arr = parsed.value();
        for (int i = 0; i < arr.size(); ++i) {
            if (!arr[i].isObject()) continue;
            auto obj = arr[i].asObject();
            if (!obj.has("type") || !obj.get("type").isString()) continue;
            if (obj.get("type").asString() != "tool_result") continue;
            if (obj.has("content")) {
                collectContentText(obj.get("content"), out);
            }
        }
    }
    return out;
}

std::string truncateForLog(const std::string& text, std::size_t n) {
    if (text.size() <= n) return text;
    return text.substr(0, n) + "...(截断)";
}

bool runSettled(const std::string& status) {
    return status == "completed" || status == "failed" ||
           status == "limited" || status == "cancelled";
}


struct FlowResult {
    std::string name;
    std::string verdict;  // 过 / 降级过 / 败 / 未跑
    std::string detail;
};

}  // namespace

int main(int argc, char* argv[]) {
    // scratch 数据根必须最先设：core::paths（日志文件等）随它走，不碰用户真实根
    const std::filesystem::path scratchRoot =
        std::filesystem::temp_directory_path() /
        ("devdesk_agent_e2e_root_" + std::to_string(static_cast<int>(getpid())));
    const std::filesystem::path notesDir =
        std::filesystem::temp_directory_path() /
        ("devdesk_agent_e2e_notes_" + std::to_string(static_cast<int>(getpid())));
    std::error_code ec;
    // 清上次残留（probe 启动时 remove_all 再重建，避免累积；目录名带 pid 天然区分）
    std::filesystem::remove_all(scratchRoot, ec);
    std::filesystem::remove_all(notesDir, ec);
    std::filesystem::create_directories(scratchRoot, ec);
    std::filesystem::create_directories(notesDir, ec);
    test_support::setEnvVar("DEVDESK_DATA_ROOT", scratchRoot.string().c_str());

    const auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("agent_e2e_probe", args.windowW,
                                    args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // ---------- scratch 世界 ----------
    test_support::TestDb tdb("e2e_", "agentflow");
    core::data::SqliteDb& db = tdb.handle();

    const auto tStart = std::chrono::steady_clock::now();

    // 通知腿：复位 + 装 spy（先于一切 send）
    platform::notify::resetForTest();
    SpyNotifyBackend spy;
    platform::notify::setBackendForTest(&spy);

    // LLM 腿：生产凭据密文搬运（只读一次）
    const ProdCredential cred = readProdCredential();
    bool llmLegReady = false;
    module::claude::ClaudeService claudeSvc(db);
    ProbeProfileSource profileSource(claudeSvc);
    if (cred.ok && seedScratchProfile(claudeSvc, db, cred)) {
        llmLegReady = true;
        std::cerr << "agent_e2e_probe: 密文搬运完成（scratch 库激活 profile "
                     "已指向生产上游）\n";
    }

    // F1 种子：一条逾期未完成 todo
    module::todo::TodoService todoSvc(db);
    {
        auto col = todoSvc.createColumn("待办");
        if (col.isFail()) {
            std::cerr << "agent_e2e_probe: todo 列创建失败: "
                      << col.errorMessage() << "\n";
        } else {
            module::todo::NewTaskAttributes attrs;
            attrs.dueAt = core::time::nowUnix() - 3600;  // 1 小时前 = 逾期
            auto task = todoSvc.createTask(col.value().id,
                                           std::string(kTodoMarker) + "（缴费）",
                                           attrs);
            if (task.isFail()) {
                std::cerr << "agent_e2e_probe: 逾期 todo 种子失败: "
                          << task.errorMessage() << "\n";
            }
        }
    }

    // F2 的 scratch 笔记目录（库在临时路径，不与笔记目录嵌套——FileWatcher 契约；
    // 已在 probe 启动时 remove_all + 重建，此处不再定义）
    module::markdown::MarkdownService mdSvc(db, notesDir);

    // 种子任务 F1/F2（daily，当天首 tick 即派发）与 F3/F5（manual）
    int64_t idF1 = 0, idF2 = 0, idF3 = 0, idF5 = 0;
    {
        AgentStore seedStore(db);
        auto mk = [&](const char* name, const char* goal, const char* trigger,
                      int iter) -> int64_t {
            auto r = seedStore.createTask(name, goal, trigger, "default", iter,
                                          false);
            return r.isOk() ? r.value() : 0;
        };
        idF1 = mk("E2E每日逾期检查", kGoalF1, "daily", 8);
        idF2 = mk("E2E每日流水日记", kGoalF2, "daily", 8);
        idF3 = mk("E2E热点top10", kGoalF3, "manual", 8);
        idF5 = mk("E2E连通检测", kGoalF5, "manual", 4);
        if (idF1 == 0 || idF2 == 0 || idF3 == 0 || idF5 == 0) {
            std::cerr << "agent_e2e_probe: 种子任务 createTask 失败\n";
        }
    }

    // 真工具注册（各模块 onStart 同款批次口）
    module::todo::TodoTools todoTools(todoSvc);
    module::markdown::MarkdownTools mdTools(mdSvc);
    module::claude::ClaudeTools claudeTools(claudeSvc);
    todoTools.registerAll();
    mdTools.registerAll();
    claudeTools.registerAll();

    // 真 AgentModule（真 DirectLlmClient + 真 Scheduler 真实时钟；http=nullptr=生产）
    module::agent::AgentModule::Deps deps;
    deps.db = &db;
    deps.profileSource = &profileSource;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    AgentStore probeStore(db);
    auto latestRunStatus = [&](int64_t taskId) -> std::string {
        auto runs = probeStore.listRuns(taskId, 1);
        if (runs.isFail() || runs.value().empty()) return std::string();
        return runs.value()[0].status;
    };
    auto runMessages = [&](int64_t taskId) -> std::vector<AgentMessageRow> {
        auto runs = probeStore.listRuns(taskId, 1);
        if (runs.isFail() || runs.value().empty()) return {};
        auto msgs = probeStore.listMessages(runs.value()[0].id);
        return msgs.isOk() ? msgs.value() : std::vector<AgentMessageRow>{};
    };

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    const std::string pageId = "agent";
    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    core::dsl::Ui* ui = nullptr;
    auto composeFrame = [&]() {
        probe::composeModuleFrame(runtime, window, pageId, w, h,
                                  [&](eui::Ui& u, const eui::Screen& screen) {
                                      ui = &u;
                                      mod.compose(u, screen);
                                  });
    };
    auto findEl = [&](const std::string& id) -> const core::dsl::Element* {
        if (ui == nullptr) return nullptr;
        if (const core::dsl::Element* e = ui->find(id)) return e;
        return ui->find(id + ".hit");
    };
    auto clickEl = [&](const core::dsl::Element& e) {
        const float cx = e.frame.x + e.frame.width * 0.5f;
        const float cy = e.frame.y + e.frame.height * 0.5f;
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Press, {});
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Release, {});
    };
    auto clickById = [&](const std::string& id) -> bool {
        const core::dsl::Element* e = findEl(id);
        if (e == nullptr) return false;
        clickEl(*e);
        composeFrame();
        return true;
    };
    // 有界轮询：泵 UI 帧 + 150ms 短间隔，禁长睡
    auto pumpUntil = [&](int deadlineMs,
                         const std::function<bool()>& done) -> bool {
        const auto start = std::chrono::steady_clock::now();
        while (!done()) {
            const auto elapsedMs = std::chrono::duration_cast<
                std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                           start)
                                           .count();
            if (elapsedMs > deadlineMs) return false;
            composeFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        return true;
    };

    std::vector<FlowResult> flows;
    bool passed = true;
    auto check = [&](bool cond, const std::string& what) {
        if (!cond) {
            std::cerr << "agent_e2e_probe: FAIL " << what << "\n";
            passed = false;
        }
    };

    composeFrame();

    // ---------- F4 经真 UI 弹窗创建（once_at = now+90s 取整到分） ----------
    // UAT6 4 步向导：基础信息 → 触发 → 指令 → 执行与安全；非末步 save 只前进不创建
    //（agent_module.cpp:2768-2794）。每步用「下一步」真实点按推进，并断言下一
    // 步专属元素已渲染，末步才点「创建」。
    const std::int64_t onceAtSec =
        (core::time::nowUnix() + 90) / 60 * 60;  // floor(now+90s 到分钟)：距 now ≥+30s 且 ≤+90s
    bool dialogOk = false;
    if (clickById("agent.page.newTask") &&
        findEl("agent.dialog.task") != nullptr) {
        // 点「下一步/创建」并断言目标步专属元素出现（canNext 为假时点击无效）
        auto nextStep = [&](const char* expectId) -> bool {
            const bool clicked = clickById("agent.dialog.task.save.bg");
            composeFrame();
            const bool arrived = findEl(expectId) != nullptr;
            if (!clicked || !arrived) {
                std::cerr << "agent_e2e_probe: 向导未到达 " << expectId
                          << "（clicked=" << clicked << "）\n";
            }
            return clicked && arrived;
        };

        // Step 0「基础信息」：名称留空 = 按任务目标自动取名 → 下一步
        bool stepOk = nextStep("agent.dialog.task.trigger");
        // Step 1「触发」：闹钟（UAT5 segment 选项钮，kTriggerOnceIdx=5 的表单契约）
        if (stepOk) {
            if (!clickById("agent.dialog.task.trigger.item.5.bg")) {
                std::cerr << "agent_e2e_probe: 触发 segment「闹钟」项未出现\n";
                stepOk = false;
            }
            // 闹钟时间：输入框只能追加，用产品测试缝 setEditOnceDraft 覆盖写
            mod.setEditOnceDraft(formatOnceDraft(onceAtSec));
            composeFrame();
        }
        // Step 2「指令」：点击聚焦 → queueText（真实输入事件路径）
        if (stepOk) stepOk = nextStep("agent.dialog.task.goal");
        if (stepOk && clickById("agent.dialog.task.goal")) {
            probe::queueText(window, runtime, kGoalF4);
            composeFrame();
        }
        // Step 3「执行与安全」：测试试运行按钮仅末步渲染，作为到位判据
        if (stepOk) stepOk = nextStep("agent.dialog.task.test");
        // 构图①：任务创建弹窗（末步填写完成态）
        if (!args.screenshotPath.empty()) {
            if (!probe::captureFramePng(window, runtime, args.screenshotPath)) {
                std::cerr << "agent_e2e_probe: --screenshot 落图失败: "
                          << args.screenshotPath << "\n";
                passed = false;
            }
        }
        // 末步「创建」：此时才真正落库
        if (stepOk) clickById("agent.dialog.task.save.bg");
        composeFrame();
        dialogOk = stepOk && findEl("agent.dialog.task") == nullptr;
        if (!dialogOk) {
            const core::dsl::Element* err = findEl("agent.dialog.task.error");
            std::cerr << "agent_e2e_probe: F4 弹窗未关（保存失败？）error='"
                      << (err != nullptr ? err->text : std::string("<无>"))
                      << "'\n";
        }
    } else {
        std::cerr << "agent_e2e_probe: 新建任务弹窗打不开\n";
    }
    check(dialogOk, "F4 任务经真 UI 弹窗创建");

    // F4 任务行落到 store
    int64_t idF4 = 0;
    pumpUntil(15000, [&] {
        auto tasks = probeStore.listTasks();
        if (tasks.isFail()) return false;
        for (const auto& t : tasks.value()) {
            if (t.triggerType == "once") {
                idF4 = t.id;
                return true;
            }
        }
        return false;
    });
    check(idF4 != 0, "F4 once 任务已在库（triggerType=once）");

    // ---------- F1/F2：daily 已在模块启动首 tick 派发，等跑完 ----------
    // 任何 run 都没起来（LLM 腿全死）→ 早停诊断，避免五路各白等满超时
    const bool anyRunStarted = pumpUntil(20000, [&] {
        return !latestRunStatus(idF1).empty() || !latestRunStatus(idF2).empty();
    });
    if (!llmLegReady) {
        // 前置缺失 = 探针失败（不是静默跳过）：W5 门禁只读退出码，未跑仍 PASS
        // 等于假绿。想跑通需：本机生产库存在激活 Claude Profile + 可用 API key。
        flows.push_back({"F1 逾期todo通知", "未跑", "无生产凭据，LLM 腿不可用"});
        flows.push_back({"F2 每日流水笔记", "未跑", "无生产凭据，LLM 腿不可用"});
        flows.push_back({"F3 热点top10", "未跑", "无生产凭据，LLM 腿不可用"});
        flows.push_back({"F4 闹钟通知", "未跑", "无生产凭据，LLM 腿不可用"});
        flows.push_back({"F5 连通检测", "未跑", "无生产凭据，LLM 腿不可用"});
        std::cerr << "agent_e2e_probe: FAIL 前置缺失——生产凭据密文搬运失败，"
                     "LLM 腿不可用，五流程未执行（F1-F5 计「未跑」而非通过）。"
                     "需本机生产库存在激活 Profile 且 API key 可读\n";
        passed = false;
    } else {
        // F1
        {
            const bool settled = pumpUntil(150000, [&] {
                return runSettled(latestRunStatus(idF1));
            });
            const std::string st = latestRunStatus(idF1);
            const bool notified = spy.anyContains(kTodoMarker);
            if (!settled) {
                flows.push_back({"F1 逾期todo通知", "败",
                                 "等待超时（150s），最后状态='" + st + "'"});
                passed = false;
            } else if (st != "completed") {
                flows.push_back({"F1 逾期todo通知", "败",
                                 "run 状态=" + st + "（期望 completed）"});
                passed = false;
            } else if (!notified) {
                flows.push_back({"F1 逾期todo通知", "败",
                                 "run completed 但 spy 未见含逾期任务标题的通知"});
                passed = false;
            } else {
                flows.push_back({"F1 逾期todo通知", "过",
                                 "run completed + app.notify 已发（含 '" +
                                     std::string(kTodoMarker) + "'）"});
            }
        }
        // F2
        {
            const bool settled = pumpUntil(150000, [&] {
                return runSettled(latestRunStatus(idF2));
            });
            const std::string st = latestRunStatus(idF2);
            const std::string date = todayDateStr();
            std::filesystem::path notePath =
                notesDir / "流水" / (date + ".md");
            if (!std::filesystem::exists(notePath)) {
                for (const auto& f : recursiveFiles(notesDir)) {
                    if (f.filename() == date + ".md") notePath = f;
                }
            }
            const std::string content =
                std::filesystem::exists(notePath)
                    ? readSmallFile(notePath)
                    : std::string();
            if (!settled) {
                flows.push_back({"F2 每日流水笔记", "败",
                                 "等待超时（150s），最后状态='" + st + "'"});
                passed = false;
            } else if (content.find("# 今日待办") == std::string::npos) {
                flows.push_back({"F2 每日流水笔记", "败",
                                 "未找到 " + date + ".md 或内容缺「# 今日待办」"
                                 "（实际路径=" + notePath.string() + "）"});
                passed = false;
            } else {
                flows.push_back({"F2 每日流水笔记", "过",
                                 "文件真落盘 " + notePath.string() +
                                     "，run=" + st});
            }
        }

        // ---------- F3：手动启动（真点击「启动」，详情弹窗随之打开） ----------
        {
            composeFrame();
            const bool clicked =
                idF3 != 0 && clickById("agent.task." + std::to_string(idF3) +
                                       ".run");
            bool settled = false;
            if (clicked) {
                composeFrame();
                settled = pumpUntil(180000, [&] {
                    return runSettled(latestRunStatus(idF3));
                });
            }
            const std::string st = latestRunStatus(idF3);
            auto msgs = runMessages(idF3);
            const bool hotCalled = hasToolUse(msgs, "hot.list");
            int bestItems = -1;
            bool degraded = false;
            std::string hotSource;
            for (const auto& text : toolResultTexts(msgs)) {
                auto parsed = pj::parse(text);
                if (parsed.isFail() || !parsed.value().isObject()) continue;
                auto obj = parsed.value().asObject();
                if (!obj.has("items") || !obj.get("items").isArray()) continue;
                const int n = obj.get("items").size();
                if (n > bestItems) {
                    bestItems = n;
                    degraded = obj.has("degraded") &&
                               obj.get("degraded").isBool() &&
                               obj.get("degraded").asBool();
                    hotSource = obj.has("source") && obj.get("source").isString()
                                    ? obj.get("source").asString()
                                    : std::string();
                }
            }
            // 构图②：run 结果区（详情弹窗稳定帧）——不管过败都拍，留证据
            if (clicked && !args.screenshotSeedPath.empty()) {
                composeFrame();
                composeFrame();
                if (!probe::captureFramePng(window, runtime,
                                            args.screenshotSeedPath)) {
                    std::cerr << "agent_e2e_probe: --screenshot-seeded 落图失败:"
                              " " << args.screenshotSeedPath << "\n";
                    passed = false;
                }
            }
            // 关详情弹窗，给 F5 让路
            clickById("agent.dialog.detail.close");
            composeFrame();
            if (!clicked) {
                flows.push_back({"F3 热点top10", "败", "「启动」点击未生效"});
                passed = false;
            } else if (!settled) {
                flows.push_back({"F3 热点top10", "败",
                                 "等待超时（180s），最后状态='" + st + "'"});
                passed = false;
            } else if (!hotCalled) {
                flows.push_back({"F3 热点top10", "败",
                                 "run 消息里没有 hot.list 的 tool_use"});
                passed = false;
            } else if (bestItems >= 1) {
                std::ostringstream d;
                d << "hot.list " << bestItems << " 条条目，source='" << hotSource
                  << (degraded ? "'，走降级路径（留证：degraded=true）" : "'");
                flows.push_back({"F3 热点top10", degraded ? "降级过" : "过",
                                 d.str()});
                if (degraded) std::cerr << "agent_e2e_probe: F3 降级留证: " << d.str() << "\n";
            } else {
                // 留证：failureResult 的 attempts 数组带逐源错误原文
                std::string diag;
                for (const auto& text : toolResultTexts(msgs)) {
                    if (text.find("attempts") != std::string::npos ||
                        text.find("attempted") != std::string::npos) {
                        diag = truncateForLog(text, 500);
                        break;
                    }
                }
                std::cerr << "agent_e2e_probe: F3 逐源错误留证: " << diag << "\n";
                flows.push_back({"F3 热点top10", "败",
                                 "hot.list 被调但所有结果 0 条目（四源全败且"
                                 "无缓存）；attempts=" + diag});
                passed = false;
            }
        }

        // ---------- F5：手动启动连通检测 ----------
        {
            composeFrame();
            const bool clicked =
                idF5 != 0 && clickById("agent.task." + std::to_string(idF5) +
                                       ".run");
            bool settled = false;
            if (clicked) {
                composeFrame();
                settled = pumpUntil(120000, [&] {
                    return runSettled(latestRunStatus(idF5));
                });
            }
            const std::string st = latestRunStatus(idF5);
            auto msgs = runMessages(idF5);
            const bool connCalled =
                hasToolUse(msgs, "claude.test_connection");
            bool sawStatusCode = false;
            bool ok2xx = false;
            int64_t statusCode = -1;
            for (const auto& text : toolResultTexts(msgs)) {
                if (text.find("statusCode") == std::string::npos) continue;
                auto parsed = pj::parse(text);
                if (parsed.isFail() || !parsed.value().isObject()) continue;
                auto obj = parsed.value().asObject();
                if (!obj.has("statusCode") || !obj.get("statusCode").isNumber())
                    continue;
                sawStatusCode = true;
                statusCode = obj.get("statusCode").asInt();
                ok2xx = obj.has("ok") && obj.get("ok").isBool() &&
                        obj.get("ok").asBool();
            }
            clickById("agent.dialog.detail.close");
            composeFrame();
            if (!clicked) {
                flows.push_back({"F5 连通检测", "败", "「启动」点击未生效"});
                passed = false;
            } else if (!settled) {
                flows.push_back({"F5 连通检测", "败",
                                 "等待超时（120s），最后状态='" + st + "'"});
                passed = false;
            } else if (!connCalled) {
                flows.push_back({"F5 连通检测", "败",
                                 "run 消息里没有 claude.test_connection 的 "
                                 "tool_use"});
                passed = false;
            } else if (sawStatusCode) {
                std::ostringstream d;
                d << "真请求已发出：上游 statusCode=" << statusCode
                  << (ok2xx ? "（2xx 连通成功）" : "（非 2xx，上游拒绝但请求真实出网）");
                flows.push_back({"F5 连通检测", "过", d.str()});
            } else {
                // 留证：toolError 的错误原文（含网络层失败原因）
                std::string diag;
                for (const auto& text : toolResultTexts(msgs)) {
                    if (text.find("连通") != std::string::npos ||
                        text.find("测试") != std::string::npos ||
                        text.find("Profile") != std::string::npos) {
                        diag = truncateForLog(text, 300);
                        break;
                    }
                }
                std::cerr << "agent_e2e_probe: F5 工具错误留证: " << diag << "\n";
                const bool transient =
                    diag.find("超时") != std::string::npos ||
                    diag.find("timeout") != std::string::npos ||
                    diag.find("timed out") != std::string::npos ||
                    diag.find("连接") != std::string::npos;
                if (transient) {
                    flows.push_back({"F5 连通检测", "降级过",
                                     "请求已发起但未拿到状态码（证据：" + diag +
                                     "）；完成性未证，需人工复核"});
                } else {
                    flows.push_back({"F5 连通检测", "败",
                                     "tool 被调但无 statusCode（证据：" + diag +
                                     "）"});
                    passed = false;
                }
            }
        }

        // ---------- F4：等 once_at 真触发（有界轮询，deadline=once_at+180s） ----------
        {
            const int64_t deadlineSec = onceAtSec + 180;
            bool fired = false;
            {
                while (core::time::nowUnix() < deadlineSec) {
                    if (spy.count() > 0 && runSettled(latestRunStatus(idF4))) {
                        fired = true;
                        break;
                    }
                    composeFrame();
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(500));
                }
            }
            const std::string st = latestRunStatus(idF4);
            bool disabledAfter = false;
            if (idF4 != 0) {
                auto t = probeStore.getTask(idF4);
                disabledAfter = t.isOk() && !t.value().enabled;
            }
            const bool notified = spy.anyContains(kAlarmMarker);
            if (!fired) {
                flows.push_back({"F4 闹钟通知", "败",
                                 "once_at=" + std::to_string(onceAtSec) +
                                     " 后 180s 内未见通知+run 收尾（状态='" +
                                     st + "'）"});
                passed = false;
            } else if (!notified) {
                flows.push_back({"F4 闹钟通知", "败",
                                 "run 收尾（" + st + "）但通知缺 '" +
                                     std::string(kAlarmMarker) + "'"});
                passed = false;
            } else if (!disabledAfter) {
                flows.push_back({"F4 闹钟通知", "败",
                                 "通知已发但任务未自动停用（enabled 仍真）"});
                passed = false;
            } else {
                flows.push_back({"F4 闹钟通知", "过",
                                 "Scheduler 到点派发 + 通知已发（含 '" +
                                     std::string(kAlarmMarker) + "'）+ 任务"
                                     "响过即停用，run=" + st});
            }
        }

        // F1 附加断言：daily 当天不重发（全程恰好 1 条 run）
        if (idF1 != 0) {
            auto runs = probeStore.listRuns(idF1, 10);
            const int n = runs.isOk() ? static_cast<int>(runs.value().size()) : -1;
            check(n == 1, "F1 daily 全程恰好 1 条 run（实测 " +
                              std::to_string(n) + "，当天重发即违例）");
        }
        if (!anyRunStarted && llmLegReady) {
            std::cerr << "agent_e2e_probe: ⚠ 20s 内无任何 run 起跑——Scheduler"
                         " 或 LLM 通道可能未就绪\n";
        }
    }

    // 失败透传：把所有 run 的 error 字段原文打到 stderr，排查不再依赖 scratch 考古
    {
        std::vector<int64_t> allTaskIds = {idF1, idF2, idF3, idF4, idF5};
        for (int64_t tid : allTaskIds) {
            if (tid == 0) continue;
            auto runs = probeStore.listRuns(tid, 1);
            if (runs.isFail() || runs.value().empty()) continue;
            const auto& run = runs.value()[0];
            if (run.error.has_value() && !run.error->empty()) {
                std::cerr << "agent_e2e_probe: run<" << run.id << "> error 原文: "
                          << truncateForLog(run.error.value(), 500) << "\n";
            }
        }
    }

    // ---------- 结论表 ----------
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - tStart)
                               .count();
    std::cout << "agent_e2e_probe: ===== 五流程结论 =====\n";
    for (const auto& f : flows) {
        std::cout << "  [" << f.verdict << "] " << f.name << " — " << f.detail
                  << "\n";
    }
    std::cout << "  LLM 腿: " << (llmLegReady ? "密文搬运成功" : "搬运失败")
              << (llmLegReady ? "（生产凭据列原样复制，未打印）" : "")
              << "\n";
    std::cout << "  通知 spy 收到 send: " << spy.count() << " 条\n";
    std::cout << "  probe 总耗时: " << (elapsedMs / 1000) << "s\n";

    // ---------- 收尾（顺序同 agent_dialog_probe：模块 → runtime → 窗口 → 后端） ----------
    mod.onStop();
    todoTools.unregisterAll();
    mdTools.unregisterAll();
    claudeTools.unregisterAll();
    platform::notify::resetForTest();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    // scratch 根/笔记目录保留供排查（下次运行前自清），不进用户数据根
    std::cerr << "agent_e2e_probe: scratch 保留于 " << scratchRoot.string()
              << "（下次运行前自清）\n";
    if (!passed) {
        std::cerr << "FAIL\n";
    } else {
        std::cout << "agent_e2e_probe: PASS\n";
    }
    return passed ? 0 : 1;
}
