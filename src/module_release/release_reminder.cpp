// 提醒引擎（票据 09）实现：到期聚合 + 全屏提醒窗。
//
// 反重构警告：
//   - 到期判定只有 release_logic::dueItems 一个源，本文件不得自算或合并条目
//     （合并会改变「一条一处理入口」的契约，条目位置也无法回跳）。
//   - 用户关闭后的 5 分钟冷却与「同内容指纹不重复弹」是两条独立护栏：前者
//     用 suppressUntil，后者用 lastShownAt + lastShownFingerprint；删任一条
//     都会在窗销毁回调缺失/时钟回拨时退化成每秒重复开窗。
//   - 窗被打开期间只刷新内容、不重开；内容变空时自动关窗（全部做完就不弹）。
#include "release_reminder.h"
#include "release_reminder_logic.h"

#include "release_logic.h"
#include "release_store.h"

#include <core/component_theme.h>
#include <core/logging.h>
#include <core/platform/window_effect.h>
#include <core/theme.h>
#include <core/time.h>
#include <core/window/window_backend.h>

#include <components/scrollview.h>
#include <eui/dsl_app.h>
#include <eui/types.h>
#include <ui_kit/button.h>

#include <algorithm>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace module::release {

namespace {
// 「上线提醒弹窗」总开关（默认关）：进程级状态，tick 每次读——设置页经桥
// 改动后下一 tick 生效，无需实例指针；模块实例未构造时启动期写入也成立。
bool g_autoPopup = false;
}  // namespace

void setReminderAutoPopup(bool on) { g_autoPopup = on; }
bool reminderAutoPopup() { return g_autoPopup; }

namespace {

// 冷却时长 / 开窗宽限期两个常量已随决策逻辑迁至 release_reminder_logic.h
// （唯一一处定义），本文件直接引用。

// 全屏窗版面（逻辑像素）：标题区固定，列表区吃剩余高度。
constexpr float kOuterInsetWide = 48.0f;
constexpr float kOuterInsetNarrow = 24.0f;
constexpr float kHeaderTitleY = 20.0f;
constexpr float kHeaderSubtitleY = 56.0f;
constexpr float kListTop = 84.0f;
constexpr float kListBottomReserve = 34.0f;
constexpr float kCloseButtonW = 88.0f;
constexpr float kCloseButtonH = 36.0f;

// 任务卡版面（与 reminderCardHeight 必须同步；改一处必须改另一处）。
constexpr float kCardInset = 18.0f;
constexpr float kCardHeaderH = 30.0f;
constexpr float kSectionH = 30.0f;
constexpr float kItemRowH = 58.0f;
constexpr float kCardGap = 14.0f;
constexpr float kCardBottomPad = 14.0f;
constexpr float kGoButtonW = 88.0f;
constexpr float kGoButtonH = 34.0f;

int levelRank(RemindLevel level) {
    switch (level) {
        case RemindLevel::Red: return 2;
        case RemindLevel::Orange: return 1;
        case RemindLevel::Yellow: return 0;
    }
    return 0;
}

// 三档提醒色（票据指定色系）：红 #EF4444 / 橙 #F59E0B / 黄 #EAB308。
eui::Color levelAccent(RemindLevel level) {
    switch (level) {
        case RemindLevel::Red: return eui::Color::fromHex(0xEF4444u);
        case RemindLevel::Orange: return eui::Color::fromHex(0xF59E0Bu);
        case RemindLevel::Yellow: return eui::Color::fromHex(0xEAB308u);
    }
    return eui::Color::fromHex(0xEAB308u);
}

const char* levelHeadline(RemindLevel level) {
    switch (level) {
        case RemindLevel::Red: return "已到上线时间，仍有事项未完成";
        case RemindLevel::Orange: return "上线前 1 小时，前置项未完成";
        case RemindLevel::Yellow: return "上线公告尚未发布";
    }
    return "有待处理事项";
}

const char* levelChipText(RemindLevel level) {
    switch (level) {
        case RemindLevel::Red: return "立即处理";
        case RemindLevel::Orange: return "1 小时内";
        case RemindLevel::Yellow: return "公告未发";
    }
    return "待处理";
}

const char* taskStatusText(TaskStatus status) {
    switch (status) {
        case TaskStatus::Draft: return "配置中";
        case TaskStatus::Pending: return "待执行";
        case TaskStatus::Running: return "执行中";
        case TaskStatus::Done: return "已完成";
        case TaskStatus::Voided: return "已作废";
    }
    return "配置中";
}

std::string joinText(const std::vector<std::string>& parts, const char* separator) {
    std::string joined;
    for (const std::string& part : parts) {
        if (!joined.empty()) joined += separator;
        joined += part;
    }
    return joined;
}

// ---------- 调试/演示专用入口 ----------
//
// 生产路径不依赖以下任何函数：只有显式设置 DEVDESK_RELEASE_DEMO 才会走到。
// 演示模式只替换 tick 的数据来源（假数据代替真实到期聚合），开窗、冷却、
// 补弹、指纹去重等行为全部复用真实路径，因此不会让演示态与真实态产生
// 行为漂移；未设置该变量时默认逻辑一字不变。
// 演示开关的电平解析（readDemoLevelFromEnv）已随决策逻辑迁至
// release_reminder_logic，本文件只保留演示数据的构造。

// 演示任务的计划日取当天本地日期：窗标题里的「上线日」看起来像真实任务。
// 本地时间统一走 core::time::localTime（线程安全口径），不直用 std::localtime。
std::string demoPlanDate(std::int64_t nowUnix) {
    const std::time_t now = static_cast<std::time_t>(nowUnix);
    std::tm local{};
    core::time::localTime(now, &local);
    std::string month = std::to_string(local.tm_mon + 1);
    std::string day = std::to_string(local.tm_mday);
    if (month.size() < 2) month.insert(month.begin(), '0');
    if (day.size() < 2) day.insert(day.begin(), '0');
    return std::to_string(local.tm_year + 1900) + "-" + month + "-" + day;
}

} // namespace

