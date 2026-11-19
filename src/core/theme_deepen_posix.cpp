// need356 平台拆分的 posix 侧：mac/Linux 颜色不加深（老板令仅 Windows 生效）。
// 接口在 theme.h；Windows 档见 theme_deepen_win.cpp。
#include "theme.h"

namespace core::theme {

float colorDeepenFactor() {
    return 1.0f;  // 恒等 = 不加深
}

} // namespace core::theme
