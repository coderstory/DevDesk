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
    std::string contextNote;
    int autoNote = 1;
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
// 任务级 run 聚合（详情弹窗汇总行）：一条 GROUP BY 出全部任务，无 run 的任务不在结果里
struct AgentTaskRunSummary {
    int64_t taskId = 0;
    int64_t totalRuns = 0;                 // 含 running/cancelled/limited
    int64_t okRuns = 0;                    // 仅 status='completed'
    int64_t totalSeconds = 0;              // Σ 单 run 时长（ended-started，负差钳 0，running 计 0）
    std::optional<int64_t> lastEndedAt;    // MAX(ended_at)；全部未结束 → nullopt
};

class AgentStore {
public:
    explicit AgentStore(core::data::SqliteDb& db);
    // triggerConfig 尾参默认空串；createTask 是「建任务」唯一入口，cron 只经它/updateTask 写入
    Result<int64_t> createTask(const std::string& name, const std::string& goal,
                               const std::string& triggerType,
                               const std::string& modelSlot,
                               int maxIterations, bool allowDestructive,
                               const std::string& triggerConfig = "",
                               const std::string& contextNote = "",
                               int autoNote = 1);
    Result<std::vector<AgentTaskRow>> listTasks();
    Result<AgentTaskRow> getTask(int64_t id);                    // 不存在 → NotFound
    // 任务的唯一「同 task 不并发」闸：锁内查 running 再插 = 原子；已有 running → fail(AlreadyExists) 且不落行
    Result<int64_t> startRunExclusive(int64_t taskId);
    Result<int64_t> startRun(int64_t taskId);                    // INSERT status='running'
    // 九列覆盖 + updated_at；enabled 归 setTaskEnabled（两动作互不覆盖）；id 不存在 → NotFound
    Result<void> updateTask(int64_t id, const std::string& name, const std::string& goal,
                            const std::string& triggerType,
                            const std::string& triggerConfig,
                            const std::string& modelSlot, int maxIterations,
                            bool allowDestructive,
                            const std::string& contextNote = "",
                            int autoNote = 1);
    Result<void> setTaskEnabled(int64_t id, bool enabled);   // 不存在 → NotFound
    // 删任务并清全部运行痕迹：单事务三条显式 DELETE（messages 按 runs 子查询 → runs →
    // tasks），不把级联押在 FK CASCADE 的 pragma 状态上；id 不存在 → NotFound 且三表零变化
    Result<void> deleteTask(int64_t id);
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
    // 库内是否存在 running run（EXISTS 语义，单条 SELECT 不拉行）：pollRunEnd 三守卫
    // 全假时冷启动兜底的唯一探测口——外部直写库插入的 running run 只能靠它发现
    Result<bool> hasRunningRun();
    // 任务级 run 聚合（COUNT/SUM/MAX 一条 SQL，不拉全表进内存）；配合 writeVersion 做 UI 侧缓存
    Result<std::vector<AgentTaskRunSummary>> listTaskRunSummaries();
    Result<void> sweepStaleRuns();   // running → failed('应用异常退出（悬挂 run）')
    // UI 侧 version 缓存判脏（读侧 acquire load，无锁不查库；透传 db_ 的写版本号）
    uint64_t writeVersion() const { return db_.writeVersion(); }
    // 追加文本到 context_note：锁内读-拼-裁-写单事务；超 4096 字节从头丢最旧至 UTF-8 边界；
    // 空串 no-op；taskId 不存在 → NotFound
    Result<void> appendContextNote(int64_t taskId, const std::string& text);
private:
    core::data::SqliteDb& db_;
};
} // namespace module::agent