// 视图类型（ReminderEnvSection / ReminderTaskGroup / ReminderView）已迁至
// release_reminder_logic.h（namespace module::release），本文件直接使用。

namespace {

// 聚合到期项。任一读库失败即整体放弃本 tick（部分数据会让「全做完」误判，
// 从而错误关窗）；错误由调用方节流记录。
bool collectReminderView(ReleaseStore& store, std::int64_t nowUnix,
                         ReminderView& view, std::string& error) {
    auto tasksResult = store.listTasks();
    if (tasksResult.isFail()) {
        error = "读取任务失败：" + tasksResult.errorMessage();
        return false;
    }

    for (const Task& task : tasksResult.value()) {
        if (task.status == TaskStatus::Done || task.status == TaskStatus::Voided) {
            continue;
        }

        // nowMinute 换算走 release_logic 单一口径（本地零点起算，DST 日不偏移）。
        std::int64_t minute = 0;
        if (!planDateElapsedMinutes(task.planDate, nowUnix, minute)) continue;
        const int nowMinute = static_cast<int>(std::clamp<std::int64_t>(
            minute, static_cast<std::int64_t>(std::numeric_limits<int>::min()),
            static_cast<std::int64_t>(std::numeric_limits<int>::max())));
        // 任何环境的最早基准都 >= 0，故 nowMinute < -60 时不可能有到期项。
        if (nowMinute < -60) continue;

        auto envsResult = store.listTaskEnvs(task.id);
        if (envsResult.isFail()) {
            error = "读取任务环境失败：" + envsResult.errorMessage();
            return false;
        }
        const std::vector<TaskEnv>& envs = envsResult.value();
        const std::int64_t baseline = taskBaseline(envs);
        if (baseline < 0) continue;
        // 配置中且未到基准：整单静默；未到「基准 − 1h」同样无事可提醒。
        if (task.status == TaskStatus::Draft &&
            static_cast<std::int64_t>(nowMinute) < baseline) {
            continue;
        }
        if (static_cast<std::int64_t>(nowMinute) < baseline - 60) continue;

        std::vector<TaskService> services;
        std::vector<TaskConfig> configs;
        std::vector<TaskStep> steps;

        // 步骤一次 SQL 拉全任务（经 release_task_service / release_task_env
        // 两级 JOIN），下面按 taskServiceId 分发到各服务——替代原「每服务
        // 一条 listTaskSteps」的 N+1 扫描（一个任务 8 个服务 = 8 条变 1 条）。
        auto stepsResult = store.listTaskStepsByTask(task.id);
        if (stepsResult.isFail()) {
            error = "读取任务步骤失败：" + stepsResult.errorMessage();
            return false;
        }
        const std::vector<TaskStep>& allSteps = stepsResult.value();

        for (const TaskEnv& env : envs) {
            if (env.removed) continue;
            auto servicesResult = store.listTaskServices(env.id);
            if (servicesResult.isFail()) {
                error = "读取任务服务失败：" + servicesResult.errorMessage();
                return false;
            }
            for (const TaskService& service : servicesResult.value()) {
                const bool removed = service.removed;
                services.push_back(service);
                if (removed) continue;  // 已移除服务不欠工单/部署
                auto configsResult = store.listTaskConfigs(service.id);
                if (configsResult.isFail()) {
                    error = "读取任务配置失败：" + configsResult.errorMessage();
                    return false;
                }
                for (const TaskConfig& config : configsResult.value()) {
                    configs.push_back(config);
                }
                for (const TaskStep& step : allSteps) {
                    if (step.taskServiceId == service.id) steps.push_back(step);
                }
            }
        }

        std::vector<DueItem> items = dueItems(task, envs, services, configs, steps, nowMinute);
        if (items.empty()) continue;

        ReminderTaskGroup group;
        group.taskId = task.id;
        group.planDate = task.planDate;
        group.status = task.status;
        group.level = highestLevel(items);
        for (const TaskEnv& env : envs) {
            if (!env.removed) group.envNames.push_back(env.envName);
        }
        for (const DueItem& item : items) {
            if (item.scope == "task") {
                group.taskItems.push_back(item);
                continue;
            }
            auto sectionIt = std::find_if(
                group.envSections.begin(), group.envSections.end(),
                [&item](const ReminderEnvSection& section) {
                    return section.envName == item.scope;
                });
            if (sectionIt == group.envSections.end()) {
                group.envSections.push_back(ReminderEnvSection{item.scope, {}});
                sectionIt = group.envSections.end() - 1;
            }
            sectionIt->items.push_back(item);
        }

        view.items.insert(view.items.end(), items.begin(), items.end());
        view.tasks.push_back(std::move(group));
    }

    std::stable_sort(view.tasks.begin(), view.tasks.end(),
                     [](const ReminderTaskGroup& left, const ReminderTaskGroup& right) {
                         const int leftRank = levelRank(left.level);
                         const int rightRank = levelRank(right.level);
                         if (leftRank != rightRank) return leftRank > rightRank;
                         if (left.planDate != right.planDate) {
                             return left.planDate < right.planDate;
                         }
                         return left.taskId < right.taskId;
                     });

    view.level = highestLevel(view.items);
    view.fingerprint = buildFingerprint(view);
    return true;
}

// 演示用假数据：任务级 / 环境级 / 服务级三层各至少一条，全部使用同一档位，
// 便于在没有真实任务数据时一眼核对配色与分区展示。
// 分组口径与真实数据完全一致（scope == "task" 进任务级区块，其余按环境名
// 分组），所以绘制、「去处理」回调与指纹去重走的都是真实路径。
ReminderView buildDemoView(RemindLevel level, std::int64_t nowUnix) {
    constexpr std::int64_t kDemoTaskId = 900001;

    ReminderTaskGroup group;
    group.taskId = kDemoTaskId;
    group.planDate = demoPlanDate(nowUnix);
    group.status = TaskStatus::Running;  // 展示「执行中」，更接近真实到期场景
    group.level = level;
    group.envNames = {"EU"};

    // 任务级：工单审批 + 公告。
    group.taskItems.push_back(
        {level, kDemoTaskId, "task", "EM · 数据库工单未审批",
         "任务内有数据库配置内容，需在上线前 60 分钟完成工单"});
    group.taskItems.push_back({level, kDemoTaskId, "task", "上线公告未发",
                               "时间基准已到，公告尚未发布"});

    // 环境级 + 服务级：服务条挂在对应环境分区下（与真实数据的 scope 口径一致）。
    ReminderEnvSection envSection;
    envSection.envName = "EU";
    envSection.items.push_back({level, kDemoTaskId, "EU",
                                "EU 已到上线时间，3/8 个服务未完成",
                                "环境「EU」开始时间已到，仍有服务未完成"});
    envSection.items.push_back({level, kDemoTaskId, "EU",
                                "rs-host-service-order · 数据库脚本未执行",
                                "服务级：rs-host-service-order 的数据库脚本尚未执行"});
    group.envSections.push_back(std::move(envSection));

    ReminderView view;
    for (const DueItem& item : group.taskItems) view.items.push_back(item);
    for (const ReminderEnvSection& section : group.envSections) {
        view.items.insert(view.items.end(), section.items.begin(),
                          section.items.end());
    }
    view.tasks.push_back(std::move(group));
    view.level = highestLevel(view.items);  // 档位全部来自演示开关，结果同 level
    view.fingerprint = buildFingerprint(view);
    return view;
}

} // namespace

