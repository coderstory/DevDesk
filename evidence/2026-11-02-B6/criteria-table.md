# B6 字号缩放四档判据表（页 × 档 × 三判据）

探针：`build/tests/uat62_page_shots_probe --size 1600x1000 --font-scale {0.8,1.0,1.3,1.5}`
机制：每页按当前档渲染 → 中途 `setFontScale` → 再渲 24 帧（`uat62_page_shots_probe.cpp:240-249`，未改动）。
判据：① 零文字裁切 ② 零元素重叠 ③ 零横向溢出。像素 diff 未用于文字判据（对文字放大不敏感）。

| 页 | 0.8 | 1.0 | 1.3 | 1.5 | 备注 |
|---|---|---|---|---|---|
| 01 home | ✓ | ✓ | ✓ | ✓ | 统计卡行盒/卡高改 scaled（修前 1.5 档数字压标签） |
| 02 markdown | ✓ | ✓ | ✓ | ✓ | 页头标题列/模式 Tab 段按可用宽收口（修前 1.5 档「笔记本」压「编辑」Tab） |
| 03 todo | ✓ | ✓ | ✓ | ✓ | 页标题/列头/错误行行盒改 scaled |
| 04 schedule | ✓ | ✓ | ✓ | ✓ | 行卡高 + padY 内偏移改 scaled（修前 1.5 档第二行溢出卡外） |
| 05 claude_proxy | ✓ | ✓ | ✓ | ✓ | 详情字段标签行/值盒/行距改 scaled（修前 1.5 档字段行叠印） |
| 06 agent | ✓ | ✓ | ✓ | ✓ | 任务卡三行盒/卡高/行距改 scaledText（修前 1.3+ 第三行出卡） |
| 07 memory | ✓ | ✓ | ✓ | ✓ | 引擎卡/四张健康卡/消耗卡/记录表卡高与行盒改 scaled（修前 1.5 档数值压单位行、卡间叠印） |
| 09 mcp | ✓ | ✓ | ✓ | ✓ | 记忆模块行高/描述行顶改 scaled（修前 1.5 档标题与描述叠字） |
| 11 release_tasks | ✓ | ✓ | ✓ | ✓ | 无改动，回归确认 |
| 12 release_config | ✓ | ✓ | ✓ | ✓ | 无改动，回归确认 |
| 13 settings（默认 tab） | ✓ | ✓ | ✓ | ✓ | 见下 5 个分 tab |
| 13 settings-tab0 通用 | ✓ | ✓ | ✓ | ✓ | cardTitle/cardDesc helper 全 roundFs；动态背景开关盒 y/宽/高 scaled（修前 0.8 档开关压 desc、1.5 档标题被挤） |
| 13 settings-tab1 模块 | ✓ | ✓ | ✓ | ✓ | **用户报「全乱」的那一屏**：格高/行距内容派生、MCP 盒宽 roundFs(150)、>1.15 档两列降单列（修前 1.3/1.5 档标题被挤 + 描述压标题） |
| 13 settings-tab2 日志 | ✓ | ✓ | ✓ | ✓ | 行距 roundFs(16) + 日志行 maxWidth/clip（修前 1.5 档长行画到窗口右缘被硬切） |
| 13 settings-tab3 同步 | ✓ | ✓ | ✓ | ✓ | 无改动，回归确认（1.5 档表单仍完整可读） |
| 13 settings-tab4 项目卡 | ✓ | ✓ | ✓ | ✓ | 无改动，回归确认 |
| 14 about | ✓ | ✓ | ✓ | ✓ | 行高/组距 scaled + 值列 maxWidth/Ellipsis（修前 1.5 档「构建信息」压「构建时间」、构建时间被窗口切尾） |

图：`fs-{80,100,130,150}/*.png`（17 页/档，含 settings 5 个分 tab）；总览 `sheet-fs-*.png`；局部放大 `*-crop.png`。
审计清单：`audit-hits.txt`（脚本 `scripts/audit-font-scale.sh`；整改前 A=166/B=231，整改后 A=67/B=135）。

## F-69-4 settings 探针口的选择与理由
选「uat62 补 settings 分 tab 落图」（`--no-settings-tabs` 可关），不用 `settings_dropdown_z_probe` 单拍：
- 设置页 5 个 tab 卡集完全不同，用户报「模块 tab 全乱」的那一屏在常规十页清单里根本不存在 → 一拍不够，需要 5 拍；
- uat62 已有「中途切档 + 24 帧稳帧」机制（未改动），补 tab 走同一路径即可复现真机改档路径，不必在第二个探针里复制该机制；
- `settings_dropdown_z_probe` 的职责是 z 序/命中序，不属于字号判据，混进去会破坏其单一职责。

## 审计残留（本轮未修，非本次需求的 90% 内）
判据 A/B 剩余 202 行集中在：`module_release` 62 / `module_claude_proxy` 42 / `module_todo` 23 /
`module_memory` 21（多为弹窗 `*_modals.cpp`）/ `module_agent` 18（多为对话框）/ 等，绝大多数是
**弹窗与子面板**（不在本次「高危区」清单：agent 任务行、schedule 列表、settings 模块 tab、
home 统计卡、md 预览均已整改）。剩余项已如实记录，建议挂池后续批次处理。

## 判定口径说明
- 「✓」= 逐页看图确认三判据均过（字形上下沿不贴边、行间不叠、文字不撞/不出卡右缘）。
- 存量结构性红（非本批引入，见汇报「未验证/遗留」）：agent_e2e_probe 五流程依赖真实上游订阅（HTTP 400 InvalidSubscription）；claude 头像用例期望 40px 而代码 32px（HEAD 上即红）。
