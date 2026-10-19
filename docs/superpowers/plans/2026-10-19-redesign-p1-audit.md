# 智能体详情弹窗 P1 布局壳 — 独立审计报告

> 日期：2026-10-19 ｜ 审计对象：branch `redesign-p1-20261019` commit `b828da10`（基线 dev `2b3b0053`）
> 三背景：spec `2026-10-18-agent-detail-dialog-redesign.md` §13 P1、task-map `2026-10-18-ui-redesign-task-map.md` 卡 P1、交接 `2026-10-19-handoff-pause.md` 六条拍板
> 审计者：独立拆分子任务（只读，与实现者无关）。本文件是唯一写动作。

## Findings 分布

| 严重度 | 数量 |
|--------|------|
| HIGH   | 3    |
| MEDIUM | 1    |
| LOW    | 0    |

---

## 维度 1：正确性 — 六条拍板逐项对照

| # | 拍板项 | 状态 | 证据 |
|---|--------|------|------|
| ① | 三卡去 1px 边框改色块+radius | ✅ 已做 | diff 删 `summaryCard` lambda 内 `.border(1.0f, t.colors.borderDefault)`，现用 `.color(surfaceElevated)+.radius(t.sizes.radius)`（`agent_detail_dialog.cpp:220-224`） |
| ② | 窄屏单列三行（2+1→3×全宽） | ✅ 已做 | diff：rowH 从 `/2.0f`→`/3.0f`，三卡间距 `2*(rowH+kCardGap)`，全用 `innerW` 宽（`:277-291`） |
| ③ | 配置条改 chip 可折行 | ❌ **未做** | 折叠态 hint 仍为单行文本串 `"触发："+label+" · cron："+cron+" · 模型："+slot+" · 迭代："+n`（`:300-311`），渲染为单个 `agent.dialog.detail.config.header.hint` text 节点（`:336-349`），零 chip |
| ④ | 停止 chip 文案缩短 | ❌ **未做** | 文案仍为 `"停止 · 当前轮结束后生效"`（`:135`）和 `"停止中 · 当前轮结束后生效"`（`:134`）；handoff 要求主文案缩「停止」、副文案挪另处 |
| ⑤ | 左栏 clamp(innerW*0.42,300,360)+耗时右对齐 | ✅ 已做 | diff：`:89-90` clamp 从 `(0.40,240,282)`→`(0.42,300,360)`；`:565` duration text 加 `.horizontalAlign(Right)` |
| ⑥ | 消灭卡中卡双边框 | ⚠️ **部分完成** | 摘要卡 border 已删 ✅；但 `config.header.bg` 仍有 `.border(1.0f, t.colors.borderDefault)`（`:325`）——这是 `.config.header` stack 内的 bg rect，嵌套在 dialog frame 内形成「卡中卡」双边框 |

**③④ 归属判定**：handoff 拍板未区分 P1/P3。task-map 卡 P1 写「config.header 常驻头（只画头，浮层在 P3）」——chip 渲染改的是折叠态 hint 表现，属 config.header 常驻态，理论上在 P1。**按 spec 判**：spece §13 P1 内容为「实际宽高、宽/窄分支、摘要/配置/主区骨架」——chip 渲染属 P3「配置浮层」的产物。结论：③④ 按 spec 属 P3 范围，不构成 P1 缺口；但 handoff 拍板未分阶段，主会话需确认归属。

### R-A：overlayInset 真源验证 — 已核

| 断言 | 位置 | 结论 |
|------|------|------|
| `overlayInset` 读 `themeTokens.metrics.spacing.overlay` | `agent_detail_dialog.cpp:61` | ✅ 实读上游 `ThemeColorTokens.metrics.spacing.overlay`，非硬编码 |
| 注释指向 `EUI-NEO components/theme.h:47` | `agent_detail_dialog.cpp:49-50` | ✅ theme.h:47 为 `float overlay = 48.0f`，行号真实 |
| 注释指向 `EUI-NEO components/dialog.h:81-82` | `agent_detail_dialog.cpp:46-48` | ✅ dialog.h:81 为 `width = min(width_, screenWidth_ - metrics_.spacing.overlay)`，:82 为 height 同公式，行号真实 |
| DevDesk 复算公式与上游一致 | `agent_detail_dialog.cpp:53` | ✅ `min(requested, max(0, screenExtent - overlayInset))` 与 dialog.h:81-82 逻辑一致 |

---

## 维度 2：回归面