namespace detail {

// 提醒窗共享态：compose 闭包只持有本结构，不持有引擎指针——引擎销毁后窗
// 仍能安全绘制到主循环完成销毁（onClosed 由引擎销毁前清空）。
struct ReminderWindowState {
    core::window::Handle handle = nullptr;
    bool closeRequested = false;
    std::uint64_t revision = 0;  // 内容版本（ScrollView 量高缓存 key）
    ReminderView view;
    ReleaseReminderAction onHandle;
    // 句柄确认回调（引擎侧置 windowOpen / 记录展示时刻）；每次开窗前设置，
    // shutdown 时清空，避免引擎析构后回调悬垂。
    std::function<void()> onCreated;
    std::function<void()> onClosed;
    // 开窗代数：每次发起开窗前自增；创建/销毁回调捕获发起时的代数，
    // 过期代数的迟到回调一律忽略——防止看门狗放弃旧请求重开后，
    // 旧请求的窗口被创建/销毁时搅乱当前窗的状态。
    std::uint64_t openGeneration = 0;
    // 置顶降层：开窗置顶 8 秒抢注意，之后或任意点击后让位回普通层。
    // 判定与切换都在 compose（主线程）——GLFW 窗口操作跨线程未定义。
    std::chrono::steady_clock::time_point openedAt{std::chrono::steady_clock::now()};
    bool topDemoted = false;

    // 幂等出口：8 秒超时与点击共用，topDemoted 单向闩防每帧重复设 attrib。
    void demoteTop() {
        if (topDemoted) return;
        topDemoted = true;
        if (handle != nullptr) {
            core::window::setWindowAlwaysOnTop(handle, false);
        }
    }

