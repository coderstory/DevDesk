// AgentStore 落库层实现：统一模式 = db_.mutex() 锁内 prepare → bind → step。
// 列序按 brief 固定表、bind 顺序 = 问号顺序；不抛不吞，各步失败逐个透传
#include "module_agent/agent_store.h"

#include <ctime>
#include <mutex>
#include <utility>

#include <sqlite3.h>  // sqlite3_last_insert_rowid

namespace module::agent {

using core::ErrorCode;

namespace {

// Result<void> 无 operator&&，故用 helper 收拢 bind 链——按序绑定、失败即停透传
template <typename... Args>
core::Result<void> bindAll(core::data::Statement& s, Args&&... args) {
    int index = 1;
    core::Result<void> r = core::Result<void>::ok();
    auto one = [&](auto&& v) {
        if (r.isOk()) {
            r = s.bind(index++, std::forward<decltype(v)>(v));
        }
    };
    (one(std::forward<Args>(args)), ...);
    return r;
}

// 行取值 helper：失败转 (code,msg)，成功填 out 返 nullopt（列名漂移不容静默默认值）
using RowError = std::pair<ErrorCode, std::string>;

std::optional<RowError> rowInt(const core::data::SqlRow& row, const char* col,
                               int64_t& out) {
    auto v = row.getInt64(col);
    if (v.isFail()) return RowError{v.errorCode(), v.errorMessage()};
    out = v.value();
    return std::nullopt;
}

std::optional<RowError> rowStr(const core::data::SqlRow& row, const char* col,
                               std::string& out) {
    auto v = row.getString(col);
    if (v.isFail()) return RowError{v.errorCode(), v.errorMessage()};
    out = std::move(v.value());
    return std::nullopt;
}

// agent_tasks 11 列列序：listTasks / getTask 共用同一份列清单与行映射
constexpr const char* kTaskColumns =
    "id,name,goal,trigger_type,trigger_config,model_slot,"
    "max_iterations,allow_destructive,enabled,context_note,auto_note";

// 一行 → AgentTaskRow（列缺失/类型不符一律 fail）
Result<AgentTaskRow> taskRowFrom(const core::data::SqlRow& row) {
    AgentTaskRow t;
    int64_t maxIter = 0, allowDestr = 0, enabled = 0, autoNoteInt = 0;
    if (auto e = rowInt(row, "id", t.id)) {
        return Result<AgentTaskRow>::fail(e->first, e->second);
    }
    if (auto e = rowStr(row, "name", t.name)) {
        return Result<AgentTaskRow>::fail(e->first, e->second);
    }
    if (auto e = rowStr(row, "goal", t.goal)) {
        return Result<AgentTaskRow>::fail(e->first, e->second);
    }
    if (auto e = rowStr(row, "trigger_type", t.triggerType)) {
        return Result<AgentTaskRow>::fail(e->first, e->second);
    }
    if (auto e = rowStr(row, "trigger_config", t.triggerConfig)) {
        return Result<AgentTaskRow>::fail(e->first, e->second);
    }
    if (auto e = rowStr(row, "model_slot", t.modelSlot)) {
        return Result<AgentTaskRow>::fail(e->first, e->second);
    }
    if (auto e = rowInt(row, "max_iterations", maxIter)) {
        return Result<AgentTaskRow>::fail(e->first, e->second);
    }
    if (auto e = rowInt(row, "allow_destructive", allowDestr)) {
        return Result<AgentTaskRow>::fail(e->first, e->second);
    }
    if (auto e = rowInt(row, "enabled", enabled)) {
        return Result<AgentTaskRow>::fail(e->first, e->second);
    }
    if (auto e = rowStr(row, "context_note", t.contextNote)) {
        return Result<AgentTaskRow>::fail(e->first, e->second);
    }
    if (auto e = rowInt(row, "auto_note", autoNoteInt)) {
        return Result<AgentTaskRow>::fail(e->first, e->second);
    }
    t.maxIterations = static_cast<int>(maxIter);
    t.allowDestructive = (allowDestr != 0);
    t.enabled = (enabled != 0);
    t.autoNote = static_cast<int>(autoNoteInt);
    return Result<AgentTaskRow>::ok(std::move(t));
}

} // namespace

AgentStore::AgentStore(core::data::SqliteDb& db) : db_(db) {}

Result<int64_t> AgentStore::createTask(const std::string& name, const std::string& goal,
                                       const std::string& triggerType,
                                       const std::string& modelSlot,
                                       int maxIterations, bool allowDestructive,
                                       const std::string& triggerConfig,
                                       const std::string& contextNote,
                                       int autoNote) {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare(
        "INSERT INTO agent_tasks(name, goal, trigger_type, trigger_config, model_slot,"
        " max_iterations, allow_destructive, enabled, context_note, auto_note,"
        " created_at, updated_at)"
        " VALUES(?,?,?,?,?,?,?,1,?,?,?,?)");
    if (st.isFail()) return Result<int64_t>::fail(st.errorCode(), st.errorMessage());
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    auto& s = st.value();
    auto ok = bindAll(s, name, goal, triggerType, triggerConfig, modelSlot,
                      static_cast<int64_t>(maxIterations),
                      static_cast<int64_t>(allowDestructive ? 1 : 0),
                      contextNote, static_cast<int64_t>(autoNote), now, now);
    if (ok.isFail()) return Result<int64_t>::fail(ok.errorCode(), ok.errorMessage());
    auto step = s.step();
    if (step.isFail()) return Result<int64_t>::fail(step.errorCode(), step.errorMessage());
    return Result<int64_t>::ok(
        static_cast<int64_t>(sqlite3_last_insert_rowid(db_.handle())));
}

Result<std::vector<AgentTaskRow>> AgentStore::listTasks() {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare(std::string("SELECT ") + kTaskColumns +
                          " FROM agent_tasks ORDER BY id");
    if (st.isFail()) {
        return Result<std::vector<AgentTaskRow>>::fail(st.errorCode(), st.errorMessage());
    }
    auto rows = st.value().fetchAll();
    if (rows.isFail()) {
        return Result<std::vector<AgentTaskRow>>::fail(rows.errorCode(), rows.errorMessage());
    }
    std::vector<AgentTaskRow> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto t = taskRowFrom(row);
        if (t.isFail()) {
            return Result<std::vector<AgentTaskRow>>::fail(t.errorCode(), t.errorMessage());
        }
        out.push_back(std::move(t.value()));
    }
    return Result<std::vector<AgentTaskRow>>::ok(std::move(out));
}

