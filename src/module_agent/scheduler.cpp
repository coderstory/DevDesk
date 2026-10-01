// src/module_agent/scheduler.cpp
// 触发侧实现（agent M2 Task 4）：tickNow 扫描 → cron 匹配 → 同分钟防重 → 薄 spawn。
// 不抛异常（Result 传递；线程体自带 catch 兜底，不让异常逃出线程入口）。
#include "module_agent/scheduler.h"

#include <chrono>
#include <ctime>
#include <exception>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/logging.h"
#include "core/time.h"
#include "module_agent/cron.h"
#include "platform/json.h"

namespace module::agent {
namespace {

// tick 轮询粒度：200ms 一轮（spec §5.1「按分钟边界醒来」的实现：轮询足够细，
// 分钟翻转即触发；错过的分钟不补跑——lastFiredMinute 只记最近一次）
constexpr int kTickSleepSliceMs = 20;
constexpr int kTickSlicesPerRound = 10;   // 20ms × 10 = 200ms

int64_t systemNowSec() {
    return static_cast<int64_t>(std::time(nullptr));
}

} // namespace

Scheduler::Scheduler(AgentStore& store, AgentLoop& loop, NowFn now)
    : store_(store), loop_(loop), now_(std::move(now)) {}

Scheduler::~Scheduler() {
    stopAll();   // 兜底（幂等）：调用方漏调也不会留下 joinable 线程
}

void Scheduler::start() {
    if (stopFlag_.load()) return;   // 已停：不重启（幂等守卫）
    try {
        if (!tickThread_.joinable())
            tickThread_ = std::thread([this] { tickThreadLoop(); });
    } catch (const std::system_error& e) {
        LOG_ERROR(std::string("Scheduler: tick 线程创建失败: ") + e.what());
    }
    try {
        if (!residentThread_.joinable())
            residentThread_ = std::thread([this] { residentThreadLoop(); });
    } catch (const std::system_error& e) {
        LOG_ERROR(std::string("Scheduler: 常驻线程创建失败: ") + e.what());
    }
}

void Scheduler::enqueueResident(int64_t taskId) {
    {
        // 停止判定与入队**同锁**（评审修复轮）：锁外判 stopFlag_ 时，stopAll 窗口里
        // 落地的 push 会留在队列里永不被消费（常驻线程已 join 退出）——幽灵单。
        // 锁内判则二者原子：要么在 stop 前入队（可能被消费、也可能随 teardown 丢弃），
        // 要么根本不入队。
        std::lock_guard<std::mutex> lock(mu_);
        if (stopFlag_.load()) return;   // 已停：不再接单
        residentQueue_.push_back(taskId);
    }
    cv_.notify_one();
}

void Scheduler::stopAll() {
    // 幂等但**不早退**：stopFlag_ 置位可反复（线程退出条件只读它），而每次调用
    // 都必须把**当前** runs_ 收干。早退（exchange 守卫）会让「首次 stopAll 之后
    // 又派发出来的 run」逃出收尾——它留在 runs_ 里，~Scheduler 析构 joinable
    // thread 即 std::terminate（实测崩溃报告：~ActiveRun → terminate）。
    // tickNow 派发不看 stopFlag_（测试直调零等待），故这条路径真实存在。
    stopFlag_.store(true);
    {
        std::lock_guard<std::mutex> lock(mu_);
        cv_.notify_all();   // 唤醒常驻线程（其 wait 谓词含 stopFlag_）
    }
    // 顺序契约（spec §3，M2 审计修订）：**先灭派发源、再收在跑 run**——反序会让
    // 常驻/tick 线程在收尾窗口里再 pop 一个 job、spawn 出新 run 逃出 teardown。
    // 常驻线程先于 tick 线程 join（它是唯一会在「收尾窗口」里继续接单的源）。
    if (residentThread_.joinable()) residentThread_.join();
    if (tickThread_.joinable()) tickThread_.join();
    std::vector<std::unique_ptr<ActiveRun>> doomed;
    {
        std::lock_guard<std::mutex> lock(mu_);
        doomed.swap(runs_);
    }
    for (auto& r : doomed) r->cancel.store(true);
    for (auto& r : doomed) {
        if (r->thread.joinable()) r->thread.join();
    }
    doomed.clear();
    LOG_INFO("Scheduler: stopAll 完成（tick/常驻线程已停，在跑 run 已 cancel+join）");
}

size_t Scheduler::activeRunCount() {
    std::lock_guard<std::mutex> lock(mu_);
    return runs_.size();
}

size_t Scheduler::residentQueueSize() {
    std::lock_guard<std::mutex> lock(mu_);
    return residentQueue_.size();
}

void Scheduler::reapDone() {
    // 摘除已收尾条目（join 在锁外：run 线程不碰 mu_，但持锁 join 会白堵派发方）
    std::vector<std::unique_ptr<ActiveRun>> finished;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = runs_.begin(); it != runs_.end();) {
            if ((*it)->done.load()) {
                finished.push_back(std::move(*it));
                it = runs_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& r : finished) {
        if (r->thread.joinable()) r->thread.join();
    }
}

void Scheduler::tickThreadLoop() {
    int64_t lastSeenMinute = std::numeric_limits<int64_t>::min();
    try {
        while (!stopFlag_.load()) {
            for (int i = 0; i < kTickSlicesPerRound && !stopFlag_.load(); ++i)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(kTickSleepSliceMs));
            if (stopFlag_.load()) break;
            const int64_t nowSec = now_ ? now_() : systemNowSec();
            const int64_t minute = nowSec / 60;
            if (minute == lastSeenMinute) continue;   // 分钟没翻转 → 不重复扫
            lastSeenMinute = minute;
            auto ticked = tickNow();
            if (ticked.isFail())
                LOG_WARN("Scheduler: tick 失败: " + ticked.errorMessage());
        }
    } catch (const std::exception& e) {
        // 线程体兜底：异常不许逃出线程入口（逃出 = std::terminate）
        LOG_ERROR(std::string("Scheduler: tick 线程异常退出: ") + e.what());
    } catch (...) {
        LOG_ERROR("Scheduler: tick 线程未知异常退出");
    }
}