    // 先请求关闭再让调用方决定后续（handle 未创建时由 onWindowCreated 补发）。
    void requestClose() {
        closeRequested = true;
        if (handle != nullptr) {
            core::window::requestWindowClose(handle);
        }
    }
};

} // namespace detail

namespace {

void composeReminderWindow(
    eui::Ui& ui, const eui::Screen& screen,
    const std::shared_ptr<detail::ReminderWindowState>& state);

} // namespace

using detail::ReminderWindowState;

// ReleaseReminder::Impl 必须在命名空间作用域定义（嵌套类的类外定义不能落在
// 匿名命名空间里）。
struct ReleaseReminder::Impl {
    Impl(ReleaseStore& storeValue, ReleaseReminderAction callback)
        : store(storeValue), onHandle(std::move(callback)) {
        // 演示开关在构造时读一次后固定，运行期（每秒 tick）不再查环境变量。
        demoMode = readDemoLevelFromEnv(demoLevel);
        if (demoMode) {
            LOG_INFO("Release 提醒：演示模式已启用（DEVDESK_RELEASE_DEMO），"
                     "使用内置假数据，不读取真实任务");
        }
        window = std::make_shared<ReminderWindowState>();
        window->onHandle = onHandle;
        window->onClosed = [this] { handleWindowClosed(); };
    }

    void tick(std::int64_t nowUnix);
    void shutdown();
    void openReminder(const ReminderView& view, std::int64_t nowUnix);
    void closeForEmpty();
    void handleWindowClosed();
    void noteStoreError(const std::string& message);

    ReleaseStore& store;
    ReleaseReminderAction onHandle;
    std::shared_ptr<ReminderWindowState> window;
    bool demoMode = false;  // 调试/演示入口；生产默认关闭
    RemindLevel demoLevel = RemindLevel::Yellow;
    bool running = true;
    // 窗开着的唯一真相是「句柄已确认」：windowOpen 只在 onWindowCreated
    // 回调里置 true，销毁回调 / shutdown 里同步清——开窗请求发出到句柄
    // 确认之间不算开着，也不允许重复发请求。
    bool windowOpen = false;
    bool autoClosing = false;
    bool storeErrorLogged = false;
    std::int64_t currentNow = 0;
    std::int64_t lastScanUnix = 0;
    std::int64_t lastShownAt = 0;
    std::int64_t suppressUntil = 0;
    std::string lastShownFingerprint;
    // 开窗请求发出的时刻（0 = 无在飞请求）；供 tick 对账：超过宽限期仍无
    // 句柄即判定创建失败，清态允许重试（防 fail-dead）。
    std::int64_t openRequestedAt = 0;
    // 句柄确认（windowOpen 置 true）的时刻；供 tick 对账「开窗状态与句柄
    // 失配」的看门狗。
    std::int64_t openConfirmedAt = 0;
    // 弹窗开关关闭路径的上次日志时刻（60 秒节流；0 = 从未记过，下个 tick 即记）。
    std::int64_t gateLogAt = 0;
};

void ReleaseReminder::Impl::tick(std::int64_t nowUnix) {
    if (!running || nowUnix <= 0) return;
    currentNow = nowUnix;
    if (nowUnix == lastScanUnix) return;  // 同一秒重复调用不重复扫描
    lastScanUnix = nowUnix;

    ReminderView view;
    if (demoMode) {
        // 只替换数据来源：下面所有开窗 / 冷却 / 补弹 / 去重逻辑与真实路径共用。
        view = buildDemoView(demoLevel, nowUnix);
    } else {
        std::string error;
        if (!collectReminderView(store, nowUnix, view, error)) {
            noteStoreError(error);
            return;  // 保持现窗内容，不在读库失败时误判「全部完成」
        }
        if (storeErrorLogged) {
            LOG_INFO("Release 提醒：数据读取恢复");
            storeErrorLogged = false;
        }
    }

    if (!hasDueItems(view.items)) {
        closeForEmpty();  // 全部做完就不弹：已开的窗也要收掉
        return;
    }

    // 对账看门狗：已开窗但句柄为空（销毁回调缺失等异常路径）超过宽限期，
    // 视为窗已丢——清态并允许重新弹窗，而不是提醒静默失效。
    if (windowOpen && (window == nullptr || window->handle == nullptr)) {
        if (watchdogLostHandleGraceElapsed(currentNow, openConfirmedAt)) {
            LOG_WARN("Release 提醒：提醒窗句柄丢失（开窗状态与句柄失配超过 "
                     + std::to_string(kWindowOpenGraceSeconds) +
                     " 秒），重置开窗状态并允许重试");
            windowOpen = false;
            openConfirmedAt = 0;
            // 窗已丢，用户没看到内容：清指纹护栏，允许立即重新弹出。
            lastShownFingerprint.clear();
        } else {
            return;  // 宽限期内保持现状，等句柄确认/销毁回调
        }
    }

    if (windowOpen) {
        if (window != nullptr && window->view.fingerprint != view.fingerprint) {
            window->view = std::move(view);
            ++window->revision;
            app::requestUpdate();
        }
        return;
    }

    // 对账看门狗：开窗请求在飞但句柄迟迟未确认（EUI-NEO 创建失败时
    // 创建/销毁回调都不触发）——超过宽限期判定失败，清态允许下一轮重开，
    // 避免提醒永久哑火。
    if (openRequestedAt != 0) {
        if (openRequestGraceElapsed(currentNow, openRequestedAt)) {
            LOG_WARN("Release 提醒：开窗请求超过 "
                     + std::to_string(kWindowOpenGraceSeconds) +
                     " 秒未确认窗口句柄，判定创建失败并允许重试");
            openRequestedAt = 0;
        } else {
            return;  // 在飞：不重复发请求
        }
    }

    // 「上线提醒弹窗」总开关（默认关）：关 = 引擎照常扫描，到期项只记日志
    // 不开窗（60 秒节流，tick 每秒跑不能刷屏）。已开的窗不强制收起。
    if (!reminderAutoPopup()) {
        if (gateLogAt == 0 || currentNow - gateLogAt >= 60) {
            gateLogAt = currentNow;
            LOG_INFO("Release 提醒：弹窗开关关闭，跳过开窗（到期项 " +
                     std::to_string(view.items.size()) + " 条）");
        }
        return;
    }

    // 补弹/重弹最终判据（总开关门 + 冷却门 + 同指纹重弹窗护栏）已抽到
    // release_reminder_logic::shouldOpenReminder，本文件只执行其结论。
    if (!shouldOpenReminder(currentNow, suppressUntil, lastShownAt,
                            lastShownFingerprint, view.fingerprint,
                            /*popupEnabled=*/true)) {
        return;
    }

    openReminder(view, nowUnix);
}

