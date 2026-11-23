// app 壳内部缝：自 app_sidebar.cpp 拆出的 TU 之间共享，不进公共头（app_sidebar.h）。
#pragma once

#include <eui/dsl.h>
#include <eui/types.h>

namespace app {
namespace internal {

// 确定性 0~1 hash（splash 墨染同款 sin 散列取小数）：流质卫星几何与侧栏收缩按钮
// 粒子共用同一噪声源（原 app_sidebar.cpp 匿名 namespace 上移，跨 TU 共用故去匿名）。
float fluidHash01(int n);

// 自绘标题条（compose 顶层调用；实现见 app_titlebar.cpp）。
void drawTitlebar(eui::Ui& ui, const eui::Screen& screen);

} // namespace internal
} // namespace app