void Scheduler::residentThreadLoop() {
    try {
        while (true) {
            int64_t taskId = 0;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this] {
                    return stopFlag_.load() || !residentQueue_.empty();
                });
                // 先灭派发源：停了就**不 pop**（收尾窗口里不再起新 run）
                if (stopFlag_.load()) break;
                taskId = residentQueue_.front();
                residentQueue_.pop_front();
            }
            // pop 即 spawn、不等上一跑结束；同 task 连发两单在 AgentLoop 的单闸处
            // 被拒（WARN 后收尾，无行可落），不阻塞队列
            auto fired = dispatchRun(taskId);
            if (fired.isFail())
                LOG_WARN("Scheduler: 常驻派发失败 task=" + std::to_string(taskId) +
                         ": " + fired.errorMessage());
        }
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("Scheduler: 常驻线程异常退出: ") + e.what());
    } catch (...) {
        LOG_ERROR("Scheduler: 常驻线程未知异常退出");
    }
}

core::Result<void> Scheduler::tickNow() {
    reapDone();
    auto tasks = store_.listTasks();
    if (tasks.isFail())
        return core::Result<void>::fail(tasks.errorCode(), tasks.errorMessage());
    const int64_t nowSec = now_ ? now_() : systemNowSec();
    const int64_t minute = nowSec / 60;
    std::tm local{};
    core::time::localTime(static_cast<std::time_t>(nowSec), &local);   // tm_wday 同步填充
    for (const auto& t : tasks.value()) {
        if (!t.enabled || t.triggerType != "schedule") continue;
        auto cfg = platform::json::parse(t.triggerConfig);
        std::string cronText;
        if (cfg.isOk() && cfg.value().isObject()) {
            auto v = cfg.value().asObject().get("cron");
            if (v.isString()) cronText = v.asString();
        }
        if (cronText.empty()) continue;              // 配置缺失/坏 → 静默跳过不派发
        auto expr = parseCron(cronText);
        if (expr.isFail()) continue;                 // 保存期已校验；运行期坏配置不炸 tick
        if (!cronMatches(expr.value(), local)) continue;
        {   // 同分钟防重（spec §5.1 lastFiredMinute）
            std::lock_guard<std::mutex> lock(mu_);
            if (lastFiredMinute_[t.id] == minute) continue;
            lastFiredMinute_[t.id] = minute;
        }
        auto fired = dispatchRun(t.id);
        if (fired.isFail())
            LOG_WARN("Scheduler: 派发失败 task=" + std::to_string(t.id) + ": " +
                     fired.errorMessage());
    }
    return core::Result<void>::ok();
}

core::Result<void> Scheduler::dispatchRun(int64_t taskId) {
    auto row = store_.getTask(taskId);
    if (row.isFail()) return core::Result<void>::fail(row.errorCode(), row.errorMessage());
    AgentTask task = taskFromRow(row.value());
    auto run = std::make_unique<ActiveRun>();
    run->taskId = taskId;
    ActiveRun* raw = run.get();
    // 入表 + 线程创建在**同一把 mu_** 内完成（评审修复轮）：stopAll / reapDone 在锁内
    // 读 `joinable()` 并 join，创建若在锁外，收尾窗口会看到「表里有条目、thread 尚未
    // 赋值」的半成品（漏 join 或对未赋值 thread 操作）。run 线程体不取 mu_（只碰
    // cancel/done/runId 三个原子 + loop_/store_），故持锁创建不会死锁。
    std::lock_guard<std::mutex> lock(mu_);
    runs_.push_back(std::move(run));
    try {
        raw->thread = std::thread([this, raw, task] {
            try {
                auto r = loop_.run(task, raw->cancel);   // 单闸 + 落行都在 loop 内
                if (r.isFail()) {
                    // 撞闸（他路同 task 在跑，ErrorCode::AlreadyExists）或起跑失败：
                    // 本路从未落行，无悬挂 running 要收尾，只记 WARN（Review Focus 1）
                    LOG_WARN("Scheduler: run 未起 task=" + std::to_string(task.id) +
                             ": " + r.errorMessage());
                } else {
                    raw->runId.store(r.value());         // runId 取自 loop_.run 的返回值
                }
            } catch (const std::exception& e) {
                // 线程体兜底：异常不许逃出线程入口（逃出 = std::terminate）
                LOG_ERROR("Scheduler: run 线程异常 task=" + std::to_string(task.id) +
                          ": " + e.what());
            } catch (...) {
                LOG_ERROR("Scheduler: run 线程未知异常 task=" +
                          std::to_string(task.id));
            }
            raw->done.store(true);
        });
    } catch (const std::system_error& e) {
        // 线程创建失败：闸尚未被调用（在 loop 内）→ 库中无行可收尾，只摘除占位
        for (auto it = runs_.begin(); it != runs_.end(); ++it) {
            if (it->get() == raw) {
                runs_.erase(it);
                break;
            }
        }
        return core::Result<void>::fail(core::ErrorCode::Internal, e.what());
    }
    return core::Result<void>::ok();
}

} // namespace module::agent
