# UAT6.2 W3 审计 B 报告 —— 范围纪律 / S6 反向核查 / 规格抽核 / 遗留债务合并

- **日期**: 2026-10-22
- **审计对象**: worktree `/d/wt-uat62-ds`，commits `d7608c91..942faea3`（W3 逐页改造六批 B1-B6）
- **审计者**: 独立审计专家 B（范围纪律/S6 裁决/规格符合/遗留合并）
- **依据**: 设计规范 v1.0（S0-S8）+ 执行计划 W3 段 + 整改清单 W3 待办段

---

## 1. 范围纪律

### 1.1 文件域 vs 派单边界

每批文件归属干净，无跨批同文件写冲突：

| 批次 | 提交 | 文件域 |
|------|------|--------|
| B1 | `31eddb47` | `home_module.cpp`, `about_module.cpp`, `ui_kit/button.*`, `ui_kit/wswitch.cpp`, `core/theme.*` |
| B2 | `de40125b` | `markdown_module.cpp` |
| B3 | `355399b6`..`3d145662` | `todo_ui_board.*`, `todo_ui_dialogs.cpp`, `todo_ui_logic.h`, `todo_module.cpp` |
| B4 | `b685a578`..`ec27db38` | `claude_ui_detail.cpp`, `claude_ui_list.*`, `claude_ui_state.h`, `claude_module.cpp` |
| B5 | `e713bf36` | `agent_module.cpp`, `test_agent_ui.cpp` |
| B6 | `56f051a2` | `settings_ui_cards.*`, `settings_module.cpp`, `github_sync_card.cpp`, `test_settings.cpp` |

**测试适配提交**（`5e4b0556`..`13a23cce` 等）全部只改 `tests/` 下文件，预期行为。

### 1.2 B5「禁碰 wizard」边界

B5 提交 `e713bf36` 的 diff 范围全部在 `AgentModule::compose()` 的页面渲染段（工具列表 / 任务卡 / 空状态），**未触碰 wizard 代码**（`kTaskDialogH` 附近 L2239+）。边界干净。

### 1.3 「B5 碰 settings 编译修复」已知例外

**未发现**。B5 提交只涉及 `agent_module.cpp` + `test_agent_ui.cpp`，无 settings 文件触及。所有编译修复提交（`11d7d3f9` 等）均在各自模块域内。

### 1.4 域覆盖缺口

以下文件含有 S6.1 玻璃残留（见第 2 节），但 W3 无任何批次覆盖其玻璃退役：

| 文件 | 玻璃残留 | W3 触及？ | 触及批次 |
|------|---------|-----------|---------|
| `claude_ui_detail.cpp` | 9 处 drawGlassCardLayers | 是 | B4（S3.4 #1-#9，不含玻璃退役） |
| `claude_ui_list.cpp` | 1 处 drawGlassCardLayers | 是 | B4（同上） |
| `agent_detail_dialog.cpp` | 1 处 drawGlassCardLayers | **否** | 未被任何 W3 批次触及 |

**定性**: 这些是 S6.1 执行面的**范围缺口**（不属于 B4/B5 的契约内容），不是批内越界。

---

## 2. S6 裁决执行反向核查

### 2.1 S6.1 玻璃体系退役 — 残留 11 处 + 头文件未归档

| 残留类型 | 计数 | 位置 |
|---------|------|------|
| `drawGlassCardLayers` 调用 | **11** | `claude_ui_detail.cpp:74,120,168,308,453,791,810,858,1108` + `claude_ui_list.cpp:916` + `agent_detail_dialog.cpp:362` |
| `#include <ui_kit/glass_card.h>` | **3** | `claude_ui_detail.cpp:20`, `claude_ui_list.cpp:12`, `agent_detail_dialog.cpp:27` |
| `glass_card.h` 头文件 | **1** | `src/ui_kit/glass_card.h` — 未被删除/归档 |

**豁免项已确认（不在残留计数内）**：
- `splash.cpp` — 窗口磨砂/印染调色（S6.1 落地口径第 5 条：splash 色调从 #07C160 重调为 #36D399 10% 透明度，属豁免清单）。
- `home_module.cpp:52` / `about_module.cpp:3` — 注释声明「不再使用 glass_card.h」，属文档标记，不算残留。

