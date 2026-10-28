// 提醒引擎（票据 09）：到期聚合调度 + 独立全屏提醒窗。
//
// 契约：
//   - 只在主线程使用（帧泵 / 模块主循环），内部不启动线程、不持有全局状态。
//   - tick(nowUnix) 约每秒调用一次：把「已到期但未完成」的项汇总成一个窗；
//     窗被用户关闭后 5 分钟内不重复弹（同内容指纹亦不重复），应用重启后首
//     tick 即补弹（错过不吞）。
//   - 「去处理」只关闭本窗并回调调用方注入的 action，引擎内不做任何导航。
//   - 不抛异常：store 失败走 Result，失败时本 tick 不更新提醒（保持现窗）。
//   - 调试/演示开关（生产默认关闭）：环境变量 DEVDESK_RELEASE_DEMO 取 red /
//     orange / yellow 时，改用内置假数据开窗。它只替换「数据来源」，开窗、
//     冷却、补弹、指纹去重等逻辑与真实路径完全一致；未设置或取值非法时
//     不读取、也不依赖该变量，默认行为一字不变。
#pragma once

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>

namespace module::release {

class ReleaseStore;

// 去处理回调：taskId = 任务 id；scope = "task"（任务级前置/公告）或环境名。
// 回调发生时提醒窗已请求关闭（销毁由主循环下一帧完成）；导航由调用方完成。
using ReleaseReminderAction = std::function<void(std::int64_t taskId,
                                                 const std::string& scope)>;

class ReleaseReminder {
public:
    ReleaseReminder(ReleaseStore& store, ReleaseReminderAction onHandle);
    ~ReleaseReminder();

    ReleaseReminder(const ReleaseReminder&) = delete;
    ReleaseReminder& operator=(const ReleaseReminder&) = delete;

    // nowUnix = 当前 Unix 秒（本地时区）。同一秒内重复调用只扫描一次；
    // 窗已打开时只刷新内容，不会重复开窗。
    void tick(std::int64_t nowUnix);

    // 停止引擎并关闭提醒窗（应用退出 / 模块卸载）。之后 tick 为无操作。
    void shutdown();

    // 演示模式查询：DEVDESK_RELEASE_DEMO 取 red / orange / yellow 时为 true，
    // 未设置或取值非法时为 false。不依赖引擎实例（onStart 阶段即可判定），
    // 取值口径必须与引擎构造时的解析保持一致；仅用于调试/演示路径，生产
    // 环境恒为 false，调用方不得据此改变默认行为。
    static bool demoModeActive() {
        const char* raw = std::getenv("DEVDESK_RELEASE_DEMO");
        if (raw == nullptr) return false;
        const std::string value(raw);
        return value == "red" || value == "orange" || value == "yellow";
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace module::release