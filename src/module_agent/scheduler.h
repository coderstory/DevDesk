// src/module_agent/scheduler.h
// Scheduler：agent 任务的触发侧。定时（cron）触发 + 常驻 worker 队列；自己的 run
// 线程表 + 分钟防重表。「同 task 不并发」的闸不在本类——单闸在 AgentLoop::run，
// 本类只做「起线程 + 等收尾」的薄 spawn。
// 契约：① 成员声明顺序——Scheduler 必须声明在 loop_/llm_/store_ 之后（逆序析构下
// Scheduler 先亡，其 run 线程 join 期间依赖的 loop/store 仍存活）；② stopAll 可重复
// （每次收干当前 runs_），但不支持并发调用（单线程收尾口径）。
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
    // unix 秒时钟缝（测试构造注入，零等待）；nullptr = 真实系统时间
    using NowFn = std::function<int64_t()>;
    Scheduler(AgentStore& store, AgentLoop& loop, NowFn now = {});
    ~Scheduler();                       // 兜底 stopAll（幂等）
    void start();                       // 起 tick 轮询线程 + 常驻 worker 线程（各自幂等守卫）
    void stopAll();                     // 停派发线程（tick/常驻）→ cancel 全部在跑 run → join（幂等）
    // 每次扫 enabled 且 schedule 的任务：cron 匹配 + 同分钟防重 → 派发。公开供测试直调
    core::Result<void> tickNow();
    size_t activeRunCount();            // 测试观察面：runs_ 容器条目数（含已收尾未 reap 的）
    // 常驻 worker 手动入队。pop 即 spawn、不等上一跑结束；同 task 连发两单由
    // AgentLoop 单闸拒绝。停止后不再入队（避免幽灵单）
    void enqueueResident(int64_t taskId);
    // 测试观察面：常驻待跑队列长度。停止后入队必须不落地，否则留幽灵单——
    // 这条不变式只能由队列长度断言钉住
    size_t residentQueueSize();

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
    void residentThreadLoop();
    AgentStore& store_;
    AgentLoop& loop_;
    NowFn now_;
    std::atomic<bool> stopFlag_{false};
    std::thread tickThread_;
    std::thread residentThread_;
    std::mutex mu_;                                 // 护 runs_ / lastFiredMinute_ / residentQueue_
    std::condition_variable cv_;                    // 常驻队列有新单 / stopAll 唤醒
    std::map<int64_t, int64_t> lastFiredMinute_;    // taskId → 上次触发的分钟序（now/60）
    std::vector<std::unique_ptr<ActiveRun>> runs_;  // 派发线程写 / stopAll 与观察面读（mu_ 护）
    std::deque<int64_t> residentQueue_;             // 常驻待跑 taskId（mu_ 护）
};
} // namespace module::agent
