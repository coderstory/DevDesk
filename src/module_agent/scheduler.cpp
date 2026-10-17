// 触发侧实现：tickNow 扫描 → cron 匹配 → 同分钟防重 → 薄 spawn。不抛异常（线程体自带 catch）
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
#include "module_agent/trigger_config.h"

namespace module::agent {
namespace {

// tick 轮询粒度 200ms（20ms × 10）：轮询够细，分钟翻转即触发；错过的分钟不补跑
constexpr int kTickSleepSliceMs = 20;
constexpr int kTickSlicesPerRound = 10;   // 20ms × 10 = 200ms

int64_t systemNowSec() {
    return static_cast<int64_t>(std::time(nullptr));
}

} // namespace

Scheduler::Scheduler(AgentStore& store, AgentLoop& loop, NowFn now)
    : store_(store), loop_(loop), now_(std::move(now)) {}

Scheduler::~Scheduler() {
    stopAll();   // 兜底（幂等）：漏调也不会留下 joinable 线程
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
    try {
        if (!eventThread_.joinable())
            eventThread_ = std::thread([this] { eventThreadLoop(); });
    } catch (const std::system_error& e) {
        LOG_ERROR(std::string("Scheduler: 事件线程创建失败: ") + e.what());
    }
}

void Scheduler::enqueueResident(int64_t taskId) {
    {
        // 停止判定与入队同锁：锁外判 stopFlag_ 时 stopAll 窗口里落地的 push 会成为幽灵单
        std::lock_guard<std::mutex> lock(mu_);
        if (stopFlag_.load()) return;   // 已停：不再接单
        residentQueue_.push_back(taskId);
    }
    // 必须 notify_all：常驻/事件两个等待者共用 cv_，notify_one 摇中错误等待者时
    // （它谓词为假、回去重睡）真正有新单的队列就丢了唤醒——事件型任务永不起跑
    cv_.notify_all();
}

void Scheduler::stopAll() {
    // 幂等但不早退：早退会让「stopAll 之后又派发的 run」逃出收尾，析构 joinable thread 即 terminate
    stopFlag_.store(true);
    {
        std::lock_guard<std::mutex> lock(mu_);
        cv_.notify_all();   // 唤醒常驻线程（其 wait 谓词含 stopFlag_）
    }
    // 顺序契约：先灭派发源、再收在跑 run；常驻线程先 join（收尾窗口里唯一继续接单的源）
    if (residentThread_.joinable()) residentThread_.join();
    if (tickThread_.joinable()) tickThread_.join();
    if (eventThread_.joinable()) eventThread_.join();
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

size_t Scheduler::eventQueueSize() {
    std::lock_guard<std::mutex> lock(mu_);
    return eventQueue_.size();
}

void Scheduler::onEvent(const std::string& name) {
    {
        // 与 enqueueResident 同因：停止判定与入队同锁，锁外判 stopFlag_ 会有幽灵单窗口
        std::lock_guard<std::mutex> lock(mu_);
        if (stopFlag_.load()) return;   // 已停：不再接单
        eventQueue_.push_back(name);
    }
    // 同 enqueueResident：共用 cv_ 时 notify_one 会摇错等待者（事件丢失），必须 notify_all
    cv_.notify_all();
}

void Scheduler::reapDone() {
    // 摘除已收尾条目（join 在锁外：持锁会白堵派发方）
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
                // 停了就不 pop（收尾窗口里不再起新 run）
                if (stopFlag_.load()) break;
                taskId = residentQueue_.front();
                residentQueue_.pop_front();
            }
            // pop 即 spawn，不等上一跑结束；同 task 连发两单在 AgentLoop 单闸被拒，不阻塞队列
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

void Scheduler::eventThreadLoop() {
    try {
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this] {
                    return stopFlag_.load() || !eventQueue_.empty();
                });
                // 停了就不消费（收尾窗口里不再起新 run）
                if (stopFlag_.load()) break;
            }
            // drain 内部先锁外取全队再逐个路由：事件消费不依赖 cron tick 的分钟节奏
            auto drained = drainEventQueue();
            if (drained.isFail())
                LOG_WARN("Scheduler: 事件队列消费失败: " + drained.errorMessage());
        }
    } catch (const std::exception& e) {
        // 线程体兜底：异常不许逃出线程入口（逃出 = std::terminate）
        LOG_ERROR(std::string("Scheduler: 事件线程异常退出: ") + e.what());
    } catch (...) {
        LOG_ERROR("Scheduler: 事件线程未知异常退出");
    }
}

