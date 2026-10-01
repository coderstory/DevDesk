// src/module_agent/scheduler.h
// Scheduler：agent 任务的触发侧（agent M2）。定时（cron）+（Task 5）常驻 worker
// 队列；自己的 run 线程表 + 分钟防重表。**「同 task 不并发」的闸不在本类**——
// 单闸单点在 AgentLoop::run（内调 AgentStore::startRunExclusive），本类只做
// 「起线程 + 等它自己收尾」的薄 spawn 包装（spec §5.1 / §10，M2 审计修订）。
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "module_agent/agent_loop.h"
#include "module_agent/agent_store.h"

namespace module::agent {
class AgentLoop;

class Scheduler {
public:
    // now：unix 秒时钟缝（spec §5.1「时钟注入：构造注入 std::function<int64_t()>，
    // 测试零等待」）。nullptr = 真实系统时间。
    using NowFn = std::function<int64_t()>;
    Scheduler(AgentStore& store, AgentLoop& loop, NowFn now = {});
    ~Scheduler();                       // 兜底 stopAll（幂等）
    void start();                       // 起 tick 轮询线程（Task 5 再起常驻线程）
    void stopAll();                     // 停派发线程（tick/常驻）→ cancel 全部在跑 run → join（幂等）
    // 对每个 enabled 且 trigger_type='schedule' 的任务：解析 trigger_config 的
    // cron → matches(本地 tm) → 命中且 lastFiredMinute_[id] != 本分钟 → 派发。
    // 公开供测试直调（零等待）；tick 线程每 200ms 实时醒来探一次分钟翻转。
    core::Result<void> tickNow();
    size_t activeRunCount();            // 测试观察面：runs_ 容器条目数（含已收尾未 reap 的）

private:
    struct ActiveRun {
        int64_t taskId = 0;
        std::thread thread;
        std::atomic<bool> cancel{false};
        std::atomic<bool> done{false};
        std::atomic<int64_t> runId{0};  // loop_.run 返回；0 = 未起（撞闸/起跑失败）
    };
    core::Result<void> dispatchRun(int64_t taskId);  // 薄 spawn 包装：闸在 AgentLoop::run 内
    void reapDone();
    void tickThreadLoop();
    AgentStore& store_;
    AgentLoop& loop_;
    NowFn now_;
    std::atomic<bool> stopFlag_{false};
    std::thread tickThread_;
    std::mutex mu_;                                 // 护 runs_ / lastFiredMinute_
    std::map<int64_t, int64_t> lastFiredMinute_;    // taskId → 上次触发的分钟序（now/60）
    std::vector<std::unique_ptr<ActiveRun>> runs_;  // 派发线程写 / stopAll 与观察面读（mu_ 护）
};
} // namespace module::agent