Result<AgentTaskRow> AgentStore::getTask(int64_t id) {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare(std::string("SELECT ") + kTaskColumns +
                          " FROM agent_tasks WHERE id=?");
    if (st.isFail()) return Result<AgentTaskRow>::fail(st.errorCode(), st.errorMessage());
    auto& s = st.value();
    if (auto r = s.bind(1, id); r.isFail()) {
        return Result<AgentTaskRow>::fail(r.errorCode(), r.errorMessage());
    }
    auto has = s.step();
    if (has.isFail()) return Result<AgentTaskRow>::fail(has.errorCode(), has.errorMessage());
    if (!has.value()) {
        return Result<AgentTaskRow>::fail(ErrorCode::NotFound,
                                          "任务不存在：id=" + std::to_string(id));
    }
    auto row = s.currentRow();
    if (row.isFail()) return Result<AgentTaskRow>::fail(row.errorCode(), row.errorMessage());
    return taskRowFrom(row.value());
}

Result<int64_t> AgentStore::startRun(int64_t taskId) {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare("INSERT INTO agent_runs(task_id,status,started_at) VALUES(?,?,?)");
    if (st.isFail()) return Result<int64_t>::fail(st.errorCode(), st.errorMessage());
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    auto& s = st.value();
    auto ok = bindAll(s, taskId, std::string("running"), now);
    if (ok.isFail()) return Result<int64_t>::fail(ok.errorCode(), ok.errorMessage());
    auto step = s.step();
    if (step.isFail()) return Result<int64_t>::fail(step.errorCode(), step.errorMessage());
    return Result<int64_t>::ok(
        static_cast<int64_t>(sqlite3_last_insert_rowid(db_.handle())));
}

