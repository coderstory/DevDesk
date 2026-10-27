// src/module_wallpaper/core/arbiter.h
// 让路仲裁：系统原因集合 + 手动暂停 → 单一 shouldPlay 决策。
//
// 契约：
//   - 系统原因和手动暂停都参与「是否暂停」；但 systemHolds() 只含系统位，
//     手动暂停永远不出现在快照原因里（UI 不显示手动暂停原因）。
//   - 锚点只在「未暂停 → 暂停」那一刻写入；「暂停 → 未暂停」时产生一次 seek 请求，
//     由服务取出后交给播放端，避免解禁时从头播。
//   - set 幂等：重复置位/解除不重复写锚点、不重复产生 seek。
//   - pauseOnBattery 关闭时电池原因被忽略；关闭瞬间已存在的电池位会被移除。
// 线程约束：不持锁、不启线程；只在 WallpaperService 的 mu_ 内调用。
#pragma once

#include "module_wallpaper/core/model.h"

namespace module::wallpaper {

class HoldArbiter {
public:
    // 置位/解除一个系统原因。positionSec 只在「未暂停 → 暂停」那一刻用于写锚点。
    // 返回 true = 状态确实发生变化。reason == ManualPause 时转交 setManualPause。
    bool setSystemReason(HoldReason reason, bool active, float positionSec);

    // 手动暂停与系统原因分开保存。positionSec 语义同上。
    bool setManualPause(bool active, float positionSec);

    // 电池让路门控。关闭时移除已存在的电池位（可能因此解除暂停）。
    void setPauseOnBattery(bool enabled);
    bool pauseOnBattery() const { return pauseOnBattery_; }

    // 取出并清空待执行的恢复 seek。返回 false = 本次解禁无需 seek。
    bool takeResumeSeek(float* seconds);

    // 换片后旧锚点失效（新的播放位置与旧锚点无关）。
    void invalidateResumeAnchor();

    bool held() const;  // 手动暂停或任一系统原因
    bool shouldPlay() const { return !held(); }
    bool manuallyPaused() const { return manualPause_; }
    HoldReasonSet systemHolds() const { return systemHolds_; }
    bool hasAnchor() const { return hasAnchor_; }
    float anchorSec() const { return anchorSec_; }

private:
    // 状态变化后的公共收尾：锚点/seek 的 ∅↔非∅ 逻辑只在这里实现一次。
    void applyTransition(bool beforeHeld, float positionSec);

    bool manualPause_ = false;
    HoldReasonSet systemHolds_ = 0;
    bool pauseOnBattery_ = false;
    bool hasAnchor_ = false;
    float anchorSec_ = 0.0f;
    bool hasPendingSeek_ = false;
    float pendingSeekSec_ = 0.0f;
};

} // namespace module::wallpaper