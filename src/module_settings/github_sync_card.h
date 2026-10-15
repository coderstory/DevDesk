// src/module_settings/github_sync_card.h
// GitHub Sync 设置卡片：仓库 URL、分支、Token、一键同步/拉取。
// 自由函数签名，与 settings_ui_cards 同模。
#pragma once

#include "settings_ui_cards.h"

namespace module::settings {

// GitHub Sync 配置卡片（设置页"模块"tab）
void drawGithubSyncCard(const CardDrawArgs& args);

} // namespace module::settings