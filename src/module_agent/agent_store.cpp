// src/module_agent/agent_store.cpp
// AgentStore 落库层实现：统一模式 = db_.mutex() 锁内 prepare → bind → step。
// 约束（agent M1 plan task-3-brief）：
//   - 每条 SQL 与列序按 brief 固定表，bind 顺序 = 问号顺序
//   - 不抛不吞：prepare/bind/step/fetchAll/行取值的失败一律逐个检查并透传
//   - optional 入参/列 → nullptr 绑定 / isNull 判空映射 std::nullopt
#include "module_agent/agent_store.h"

#include <ctime>
#include <mutex>
#include <utility>

#include <sqlite3.h>  // sqlite3_last_insert_rowid

namespace module::agent {

using core::ErrorCode;

namespace {

// Result<void> 没有 operator&&（core/result.h 的 void 特化无该运算符）：
// 全参数非 optional 的语句用这个小 helper 收拢 bind 链——按序绑定，
// 第一个失败即停并透传其 code + msg，不静默跳过。
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

// 行取值 helper：getInt64/getString 失败转 (code, msg) 返回，成功填 out 返
// nullopt。列名与 SELECT 拼写漂移是编译期查不出的错，不容 valueOr 静默默认值。
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

// agent_tasks 的九列列序：listTasks / getTask 共用同一份 SELECT 列清单 +
// 同一份行映射——列名与取值散成两份就是下一次「改一处漏一处」。
constexpr const char* kTaskColumns =
    "id,name,goal,trigger_type,trigger_config,model_slot,"
    "max_iterations,allow_destructive,enabled";

// 一行 agent_tasks → AgentTaskRow（列缺失/类型不符一律 fail，不静默默认值）
Result<AgentTaskRow> taskRowFrom(const core::data::SqlRow& row) {
    AgentTaskRow t;
    int64_t maxIter = 0, allowDestr = 0, enabled = 0;
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
    t.maxIterations = static_cast<int>(maxIter);
    t.allowDestructive = (allowDestr != 0);
    t.enabled = (enabled != 0);
    return Result<AgentTaskRow>::ok(std::move(t));
}

} // namespace

AgentStore::AgentStore(core::data::SqliteDb& db) : db_(db) {}

Result<int64_t> AgentStore::createTask(const std::string& name, const std::string& goal,
                                       const std::string& triggerType,
                                       const std::string& modelSlot,
                                       int maxIterations, bool allowDestructive,
                                       const std::string& triggerConfig) {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare(
        "INSERT INTO agent_tasks(name, goal, trigger_type, trigger_config, model_slot,"
        " max_iterations, allow_destructive, enabled, created_at, updated_at)"
        " VALUES(?,?,?,?,?,?,?,1,?,?)");
    if (st.isFail()) return Result<int64_t>::fail(st.errorCode(), st.errorMessage());
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    auto& s = st.value();
    // brief 原示例的 && 串联不可编译（Result<void> 无 operator&&），改 bindAll
    // helper：逐个绑定、失败即透传，语义与逐个 if 检查一致
    auto ok = bindAll(s, name, goal, triggerType, triggerConfig, modelSlot,
                      static_cast<int64_t>(maxIterations),
                      static_cast<int64_t>(allowDestructive ? 1 : 0), now, now);
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
    // 查存在性 → 插行必须在**同一把锁**内（db_.mutex() 是 recursive_mutex，
    // 下面的 startRun 会再锁一次、同一线程可重入），中间不放锁 = 跨线程原子。
    // 手动「启动」与 Scheduler/cron 派发都走 AgentLoop::run → 这里（唯一闸点）。
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
                                    bool allowDestructive) {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare(
        "UPDATE agent_tasks SET name=?,goal=?,trigger_type=?,trigger_config=?,"
        "model_slot=?,max_iterations=?,allow_destructive=?,updated_at=? WHERE id=?");
    if (st.isFail()) return Result<void>::fail(st.errorCode(), st.errorMessage());
    auto& s = st.value();
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    auto ok = bindAll(s, name, goal, triggerType, triggerConfig, modelSlot,
                      static_cast<int64_t>(maxIterations),
                      static_cast<int64_t>(allowDestructive ? 1 : 0), now, id);
    if (ok.isFail()) return Result<void>::fail(ok.errorCode(), ok.errorMessage());
    auto step = s.step();
    if (step.isFail()) return Result<void>::fail(step.errorCode(), step.errorMessage());
    // 一类一规则（与 getTask 同口径）：id 不存在 → fail(NotFound)，不静默 no-op。
    // UPDATE 影响 0 行即目标行不存在（同 todo_service.cpp:329 既有口径）；持锁
    // 覆盖 step + sqlite3_changes，计数不会被并发语句插队（recursive_mutex）。
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
    // 同 updateTask：id 不存在 → fail(NotFound)，不静默 no-op
    if (sqlite3_changes(db_.handle()) == 0) {
        return Result<void>::fail(ErrorCode::NotFound,
                                  "任务不存在：id=" + std::to_string(id));
    }
    return Result<void>::ok();
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
    // 含 optional 参数：逐个绑定，nullopt → bind(i, nullptr) 表 NULL
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
        "SELECT seq,role,content_json,tool_call_id FROM agent_messages"
        " WHERE run_id=? ORDER BY seq");
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
        "SELECT id,task_id,status,started_at,ended_at,iterations,input_tokens,"
        "output_tokens,error FROM agent_runs WHERE task_id=?"
        " ORDER BY started_at DESC LIMIT ?");
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

} // namespace module::agent
