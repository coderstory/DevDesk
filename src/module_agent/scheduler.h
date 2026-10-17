// Scheduler：agent 任务的触发侧。定时（cron）触发 + 常驻 worker 队列 + 事件触发路由
//（事件源 = core::event::EventBus，订阅接线在 AgentModule 装配层）；「同 task 不并发」
// 的闸在 AgentLoop::run，本类只做「起线程 + 等收尾」的薄 spawn。契约：① 成员声明顺序
//——Scheduler 必须声明在 loop_/llm_/store_ 之后（逆序析构下先亡）；② stopAll 可重复
// 但不支持并发调用；③ 限次任务（trigger_config 的 max_runs）达上限后由本类置停用，
// 本条不只是判断——它改库（2026-10-17 加，见 runBudgetExhausted）。
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
#include <utility>
#include <vector>
#include "module_agent/agent_loop.h"
#include "module_agent/agent_store.h"
#include "module_agent/trigger_config.h"

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
    // 常驻 worker 手动入队。pop 即 spawn、不等上一跑结束；同 task 连发两单由 AgentLoop 单闸拒绝
    void enqueueResident(int64_t taskId);
    // 测试观察面：常驻待跑队列长度——「停止后入队不落地」不变式只能由队列长度断言钉住
    size_t residentQueueSize();
    // 事件触发入队口：EventBus emit 在 UI 主线程同步调用本函数——契约「轻入队」，
    // 只做锁内记事件名 + notify（锁内含 stopFlag 判定，与 enqueueResident 同因），
    // 不做 listTasks / 冷却判定等重活（不阻塞 UI 发射方）。消费归事件线程
    void onEvent(const std::string& name);
    // 事件队列消费：取出全部排队事件逐个路由（listTasks → event 型且 events 含该
    // 事件名且 enabled → 冷却判定 → dispatchRun）。事件线程体循环调用；公开供测试
    // 直调（与 tickNow 同理：零等待、不起线程即可驱动事件路径）
    core::Result<void> drainEventQueue();
    // 测试观察面：待消费事件数——「冷却窗口内事件被丢弃不排队」的不变式由它钉住
    size_t eventQueueSize();

private:
    struct ActiveRun {
        int64_t taskId = 0;
        std::thread thread;
        std::atomic<bool> cancel{false};
        std::atomic<bool> done{false};
        std::atomic<int64_t> runId{0};  // loop_.run 返回；0 = 未起（撞闸/起跑失败）
    };
    core::Result<void> dispatchRun(int64_t taskId);  // 薄 spawn 包装：闸在 AgentLoop::run 内
    // 限次 / 一次性任务的额度判定（tickNow / processEvent 的唯一共用闸）。
    // maxRuns 缺省（nullopt）= 不限次数：零查询、零行为变化（旧配置恒循环不回归）。
    // 配了上限时数 agent_runs 里该任务已「起过跑」的 run（含 running/failed/cancelled/
    // limited——记账单位是起过跑而不是跑成功，见 02 单的失败语义），达上限即
    // setTaskEnabled(false) 置停用（复用既有启停入口，不直写 SQL）并拒绝本次派发。
    // 返回 true = 本路不得派发（额度已耗尽，或计数读库失败——宁可不跑也不超跑）
    bool runBudgetExhausted(const AgentTaskRow& task, const TriggerConfig& cfg);
    void reapDone();
    void tickThreadLoop();
    void residentThreadLoop();
    void eventThreadLoop();
    // 单个事件的路由：listTasks 过滤（enabled + event 型 + events 含 name）→
    // 任务×事件级冷却（同任务不同事件互不压制；窗口内直接丢弃，不排队不延后）→ dispatchRun
    void processEvent(const std::string& name);
    AgentStore& store_;
    AgentLoop& loop_;
    NowFn now_;
    std::atomic<bool> stopFlag_{false};
    std::thread tickThread_;
    std::thread residentThread_;
    std::thread eventThread_;
    std::mutex mu_;                                 // 护 runs_ / lastFiredMinute_ / residentQueue_ / eventQueue_ / lastEventFireSec_
    std::condition_variable cv_;                    // 常驻/事件队列有新单 / stopAll 唤醒
    std::map<int64_t, int64_t> lastFiredMinute_;    // taskId → 上次触发的分钟序（now/60）
    std::map<std::pair<int64_t, std::string>, int64_t> lastEventFireSec_;   // (taskId, 事件名) → 该组合上次派发的 unix 秒（冷却起点）
    std::vector<std::unique_ptr<ActiveRun>> runs_;  // 派发线程写 / stopAll 与观察面读（mu_ 护）
    std::deque<int64_t> residentQueue_;             // 常驻待跑 taskId（mu_ 护）
    std::deque<std::string> eventQueue_;            // 待路由的事件名（mu_ 护；只存名字，冷却按消费时刻计时）
};
} // namespace module::agent
