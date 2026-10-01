// src/module_agent/agent_store.h
// 智能体落库层（agent M1）：agent_tasks / agent_runs / agent_messages 三表
// 的 CRUD + 悬挂 run 清扫。AgentLoop / AgentModule 依赖的唯一持久化口。
// 签名逐字来自 plan task-3-brief（除必要 using core::Result 外零改动）。
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "core/sqlite_db.h"

namespace module::agent {

using core::Result;   // Result<T> 本仓库统一错误类型；命名空间内 using 后签名可直书 Result<T>

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
    Result<int64_t> createTask(const std::string& name, const std::string& goal,
                               const std::string& triggerType,
                               const std::string& modelSlot,
                               int maxIterations, bool allowDestructive);
    Result<std::vector<AgentTaskRow>> listTasks();
    Result<int64_t> startRun(int64_t taskId);                    // INSERT status='running'
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
