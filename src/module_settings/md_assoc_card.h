// src/module_settings/md_assoc_card.h
// 「关联 .md 文件」开关行卡（设置页「通用」tab，F-10）。
// 状态字四态推导与卡高契约单一来源 = md_assoc_logic.h；引导行（未生效态）
// 分平台：Windows = 一键按钮直达系统默认应用设置 / macOS = 文字指引——
// 分流经 platform::open_settings::oneClickAvailable()，业务代码零平台宏。
#pragma once

#include "settings_ui_cards.h"

namespace module::settings {

void drawMdAssocCard(const CardDrawArgs& args);

} // namespace module::settings
