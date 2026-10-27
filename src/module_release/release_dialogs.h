// 上线弹层（骨架）：新建任务 / 全屏配置编辑 / 撤回确认。
#pragma once

#include <eui/dsl.h>

namespace module::release {

void drawNewTaskDialog(eui::Ui& ui);
void drawFullConfigEditorDialog(eui::Ui& ui);
void drawWithdrawConfirmDialog(eui::Ui& ui);

} // namespace module::release