void ReleaseReminder::Impl::shutdown() {
    running = false;
    if (window != nullptr) {
        // 先断回调再关窗：窗的销毁在主循环下一帧，引擎可能已先析构。
        window->onClosed = nullptr;
        window->onHandle = nullptr;
        window->onCreated = nullptr;
        window->requestClose();
        window.reset();
    }
    windowOpen = false;
    openRequestedAt = 0;
    openConfirmedAt = 0;
    autoClosing = false;
}

void ReleaseReminder::Impl::openReminder(const ReminderView& view,
                                         std::int64_t nowUnix) {
    if (window == nullptr) return;
    window->view = view;
    ++window->revision;
    window->closeRequested = false;
    window->onHandle = onHandle;

    // 只登记「请求在飞」：windowOpen 与 lastShown* 必须等 onWindowCreated
    // 确认句柄后才置位（句柄确认 = 窗真的开起来了）。创建失败时创建/销毁
    // 回调都不触发，提前置位会让引擎永远认为窗开着（fail-dead）。
    openRequestedAt = nowUnix;
    autoClosing = false;

    // 真正弹出才记展示时刻/指纹：失败的尝试不占冷却，也不该被指纹护栏
    // 挡住重试。
    window->onCreated = [this] {
        windowOpen = true;
        openConfirmedAt = currentNow;
        openRequestedAt = 0;
        lastShownAt = currentNow;
        lastShownFingerprint = window->view.fingerprint;
    };
    const std::uint64_t generation = ++window->openGeneration;

    int workX = 0;
    int workY = 0;
    int workW = 0;
    int workH = 0;
    core::window::getPrimaryMonitorWorkArea(workX, workY, workW, workH);
    if (workW <= 0 || workH <= 0) {
        // 无头/未初始化时给一个安全尺寸；真实运行时主编辑器必有工作区。
        workX = 0;
        workY = 0;
        workW = 1280;
        workH = 800;
    }

    const core::theme::Tokens& tokens = core::theme::current();
    const eui::Color accent = levelAccent(view.level);
    const eui::Color background = eui::mixColor(
        tokens.colors.background, accent, tokens.isDark ? 0.20f : 0.09f);

    // 窗标题带上下文（环境/任务）：单任务给日期 + 环境名，多任务给任务数。
    std::string title = "上线提醒";
    if (view.tasks.size() == 1) {
        const ReminderTaskGroup& only = view.tasks.front();
        title += " · " + only.planDate;
        const std::string envs = joinText(only.envNames, " / ");
        if (!envs.empty()) title += " · " + envs;
    } else {
        title += " · " + std::to_string(view.tasks.size()) + " 个任务";
    }

    std::shared_ptr<ReminderWindowState> state = window;
    app::openWindow(
        app::DslWindowConfig{}
            .title(title)
            .pageId("release.reminder")
            .windowSize(workW, workH)
            .windowPosition(workX, workY)
            .decorated(false)
            .alwaysOnTop(true)
            .resizable(false)
            .clearColor(background)
            .ignoreClearColorOverride()
            .windowEffect(core::platform::WindowEffect::None)
            .onWindowCreated([state, generation](core::window::Handle handle) {
                if (isStaleGeneration(generation, state->openGeneration)) {
                    // 过期代数的迟到窗口（看门狗已放弃该请求并重开）：
                    // 立即关掉，不碰当前窗状态。
                    core::window::requestWindowClose(handle);
                    return;
                }
                state->handle = handle;
                state->openedAt = std::chrono::steady_clock::now();
                state->topDemoted = false;
                if (state->onCreated) {
                    state->onCreated();
                    state->onCreated = nullptr;
                }
                if (state->closeRequested) {
                    core::window::requestWindowClose(handle);
                }
            })
            .onWindowDestroyed([state, generation](core::window::Handle handle) {
                if (isStaleGeneration(generation, state->openGeneration)) return;
                if (state->handle == handle) state->handle = nullptr;
                if (state->onClosed) state->onClosed();
            })
            .onKeyEvent([state](const eui::KeyEvent& key) {
                if (key.isDown() && key.key == eui::InputKey::Escape) {
                    state->requestClose();
                }
            }),
        [state](eui::Ui& ui, const eui::Screen& screen) {
            composeReminderWindow(ui, screen, state);
        });
}

