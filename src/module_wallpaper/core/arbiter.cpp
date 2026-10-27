#include "module_wallpaper/core/arbiter.h"

namespace module::wallpaper {

namespace {
float sanitizePosition(float positionSec) {
    return positionSec > 0.0f ? positionSec : 0.0f;
}
} // namespace

bool HoldArbiter::held() const {
    return manualPause_ || systemHolds_ != 0;
}

bool HoldArbiter::setSystemReason(HoldReason reason, bool active, float positionSec) {
    if (reason == HoldReason::ManualPause) {
        return setManualPause(active, positionSec);
    }
    if (reason == HoldReason::Battery && !pauseOnBattery_ && active) {
        return false;  // 电池开关关闭：电池事件不入集合
    }
    const HoldReasonSet beforeSet = systemHolds_;
    const bool beforeHeld = held();
    setHoldReason(systemHolds_, reason, active);
    if (systemHolds_ == beforeSet) return false;  // 幂等
    applyTransition(beforeHeld, positionSec);
    return true;
}

bool HoldArbiter::setManualPause(bool active, float positionSec) {
    if (manualPause_ == active) return false;  // 幂等
    const bool beforeHeld = held();
    manualPause_ = active;
    applyTransition(beforeHeld, positionSec);
    return true;
}

void HoldArbiter::applyTransition(bool beforeHeld, float positionSec) {
    const bool afterHeld = held();
    if (!beforeHeld && afterHeld) {
        // 进入暂停：记录当前位置（仅此一次；暂停期间再叠加原因不覆盖）。
        hasAnchor_ = true;
        anchorSec_ = sanitizePosition(positionSec);
        return;
    }
    if (beforeHeld && !afterHeld) {
        // 完全解禁：把锚点转成一次性 seek 请求。
        if (hasAnchor_) {
            hasPendingSeek_ = true;
            pendingSeekSec_ = anchorSec_;
        }
        hasAnchor_ = false;
    }
}

void HoldArbiter::setPauseOnBattery(bool enabled) {
    if (pauseOnBattery_ == enabled) return;
    pauseOnBattery_ = enabled;
    if (enabled) return;  // 打开不主动置位——等平台层下一帧电池事件
    if (holdsReason(systemHolds_, HoldReason::Battery)) {
        const bool beforeHeld = held();
        setHoldReason(systemHolds_, HoldReason::Battery, false);
        applyTransition(beforeHeld, anchorSec_);
    }
}

bool HoldArbiter::takeResumeSeek(float* seconds) {
    if (!hasPendingSeek_) return false;
    hasPendingSeek_ = false;
    if (seconds != nullptr) *seconds = pendingSeekSec_;
    pendingSeekSec_ = 0.0f;
    return true;
}

void HoldArbiter::invalidateResumeAnchor() {
    hasAnchor_ = false;
    anchorSec_ = 0.0f;
    hasPendingSeek_ = false;
    pendingSeekSec_ = 0.0f;
}

} // namespace module::wallpaper