Result<int64_t> AgentStore::startRunExclusive(int64_t taskId) {
    // 查存在性 → 插行同一把锁内（跨线程原子）；手动启动与 Scheduler/cron 派发都经 AgentLoop::run → 这里
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare(
        "SELECT 1 FROM agent_runs WHERE task_id=? AND status='running' LIMIT 1");
    if (st.isFail()) return Result<int64_t>::fail(st.errorCode(), st.errorMessage());
    auto& s = st.value();
    if (auto r = s.bind(1, taskId); r.isFail()) {
        return Result<int64_t>::fail(r.errorCode(), r.errorMessage());
    }
    auto running = s.step();
    if (running.isFail()) {
        return Result<int64_t>::fail(running.errorCode(), running.errorMessage());
    }
    if (running.value()) {
        return Result<int64_t>::fail(ErrorCode::AlreadyExists, "任务已在运行");
    }
    return startRun(taskId);   // 锁内 → 与上面的查同样原子
}

Result<void> AgentStore::updateTask(int64_t id, const std::string& name,
                                    const std::string& goal,
                                    const std::string& triggerType,
                                    const std::string& triggerConfig,
                                    const std::string& modelSlot, int maxIterations,
                                    bool allowDestructive,
                                    const std::string& contextNote,
                                    int autoNote) {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare(
        "UPDATE agent_tasks SET name=?,goal=?,trigger_type=?,trigger_config=?,"
        "model_slot=?,max_iterations=?,allow_destructive=?,context_note=?,"
        "auto_note=?,updated_at=? WHERE id=?");
    if (st.isFail()) return Result<void>::fail(st.errorCode(), st.errorMessage());
    auto& s = st.value();
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    auto ok = bindAll(s, name, goal, triggerType, triggerConfig, modelSlot,
                      static_cast<int64_t>(maxIterations),
                      static_cast<int64_t>(allowDestructive ? 1 : 0),
                      contextNote, static_cast<int64_t>(autoNote), now, id);
    if (ok.isFail()) return Result<void>::fail(ok.errorCode(), ok.errorMessage());
    auto step = s.step();
    if (step.isFail()) return Result<void>::fail(step.errorCode(), step.errorMessage());
    // id 不存在 → fail(NotFound)，不静默 no-op；持锁覆盖 step + sqlite3_changes，计数不被并发插队
    if (sqlite3_changes(db_.handle()) == 0) {
        return Result<void>::fail(ErrorCode::NotFound,
                                  "任务不存在：id=" + std::to_string(id));
    }
    return Result<void>::ok();
}

Result<void> AgentStore::setTaskEnabled(int64_t id, bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare(
        "UPDATE agent_tasks SET enabled=?,updated_at=? WHERE id=?");
    if (st.isFail()) return Result<void>::fail(st.errorCode(), st.errorMessage());
    auto& s = st.value();
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    auto ok = bindAll(s, static_cast<int64_t>(enabled ? 1 : 0), now, id);
    if (ok.isFail()) return Result<void>::fail(ok.errorCode(), ok.errorMessage());
    auto step = s.step();
    if (step.isFail()) return Result<void>::fail(step.errorCode(), step.errorMessage());
    // 同 updateTask：id 不存在 → fail(NotFound)
    if (sqlite3_changes(db_.handle()) == 0) {
        return Result<void>::fail(ErrorCode::NotFound,
                                  "任务不存在：id=" + std::to_string(id));
    }
    return Result<void>::ok();
}

