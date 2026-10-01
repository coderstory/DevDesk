// src/module_agent/agent_loop.h
// AgentLoop：单次 run 的执行器（agent M1）。消息累积、调 LlmEndpoint、解析
// tool_use、经 ToolRegistry 执行、回喂、迭代控制、落库。签名逐字来自
// plan task-4-brief（除 using core::Result 与 <cstdint> 卫生补 include）。
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

using core::Result;   // 同 agent_store.h:14（Result<T> 本仓库统一错误类型）

struct AgentTask {
    int64_t id = 0;
    std::string name;
    std::string goal;
    std::string modelSlot;      // "default"/"sonnet"/...（profile 槽位键名）
    int maxIterations = 10;
    bool allowDestructive = false;
};

class AgentStore;

class AgentLoop {
public:
    // 单调时钟缝（spec §5.2 墙钟上限）：返回 steady 毫秒。默认空 = 走
    // defaultNowMs()（steady_clock）；用例注入假时钟即可零等待验证超限判定。
    using NowMsFn = std::function<int64_t()>;
    AgentLoop(core::LlmEndpoint& llm, core::ToolRegistry& tools, AgentStore& store,
              NowMsFn nowMs = {});
    // 同步跑一次（调用方在工作线程调）。落库 agent_runs/agent_messages。
    // 失败不抛：见 spec §5.2 状态语义（completed/failed/limited/cancelled）。
    Result<int64_t> run(const AgentTask& task, std::atomic<bool>& cancelToken);
private:
    core::LlmEndpoint& llm_;
    core::ToolRegistry& tools_;
    AgentStore& store_;
    NowMsFn nowMs_;
};
} // namespace module::agent
