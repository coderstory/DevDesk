// 任务执行页（骨架）：页面组合入口，步骤/进度/日志由后续任务填充。
#pragma once

#include <eui/dsl.h>

#include <string>

namespace module::release {

void composeReleaseDetailPage(eui::Ui& ui, const eui::Screen& screen,
                              const std::string& taskId);

} // namespace module::release