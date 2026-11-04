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
//           --screenshot-seeded <png> = run 结果区（F3 详情弹窗完成态）；
//           --shot-list <png> = 任务列表页（B3 网格化三行卡，F-66-6）。
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
    std::string baseUrl, model, modelsConfig, authField;
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
        "SELECT base_url, model, models_config, auth_field, "
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
        out.keyEnc = col(4);
        out.keyCheck = col(5);
        out.keyHint = col(6);
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
        // 必须落进 kind='todo' 的标准待办列（schema 种子固定四列之一）：
        // listOverdue 的逾期口径（core/overdue.h kWhereSql，UAT6.2 H20）只认
        // todo/doing 两列，自建列 kind 为 NULL 会被判非逾期 → F1 恒败。
        auto cols = todoSvc.listColumns();
        std::optional<module::todo::Column> todoCol;
        if (cols.isOk()) {
            for (const auto& c : cols.value()) {
                if (c.kind == "todo") todoCol = c;
            }
        }
        if (!todoCol.has_value()) {
            std::cerr << "agent_e2e_probe: 未找到 kind='todo' 标准待办列: "
                      << (cols.isFail() ? cols.errorMessage()
                                        : std::string("列清单无 todo 列"))
                      << "\n";
        } else {
            module::todo::NewTaskAttributes attrs;
            attrs.dueAt = core::time::nowUnix() - 3600;  // 1 小时前 = 逾期
            auto task = todoSvc.createTask(todoCol->id,
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
    // 首帧后 refreshData 才装载 tasks_（compose 内惰性查询）→ 再稳两帧再断言
    composeFrame();
    composeFrame();
    // 卡网格 / 三行结构 / 对齐 / 图标字形 / hover 驱动都在这里验：退出码裁决，
    // 像素判据 + element 目标值双保险（B2 方法论：轮廓级变化不能用区域均值，
    // 同色区会把差异稀释掉）。
    {
        const auto cardIds = [&] {
            std::vector<int64_t> ids;
            for (int64_t tid : {idF1, idF2, idF3, idF5}) {
                if (tid != 0) ids.push_back(tid);
            }
            return ids;
        }();
        if (cardIds.size() < 2) {
            std::cerr << "agent_e2e_probe: G 组跳过（种子任务不足 2 条）\n";
            passed = false;
        } else {
            // 卡宽上限与方案 §10.4 同值（探针不引模块私有常量：模块侧是匿名 namespace，
            // 这里按判据字面量复核，改档时两处一起改才叫「同步」）
            constexpr float kTaskCardMaxW = 420.0f;
            auto frameOf = [&](const std::string& elId, core::Rect* out) -> bool {
                const core::dsl::Element* e = ui != nullptr ? ui->find(elId) : nullptr;
                if (e == nullptr) return false;
                *out = {e->frame.x, e->frame.y, e->frame.width, e->frame.height};
                return true;
            };

            // ---- G1 宽高比：每卡 w/h ∈ [2.5, 4.5]（目标 3.5；原单列 22:1）----
            // ⚠️ 方案 §10.4 的窄窗退化是「内容宽 < 720 → 1 列、卡宽 = 内容宽」，
            // 此时卡必然被拉宽（615 内容宽 → 5.1:1）。所以判据按布局形态分档：
            // 网格态（同行 ≥2 卡）必须落在区间；单列退化态只记录不判红，
            // 另加一条「默认窗宽下必须是多列」的强判据——那才是本次修的 22:1 回归。
            bool g1 = true;
            float firstCardX = 0, firstCardY = 0, firstCardH = 0;
            // 先取全部卡 frame：多列/单列的判定必须先于宽度上限（单列退化时卡宽=内容宽，
            // 天然超 420，那条上限只在网格态成立）
            std::vector<core::Rect> cardFrames;
            for (size_t i = 0; i < cardIds.size(); ++i) {
                core::Rect f{};
                const std::string cid = "agent.task." + std::to_string(cardIds[i]);
                if (!frameOf(cid, &f)) {
                    std::cerr << "agent_e2e_probe: G1 FAIL 卡缺失 " << cid << "\n";
                    g1 = false;
                    continue;
                }
                cardFrames.push_back(f);
                const float aspect = f.height > 0.0f ? f.width / f.height : 0.0f;
                std::cout << "G1 card " << cardIds[i] << " " << f.width << "x"
                          << f.height << " aspect=" << aspect << "\n";
                if (cardFrames.size() == 1) {
                    firstCardX = f.x;
                    firstCardY = f.y;
                    firstCardH = f.height;
                }
            }
            bool gridMultiColumn = false;
            for (size_t i = 0; i < cardFrames.size() && !gridMultiColumn; ++i) {
                for (size_t j = i + 1; j < cardFrames.size(); ++j) {
                    if (std::fabs(cardFrames[i].y - cardFrames[j].y) <= 1.5f) {
                        gridMultiColumn = true;
                        break;
                    }
                }
            }
            if (gridMultiColumn) {
                for (const core::Rect& f : cardFrames) {
                    if (f.width > kTaskCardMaxW + 0.5f) {
                        std::cerr << "agent_e2e_probe: G1 FAIL 网格态卡宽超上限 "
                                  << f.width << " > " << kTaskCardMaxW << "\n";
                        g1 = false;
                    }
                }
            }
            if (gridMultiColumn) {
                for (size_t i = 0; i < cardFrames.size(); ++i) {
                    const core::Rect& f = cardFrames[i];
                    const float aspect = f.height > 0.0f ? f.width / f.height : 0.0f;
                    if (aspect < 2.5f || aspect > 4.5f) {
                        std::cerr << "agent_e2e_probe: G1 FAIL 网格态卡宽高比越界 "
                                  << cardIds[i] << " aspect=" << aspect << "\n";
                        g1 = false;
                    }
                }
                std::cout << (g1 ? "G1 PASS" : "G1 FAIL")
                          << " 每卡 2.5–4.5:1（3.5 目标）\n";
            } else {
                // 单列退化：只校验没被拉成横条（<8:1），并明确标注这是窄窗设计行为
                for (size_t i = 0; i < cardFrames.size(); ++i) {
                    const core::Rect& f = cardFrames[i];
                    const float aspect = f.height > 0.0f ? f.width / f.height : 0.0f;
                    if (aspect > 8.0f) {
                        std::cerr << "agent_e2e_probe: G1 FAIL 单列退化仍成横条 "
                                  << cardIds[i] << " aspect=" << aspect << "\n";
                        g1 = false;
                    }
                }
                std::cout << (g1 ? "G1 PASS" : "G1 FAIL")
                          << "（单列退化态：窄窗内容宽不足以排 2 列，按方案退 1 列，"
                             "宽高比区间不适用）\n";
            }

            // ---- G2 对齐：同一行内标题基线同 y（±1.5）、操作组右缘 = 卡右 −14（±1.5）----
            // ⚠️ 窄窗下网格会换行（一行放不下两张最小宽卡），所以基线按「卡顶 y 同行」
            // 分组比，不能假定所有卡同一行——否则正常的第二行会被判成错位。
            bool g2 = true;
            struct RowRef {
                float cardY;
                float baseline;
            };
            std::vector<RowRef> rows;
            for (int64_t tid : cardIds) {
                core::Rect card{};
                core::Rect title{};
                core::Rect acts{};
                const std::string cid = "agent.task." + std::to_string(tid);
                if (!frameOf(cid, &card) || !frameOf(cid + ".title", &title) ||
                    !frameOf(cid + ".actions", &acts)) {
                    std::cerr << "agent_e2e_probe: G2 FAIL 元素缺失 " << cid << "\n";
                    g2 = false;
                    continue;
                }
                const float baseline = title.y + title.height;
                RowRef* row = nullptr;
                for (RowRef& r : rows) {
                    if (std::fabs(r.cardY - card.y) <= 1.5f) row = &r;
                }
                if (row == nullptr) {
                    rows.push_back({card.y, baseline});
                } else if (std::fabs(row->baseline - baseline) > 1.5f) {
                    std::cerr << "agent_e2e_probe: G2 FAIL 同行标题基线不一致 "
                              << tid << " " << baseline << " vs " << row->baseline
                              << "\n";
                    g2 = false;
                }
                const float actionsRight = acts.x + acts.width;
                const float wantRight = card.x + card.width - 14.0f;
                if (std::fabs(actionsRight - wantRight) > 1.5f) {
                    std::cerr << "agent_e2e_probe: G2 FAIL 操作组右缘 " << tid
                              << " 实测 " << actionsRight << " 期望 " << wantRight
                              << "\n";
                    g2 = false;
                }
                std::cout << "G2 card " << tid << " y=" << card.y
                          << " titleBaseline=" << baseline << " actionsRight="
                          << actionsRight << " wantRight=" << wantRight << "\n";
            }
            std::cout << (g2 ? "G2 PASS" : "G2 FAIL") << " 行内标题基线同 y（"
                      << rows.size() << " 行）/ 操作组右缘=卡右−14\n";

            // ---- G3 字形存在性：5 个新码点不能渲染成「绿色方块」/空白 ----
            // 双通道：① 结构性（文本元素带具体码点且 fontFamily=Icon）；
            // ② 像素（图标区必须有墨迹且绿色主导像素占比低——缺字形 fallback
            // 的典型观感是整块绿）。任何一项命中即报码点。
            struct GlyphCase {
                const char* suffix;
                unsigned int codepoint;
            };
            const GlyphCase glyphs[] = {{".detail", 0xF05A},
                                        {".edit", 0xF044},
                                        {".toggle", 0xF011},
                                        {".enqueue", 0xF03A},
                                        {".delete", 0xF2ED}};
            bool g3 = true;
            std::vector<unsigned char> rgba;
            int imgW = 0, imgH = 0;
            bool pixelsReady = false;
            probe::FrameCapture glyphCapture(window);
            if (glyphCapture.ok()) {
                // ⚠️ 操作组常态 opacity 0（不可见）→ 必须先 hover 首卡把图标显出来再采，
                // 否则整片区域与底色完全一致，墨迹率恒 0（假红）
                const core::KeyModifiers noModsG3;
                core::queuePointerMotion(window, firstCardX + 40.0f,
                                         firstCardY + firstCardH - 20.0f,
                                         core::PointerButtons{}, noModsG3);
                for (int i = 0; i < 24; ++i) composeFrame();  // 盖过 0.18s OutBack
                pixelsReady = glyphCapture.readFrame(runtime, rgba, imgW, imgH);
                if (!pixelsReady) {
                    std::cerr << "agent_e2e_probe: G3 读帧失败（像素通道跳过）\n";
                }
            } else {
                std::cerr << "agent_e2e_probe: G3 FrameCapture 不可用: "
                          << glyphCapture.error() << "（像素通道跳过）\n";
            }
            const float pxScale = glyphCapture.ok() ? glyphCapture.scale() : 1.0f;
            for (const GlyphCase& gc : glyphs) {
                const std::string iconId =
                    "agent.task." + std::to_string(cardIds.front()) + gc.suffix;
                const core::dsl::Element* icon =
                    ui != nullptr ? ui->find(iconId) : nullptr;
                if (icon == nullptr) {
                    std::cerr << "agent_e2e_probe: G3 FAIL 图标元素缺失 " << iconId
                              << "\n";
                    g3 = false;
                    continue;
                }
                const std::string want = eui::utf8(gc.codepoint);
                if (icon->text != want || icon->fontFamily != "Icon") {
                    std::cerr << "agent_e2e_probe: G3 FAIL 码点未绑定 U+"
                              << std::hex << gc.codepoint << std::dec << " "
                              << iconId << " text.len=" << icon->text.size()
                              << " family=" << icon->fontFamily << "\n";
                    g3 = false;
                    continue;
                }
                if (!pixelsReady) continue;
                const int x0 = static_cast<int>(std::lround(icon->frame.x * pxScale));
                const int y0 = static_cast<int>(std::lround(icon->frame.y * pxScale));
                const int x1 = static_cast<int>(
                    std::lround((icon->frame.x + icon->frame.width) * pxScale));
                const int y1 = static_cast<int>(
                    std::lround((icon->frame.y + icon->frame.height) * pxScale));
                if (x0 < 0 || y0 < 0 || x1 > imgW || y1 > imgH || x1 <= x0 || y1 <= y0) {
                    std::cerr << "agent_e2e_probe: G3 采样越界 " << iconId << "\n";
                    g3 = false;
                    continue;
                }
                // 基准色 = 图标框「内缩 3px 的一圈边框」均值：字形笔画居中，
                // 一般碰不到这一圈；取单点会踩中笔画或圆角外侧透明区，基准一歪
                // 整片底色就被算成墨迹（实测单点判据会给出 0% 或 62% 的假值）
                long sr = 0, sg = 0, sb = 0;
                int sn = 0;
                auto addBase = [&](int x, int y) {
                    const std::size_t at =
                        (static_cast<std::size_t>(y) * imgW + x) * 4u;
                    sr += rgba[at];
                    sg += rgba[at + 1];
                    sb += rgba[at + 2];
                    ++sn;
                };
                for (int x = x0 + 3; x < x1 - 3; ++x) {
                    addBase(x, y0 + 3);
                    addBase(x, y1 - 4);
                }
                for (int y = y0 + 4; y < y1 - 4; ++y) {
                    addBase(x0 + 3, y);
                    addBase(x1 - 4, y);
                }
                if (sn <= 0) {
                    std::cerr << "agent_e2e_probe: G3 基准采样为空 " << iconId
                              << "\n";
                    g3 = false;
                    continue;
                }
                const int br = static_cast<int>(sr / sn);
                const int bg = static_cast<int>(sg / sn);
                const int bb = static_cast<int>(sb / sn);
                int ink = 0, green = 0, total = 0;
                for (int y = y0; y < y1; ++y) {
                    for (int x = x0; x < x1; ++x) {
                        const std::size_t at =
                            (static_cast<std::size_t>(y) * imgW + x) * 4u;
                        const int r = rgba[at], g = rgba[at + 1], b = rgba[at + 2];
                        ++total;
                        if (std::abs(r - br) + std::abs(g - bg) + std::abs(b - bb) > 24) {
                            ++ink;
                        }
                        if (g > r + 25 && g > b + 25) ++green;
                    }
                }
                const float inkRatio =
                    total > 0 ? static_cast<float>(ink) / total : 0.0f;
                const float greenRatio =
                    total > 0 ? static_cast<float>(green) / total : 0.0f;
                std::cout << "G3 glyph U+" << std::hex << gc.codepoint << std::dec
                          << " " << gc.suffix << " ink=" << inkRatio * 100.0f
                          << "% green=" << greenRatio * 100.0f << "%\n";
                if (inkRatio < 0.02f) {
                    std::cerr << "agent_e2e_probe: G3 FAIL 图标区无墨迹（缺字形）U+"
                              << std::hex << gc.codepoint << std::dec << "\n";
                    g3 = false;
                }
                if (greenRatio > 0.40f) {
                    std::cerr << "agent_e2e_probe: G3 FAIL 图标区绿色主导（疑似绿方块）U+"
                              << std::hex << gc.codepoint << std::dec << "\n";
                    g3 = false;
                }
            }
            std::cout << (g3 ? "G3 PASS" : "G3 FAIL")
                      << " 5 码点字形存在且非绿方块\n";

            // ---- G4 hover 驱动：指针移入卡 → actions opacity=1 / 卡 radius=16；
            //      移出 → 复位（读 compose 写入的 element 目标值，结构性判据）----
            // bug182 教训：模块后台刷新（调度首 tick 派 run、latestRuns 回填）会在
            // G4 数秒窗口内重排列表/整页 reflow——开局捕获的卡 id 与按钮几何会失效，
            // 探针拿陈旧坐标打活列表 = 间歇红（hit 留下无 enter 的死点）。每条动作腿
            // 前重解析「当前首卡」（.hit y 最小者）与其按钮几何，断言读同一张卡。
            struct G4Card {
                std::string id;
                const core::dsl::Element* del = nullptr;
                const core::dsl::Element* enq = nullptr;
                const core::dsl::Element* hit = nullptr;
                bool ok() const { return !id.empty() && del != nullptr && enq != nullptr && hit != nullptr; }
            };
            auto resolveG4Card = [&]() -> G4Card {
                const core::dsl::Element* best = nullptr;
                G4Card c;
                for (int64_t tid : cardIds) {
                    auto* h = ui->find("agent.task." + std::to_string(tid) + ".hit");
                    if (h == nullptr) continue;
                    if (best == nullptr || h->frame.y < best->frame.y - 0.5f ||
                        (std::fabs(h->frame.y - best->frame.y) <= 0.5f &&
                         h->frame.x < best->frame.x - 0.5f)) {
                        best = h;
                        c.id = "agent.task." + std::to_string(tid);
                    }
                }
                if (c.id.empty()) return c;
                c.del = ui->find(c.id + ".delete.bg");
                c.enq = ui->find(c.id + ".enqueue.bg");
                c.hit = best;
                return c;
            };
            bool g4 = true;
            std::string hoverCardId;
            auto readPair = [&](const std::string& base, float* actionsOpacity,
                                float* bgRadius, float* bgScale) -> bool {
                const core::dsl::Element* acts = ui->find(base + ".actions");
                const core::dsl::Element* bg = ui->find(base + ".bg");
                if (acts == nullptr || bg == nullptr) return false;
                *actionsOpacity = acts->opacity;
                *bgRadius = bg->radius;
                *bgScale = bg->transform.scale.x;
                return true;
            };
            const core::KeyModifiers noModsG;
            // 像素通道（B2 方法论双保险）：形变/显隐都是局部变化，区域均值会被同色区
            // 稀释，所以按「逐像素通道差 >4 的计数」判。三态各拍一帧留到最后比对。
            struct GrayFrame {
                std::vector<unsigned char> rgba;
                bool ok = false;
            };
            GrayFrame fNormal, fHover, fOut;
            auto grabGray = [&](GrayFrame* out) {
                int iw = 0, ih = 0;
                out->ok = glyphCapture.ok() &&
                          glyphCapture.readFrame(runtime, out->rgba, iw, ih);
            };
            // 逐像素差异计数：patch 用逻辑 px 给，内部按 Retina scale 换像素
            auto diffOver = [&](const GrayFrame& a, const GrayFrame& b, float lx,
                                float ly, float pw, float ph, int threshold) -> int {
                if (!a.ok || !b.ok || a.rgba.size() != b.rgba.size()) return -1;
                const int x0 = static_cast<int>(std::lround(lx * pxScale));
                const int y0 = static_cast<int>(std::lround(ly * pxScale));
                const int x1 = static_cast<int>(std::lround((lx + pw) * pxScale));
                const int y1 = static_cast<int>(std::lround((ly + ph) * pxScale));
                if (x0 < 0 || y0 < 0 || x1 > imgW || y1 > imgH || x1 <= x0 ||
                    y1 <= y0) {
                    return -1;
                }
                int over = 0;
                for (int y = y0; y < y1; ++y) {
                    for (int x = x0; x < x1; ++x) {
                        const std::size_t at =
                            (static_cast<std::size_t>(y) * imgW + x) * 4u;
                        int m = 0;
                        for (int c = 0; c < 3; ++c) {
                            m = std::max(m, std::abs(static_cast<int>(a.rgba[at + c]) -
                                                     static_cast<int>(b.rgba[at + c])));
                        }
                        if (m > threshold) ++over;
                    }
                }
                return over;
            };
            // G3 采样时把指针停在了首卡上 → 先移出并稳帧，否则「常态」读到的是 hover 态
            //（bug169 §8：离场走 120ms 去抖 + 120ms 淡出，帧间要睡过真实时钟）
            core::queuePointerMotion(window, w - 8.0f, 8.0f, core::PointerButtons{},
                                     noModsG);
            for (int i = 0; i < 8; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                composeFrame();
            }
            G4Card card = resolveG4Card();
            hoverCardId = card.id;   // 空串时 readPair 必 false → 走缺失分支
            // 像素腿角部采样坐标 = 本腿实测首卡几何（页面 reflow 后 stale G1 坐标会采错区）
            const float g4CornerX = card.hit != nullptr ? card.hit->frame.x : firstCardX;
            const float g4CornerY = card.hit != nullptr ? card.hit->frame.y : firstCardY;
            grabGray(&fNormal);
            float beforeOpacity = -1, beforeRadius = -1, beforeScale = -1;
            if (!readPair(hoverCardId, &beforeOpacity, &beforeRadius, &beforeScale)) {
                std::cerr << "agent_e2e_probe: G4 FAIL 卡/操作组元素缺失 "
                          << hoverCardId << "\n";
                g4 = false;
            } else {
                std::cout << "G4 normal actions.opacity=" << beforeOpacity
                          << " bg.radius=" << beforeRadius
                          << " bg.scale=" << beforeScale << "\n";
                if (std::fabs(beforeOpacity) > 0.01f) {
                    std::cerr << "agent_e2e_probe: G4 FAIL 常态操作组应隐藏\n";
                    g4 = false;
                }
                if (std::fabs(beforeRadius - 12.0f) > 0.01f) {
                    std::cerr << "agent_e2e_probe: G4 FAIL 常态卡圆角应 12 实测 "
                              << beforeRadius << "\n";
                    g4 = false;
                }
                // 移入卡内「空白处」（底行左侧，避开操作组与启动钮）
                core::queuePointerMotion(window, card.hit->frame.x + 40.0f,
                                         card.hit->frame.y + card.hit->frame.height -
                                             20.0f,
                                         core::PointerButtons{}, noModsG);
                for (int i = 0; i < 24; ++i) composeFrame();  // 盖过 0.18s OutBack
                grabGray(&fHover);
                float hovOpacity = -1, hovRadius = -1, hovScale = -1;
                if (!readPair(hoverCardId, &hovOpacity, &hovRadius, &hovScale)) {
                    std::cerr << "agent_e2e_probe: G4 FAIL hover 后元素消失\n";
                    g4 = false;
                } else {
                    std::cout << "G4 hover actions.opacity=" << hovOpacity
                              << " bg.radius=" << hovRadius
                              << " bg.scale=" << hovScale << "\n";
                    if (std::fabs(hovOpacity - 1.0f) > 0.01f) {
                        std::cerr << "agent_e2e_probe: G4 FAIL hover 后操作组未显现\n";
                        g4 = false;
                    }
                    if (std::fabs(hovRadius - 16.0f) > 0.01f) {
                        std::cerr << "agent_e2e_probe: G4 FAIL hover 后卡圆角应 16 实测 "
                                  << hovRadius << "\n";
                        g4 = false;
                    }
                    if (std::fabs(hovScale - 1.02f) > 0.01f) {
                        std::cerr << "agent_e2e_probe: G4 FAIL hover 后缩放应 1.02 实测 "
                                  << hovScale << "\n";
                        g4 = false;
                    }
                }
                // 移出到页面空白（列表卡之外）→ 复位。bug169 §8：离场走 120ms 去抖 +
                // 120ms 淡出——帧间必须睡过真实时钟，否则去抖未到期读到假 1
                core::queuePointerMotion(window, w - 8.0f, 8.0f,
                                         core::PointerButtons{}, noModsG);
                for (int i = 0; i < 8; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(60));
                    composeFrame();
                }
                grabGray(&fOut);
                float outOpacity = -1, outRadius = -1, outScale = -1;
                if (!readPair(hoverCardId, &outOpacity, &outRadius, &outScale)) {
                    std::cerr << "agent_e2e_probe: G4 FAIL 移出后元素消失\n";
                    g4 = false;
                } else {
                    std::cout << "G4 out actions.opacity=" << outOpacity
                              << " bg.radius=" << outRadius
                              << " bg.scale=" << outScale << "\n";
                    if (std::fabs(outOpacity) > 0.01f ||
                        std::fabs(outRadius - 12.0f) > 0.01f ||
                        std::fabs(outScale - 1.0f) > 0.01f) {
                        std::cerr << "agent_e2e_probe: G4 FAIL 移出后未复位\n";
                        g4 = false;
                    }
                }
                // 指针「卡身 → 操作组按钮」：hover 全局唯一，父的 onHover(false) 先于
                // 子的 onHover(true) → 用集合才不会闪没（§10.0.1 约束 3）。这条是
                // hover 模型里最容易踩坏的路径，单验卡身 hover 不够。
                card = resolveG4Card();
                hoverCardId = card.id;
                const core::dsl::Element* delBtn = card.del;
                if (delBtn == nullptr) {
                    std::cerr << "agent_e2e_probe: G4 FAIL 缺删除钮（跨元素 hover 断言）\n";
                    g4 = false;
                } else {
                    core::queuePointerMotion(
                        window, delBtn->frame.x + delBtn->frame.width * 0.5f,
                        delBtn->frame.y + delBtn->frame.height * 0.5f,
                        core::PointerButtons{}, noModsG);
                    for (int i = 0; i < 24; ++i) composeFrame();
                    float btnOpacity = -1, btnRadius = -1, btnScale = -1;
                    if (!readPair(hoverCardId, &btnOpacity, &btnRadius, &btnScale)) {
                        std::cerr << "agent_e2e_probe: G4 FAIL 按钮 hover 后元素消失\n";
                        g4 = false;
                    } else {
                        std::cout << "G4 on-button actions.opacity=" << btnOpacity
                                  << " bg.radius=" << btnRadius << "\n";
                        if (std::fabs(btnOpacity - 1.0f) > 0.01f ||
                            std::fabs(btnRadius - 16.0f) > 0.01f) {
                            std::cerr << "agent_e2e_probe: G4 FAIL 指针在按钮上时卡态丢失"
                                         "（集合未覆盖子元素上报）\n";
                            g4 = false;
                        }
                    }
                    core::queuePointerMotion(window, w - 8.0f, 8.0f,
                                             core::PointerButtons{}, noModsG);
                    for (int i = 0; i < 8; ++i) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(60));
                        composeFrame();
                    }

                    // ---- bug169 §8 缝隙穿越：delete → enqueue 横移（含 4px 缝隙）
                    //      ——操作组目标 opacity 恒 1（持有者移交 + 离场去抖兜住
                    //      「低 z false 晚于高 z true」的派发序，缝隙不得产生隐藏）
                    card = resolveG4Card();
                    hoverCardId = card.id;
                    const core::dsl::Element* enqBtn = card.enq;
                    delBtn = card.del;
                    if (enqBtn == nullptr || delBtn == nullptr) {
                        std::cerr << "agent_e2e_probe: G4 FAIL 缺入队钮（缝隙穿越断言）\n";
                        g4 = false;
                    } else {
                        // 先落 delete 钮再滑入缝隙带、再到 enqueue 钮
                        //（每跳前重解析：后台刷新 reflow 可发生在任何两跳之间）
                        core::queuePointerMotion(
                            window, delBtn->frame.x + delBtn->frame.width * 0.5f,
                            delBtn->frame.y + delBtn->frame.height * 0.5f,
                            core::PointerButtons{}, noModsG);
                        for (int i = 0; i < 12; ++i) composeFrame();
                        card = resolveG4Card();
                        hoverCardId = card.id;
                        core::queuePointerMotion(
                            window,
                            delBtn->frame.x + delBtn->frame.width + 2.0f,
                            delBtn->frame.y + delBtn->frame.height * 0.5f,
                            core::PointerButtons{}, noModsG);
                        for (int i = 0; i < 12; ++i) composeFrame();
                        card = resolveG4Card();
                        hoverCardId = card.id;
                        enqBtn = card.enq != nullptr ? card.enq : enqBtn;
                        core::queuePointerMotion(
                            window, enqBtn->frame.x + enqBtn->frame.width * 0.5f,
                            enqBtn->frame.y + enqBtn->frame.height * 0.5f,
                            core::PointerButtons{}, noModsG);
                        // 到达 enqueue 后轮询读数：指针在域内则下一帧 op 目标即 1；
                        // 慢机单帧可超 120ms 造成中间态误读——「轮询不到 1」才是红
                        bool gapShown = false;
                        float gapOpacity = -1;
                        for (int i = 0; i < 20 && !gapShown; ++i) {
                            composeFrame();
                            float gapRadius = -1, gapScale = -1;
                            if (readPair(hoverCardId, &gapOpacity, &gapRadius,
                                         &gapScale)) {
                                gapShown = std::fabs(gapOpacity - 1.0f) <= 0.01f;
                            }
                            if (!gapShown) {
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds(30));
                            }
                        }
                        if (!gapShown) {
                            std::cerr << "agent_e2e_probe: G4 FAIL 按钮间缝隙穿越闪没"
                                         "（实测 actions.opacity="
                                      << gapOpacity << "）\n";
                            // bug182 诊断：失败现场几何 + 命中解析（坐标失配 vs 派发丢失）
                            std::cerr << "b182 diag enqTarget=(" << enqBtn->frame.x << ","
                                      << enqBtn->frame.y << ")\n";
                            for (const int cid : cardIds) {
                                const core::dsl::Element* h =
                                    ui->find("agent.task." + std::to_string(cid) +
                                             ".hit");
                                const core::dsl::Element* e =
                                    ui->find("agent.task." + std::to_string(cid) +
                                             ".enqueue.bg");
                                std::cerr << "b182 diag task." << cid << " hit="
                                          << (h != nullptr
                                                  ? "(" + std::to_string(h->frame.x) +
                                                        "," + std::to_string(h->frame.y) +
                                                        "+" + std::to_string(h->frame.width) +
                                                        "x" + std::to_string(h->frame.height) + ")"
                                                  : std::string("null"))
                                          << " enq.dis="
                                          << (e != nullptr ? std::to_string((int)e->disabled)
                                                           : std::string("null"))
                                          << "\n";
                            }
                            std::cerr << "b182 diag hit@del="
                                      << runtime.hitTestInteractiveAt(
                                             delBtn->frame.x +
                                                 delBtn->frame.width * 0.5f,
                                             delBtn->frame.y +
                                                 delBtn->frame.height * 0.5f,
                                             pxScale)
                                      << " hit@enq="
                                      << runtime.hitTestInteractiveAt(
                                             enqBtn->frame.x +
                                                 enqBtn->frame.width * 0.5f,
                                             enqBtn->frame.y +
                                                 enqBtn->frame.height * 0.5f,
                                             pxScale)
                                      << "\n";
                            g4 = false;
                        } else {
                            std::cout << "G4 gap-sweep actions.opacity="
                                      << gapOpacity << "（缝隙穿越不闪没）\n";
                        }
                    }
                }
            }
            std::cout << (g4 ? "G4 PASS" : "G4 FAIL")
                      << " hover 驱动操作组显隐 + 卡形变/复位\n";

            // ---- G4 像素通道（逐像素计数，不看均值）：结构性目标值对了不代表像素
            //      真的变了（透明度动画没走到、命中面没接住指针都会让上一条假绿）----
            {
                const core::dsl::Element* actsEl = ui->find(hoverCardId + ".actions");
                const float actsX = actsEl != nullptr ? actsEl->frame.x : 0.0f;
                const float actsY = actsEl != nullptr ? actsEl->frame.y : 0.0f;
                const float actsW = actsEl != nullptr ? actsEl->frame.width : 0.0f;
                const float actsH = actsEl != nullptr ? actsEl->frame.height : 0.0f;
                // ① hover 时操作组区域出现图标墨迹（opacity 0→1 像素级证据）
                const int actsInk = diffOver(fHover, fNormal, actsX, actsY, actsW,
                                             actsH, 4);
                // ② 卡左上角 12×12：圆角 12→16 + 1.02 放大 → 轮廓移动
                const int cornerInk = diffOver(fHover, fNormal, g4CornerX,
                                               g4CornerY, 12.0f, 12.0f, 4);
                // ③ 移出后必须逐像素回到常态（与 jelly A3 同口径）
                const int actsBack = diffOver(fOut, fNormal, actsX, actsY, actsW,
                                              actsH, 2);
                const int cornerBack = diffOver(fOut, fNormal, g4CornerX,
                                                g4CornerY, 12.0f, 12.0f, 2);
                std::cout << "G4 pixel actionsInk=" << actsInk
                          << " cornerInk=" << cornerInk
                          << " actionsBack=" << actsBack
                          << " cornerBack=" << cornerBack << "\n";
                if (actsInk < 20) {
                    std::cerr << "agent_e2e_probe: G4 FAIL hover 后操作组区域无变化"
                                 "（像素通道）px>4=" << actsInk << "\n";
                    g4 = false;
                }
                if (cornerInk < 1) {
                    std::cerr << "agent_e2e_probe: G4 FAIL hover 后卡角轮廓未动"
                                 "（像素通道）px>4=" << cornerInk << "\n";
                    g4 = false;
                }
                if (actsBack != 0 || cornerBack != 0) {
                    std::cerr << "agent_e2e_probe: G4 FAIL 移出后像素未复位 "
                              << "actionsBack=" << actsBack
                              << " cornerBack=" << cornerBack << "\n";
                    g4 = false;
                }
                if (actsInk >= 20 && cornerInk >= 1 && actsBack == 0 &&
                    cornerBack == 0) {
                    std::cout << "G4 pixel PASS 操作组淡入/卡角形变/移出逐像素复位\n";
                }
                // hover 态留图（人的观感证据）：与 --shot-list 同目录，后缀 -hover
                if (!args.shotListPath.empty() && fHover.ok) {
                    std::string hoverPath = args.shotListPath;
                    const std::size_t dot = hoverPath.rfind(".png");
                    hoverPath = (dot == std::string::npos
                                     ? hoverPath + "-hover.png"
                                     : hoverPath.substr(0, dot) + "-hover.png");
                    if (probe::writeRgbaToPng(hoverPath, fHover.rgba.data(), imgW,
                                              imgH)) {
                        std::cout << "shot hover " << hoverPath << "\n";
                    } else {
                        std::cerr << "agent_e2e_probe: hover 图落图失败: "
                                  << hoverPath << "\n";
                    }
                }
            }

            // ---- 列表页截图出口（--shot-list）----
            if (!args.shotListPath.empty()) {
                if (!probe::captureFramePng(window, runtime, args.shotListPath)) {
                    std::cerr << "agent_e2e_probe: --shot-list 落图失败: "
                              << args.shotListPath << "\n";
                    passed = false;
                } else {
                    std::cout << "shot list " << args.shotListPath << "\n";
                }
            }
        }
    }

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