// 删任务并清全部运行痕迹：单事务显式 DELETE messages → runs → tasks（顺序子→父，
// 显式删不依赖 FK CASCADE——pragma 状态不属于本层契约）；id 不存在 → NotFound，
// 事务整体 ROLLBACK 前两条 DELETE 一并撤销。全程持 db_ 锁 + 事务内重入同锁
Result<void> AgentStore::deleteTask(int64_t id) {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    return db_.transaction([&]() -> Result<void> {
        auto msgStmt = db_.prepare(
            "DELETE FROM agent_messages WHERE run_id IN"
            " (SELECT id FROM agent_runs WHERE task_id=?)");
        if (msgStmt.isFail()) {
            return Result<void>::fail(msgStmt.errorCode(), msgStmt.errorMessage());
        }
        auto okMsg = bindAll(msgStmt.value(), id);
        if (okMsg.isFail()) {
            return Result<void>::fail(okMsg.errorCode(), okMsg.errorMessage());
        }
        auto stepMsg = msgStmt.value().step();
        if (stepMsg.isFail()) {
            return Result<void>::fail(stepMsg.errorCode(), stepMsg.errorMessage());
        }
        auto runStmt = db_.prepare("DELETE FROM agent_runs WHERE task_id=?");
        if (runStmt.isFail()) {
            return Result<void>::fail(runStmt.errorCode(), runStmt.errorMessage());
        }
        auto okRun = bindAll(runStmt.value(), id);
        if (okRun.isFail()) {
            return Result<void>::fail(okRun.errorCode(), okRun.errorMessage());
        }
        auto stepRun = runStmt.value().step();
        if (stepRun.isFail()) {
            return Result<void>::fail(stepRun.errorCode(), stepRun.errorMessage());
        }
        auto taskStmt = db_.prepare("DELETE FROM agent_tasks WHERE id=?");
        if (taskStmt.isFail()) {
            return Result<void>::fail(taskStmt.errorCode(), taskStmt.errorMessage());
        }
        auto okTask = bindAll(taskStmt.value(), id);
        if (okTask.isFail()) {
            return Result<void>::fail(okTask.errorCode(), okTask.errorMessage());
        }
        auto stepTask = taskStmt.value().step();
        if (stepTask.isFail()) {
            return Result<void>::fail(stepTask.errorCode(), stepTask.errorMessage());
        }
        // 同 setTaskEnabled：id 不存在 → fail(NotFound)，事务回滚保证三表零变化
        if (sqlite3_changes(db_.handle()) == 0) {
            return Result<void>::fail(ErrorCode::NotFound,
                                      "任务不存在：id=" + std::to_string(id));
        }
        return Result<void>::ok();
    });
}

Result<void> AgentStore::finishRun(int64_t runId, const std::string& status,
                                   int iterations,
                                   std::optional<int64_t> inputTokens,
                                   std::optional<int64_t> outputTokens,
                                   const std::optional<std::string>& error) {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare(
        "UPDATE agent_runs SET status=?,ended_at=?,iterations=?,input_tokens=?,"
        "output_tokens=?,error=? WHERE id=?");
    if (st.isFail()) return Result<void>::fail(st.errorCode(), st.errorMessage());
    auto& s = st.value();
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    // optional 逐个绑定，nullopt → bind nullptr 表 NULL
    if (auto r = s.bind(1, status); r.isFail()) {
        return Result<void>::fail(r.errorCode(), r.errorMessage());
    }
    if (auto r = s.bind(2, now); r.isFail()) {
        return Result<void>::fail(r.errorCode(), r.errorMessage());
    }
    if (auto r = s.bind(3, static_cast<int64_t>(iterations)); r.isFail()) {
        return Result<void>::fail(r.errorCode(), r.errorMessage());
    }
    if (auto r = inputTokens.has_value() ? s.bind(4, *inputTokens)
                                         : s.bind(4, nullptr);
        r.isFail()) {
        return Result<void>::fail(r.errorCode(), r.errorMessage());
    }
    if (auto r = outputTokens.has_value() ? s.bind(5, *outputTokens)
                                          : s.bind(5, nullptr);
        r.isFail()) {
        return Result<void>::fail(r.errorCode(), r.errorMessage());
    }
    if (auto r = error.has_value() ? s.bind(6, *error) : s.bind(6, nullptr);
        r.isFail()) {
        return Result<void>::fail(r.errorCode(), r.errorMessage());
    }
    if (auto r = s.bind(7, runId); r.isFail()) {
        return Result<void>::fail(r.errorCode(), r.errorMessage());
    }
    auto step = s.step();
    if (step.isFail()) return Result<void>::fail(step.errorCode(), step.errorMessage());
    return Result<void>::ok();
}