void ReleaseReminder::Impl::closeForEmpty() {
    if (window == nullptr) return;
    // 已有一次关闭在飞（用户关 / 本函数上轮触发）→ 不覆盖其语义：
    // 用户关必须保持 5 分钟冷却，不能被「内容变空」误标成自动关。
    if (window->closeRequested) return;
    if (!windowOpen) {
        // 开窗请求在飞（句柄未确认）：标记关闭，onWindowCreated 拿到句柄
        // 后补发关闭；创建失败则由看门狗清态，不影响后续弹窗。
        if (openRequestedAt == 0) return;
        autoClosing = true;
        window->closeRequested = true;
        return;
    }
    // 空集的关窗不是「用户关闭」：不进入 5 分钟冷却（窗尚未创建的在飞
    // 情形已在上方分支标记 closeRequested，onWindowCreated 时会补发关闭）。
    autoClosing = true;
    window->requestClose();
    // windowOpen 保持 true 直到销毁回调，避免同一秒内重复排队开窗。
}

void ReleaseReminder::Impl::handleWindowClosed() {
    windowOpen = false;
    openRequestedAt = 0;
    openConfirmedAt = 0;
    if (!running) return;
    if (!autoClosing) {
        suppressUntil = computeSuppressUntil(currentNow, suppressUntil);
    }
    autoClosing = false;
}

void ReleaseReminder::Impl::noteStoreError(const std::string& message) {
    if (storeErrorLogged) return;  // 每秒 tick 下只记首次，恢复时再记一条
    storeErrorLogged = true;
    LOG_WARN("Release 提醒： " + message + "（本 tick 跳过，后续重试）");
}