// 限次额度闸（契约见 scheduler.h）。放在两条派发路径的**派发前**：
//   schedule 型 → tickNow 的 cronMatches **之前**、坏配置/坏 cron 两条点名 WARN **之后**
//     （WARN 在前：静默失败面不许被额度闸提前 continue 掉而消失）。判定与本分钟是否
//     匹配无关 —— 每分钟扫描都重判，于是「跑满即停」的停用最迟在下一分钟落地；若放到
//     cronMatches 之后，「每天 18:00 只跑一次」的任务会一直显示启用状态直到第二天
//     18:00 才被停用（用户看不见停）。
//     判据：tests/test_modules/test_agent_scheduler.cpp 的
//     `scheduler_max_runs_disables_on_non_matching_minute`
//   event 型 → processEvent 的 events 比对之前：事件型没有周期扫描，下一次事件到达是
//     唯一的调度扫描机会，无论该事件匹不匹配都把额度耗尽的任务置停用（只改启停列，
//     绝不因此派发任何 run）
bool Scheduler::runBudgetExhausted(const AgentTaskRow& task, const TriggerConfig& cfg) {
    if (!cfg.maxRuns || *cfg.maxRuns < 1) return false;   // 不限次数：旧配置的老路径
    // 计数源 = agent_runs 的既有查询：LIMIT=maxRuns，返回行数 == maxRuns 即「至少跑过
    // maxRuns 次」（< maxRuns 时返回实际条数）。上限已在 decode 里钳到 kMaxRunsUpperBound，
    // 故 LIMIT 不会拉爆内存。重启后计数仍在库里 —— 不需要任何内存态，也就没有「重启
    // 后额度复位」的回归面
    auto runs = store_.listRuns(task.id, *cfg.maxRuns);
    if (runs.isFail()) {
        // 读不出一律不派发：配置了上限的任务宁可本分钟不跑，也不能在不知额度的情况下超跑
        LOG_WARN("Scheduler: 任务 " + std::to_string(task.id) +
                 " 的运行次数读取失败，本次不派发：" + runs.errorMessage());
        return true;
    }
    if (static_cast<int>(runs.value().size()) < *cfg.maxRuns) return false;
    auto off = store_.setTaskEnabled(task.id, false);
    if (off.isFail()) {
        // 置停用失败：本路仍不派发（额度判定已经成立），下一分钟/下一个事件会重试写库
        LOG_WARN("Scheduler: 任务 " + std::to_string(task.id) +
                 " 已达运行次数上限，但置停用失败：" + off.errorMessage());
        return true;
    }
    LOG_INFO("Scheduler: 任务 " + std::to_string(task.id) + " 已达运行次数上限 " +
             std::to_string(*cfg.maxRuns) + "，自动停用（不再派发）");
    return true;
}

core::Result<void> Scheduler::drainEventQueue() {
    reapDone();
    std::deque<std::string> pending;
    {
        std::lock_guard<std::mutex> lock(mu_);
        pending.swap(eventQueue_);   // 取走即清：路由耗时不再堵 onEvent 入队
    }
    for (const auto& name : pending) processEvent(name);
    return core::Result<void>::ok();
}

