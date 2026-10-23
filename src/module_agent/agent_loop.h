// AgentLoop：单次 run 的执行器（消息累积 → LLM → tool_use 执行 → 回喂 → 落库）
#pragma once
#include <atomic>
#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <vector>
#include "core/llm_endpoint.h"
#include "core/result.h"
#include "core/tool.h"

namespace module::agent {

using core::Result;

// 运行时注入的「当前本地时间」行（YYYY-MM-DD HH:MM）。模型没有时钟：日期类任务
// （如「{年-月-日} 晨会」）必须由宿主给出「今天」，不能让模型自行推算/猜测。
// 纯函数，now 由调用方注入（测试可钉住具体日期）。
std::string currentTimePromptLine(std::time_t nowSec);

struct AgentTask {
    int64_t id = 0;
    std::string name;
    std::string goal;
    std::string modelSlot;      // "default"/"sonnet"/...（profile 槽位键名）
    int maxIterations = 10;
    bool allowDestructive = false;
    std::string systemPrompt;   // 用户可编辑的系统提示词（空串 = 用 run() 内默认）
};

// store 行 → 执行体入参的唯一映射：AgentModule/Scheduler/测试三方共用，避免手抄漂移
struct AgentTaskRow;
AgentTask taskFromRow(const AgentTaskRow& row);

class AgentStore;

class AgentLoop {
public:
    // 单调时钟缝：返回 steady 毫秒；用例注入假时钟可零等待验证超限判定
    using NowMsFn = std::function<int64_t()>;
    // 收尾写库失败的回报缝：run() 恒返回 ok(runId)，DB 行永停 'running' → UI 轮询永不判停，
    // 调用方（AgentModule）需把该 runId 记为孤儿排除出轮询判停。默认空 = 无人关心
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
