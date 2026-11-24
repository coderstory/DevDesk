// src/platform/reduced_motion_win.cpp
// Windows 实现：SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION) ——「动画效果」
// 关（设置 > 辅助功能 > 视觉效果）= 用户要求减少动态效果。查询失败按未开启处理
//（fail-open = 不减动效，见 reduced_motion.h）。
#include "reduced_motion.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace platform::reduced_motion {

bool systemReducedMotion() {
    BOOL enabled = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0,
                               static_cast<void*>(&enabled), 0)) {
        return false;
    }
    return enabled == FALSE;  // FALSE = 用户关了「动画效果」= 减动效
}

}  // namespace platform::reduced_motion
