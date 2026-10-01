// src/module_agent/scheduler.h
// Scheduler：agent 任务的触发侧（agent M2）。定时（cron）触发 + 常驻 worker 队列；
// 自己的 run 线程表 + 分钟防重表。**「同 task 不并发」的闸不在本类**——
// 单闸单点在 AgentLoop::run（内调 AgentStore::startRunExclusive），本类只做
// 「起线程 + 等它自己收尾」的薄 spawn 包装（spec §5.1 / §10，M2 审计修订）。
//
// 契约（T6 接线依赖，勿回退）：
//   ① **成员声明顺序**：Scheduler 实例必须声明在它引用的 loop_ / llm_ / store_ **之后**
//      （C++ 逆序析构：Scheduler 先亡，其 run 线程 join 期间 loop/store 必须仍存活）。
//   ② stopAll 可重复调用（幂等，每次收干当前 runs_），但**不支持并发调用**——它不做
//      tickThread_/residentThread_ 的 join 与 runs_ 摘取的跨线程互斥（单线程收尾口径，
//      与 AgentModule::onStop 一致）。
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
    void start();                       // 起 tick 轮询线程 + 常驻 worker 线程（各自幂等守卫）
    void stopAll();                     // 停派发线程（tick/常驻）→ cancel 全部在跑 run → join（幂等）
    // 对每个 enabled 且 trigger_type='schedule' 的任务：解析 trigger_config 的
    // cron → matches(本地 tm) → 命中且 lastFiredMinute_[id] != 本分钟 → 派发。
    // 公开供测试直调（零等待）；tick 线程每 200ms 实时醒来探一次分钟翻转。
    core::Result<void> tickNow();
    size_t activeRunCount();            // 测试观察面：runs_ 容器条目数（含已收尾未 reap 的）
    // 常驻 worker 手动入队（spec §5.1：常驻本阶段只跑手动入队子任务——事件触发
    // 降级 Phase 2）。pop 即 spawn、不等上一跑结束；同 task 连发两单由 AgentLoop
    // 内的单闸拒绝（不阻塞队列）。停止后不再入队（避免留永远不被消费的幽灵单）。
    void enqueueResident(int64_t taskId);
    // 测试观察面（同 activeRunCount 口径）：常驻待跑队列长度。停止后入队必须不落地，
    // 否则队列里会留一条永不被消费的幽灵单——这条不变式只能由队列长度断言钉住
    // （去掉入队守卫时，run 数观察面看不见差别）。
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