Result<void> AgentStore::appendMessage(int64_t runId, int64_t seq, const std::string& role,
                                       const std::string& contentJson,
                                       const std::optional<std::string>& toolCallId) {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare(
        "INSERT INTO agent_messages(run_id,seq,role,content_json,tool_call_id,created_at)"
        " VALUES(?,?,?,?,?,?)");
    if (st.isFail()) return Result<void>::fail(st.errorCode(), st.errorMessage());
    auto& s = st.value();
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    if (auto r = s.bind(1, runId); r.isFail()) {
        return Result<void>::fail(r.errorCode(), r.errorMessage());
    }
    if (auto r = s.bind(2, seq); r.isFail()) {
        return Result<void>::fail(r.errorCode(), r.errorMessage());
    }
    if (auto r = s.bind(3, role); r.isFail()) {
        return Result<void>::fail(r.errorCode(), r.errorMessage());
    }
    if (auto r = s.bind(4, contentJson); r.isFail()) {
        return Result<void>::fail(r.errorCode(), r.errorMessage());
    }
    if (auto r = toolCallId.has_value() ? s.bind(5, *toolCallId)
                                        : s.bind(5, nullptr);
        r.isFail()) {
        return Result<void>::fail(r.errorCode(), r.errorMessage());
    }
    if (auto r = s.bind(6, now); r.isFail()) {
        return Result<void>::fail(r.errorCode(), r.errorMessage());
    }
    auto step = s.step();
    if (step.isFail()) return Result<void>::fail(step.errorCode(), step.errorMessage());
    return Result<void>::ok();
}

Result<std::vector<AgentMessageRow>> AgentStore::listMessages(int64_t runId) {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare(
        "SELECT seq,role,content_json,tool_call_id,created_at"
        " FROM agent_messages WHERE run_id=? ORDER BY seq");
    if (st.isFail()) {
        return Result<std::vector<AgentMessageRow>>::fail(st.errorCode(), st.errorMessage());
    }
    auto& s = st.value();
    if (auto r = s.bind(1, runId); r.isFail()) {
        return Result<std::vector<AgentMessageRow>>::fail(r.errorCode(), r.errorMessage());
    }
    auto rows = s.fetchAll();
    if (rows.isFail()) {
        return Result<std::vector<AgentMessageRow>>::fail(rows.errorCode(), rows.errorMessage());
    }
    std::vector<AgentMessageRow> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        AgentMessageRow m;
        if (auto e = rowInt(row, "seq", m.seq)) {
            return Result<std::vector<AgentMessageRow>>::fail(e->first, e->second);
        }
        if (auto e = rowStr(row, "role", m.role)) {
            return Result<std::vector<AgentMessageRow>>::fail(e->first, e->second);
        }
        if (auto e = rowStr(row, "content_json", m.contentJson)) {
            return Result<std::vector<AgentMessageRow>>::fail(e->first, e->second);
        }
        if (auto e = rowInt(row, "created_at", m.createdAt)) {
            return Result<std::vector<AgentMessageRow>>::fail(e->first, e->second);
        }
        if (row.isNull("tool_call_id")) {
            m.toolCallId = std::nullopt;
        } else {
            std::string v;
            if (auto e = rowStr(row, "tool_call_id", v)) {
                return Result<std::vector<AgentMessageRow>>::fail(e->first, e->second);
            }
            m.toolCallId = std::move(v);
        }
        out.push_back(std::move(m));
    }
    return Result<std::vector<AgentMessageRow>>::ok(std::move(out));
}

