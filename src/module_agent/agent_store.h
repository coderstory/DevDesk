// 智能体落库层：agent_tasks / agent_runs / agent_messages 三表 CRUD + 悬挂 run 清扫，
// 是 AgentLoop / AgentModule 唯一的持久化口
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "core/sqlite_db.h"

namespace module::agent {

using core::Result;

struct AgentTaskRow {
    int64_t id = 0;
    std::string name, goal, triggerType, triggerConfig, modelSlot;
    int maxIterations = 10;
    bool allowDestructive = false;
    bool enabled = true;
};
struct AgentRunRow {
    int64_t id = 0, taskId = 0;
    std::string status;                 // running/completed/failed/limited/cancelled
    int64_t startedAt = 0;
    std::optional<int64_t> endedAt;
    int iterations = 0;
    std::optional<int64_t> inputTokens, outputTokens;
    std::optional<std::string> error;
};
struct AgentMessageRow {
    int64_t seq = 0;
    std::string role, contentJson;
    std::optional<std::string> toolCallId;
};

class AgentStore {
public:
    explicit AgentStore(core::data::SqliteDb& db);
    // triggerConfig 尾参默认空串；createTask 是「建任务」唯一入口，cron 只经它/updateTask 写入
    Result<int64_t> createTask(const std::string& name, const std::string& goal,
                               const std::string& triggerType,
                               const std::string& modelSlot,
                               int maxIterations, bool allowDestructive,
                               const std::string& triggerConfig = "");
    Result<std::vector<AgentTaskRow>> listTasks();
    Result<AgentTaskRow> getTask(int64_t id);                    // 不存在 → NotFound
    // 任务的唯一「同 task 不并发」闸：锁内查 running 再插 = 原子；已有 running → fail(AlreadyExists) 且不落行
    Result<int64_t> startRunExclusive(int64_t taskId);
    Result<int64_t> startRun(int64_t taskId);                    // INSERT status='running'
    // 七列覆盖 + updated_at；enabled 归 setTaskEnabled（两动作互不覆盖）；id 不存在 → NotFound
    Result<void> updateTask(int64_t id, const std::string& name, const std::string& goal,
                            const std::string& triggerType,
                            const std::string& triggerConfig,
                            const std::string& modelSlot, int maxIterations,
                            bool allowDestructive);
    Result<void> setTaskEnabled(int64_t id, bool enabled);   // 不存在 → NotFound
    Result<void> finishRun(int64_t runId, const std::string& status,
                           int iterations,
                           std::optional<int64_t> inputTokens,
                           std::optional<int64_t> outputTokens,
                           const std::optional<std::string>& error);
    Result<void> appendMessage(int64_t runId, int64_t seq, const std::string& role,
                               const std::string& contentJson,
                               const std::optional<std::string>& toolCallId);
    Result<std::vector<AgentMessageRow>> listMessages(int64_t runId);   // ORDER BY seq
    Result<std::vector<AgentRunRow>> listRuns(int64_t taskId, int limit);
    Result<void> sweepStaleRuns();   // running → failed('应用异常退出（悬挂 run）')
private:
    core::data::SqliteDb& db_;
};
} // namespace module::agent