### 2.2 S6.2 下划线 Tab → 块面 Tab — 残留 0

全仓 `src/` 无下划线式 Tab 残留。唯一命中 `markdown_module.cpp:529` 是 wtab 组件 API 注释（"hit/text/underline 按 wtab 约定"），非下划线 Tab 实现。

块面式 wtab 消费点：`markdown_module.cpp:536`, `claude_ui_detail.cpp:1083`, `settings_module.cpp:1062` — 全部合规。

### 2.3 S6.3 品牌绿迁移（#07C160 → #36D399） — 运行时残留 0，注释残留 9

**运行时值**：`theme.cpp` 与 `devdesk_theme.h` 中 primary/primaryText/wechat.primary 均使用 `#36D399`，**零运行时残留**。

**注释残留**（仅误导性，无运行时影响）：

| 文件 | 行 | 残留内容 |
|------|-----|---------|
| `core/theme.h` | 55 | `// #07C160 主色（按钮底/选中/高亮）` |
| `core/theme.h` | 58 | `// UAT3 W2-M2：… #07C160 落 selectedBg …` |
| `ui_kit/button.h` | 20 | `// Primary — 微信绿 #07C160 填充 + 白字` |
| `ui_kit/wswitch.h` | 28 | `// 开 = wechat.primary（#07C160）绿轨道` |
| `ui_kit/splash.cpp` | 18 | `// H 批对比度核 … wechat.primary #07C160` |
| `ui_kit/splash.cpp` | 177 | `// #059A4C … wechat.primary #07C160 在微绿底` |
| `module_todo/todo_ui_logic.h` | 288 | `// Low 微信绿 #07C160` |
| `module_settings/settings_ui_cards.cpp` | 560 | `// primary=#07C160 绿` |
| `module_markdown/markdown_module.cpp` | 689 | `// 品牌绿底（wechat.primary #07C160）` |

### 2.4 S6.5 整卡涂色 → 白底 + 优先级色条 — 残留 0

`todo_ui_board.cpp:524,560` 注释明确标注「白底优先级色条替代整卡涂色」。实现侧 `drawCard` 函数白底 rect + 左侧 3px 优先级色条 + Sm 阴影，无整卡涂色残留。

### 2.5 32px 标题 → 24px — 运行时残留 0，注释残留 1

运行时值 `fontSizePage = 24.0f`（`theme.cpp:75,147,234`）。注释残留：`theme.h:95` `// 32.0f`。

### 2.6 弹窗标题 24 → 20 — 23 处调用，3 处不合规

**合规（20 处）**：所有 claude_proxy / markdown / todo / settings / git_error 弹窗标题使用 `20.0f` 或 `fontSizeLarge + 2.0f (= 20.0f)`。

**不合规（3 处，均在 agent 模块）**：

| 文件 | 行 | 弹窗 | 当前值 | 期望值 |
|------|-----|------|--------|--------|
| `agent_module.cpp` | 2289 | 新建/编辑智能任务 | `fontSizeBody` = **15px** | 20px |
| `agent_module.cpp` | 2950 | AI 生成草稿 | `fontSizeLarge` = **18px** | 20px |
| `agent_detail_dialog.cpp` | 180 | 任务详情弹窗标题 | `fontSizeLarge` = **18px** | 20px |

---

## 3. 规格符合抽核（S3 逐页条目，8 条）

