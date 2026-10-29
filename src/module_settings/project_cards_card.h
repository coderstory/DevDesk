// src/module_settings/project_cards_card.h
// 需-11 项目卡设置卡（独立「项目卡」tab）：标题/说明 → 表单（名称独占一行 +
// 六档案字段双列三行）→ 操作行（保存/新建/删除 + 反馈行）→ 已登记列表（点选
// 装入表单 = 改，新建清空 = 增，删除作用于选中卡 = 删，查询走 MCP project_info）。
// 纯绘制函数：只吃 SettingsCardsIn 只读快照与 SettingsCardActions 回调，
// 不碰 DB、不捕获模块成员（github_sync_card 同款纪律）。
#pragma once

#include "settings_ui_cards.h"

#include <cstddef>

namespace module::settings {

// 卡高（100% 档基准常量，运行期随 roundFs 缩放）：行序与 drawProjectCardsCard
// 的纵向游标一一对应——改布局两处必须同步（kGithubSyncCardHeight 同款契约）。
// 行序 = 上下留白 16×2 + 标题 20 + 说明 16（含间距）+ 表单 4 行 (36+8)×4−8 +
// 操作行 32 + 反馈行 16（含间距）+ 分段线 24 + 列表（0 张 = 空态行 16，
// n 张 = n×28 + (n−1)×4）。100% 档：0 张 = 349 / 2 张 = 393，均在最小窗
// 卡区可用高 420 内（零滚动）；3 张起仅列表尾部进滚动。
float projectCardsCardHeightFor(std::size_t cardCount);

// 绘制整卡。in.projectCards 为空指针时按空列表渲染。
void drawProjectCardsCard(const CardDrawArgs& args);

}  // namespace module::settings
