// 任务列表（瀑布流）（骨架）：页面组合入口，具体卡片/布局由后续任务填充。
#pragma once

#include <eui/dsl.h>

namespace module::release {

void composeReleaseListPage(eui::Ui& ui, const eui::Screen& screen);

} // namespace module::release