Result<std::vector<AgentRunRow>> AgentStore::listRuns(int64_t taskId, int limit) {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare(
        // id DESC 兜底：started_at 秒精度，同秒两 run 并列时 LIMIT 窗口可能返回较旧那条；id 单调故 (started_at, id) 确定
        "SELECT id,task_id,status,started_at,ended_at,iterations,input_tokens,"
        "output_tokens,error FROM agent_runs WHERE task_id=?"
        " ORDER BY started_at DESC, id DESC LIMIT ?");
    if (st.isFail()) {
        return Result<std::vector<AgentRunRow>>::fail(st.errorCode(), st.errorMessage());
    }
    auto& s = st.value();
    // LIMIT 以 int64 绑定（bind 只有 int64 整型重载）
    auto ok = bindAll(s, taskId, static_cast<int64_t>(limit));
    if (ok.isFail()) {
        return Result<std::vector<AgentRunRow>>::fail(ok.errorCode(), ok.errorMessage());
    }
    auto rows = s.fetchAll();
    if (rows.isFail()) {
        return Result<std::vector<AgentRunRow>>::fail(rows.errorCode(), rows.errorMessage());
    }
    std::vector<AgentRunRow> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        AgentRunRow r;
        int64_t iters = 0;
        if (auto e = rowInt(row, "id", r.id)) {
            return Result<std::vector<AgentRunRow>>::fail(e->first, e->second);
        }
        if (auto e = rowInt(row, "task_id", r.taskId)) {
            return Result<std::vector<AgentRunRow>>::fail(e->first, e->second);
        }
        if (auto e = rowStr(row, "status", r.status)) {
            return Result<std::vector<AgentRunRow>>::fail(e->first, e->second);
        }
        if (auto e = rowInt(row, "started_at", r.startedAt)) {
            return Result<std::vector<AgentRunRow>>::fail(e->first, e->second);
        }
        if (row.isNull("ended_at")) {
            r.endedAt = std::nullopt;
        } else {
            int64_t v = 0;
            if (auto e = rowInt(row, "ended_at", v)) {
                return Result<std::vector<AgentRunRow>>::fail(e->first, e->second);
            }
            r.endedAt = v;
        }
        if (auto e = rowInt(row, "iterations", iters)) {
            return Result<std::vector<AgentRunRow>>::fail(e->first, e->second);
        }
        r.iterations = static_cast<int>(iters);
        if (row.isNull("input_tokens")) {
            r.inputTokens = std::nullopt;
        } else {
            int64_t v = 0;
            if (auto e = rowInt(row, "input_tokens", v)) {
                return Result<std::vector<AgentRunRow>>::fail(e->first, e->second);
            }
            r.inputTokens = v;
        }
        if (row.isNull("output_tokens")) {
            r.outputTokens = std::nullopt;
        } else {
            int64_t v = 0;
            if (auto e = rowInt(row, "output_tokens", v)) {
                return Result<std::vector<AgentRunRow>>::fail(e->first, e->second);
            }
            r.outputTokens = v;
        }
        if (row.isNull("error")) {
            r.error = std::nullopt;
        } else {
            std::string v;
            if (auto e = rowStr(row, "error", v)) {
                return Result<std::vector<AgentRunRow>>::fail(e->first, e->second);
            }
            r.error = std::move(v);
        }
        out.push_back(std::move(r));
    }
    return Result<std::vector<AgentRunRow>>::ok(std::move(out));
}

