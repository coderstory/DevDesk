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

// store 行 → 执行体入参的**唯一映射**（M2）：AgentModule（手动启动）、
// Scheduler（cron 派发）、测试三方共用一份，避免各处手抄字段搬运漂移。
// 只搬 AgentTask 关心的六项；trigger_type/trigger_config/enabled 归派发侧判定，
// 不进执行体（run 不关心自己是被谁触发的）。
struct AgentTaskRow;
AgentTask taskFromRow(const AgentTaskRow& row);

class AgentStore;

class AgentLoop {
public:
    // 单调时钟缝（spec §5.2 墙钟上限）：返回 steady 毫秒。默认空 = 走
    // defaultNowMs()（steady_clock）；用例注入假时钟即可零等待验证超限判定。
    using NowMsFn = std::function<int64_t()>;
    // **收尾写库失败**的回报缝。run() 恒返回 ok(runId)（run 确实起了，收尾只是
    // 状态落库），所以「收尾没落成」在返回值里无处表达；此前只有一行 LOG_ERROR，
    // 调用方无从得知 —— 于是 DB 里那行永远停在 'running'，UI 侧据此判「还有 run
    // 在跑」，uiPolling_ 永不复位，**每帧一次 refreshData()（N+1 次查询）直到重启**
    //（触发条件窄：磁盘满 / DB 写错；后果重：100% CPU + 持续磁盘 IO + UI 卡死）。
    // 调用方（AgentModule）用它把该 runId 记为孤儿，从轮询判停里排除。
    // 默认空 = 无人关心（测试 / 其它调用方）。**不抛异常**。
    using FinishFailureFn = std::function<void(int64_t runId, std::string reason)>;
    AgentLoop(core::LlmEndpoint& llm, core::ToolRegistry& tools, AgentStore& store,
              NowMsFn nowMs = {}, FinishFailureFn onFinishFailure = {});
    // 同步跑一次（调用方在工作线程调）。落库 agent_runs/agent_messages。
    // 失败不抛：见 spec §5.2 状态语义（completed/failed/limited/cancelled）。
    Result<int64_t> run(const AgentTask& task, std::atomic<bool>& cancelToken);
private:
    core::LlmEndpoint& llm_;
    core::ToolRegistry& tools_;
    AgentStore& store_;
    NowMsFn nowMs_;
    FinishFailureFn onFinishFailure_;
};
} // namespace module::agent
