// need534 平台拆分的 posix 侧（mac/Linux）：**取值与批次 D 改动前逐字相同**。
// 老板 2026-11-27 硬约束：mac 布局零变化（改动前后截图应像素级一致）。
// Windows 紧凑档见 density_win.cpp。接口声明在 density.h。
#include "density.h"

#include <core/theme.h>

namespace ui_kit::density {

// 与调用点原写法同源：原来各页写的是 t.sizes.spacingLarge * 2.0f。
// spacingLarge 恒 16（theme.cpp 赋值不乘 fontScale），故恒 32；
// 这里仍按原式取 token，避免「硬编码 32 与 token 日后漂移」。
float pageInsetV() {
    return core::theme::current().sizes.spacingLarge * 2.0f;
}
float cardGap() { return 16.0f; }
float cardPadV() { return 20.0f; }
float listRowH() { return 44.0f; }
// 原来各页写的是 ui_kit::kHeaderH（constexpr 40，不随字号档缩放）。
float headerH() { return 40.0f; }

} // namespace ui_kit::density
