// src/module_settings/web_access_card.h
// 「网页访问」设置卡（need#490 W6）：设置页「通用」tab 全宽卡。
// 数据只经 core::IWebTerminalService 只读快照（SettingsCardsIn::webAccess），
// 二维码矩阵由服务侧编码、按 rect 逐模块绘制（module_settings 不 include
// module_terminal，模块隔离铁律）。
#pragma once

#include "settings_ui_cards.h"

#include <core/web_terminal_service.h>

namespace module::settings {

// 卡高单一来源：compose 侧算卡高与卡片绘制共用（四态 + 删除绑定确认行）。
// 返回已按生效字号档缩放的值（roundFs 口径与其余卡一致）。
float webAccessCardHeight(const core::WebAccessSnapshot& snapshot,
                          bool bindingConfirm);

void drawWebAccessCard(const CardDrawArgs& args);

} // namespace module::settings
