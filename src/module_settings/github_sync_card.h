// src/module_settings/github_sync_card.h
// GitHub Sync 设置卡片：仓库 URL、分支、Token、一键同步/拉取。
// 自由函数签名，与 settings_ui_cards 同模。
#pragma once

#include "settings_ui_cards.h"

namespace module::settings {

// GitHub Sync 配置卡片（设置页"模块"tab）
void drawGithubSyncCard(const CardDrawArgs& args);

// 表名 → 用户看得懂的中文标签（披露文案用）。
// 只覆盖 core::sql_dump::defaultSkipTables() 会出现的表；未知表名**原样返回**，
// 不编造标签。给 skip 清单加表时这里必须同步补，否则防漂移用例会红
// （见 tests/test_modules/test_github_sync_card.cpp 的 disclosure 用例）。
std::string disclosureLabelFor(const std::string& tableName);

// 同步内容披露文案：由 core::sql_dump::defaultSkipTables() **驱动**列出「不上传」
// 的表，改 skip 清单文案随之变（单一来源，杜绝文案与实现两张皮）。
// 覆盖三点：① 同步笔记 + 整库数据库快照；② 明确列出不上传的表；③ 目标是 GitHub 私有仓库。
std::string githubSyncDisclosureText();

} // namespace module::settings