| 检查项 | 结论 | 证据 |
|--------|------|------|
| `agent_module.cpp` 零触碰 | ✅ | diff 仅 3 文件，不含 `agent_module.cpp`（详情弹窗已拆出到独立 TU） |
| `agent_ui_shared.h` 零触碰 | ✅ | 同上 |
| `test_agent_ui.cpp` 同步更新 | ❌ **未更新** | diff 零改动；7+ 条断言因左栏/窄屏公式变更必然失效（见专项「旧断言冲突清单」） |
| CMakeLists 登记新测试文件 | ✅ | `tests/CMakeLists.txt:70` 加 `test_modules/test_agent_detail_layout.cpp` |

---

## 维度 3：平台对称性

已核：新 `test_agent_detail_layout.cpp` 零 `#ifdef`；`agent_detail_dialog.cpp` diff 零平台分支；三尺寸用例全用逻辑像素断言，不涉平台路径。

---

## 维度 4：静默失败面

| 路径 | 结论 |
|------|------|
| `detailDialogActualExtent` `max(0, screen-overlay)` | ✅ 下限哨兵防负值 |
| `leftW` clamp [300,360] | ✅ 保底 300 |
| `rightW` `max(0, innerW-leftW-20)` | ✅ 零哨兵 |
| `summaryH` 窄屏公式 `3*max(48, ...)+16` | ✅ 下限 48 哨兵 |
| `summaryCard` 内 `hasSubRow` 门控副值 | ✅ 卡高不足时副值跳过，无静默失败 |

---

## 维度 5：测试有效性

新测试文件 `test_agent_detail_layout.cpp` 共 3 用例 9+ 断言：

| 用例 | 判定 |
|------|------|
| `wide_760x660` | ✅ panel 760×660 断言正确；三卡非空 ✅；`.bg` border.width≤0 负向断言✅（改前 border=1.0→可证红）；左栏 300 正确（704×0.42=295.68→clamp 300） |
| `narrow_700x560` | ✅ panel 652×512（钳制公式 min(760,700-48)=652 / min(660,560-48)=512）；nav 存在 / detail 不存在 / 三卡常驻 |
| `shortwide_900x600` | ✅ panel 760×552（min(660,600-48)=552）；宽屏无 nav；detail.scroll 存在；msgscroll 不存在 |

**三卡无边框断言强度**：`lastBg->border.width <= 0.0f` 是负向断言。改前 `summaryCard` 内 `.bg` 有 `border(1.0f, ...)`，`border.width` 应为 1.0→断言红。能证可红，非恒真。

---

## 维度 6：文档口径

| 检查项 | 结论 |
|--------|------|
| EUI-NEO 行号 `theme.h:47` | ✅ 真实（`float overlay = 48.0f`） |
| EUI-NEO 行号 `dialog.h:81-82` | ✅ 真实（`width = min(..., screenWidth_ - metrics_.spacing.overlay)` 两行） |
| 六条拍板与实现差异 | ③④ 未做，需主会话确认归属（P1 还是 P3）后回写 spec 或补做 |

---

## 专项三核

### ui id 前缀 `agent.dialog.detail.*` 增删对称性

已核：diff 零 ui id 增删。全部 `agent.dialog.detail.*` 节点 ID 与基线完全一致，仅位置/尺寸变更。

### 禁区触碰

| 禁区 | 结论 |
|------|------|
| `composeTaskDialog` | ✅ 零触碰（diff 文件全在 `agent_detail_dialog.cpp`，任务弹窗在 `agent_module.cpp`） |
| `compose()` 尾部 toast | ✅ 零触碰 |
| `test_agent_ui.cpp` | ⚠️ 零触碰——但这是**问题**（见维度 2） |

### 旧断言冲突清单（主会话排队修）

共 **9 条**，全部在 `tests/test_modules/test_agent_ui.cpp`，因左栏 282→300 + 窄屏 summaryH 116/122→动态公式必然失效：

1. `:3765` — 注释「左栏 282 宽」→ 应为 300
2. `:3839` — 注释「leftW=282」→ 应为 300
3. `:3867` — `nearPx(runlist->frame.width, 282.0f)` → 应为 300.0f（左栏 clamp 从 282→300）
4. `:3871` — `nearPx(detailCol->frame.x - panel->frame.x - 28.0f, 302.0f)` → 应为 320.0f（x0+300+20-28=320）
5. `:3874` — `nearPx(detailCol->frame.width, 402.0f)` → 应为 384.0f（704-300-20=384）
6. `:4065` — 注释「summaryH=116 → configY=184 / navY=232 / contentY=272」→ 全链失效（窄屏 summaryH 从 116→动态约 194@defaultFS）
7. `:4069` — `nearPx(header->frame.y - panel->frame.y, 184.0f)` → 应随新 configY（60+newSummaryH+8≈262）
8. `:4072` — `nearPx(nav->frame.y - panel->frame.y, 232.0f)` → 应随新 navY（≈310）
9. `:4082` — `nearPx(runlist->frame.y - panel->frame.y, 272.0f)` → 应随新 contentY（≈368）