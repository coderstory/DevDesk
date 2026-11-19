// need356（老板令「仅针对windows，应用的全部颜色加深处理——windows 的笔记本屏幕
// 素质太差，颜色寡淡」）：Windows 档加深因子，平台拆分实现（posix 侧恒 1.0，
// mac 不动）。接口声明在 theme.h，施加点在 theme.cpp applyColorDeepen。
// 调档入口=本文件这一处常量（装机实测后调；1.0f=关闭）。
#include "theme.h"

namespace core::theme {

float colorDeepenFactor() {
    // 0.92 = 适度加深 8%：全部颜色 RGB 统一乘因子、alpha 不动，
    // 前景/背景同乘 → 亮度比例保持，对比度不塌。
    return 0.92f;
}

} // namespace core::theme
