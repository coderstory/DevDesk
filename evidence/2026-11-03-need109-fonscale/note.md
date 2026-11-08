# pool 109 字号缩放审计残留清零（2026-11-03）

## 改动统计
- 21 文件 198 处，全部单行行内替换（无结构重排）：
  - 判据 B（`.size(x, 裸数字)` 文本盒高）132 处 → 第二参包 `core::theme::scaled()`；
  - 判据 A（`.lineHeight(裸数字)`）65 处 → 包 `core::theme::scaled()`；
  - 1 处补刀：claude_ui_detail.cpp:234（warn 图标盒高，行尾带 `.icon()` 未被常规规则覆盖）。
- 助手选择按各文件既有惯例：wallpaper_ui / project_cards_card 用文件内本地 `roundFs()`，其余用 `core::theme::scaled()`。
- 方形单字符图标盒（`.size(N, N)`，10 处：md tab close/search icon、todo search icon、mcp hero、wallpaper empty/folder/scanning icon、agent 三处关闭钮、release node.check）宽高双包，避免放大档字形出盒。
- 特例口径：
  - release_config_ui.cpp:535 页题盒 `scaled(kPageHeaderH)`——header stack 高度不动（kPageHeaderH 被 1210 行底部定位共用），仅文本盒随字号走；
  - agent_ui_shared.h actionChip 仅包 lineHeight（kChipH=32 是 spec §9.2 契约高，注释明示「高度仍 32」）。

## 豁免清单（9 处，审计残留 = 全部为简报禁碰文件）
| 文件:行 | 判据 | 理由 |
|---|---|---|
| module_schedule/schedule_ui_edit.cpp:434, 852 | A | 池111/112 消债刚收口（schedule_ui_list 763-773 接线同批），简报明令禁碰 |
| module_schedule/schedule_module.cpp:92 | B | 同上 |
| module_schedule/schedule_ui_edit.cpp:429, 761, 847 | B | 同上 |
| module_schedule/schedule_ui_list.cpp:1072 | B | 同上 |
| module_settings/github_sync_card.cpp:221, 460 | B | github_sync 消债已收口文件（池106/107），禁碰 |

审计终态：判据 A 67→2、判据 B 132→7，残留 9 处均为上表豁免行（见 audit-after.txt）。

## 验证摘要
- 定向测试（--dev --filter，12 域全绿，合计 992 过 / 0 红 / 1 skip）：
  release 111 ｜ claude 134 ｜ todo 150 ｜ memory 80+1skip ｜ agent 144 ｜
  mcp 171 ｜ markdown 117 ｜ wallpaper 50 ｜ project_cards 13 ｜ uiModern 16 ｜ hierarchy 4 ｜ module_icons 2
- 跨文件元素 id 反查：16 组弹窗/引导/加载态 id grep tests/ 命中 13 个测试文件（含 test_ui_modern / test_theme_hierarchy / test_module_icons 漏网高危），已全部纳入上述 filter 跑绿。
- 字号档探针：`uat62_page_shots_probe --size 1600x1000`，--font-scale 1.5 与 0.8 各 17 页 PASS（图：本目录 fs150/、fs80/，各 17 张）。

## 未验证 / 移交 qa
- 1.0 / 1.3 档探针未跑（B6 四档全量核验归 qa 收口档）。
- 探针自身跳过 wallpaper、release_group 2 页（平台能力未注册，与本次改动无关）。
- 逐页像素三判据（零裁切/零重叠/零溢出）人工看图判定未做——本批自证深度 = 构建绿 + 用例绿 + 探针出图 PASS，判据级验收归 qa。
- 弹窗内部 position 算术仍为字面量：本批按判据只改行盒/文本盒/lineHeight 随字号缩放，弹窗网格坐标未重排（放大档行盒已同步生长，绝对定位间距不随档变化）。