// 任务级 run 聚合：COUNT/SUM/MAX 一条 SQL 出全任务，不拉 run 行进内存。
// 时长口径 = 单 run (ended-started) 求和，负差钳 0（与 UI 的 runRowText 同口径），
// running（ended NULL）计 0；last_ended_at = MAX(ended_at)，NULL 自动被 MAX 忽略
Result<bool> AgentStore::hasRunningRun() {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare("SELECT 1 FROM agent_runs WHERE status='running' LIMIT 1");
    if (st.isFail()) {
        return Result<bool>::fail(st.errorCode(), st.errorMessage());
    }
    auto step = st.value().step();
    if (step.isFail()) {
        return Result<bool>::fail(step.errorCode(), step.errorMessage());
    }
    return Result<bool>::ok(step.value());
}

Result<std::vector<AgentTaskRunSummary>> AgentStore::listTaskRunSummaries(
    int64_t windowStartSec) {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare(
        "SELECT task_id,"
        " COUNT(*) AS total_runs,"
        " SUM(status='completed') AS ok_runs,"
        " SUM(CASE WHEN ended_at IS NULL THEN 0"
        "          WHEN ended_at - started_at > 0 THEN ended_at - started_at"
        "          ELSE 0 END) AS total_seconds,"
        " MAX(ended_at) AS last_ended_at,"
        " SUM(CASE WHEN started_at >= ?1 AND status='completed' THEN 1"
        "          ELSE 0 END) AS ok_runs_7d,"
        " SUM(CASE WHEN started_at >= ?1 AND status IN ('failed','limited')"
        "          THEN 1 ELSE 0 END) AS failed_runs_7d,"
        " SUM(CASE WHEN started_at >= ?1 AND status='limited' THEN 1"
        "          ELSE 0 END) AS limited_runs_7d"
        " FROM agent_runs GROUP BY task_id ORDER BY task_id");
    if (st.isFail()) {
        return Result<std::vector<AgentTaskRunSummary>>::fail(st.errorCode(),
                                                              st.errorMessage());
    }
    // 三个窗口占位绑同一值（?1 复用 = 一处绑定；sqlite 参数序号从 1 起）
    auto& s0 = st.value();
    if (auto r = s0.bind(1, windowStartSec); r.isFail()) {
        return Result<std::vector<AgentTaskRunSummary>>::fail(r.errorCode(),
                                                              r.errorMessage());
    }
    auto rows = s0.fetchAll();
    if (rows.isFail()) {
        return Result<std::vector<AgentTaskRunSummary>>::fail(rows.errorCode(),
                                                              rows.errorMessage());
    }
    std::vector<AgentTaskRunSummary> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        AgentTaskRunSummary s;
        if (auto e = rowInt(row, "task_id", s.taskId)) {
            return Result<std::vector<AgentTaskRunSummary>>::fail(e->first, e->second);
        }
        if (auto e = rowInt(row, "total_runs", s.totalRuns)) {
            return Result<std::vector<AgentTaskRunSummary>>::fail(e->first, e->second);
        }
        if (auto e = rowInt(row, "ok_runs", s.okRuns)) {
            return Result<std::vector<AgentTaskRunSummary>>::fail(e->first, e->second);
        }
        if (auto e = rowInt(row, "total_seconds", s.totalSeconds)) {
            return Result<std::vector<AgentTaskRunSummary>>::fail(e->first, e->second);
        }
        if (auto e = rowInt(row, "ok_runs_7d", s.okRuns7d)) {
            return Result<std::vector<AgentTaskRunSummary>>::fail(e->first, e->second);
        }
        if (auto e = rowInt(row, "failed_runs_7d", s.failedRuns7d)) {
            return Result<std::vector<AgentTaskRunSummary>>::fail(e->first, e->second);
        }
        if (auto e = rowInt(row, "limited_runs_7d", s.limitedRuns7d)) {
            return Result<std::vector<AgentTaskRunSummary>>::fail(e->first, e->second);
        }
        if (row.isNull("last_ended_at")) {
            s.lastEndedAt = std::nullopt;
        } else {
            int64_t v = 0;
            if (auto e = rowInt(row, "last_ended_at", v)) {
                return Result<std::vector<AgentTaskRunSummary>>::fail(e->first,
                                                                      e->second);
            }
            s.lastEndedAt = v;
        }
        out.push_back(std::move(s));
    }
    return Result<std::vector<AgentTaskRunSummary>>::ok(std::move(out));
}

