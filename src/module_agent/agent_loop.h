// src/module_agent/agent_loop.h
// AgentLoop：单次 run 的执行器（消息累积 → LLM → tool_use 执行 → 回喂 → 落库）
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "core/llm_endpoint.h"
#include "core/result.h"
#include "core/tool.h"

namespace module::agent {

using core::Result;   // 本仓库统一错误类型

struct AgentTask {
    int64_t id = 0;
    std::string name;
    std::string goal;
    std::string modelSlot;      // "default"/"sonnet"/...（profile 槽位键名）
    int maxIterations = 10;
    bool allowDestructive = false;
};

// store 行 → 执行体入参的唯一映射：AgentModule/Scheduler/测试三方共用，避免手抄漂移。
// 只搬 AgentTask 关心的六项；trigger/enabled 归派发侧判定，不进执行体
struct AgentTaskRow;
AgentTask taskFromRow(const AgentTaskRow& row);

class AgentStore;

class AgentLoop {
public:
    // 单调时钟缝：返回 steady 毫秒；默认走 defaultNowMs()。用例注入假时钟可
    // 零等待验证超限判定
    using NowMsFn = std::function<int64_t()>;
    // **收尾写库失败**的回报缝。run() 恒返回 ok(runId)，收尾失败在返回值里无处
    // 表达——DB 那行永停 'running'，UI 据此判「还有 run 在跑」→ uiPolling_ 永不复位，
    // 每帧 refreshData 直到重启（触发窄：磁盘满/DB 错；后果重：CPU + IO + UI 卡死）。
    // 调用方（AgentModule）用它把该 runId 记为孤儿排除出轮询判停。默认空 = 无人关心
    using FinishFailureFn = std::function<void(int64_t runId, std::string reason)>;
    AgentLoop(core::LlmEndpoint& llm, core::ToolRegistry& tools, AgentStore& store,
              NowMsFn nowMs = {}, FinishFailureFn onFinishFailure = {});
    // 同步跑一次（调用方在工作线程调），落库 agent_runs/agent_messages；失败不抛
    Result<int64_t> run(const AgentTask& task, std::atomic<bool>& cancelToken);
private:
    core::LlmEndpoint& llm_;
    core::ToolRegistry& tools_;
    AgentStore& store_;
    NowMsFn nowMs_;
    FinishFailureFn onFinishFailure_;
};
} // namespace module::agent
