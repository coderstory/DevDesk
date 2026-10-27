// 基础配置页（骨架）：页面组合入口，具体控件由后续任务填充。
#pragma once

#include <eui/dsl.h>

namespace module::release {

void composeReleaseConfigPage(eui::Ui& ui, const eui::Screen& screen);

} // namespace module::release