Result<void> AgentStore::sweepStaleRuns() {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare(
        "UPDATE agent_runs SET status='failed', ended_at=?,"
        " error='应用异常退出（悬挂 run）' WHERE status='running'");
    if (st.isFail()) return Result<void>::fail(st.errorCode(), st.errorMessage());
    auto& s = st.value();
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    if (auto r = s.bind(1, now); r.isFail()) {
        return Result<void>::fail(r.errorCode(), r.errorMessage());
    }
    auto step = s.step();
    if (step.isFail()) return Result<void>::fail(step.errorCode(), step.errorMessage());
    return Result<void>::ok();
}

// 追加文本到 context_note：锁内读-拼-裁-写单事务；超 4KB 从头丢最旧至 UTF-8 边界；
// 空串 no-op；taskId 不存在 → NotFound。
// 裁剪逻辑：若 newTotal > 4096，从头跳过 excess 字节，退到最近 UTF-8 起始字节再截。
Result<void> AgentStore::appendContextNote(int64_t taskId, const std::string& text) {
    if (text.empty()) return Result<void>::ok();
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    // 读当前值
    auto sel = db_.prepare("SELECT context_note FROM agent_tasks WHERE id=?");
    if (sel.isFail()) return Result<void>::fail(sel.errorCode(), sel.errorMessage());
    auto& ssel = sel.value();
    if (auto r = ssel.bind(1, taskId); r.isFail()) {
        return Result<void>::fail(r.errorCode(), r.errorMessage());
    }
    auto has = ssel.step();
    if (has.isFail()) return Result<void>::fail(has.errorCode(), has.errorMessage());
    if (!has.value()) {
        return Result<void>::fail(ErrorCode::NotFound,
                                  "任务不存在：id=" + std::to_string(taskId));
    }
    auto row = ssel.currentRow();
    if (row.isFail()) return Result<void>::fail(row.errorCode(), row.errorMessage());
    std::string current;
    if (auto e = rowStr(row.value(), "context_note", current)) {
        return Result<void>::fail(e->first, e->second);
    }
    // 拼接
    std::string combined = current + text;
    // 4KB 上限：超长从头丢最旧，裁剪落在 UTF-8 字符边界
    constexpr size_t kMaxBytes = 4096;
    if (combined.size() > kMaxBytes) {
        size_t excess = combined.size() - kMaxBytes;
        // 退到 UTF-8 字符边界：跳过 continuation bytes (10xxxxxx)
        while (excess < combined.size() &&
               (static_cast<unsigned char>(combined[excess]) & 0xC0) == 0x80) {
            ++excess;
        }
        combined = combined.substr(excess);
    }
    // 写回
    auto upd = db_.prepare(
        "UPDATE agent_tasks SET context_note=?,updated_at=? WHERE id=?");
    if (upd.isFail()) return Result<void>::fail(upd.errorCode(), upd.errorMessage());
    auto& supd = upd.value();
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    auto ok = bindAll(supd, combined, now, taskId);
    if (ok.isFail()) return Result<void>::fail(ok.errorCode(), ok.errorMessage());
    auto step = supd.step();
    if (step.isFail()) return Result<void>::fail(step.errorCode(), step.errorMessage());
    return Result<void>::ok();
}

} // namespace module::agent