namespace {

// ---------- 全屏提醒窗绘制 ----------

float reminderCardHeight(const ReminderTaskGroup& group) {
    float height = kCardInset + kCardHeaderH;
    if (!group.taskItems.empty()) {
        height += kSectionH + kItemRowH * static_cast<float>(group.taskItems.size());
    }
    for (const ReminderEnvSection& section : group.envSections) {
        height += kSectionH + kItemRowH * static_cast<float>(section.items.size());
    }
    return height + kCardBottomPad;
}

void drawSectionLabel(eui::Ui& ui, const std::string& id, float x, float y,
                      float width, const std::string& text,
                      const core::theme::Tokens& tokens) {
    const float labelWidth = std::max(40.0f, width - kCardInset * 2.0f);
    ui.text(id)
        .position(x + kCardInset, y + 3.0f)
        .size(labelWidth, 20.0f)
        .text(text)
        .fontSize(tokens.sizes.fontSizeSmall)
        .fontWeight(600)
        .color(tokens.colors.textTertiary)
        .wrap(false)
        .maxWidth(labelWidth)
        .overflow(core::TextOverflow::Ellipsis)
        .build();
}

void drawDueItemRow(eui::Ui& ui, const std::string& id, float x, float y,
                    float width, const DueItem& item,
                    const std::shared_ptr<ReminderWindowState>& state,
                    const core::theme::Tokens& tokens) {
    const eui::Color accent = levelAccent(item.level);
    const float markX = x + kCardInset;
    ui.rect(id + ".mark")
        .position(markX, y + 13.0f)
        .size(3.0f, kItemRowH - 26.0f)
        .color(accent)
        .radius(1.5f)
        .build();

    const float textX = markX + 14.0f;
    const float textWidth = std::max(
        60.0f, width - kCardInset * 2.0f - 14.0f - kGoButtonW - 16.0f);
    ui.text(id + ".title")
        .position(textX, y + 8.0f)
        .size(textWidth, 22.0f)
        .text(item.title)
        .fontSize(tokens.sizes.fontSizeBody)
        .fontWeight(600)
        .color(tokens.colors.textPrimary)
        .wrap(false)
        .maxWidth(textWidth)
        .overflow(core::TextOverflow::Ellipsis)
        .build();
    if (!item.detail.empty()) {
        ui.text(id + ".detail")
            .position(textX, y + 30.0f)
            .size(textWidth, 18.0f)
            .text(item.detail)
            .fontSize(tokens.sizes.fontSizeSmall)
            .color(tokens.colors.textSecondary)
            .wrap(false)
            .maxWidth(textWidth)
            .overflow(core::TextOverflow::Ellipsis)
            .build();
    }

    // 「去处理」只跳转：先请求关窗，再回调调用方注入的导航入口。
    const std::int64_t taskId = item.taskId;
    const std::string scope = item.scope;
    ui_kit::ButtonOptions options;
    options.onClick = [state, taskId, scope]() {
        const ReleaseReminderAction action = state->onHandle;
        state->requestClose();
        if (action) action(taskId, scope);
    };
    ui_kit::button(ui, id + ".go", "去处理", ui_kit::ButtonLevel::Secondary,
                   x + width - kCardInset - kGoButtonW, y + 12.0f,
                   kGoButtonW, kGoButtonH, tokens, options);
}

// 卡片内容按「容器内相对坐标」绘制（x/y 仅为内部偏移基准，调用方传 0）。
void drawTaskCardBody(eui::Ui& ui, const std::string& prefix, float x, float y,
                      float width, const ReminderTaskGroup& group,
                      const std::shared_ptr<ReminderWindowState>& state,
                      const core::theme::Tokens& tokens) {
    const float height = reminderCardHeight(group);
    ui.rect(prefix + ".bg")
        .position(x, y)
        .size(width, height)
        .color(tokens.colors.surfaceElevated)
        .radius(tokens.sizes.radiusLarge)
        .border(1.0f, tokens.colors.borderMuted)
        .build();

    const eui::Color accent = levelAccent(group.level);
    const float chipW = 88.0f;
    const float headWidth =
        std::max(80.0f, width - kCardInset * 2.0f - chipW - 12.0f);
    std::string head = "上线日 " + group.planDate + "  ·  " +
                       taskStatusText(group.status);
    const std::string envs = joinText(group.envNames, " / ");
    if (!envs.empty()) head += "  ·  " + envs;
    ui.text(prefix + ".head")
        .position(x + kCardInset, y + kCardInset + 4.0f)
        .size(headWidth, 22.0f)
        .text(head)
        .fontSize(tokens.sizes.fontSizeBody)
        .fontWeight(600)
        .color(tokens.colors.textPrimary)
        .wrap(false)
        .maxWidth(headWidth)
        .overflow(core::TextOverflow::Ellipsis)
        .build();

    const float chipX = x + width - kCardInset - chipW;
    ui.rect(prefix + ".chip.bg")
        .position(chipX, y + kCardInset + 2.0f)
        .size(chipW, 22.0f)
        .color(eui::mixColor(tokens.colors.surfaceElevated, accent, 0.22f))
        .radius(tokens.sizes.radiusPill)
        .build();
    ui.text(prefix + ".chip.text")
        .position(chipX, y + kCardInset + 2.0f)
        .size(chipW, 22.0f)
        .text(levelChipText(group.level))
        .fontSize(tokens.sizes.fontSizeSmall - 1.0f)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(tokens.colors.textPrimary)
        .build();

    float rowY = y + kCardInset + kCardHeaderH;
    if (!group.taskItems.empty()) {
        drawSectionLabel(ui, prefix + ".section.task", x, rowY, width,
                         "任务级前置 / 公告", tokens);
        rowY += kSectionH;
        for (std::size_t i = 0; i < group.taskItems.size(); ++i) {
            drawDueItemRow(ui, prefix + ".row.task." + std::to_string(i), x, rowY,
                           width, group.taskItems[i], state, tokens);
            rowY += kItemRowH;
        }
    }

    for (std::size_t sectionIndex = 0; sectionIndex < group.envSections.size();
         ++sectionIndex) {
        const ReminderEnvSection& section = group.envSections[sectionIndex];
        const std::string sectionPrefix =
            prefix + ".section.env." + std::to_string(sectionIndex);
        drawSectionLabel(ui, sectionPrefix, x, rowY, width,
                         "环境 · " + section.envName, tokens);
        rowY += kSectionH;
        for (std::size_t i = 0; i < section.items.size(); ++i) {
            drawDueItemRow(ui,
                           prefix + ".row.env." + std::to_string(sectionIndex) +
                               "." + std::to_string(i),
                           x, rowY, width, section.items[i], state, tokens);
            rowY += kItemRowH;
        }
    }
}

void drawTaskCard(eui::Ui& ui, const std::string& prefix, float x, float y,
                  float width, const ReminderTaskGroup& group,
                  const std::shared_ptr<ReminderWindowState>& state,
                  const core::theme::Tokens& tokens) {
    // 反重构警告：ScrollView 的 content 是一个流式 column——直接挂在 column 上的
    // 子元素会按出现顺序纵向排布（position 对它们无效）。因此整张卡片必须装进
    // 一个 ignoreLayout 的 stack 作绝对定位容器；否则所有元素都会被排到撑高
    // spacer 之下、落到视口外，表现为「窗里只有标题、没有任何条目」。
    // 该写法与 release_list_ui 的卡片、todo_ui_board 的卡片列一致，不要改回
    // 「直接在 content 里 drawTaskCardBody」。
    ui.stack(prefix)
        .position(x, y)
        .ignoreLayout()
        .size(width, reminderCardHeight(group))
        .content([&] {
            drawTaskCardBody(ui, prefix, 0.0f, 0.0f, width, group, state, tokens);
        })
        .build();
}

void composeReminderWindow(eui::Ui& ui, const eui::Screen& screen,
                           const std::shared_ptr<ReminderWindowState>& state) {
    if (std::chrono::steady_clock::now() - state->openedAt > std::chrono::seconds(8)) {
        state->demoteTop();
    }
    const core::theme::Tokens& tokens = core::theme::current();
    const ReminderView& view = state->view;
    const eui::Color accent = levelAccent(view.level);

    const float inset =
        screen.width >= 900.0f ? kOuterInsetWide : kOuterInsetNarrow;
    ui.rect("release.reminder.bg")
        .position(0.0f, 0.0f)
        .size(screen.width, screen.height)
        .color(eui::mixColor(tokens.colors.background, accent,
                             tokens.isDark ? 0.20f : 0.09f))
        .onClick([state] { state->demoteTop(); })
        .build();
    ui.rect("release.reminder.accent")
        .position(0.0f, 0.0f)
        .size(screen.width, 6.0f)
        .color(accent)
        .build();

    const float titleWidth =
        std::max(120.0f, screen.width - inset * 2.0f - kCloseButtonW - 20.0f);
    ui.text("release.reminder.title")
        .position(inset, kHeaderTitleY)
        .size(titleWidth, 40.0f)
        .text("上线提醒")
        .fontSize(tokens.sizes.fontSizeTitle)
        .fontWeight(700)
        .color(tokens.colors.textPrimary)
        .build();

    const std::string subtitle =
        std::string(levelHeadline(view.level)) + " · " +
        std::to_string(view.items.size()) + " 项 · " +
        std::to_string(view.tasks.size()) + " 个任务";
    ui.text("release.reminder.subtitle")
        .position(inset, kHeaderSubtitleY)
        .size(titleWidth, 22.0f)
        .text(subtitle)
        .fontSize(tokens.sizes.fontSizeBody)
        .color(tokens.colors.textSecondary)
        .wrap(false)
        .maxWidth(titleWidth)
        .overflow(core::TextOverflow::Ellipsis)
        .build();

    ui_kit::ButtonOptions closeOptions;
    closeOptions.onClick = [state]() { state->requestClose(); };
    ui_kit::button(ui, "release.reminder.close", "关闭",
                   ui_kit::ButtonLevel::Secondary,
                   screen.width - inset - kCloseButtonW, 30.0f,
                   kCloseButtonW, kCloseButtonH, tokens, closeOptions);

    const float listWidth = std::max(200.0f, screen.width - inset * 2.0f);
    const float listHeight =
        std::max(80.0f, screen.height - kListTop - kListBottomReserve);
    components::scrollView(ui, "release.reminder.scroll")
        .position(inset, kListTop)
        .size(listWidth, listHeight)
        .step(64.0f)
        .theme(core::theme::componentTokensWeChat(tokens))
        .scrollbarOverlay()
        .contentKey("release.reminder." + std::to_string(state->revision))
        .content([state, &tokens](eui::Ui& inner, float contentWidth,
                                  float viewportHeight) {
            (void)viewportHeight;
            const ReminderView& current = state->view;
            float totalHeight = 0.0f;
            for (const ReminderTaskGroup& group : current.tasks) {
                totalHeight += reminderCardHeight(group);
            }
            if (!current.tasks.empty()) {
                totalHeight += kCardGap *
                               static_cast<float>(current.tasks.size() - 1);
            }
            totalHeight = std::max(1.0f, totalHeight);
            inner.stack("release.reminder.spacer")
                .size(contentWidth, totalHeight)
                .build();

            float cardY = 0.0f;
            for (const ReminderTaskGroup& group : current.tasks) {
                drawTaskCard(inner,
                             "release.reminder.task." +
                                 std::to_string(group.taskId),
                             0.0f, cardY, contentWidth, group, state, tokens);
                cardY += reminderCardHeight(group) + kCardGap;
            }
        })
        .build();

    ui.text("release.reminder.hint")
        .position(inset, std::max(0.0f, screen.height - 26.0f))
        .size(listWidth, 18.0f)
        .text("Esc 或右上角关闭；全部处理完成后本窗自动关闭")
        .fontSize(tokens.sizes.fontSizeSmall)
        .color(tokens.colors.textTertiary)
        .build();
}

} // namespace

ReleaseReminder::ReleaseReminder(ReleaseStore& store,
                                 ReleaseReminderAction onHandle)
    : impl_(std::make_unique<Impl>(store, std::move(onHandle))) {}

ReleaseReminder::~ReleaseReminder() {
    if (impl_ != nullptr) impl_->shutdown();
}

void ReleaseReminder::tick(std::int64_t nowUnix) { impl_->tick(nowUnix); }

void ReleaseReminder::shutdown() { impl_->shutdown(); }

} // namespace module::release