| # | S3 条目 | 批次 | 代码证据 | 结果 |
|---|---------|------|---------|------|
| 1 | B3 白底卡 + 左侧 3px 优先级色条 | B3 | `todo_ui_board.cpp:560-570` 注释 + 实现 | 通过 |
| 2 | B1 首页五色实底卡（辅助色 20% 饱和 95% 明度） | B1 | `home_module.cpp:192-196` 五色 `fromHex` 值 | 通过 |
| 3 | B2 选中态 3px 竖线 | B2 | `markdown_module.cpp:151,1140` selectionBar | 通过 |
| 4 | B4 左栏固定 240px | B4 | `claude_module.cpp:69-70` `leftW = 240.0f` | 通过 |
| 5 | B4 详情 Tab 改块面式 wtab | B4 | `claude_ui_detail.cpp:1073,1083` wtab 调用 | 通过 |
| 6 | B6 设置页块面 Tab | B6 | `settings_module.cpp:1062` wtab 调用 | 通过 |
| 7 | B1 关于页 Ghost 复制图标 | B1 | `about_module.cpp:345` Ghost 图标钮 | 通过 |
| 8 | B3 列头按钮 Ghost | B3 | `todo_ui_board.cpp` 列头区按钮样式 | 通过 |

**抽核结论**：8/8 通过。未见「报了改但实际没改」的条目。

---

## 4. W4 遗留债务合并台账（单一输入清单）

### 4.1 S6.1 玻璃退役扫尾（3 文件 + 1 头文件）

| # | 文件 | 动作 | 优先级 |
|---|------|------|--------|
| G1 | `claude_ui_detail.cpp` | 9 处 drawGlassCardLayers → 实底白卡（rect.color(#FFFFFF) + Sm 阴影） | 高 |
| G2 | `claude_ui_list.cpp` | 1 处 drawGlassCardLayers → 实底白卡 | 高 |
| G3 | `agent_detail_dialog.cpp` | 1 处 drawGlassCardLayers → 实底白卡 | 高 |
| G4 | `ui_kit/glass_card.h` | 归档/删除（三处 include 清理后） | 中（依赖 G1-G3） |

### 4.2 弹窗标题 20px 补齐（3 处）

| # | 文件:行 | 弹窗 | 当前 | 改至 |
|---|---------|------|------|------|
| T1 | `agent_module.cpp:2289` | 新建/编辑智能任务 | `fontSizeBody` (15) | `20.0f` |
| T2 | `agent_module.cpp:2950` | AI 生成草稿 | `fontSizeLarge` (18) | `20.0f` |
| T3 | `agent_detail_dialog.cpp:180` | 任务详情弹窗标题 | `fontSizeLarge` (18) | `20.0f` |

### 4.3 注释陈旧清理（10 处，低优先级）

| # | 文件:行 | 内容 |
|---|---------|------|
| C1 | `theme.h:55` | `// #07C160 主色` → `// #36D399 主色` |
| C2 | `theme.h:58` | 注释中 `#07C160` → `#36D399` |
| C3 | `theme.h:95` | `// 32.0f` → `// 24.0f` |
| C4 | `button.h:20` | `// 微信绿 #07C160` → `// 薄荷绿 #36D399` |
| C5 | `wswitch.h:28` | `// #07C160` → `// #36D399` |
| C6 | `splash.cpp:18` | `// #07C160` → `// #36D399` |
| C7 | `splash.cpp:177` | `// #07C160` → `// #36D399` |
| C8 | `todo_ui_logic.h:288` | `// #07C160` → `// #36D399` |
| C9 | `settings_ui_cards.cpp:560` | `// #07C160` → `// #36D399` |
| C10 | `markdown_module.cpp:689` | `// #07C160` → `// #36D399` |

---

## 汇总

| 维度 | 结论 | 发现数 |
|------|------|--------|
| 1. 范围纪律 | 批间无同文件写冲突；B5 wizard 边界干净；"B5 碰 settings"例外未实证 | 0 违规 / 3 缺口 |
| 2. S6 反向核查 | 玻璃 11 处残留 + 头文件未归档；下划线 Tab 0；#07C160 运行时 0/注释 9；整卡涂色 0；32px 运行时 0/注释 1；弹窗标题 20/23 合规 | 15 残留（含注释）+ 3 不合规 |
| 3. 规格抽核 | 8/8 通过 | 0 偏差 |
| 4. W4 输入清单 | 玻璃 4 项 + 弹窗标题 3 项 + 注释 10 项 | **17 条** |

**W4 输入清单（按优先级排）**：G1-G4（玻璃扫尾，高）→ T1-T3（弹窗标题补齐，高）→ C1-C10（注释陈旧，低）。