// src/module_wallpaper/platform/yield_watchers.h
// 系统让路事件源接口（平台无关）。Windows 实现 = 隐藏消息窗收 WTS 会话通知
//（锁屏/解锁）与 WM_DISPLAYCHANGE（换屏）+ GetSystemPowerStatus / EnumWindows 轮询；
// 其它平台返回 stub（start 失败并给出中文原因，事件通道永不回调、轮询恒 false）。
//
// 线程契约：
//   - 事件回调在 watcher 内部线程触发；实现方负责回调注册的并发安全，回调体必须
//     快速返回（不得在回调里做耗时工作——服务实现只是转交 arbiter）。
//   - start/stop 幂等；stop 后不再有任何回调。stop 可与回调并发：实现方自行串行化。
//   - 轮询方法允许任意线程任意频率调用，必须快速返回（不阻塞、不取锁的长临界区）。
// 注入缝：测试以本接口的假实现驱动服务侧裁决路径（见 wallpaper_service 的
// setYieldWatchersForTest），生产装配用 createYieldWatchers()。
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "module_wallpaper/core/model.h"

namespace module::wallpaper {

class YieldWatchers {
public:
    virtual ~YieldWatchers() = default;

    // 系统原因事件（Windows：WM_WTSSESSION_CHANGE 锁屏/解锁）。
    using HoldCallback = std::function<void(HoldReason reason, bool active)>;
    // 换屏广播（Windows：WM_DISPLAYCHANGE）。服务收到后重建 WorkerW 宿主。
    using DisplayChangedCallback = std::function<void()>;

    virtual void setHoldCallback(HoldCallback callback) = 0;
    virtual void setDisplayChangedCallback(DisplayChangedCallback callback) = 0;

    // 失败填中文 error（可直接展示）。启动失败不致命：只失去自动让路，播放主链路不受影响。
    virtual bool start(std::string* error) = 0;
    virtual void stop() = 0;

    // 轮询通道（tickLoop 周期调用）。查询失败/状态未知一律 false（宁可不让路，
    // 不得因接口故障把壁纸错误暂停）。
    //   onBattery：电池供电 true；AC / 未知 false。
    //   fullscreenOtherProcess：存在可见、非壁纸自身、rect 恰好覆盖整个显示器的
    //   顶层窗时 true（壁纸宿主窗口按类名排除；不按进程排除——同进程的全屏测试
    //   窗必须能触发让路，宿主层是 WS_CHILD 本就不会被枚举）。
    virtual bool pollOnBattery() = 0;
    virtual bool pollFullscreenOtherProcess() = 0;
};

// Windows 返回真实实现；其它平台返回 stub。
std::unique_ptr<YieldWatchers> createYieldWatchers();

} // namespace module::wallpaper