void Scheduler::processEvent(const std::string& name) {
    auto tasks = store_.listTasks();
    if (tasks.isFail()) {
        LOG_WARN("Scheduler: 事件路由读任务列表失败: " + tasks.errorMessage());
        return;
    }
    const int64_t nowSec = now_ ? now_() : systemNowSec();
    for (const auto& t : tasks.value()) {
        if (!t.enabled || t.triggerType != "event") continue;
        // 坏配置不静默 continue：点名 task id 的 WARN 是用户唯一可见通道（同 tickNow 口径）
        auto cfg = decodeTriggerConfig(t.triggerConfig);
        if (cfg.isFail()) {
            LOG_WARN("Scheduler: 任务 " + std::to_string(t.id) +
                     " 的 trigger_config 无法解析，本事件跳过不派发：" +
                     cfg.errorMessage());
            continue;
        }
        bool match = false;
        for (const auto& ev : cfg.value().events) {
            if (ev == name) {
                match = true;
                break;
            }
        }
        // 额度判定在匹配之前：一次性事件任务跑满后，下一次事件到达（哪怕不匹配）即
        // 置停用——事件型没有周期扫描，这是唯一的扫描机会；它只改启停列不派发 run
        if (runBudgetExhausted(t, cfg.value())) continue;
        if (!match) continue;
        const int64_t cooldown =
            cfg.value().cooldownSec > 0 ? cfg.value().cooldownSec
                                       : kDefaultEventCooldownSec;
        {   // 任务×事件级冷却：同任务不同事件互不压制；窗口内同组合的事件直接丢弃——不排队、不延后（R-5）
            std::lock_guard<std::mutex> lock(mu_);
            const auto key = std::make_pair(t.id, name);
            auto it = lastEventFireSec_.find(key);
            if (it != lastEventFireSec_.end() && nowSec - it->second < cooldown)
                continue;
            lastEventFireSec_[key] = nowSec;   // 冷却起点 = 派发判定通过之时
        }
        auto fired = dispatchRun(t.id);
        if (fired.isFail())
            LOG_WARN("Scheduler: 事件派发失败 task=" + std::to_string(t.id) + ": " +
                     fired.errorMessage());
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
        // trigger_config 解析唯一落点 = trigger_config.h；坏配置不静默 continue——点名 task id 的 WARN 是用户唯一可见通道
        auto cfg = decodeTriggerConfig(t.triggerConfig);
        if (cfg.isFail()) {
            LOG_WARN("Scheduler: 任务 " + std::to_string(t.id) +
                     " 的 trigger_config 无法解析，本分钟跳过不派发：" +
                     cfg.errorMessage());
            continue;
        }
        auto expr = parseCron(cfg.value().cron);
        if (expr.isFail()) {
            // 保存期已校验；运行期坏 cron（旧数据/手工改库）同样不静默
            LOG_WARN("Scheduler: 任务 " + std::to_string(t.id) +
                     " 的 trigger_config 里的 cron 表达式非法，本分钟跳过不派发：" +
                     expr.errorMessage());
            continue;
        }
        // 额度判定在坏配置/坏 cron 两条点名 WARN **之后**（静默失败面不许被额度闸提前
        // continue 掉）、cronMatches **之前**：判定与本分钟是否匹配无关，每分钟扫描都
        // 重判，跑满即停用最迟下一分钟落地。相对同分钟防重在前的另一重意义——同分钟那次
        // tick 也会把满额任务置停用。
        if (runBudgetExhausted(t, cfg.value())) continue;
        if (!cronMatches(expr.value(), local)) continue;
        {   // 同分钟防重（lastFiredMinute）
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
    // 入表 + 线程创建同一把 mu_ 内：否则收尾窗口看到「表里有条目、thread 未赋值」的半成品；
    // run 线程体不取 mu_，持锁创建不会死锁
    std::lock_guard<std::mutex> lock(mu_);
    runs_.push_back(std::move(run));
    try {
        raw->thread = std::thread([this, raw, task] {
            try {
                auto r = loop_.run(task, raw->cancel);   // 单闸 + 落行都在 loop 内
                if (r.isFail()) {
                    // 撞闸（AlreadyExists）或起跑失败：本路从未落行，无悬挂 running 要收尾，只记 WARN
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
        // 线程创建失败：闸未调用（在 loop 内）→ 库中无行可收尾，只摘除占位
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
