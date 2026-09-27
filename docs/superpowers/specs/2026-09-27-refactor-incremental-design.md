# DevDesk 全量重构（任务 D）设计：逐模块增量 / OOP 与设计模式层面

- 日期：2026-09-27 ｜ 状态：**设计（未实施，未 commit）** ｜ 排期：最后（编排板 `2026-09-27-orchestration.md` 第 14 项）
- 关联：`2026-09-20-devdesk-design.md`（唯一架构权威，§4.2 依赖规则 / §4.2.1 core→EUI 桥接边界）、`2026-09-22-backlog.md` §四（技术债登记）、`2026-09-27-markdown-phase3-block-render-design.md`、`2026-09-27-profile-reverse-sync-and-copy-design.md`（在飞）
- 依据：全文 file:line 均**实读源码**核对（2026-09-27，工作树含 37 项在飞改动）；未取证项集中在 §9「待验证」
- **只出设计，不写实现**。本文件不排执行时间表，只排依赖顺序

---

## 1. 需求与已定约束（用户拍板，不得改）

| # | 约束 | 含义 |
|---|------|------|
| C1 | **逐模块增量，禁大爆炸** | 每个里程碑独立可验收、可发布、全量测试全绿；禁止"一次重写全部" |
| C2 | 目标是 **OOP / 设计模式层面**的重构 | 消除重复与散落常量、理清职责边界；**不改产品行为** |
| C3 | 测试是**护栏不是目标** | 现有用例全绿是准入门槛；不为重构补写新功能测试（纯函数抽离可加断言，见 §5 各里程碑） |
| C4 | 排期在最后 | 与在飞两条 spec 冲突时让路（见 §5 前置条件） |

---

## 2. 重构原则与红线（违反即 STOP + REVERT + REFACTOR）

| # | 红线 | 出处 | 重构期的具体含义 |
|---|------|------|------------------|
| R1 | **不抛 C++ 异常**，一切可失败路径返回 `core::Result<T>` | spec §11.1、`src/core/result.h` | 抽出的新函数沿用 `Result`；禁止为了"看着现代"引入异常/optional-error 混用 |
| R2 | `module_*` 之间**不可互相依赖** | spec §4.2、CLAUDE.md 架构要点 | 抽出的共享代码**不能**放进任一 `module_*`；跨模块数据只能经 `core` 服务层或 Router |
| R3 | `core` 不可依赖 `platform`；**不可调用 EUI-NEO 运行时行为**（仅允许 header-only 类型桥接 + 纯映射） | spec §4.2.1 允许/禁止清单 | 共享 UI 助手（dialog/toast/card）**不能放 core**——它们调用 `components::*`，属"需要 eui_neo 编译实体符号的能力"，在禁止清单内。这是新建 `ui_kit` 层的直接原因（§4.1） |
| R4 | **EUI-NEO 上游优先**：通用能力缺失提上游，不在 DevDesk 造 workaround | CLAUDE.md 上游贡献政策 | 判据（既有）：设计 token / 配色 / 业务逻辑属 DevDesk 自己的事 → 本地；组件通用能力缺失 → 上游。既有上游项见 §3.4 |
| R5 | 发布形态 **单 exe、零外部 DLL** | spec §16 发布形态约束 | 新增静态库（`devdesk_ui_kit`）不引入任何新动态依赖；不新增第三方依赖 |
| R6 | **行为不变**：无 commit 修改产品可观察行为 | C2 | 每个里程碑的 diff 必须是"移动 + 改名 + 参数化"，不允许顺手改文案/数值/顺序；发现缺陷只登记不修 |
| R7 | 平台适配代码接口隔离（`*_win.cpp` / `*_macos.cpp` / `*_posix.cpp`），业务代码零 `#ifdef _WIN32` | CLAUDE.md 核心约束 5 | 重构不把平台宏引入新文件 |

**红线交叉点（易踩）**：`core` 里已有 `component_theme.h`（header-only token→`ThemeColorTokens` 映射）——它是**允许的**纯映射。若把 `drawToast` 搬进 core 就**越界**，因为 toast 会调 `components::toast()`。

---

## 3. 现状盘点（量化）

### 3.1 规模

| 目录 | 文件 | 行数 | 备注 |
|------|------|------|------|
| `src/module_markdown/` | 25 | **7579** | 最大；但已被拆得最细（9 个 `md_*` 纯函数单元） |
| `src/module_claude_proxy/` | 14 | 6638 | 服务层拆得干净，UI 层是最大单文件 |
| `src/module_todo/` | 6 | 3799 | 只有 `todo_ui_logic.h` + `todo_view_state` 两处抽离 |
| `src/platform/` | 21 | 2716 | 按平台拆文件已完成，健康 |
| `src/module_settings/` | 3 | 2292 | 有局部 `cardFrame` 助手（类内私有，未复用） |
| `src/module_pet/` | 8 | 2209 | 子窗 + 皮肤/帧/气泡已分层 |
| `src/core/` | 26 | 2207 | 健康 |
| `src/module_home/` | 3 | 890 | `home_module.cpp` 581 行 |
| `src/app_sidebar.cpp` + `src/main.cpp` | 2 | 996 | 应用级装配 |
| **`src/` 合计** | — | **29378** | |

### 3.2 最大单文件与最大函数（brace-matched 实测）

| file:line | 规模 | 说明 |
|-----------|------|------|
| `src/module_markdown/markdown_module.cpp` | 2847 行 | 单文件 |
| `src/module_claude_proxy/claude_module.cpp` | 2454 行 | 单文件 |
| `src/module_todo/todo_module.cpp` | 2432 行 | 单文件 |
| `src/module_settings/settings_module.cpp` | 1704 行 | 单文件 |
| `src/module_claude_proxy/claude_module.cpp:131` | **`compose` 859 行** | 含 24 个 `if`、9 处组件装配、63 处 `ui.*` 元素 |
| `src/module_home/home_module.cpp:188` | `compose` 369 行 | |
| `src/module_todo/todo_module.cpp:2016` | `drawDetailDialog` 310 行 | |
| `src/module_claude_proxy/claude_module.cpp:2034` | `drawCallsDialog` 213 行 | |
| `src/module_markdown/markdown_module.cpp:1253` | `drawPreviewColumn` 212 行 | |
| `src/module_claude_proxy/claude_module.cpp:1636` | `drawFormEnvTab` 202 行 | |

**嵌套深度（λ `.content([&]{...})` 链造成）**：

| 指标 | 值 | 证据 |
|------|-----|------|
| 缩进 ≥10 层（40 空格）的行数 | **702** | 全 `src/**.cpp` 扫描 |
| 其中 `claude_module.cpp` | **422 行（60%）** | |
| `markdown_module.cpp` | 115 行 | |
| 最深 | **19 层** | `markdown_module.cpp:1415` |
| claude 最深 | **17 层** | `claude_module.cpp:715` |

### 3.3 重复模式清单（8 条，每条带样例 file:line）

| # | 模式 | 证据（file:line） | 重复量 |
|---|------|-------------------|--------|
| D1 | **Toast 状态机 + 绘制全套复制** | `claude_module.cpp:2249/2265`、`markdown_module.cpp:1782/1815`、`todo_module.cpp:616`、`app_sidebar.cpp:294` | **4 份**；字段名（`toastOpen_`/`toastTitle_`/`toastIsError_`）与 `onAutoDismiss`/`onDismiss` 接线逐字相同，只有元素 id 不同 |
| D2 | **对话框表单脚手架** | 13 个 `components::dialog(` 调用点 / 13 个 `draw*Dialog`：claude 3、markdown 4、todo 6 | 标题一律 `.position(28.0f, 24.0f)` ×9；正文宽一律 `w - 56.0f` ×**72**；标准宽度 `const float w = 460.0f` ×**8**；按钮对（取消/确定）位置与样式逐字重复 |
| D3 | **表头高度 64 + 正文几何公式** | `claude_module.cpp:138`、`settings_module.cpp:502`、`todo_module.cpp:97`（注释 `/*headerH*/`）、`todo_module.cpp:640`；`markdown_module.cpp:322` 直接写字面量 `.size(contentW, 64.0f)` | `const float headerH = 64.0f` 与 `bodyH = contentH - headerH - spacingLarge…` **5 处** |
| D4 | **chip（pill 标签）绘制 + 宽度估算** | `todo_module.cpp:111 drawChip` / `:143 chipWidth`；`markdown_module.cpp:170 estimateTextWidth` 在 `:503`/`:1102` 用于同款 chip；`claude_module.cpp:537`/`:1174` 手写 `.radius(radiusPill)` | 绘制 1 份 + **3 套互不相同的宽度启发式**：markdown `ASCII=fontSize*0.55, CJK=fontSize/3`；todo `ASCII=5.5, CJK=4.0`（忽略 fontSize）；claude 无独立估算器（用元素自动宽） |
| D5 | **设计数值散落为字面量**（token 只覆盖字号/圆角） | token 用量：`fontSizeSmall` ×78、`radius` ×55、`fontSizeBody` ×52；对比字面量：`40.0f` ×**95**（`spacingXXLarge` 仅 ×7）、`16.0f` ×**158**（`spacingSmall` 仅 ×4）、`24.0f` ×58（`spacingLarge` ×18）、`12.0f` ×103、`14.0f` ×59 | 结论：**字号/圆角已 token 化，间距与坐标几乎全裸** |
| D6 | **跨模块 extern 桥（违反 R2）** | `settings_module.cpp:48/55-57/78-83/413/514/1150/971-1045/1263` | `module_settings` 同时 `#include "claude_service.h"` 直调 `::module::claude::ClaudeService::global()` **3 处**，并 forward-declare `namespace module::pet{discoverSkins/applyPetSkin/resetPetPosition/currentSkinName}` **4 个自由函数**靠链接期解析。全仓仅此一模块有跨模块引用 |
| D7 | **同一常量两处定义 + 注释约束"必须字面一致"** | `claude_service.h:260-262`（`kDefaultProxyPort=8888/kMinProxyPort=1/kMaxProxyPort=65535`）vs `settings_config.h:52-54`（同名同值，全局命名空间） | 2 份；`claude_service.h:258-259` 注释自陈"两模块不能互相依赖，故各自定义同值常量" |
| D8 | **页面名两份来源** | `main.cpp:359-368 trayPageDisplayName()` switch 返回短名（首页/笔记/待办/Claude/设置）；各模块 `Module::displayName()` 返回长名（`markdown_module.cpp:183` "Markdown 笔记本"、`todo_module.cpp:172` "Todo 看板"、`claude_module.cpp:121` "Claude 切换器"） | 2 套命名源；托盘与侧边栏用不同表 |

**次要重复（不单列里程碑，随所属模块顺手收）**：`refreshDataCache`/`reloadAll`/`refreshStats` 三种刷新语义各写各的；空状态文案（`markdown_module.cpp:568/578`、`todo_module.cpp:888/1023`、`claude_module.cpp:375/897`）无统一走查点。

### 3.4 技术债登记（backlog §四）逐条核实结论

> **重要**：backlog §四 是**登记时快照**，实测已有 3 项修完、1 项在代码中查无此项。下表为逐条取证结果，重构不得按原文照搬。

| # | 登记项 | 核实结论（file:line） | 归属 |
|---|--------|----------------------|------|
| A | 设置页 openUp 判定魔数 | **已修**：已抽成具名常量 `settings_module.cpp:145-160`（`kDropdownPopupGap`/`kDropdownItemHeight`/`kDropdownPopupPadding`…），注释自陈"tech-debt A，原为裸魔数" | 已闭环 |
| B | Todo 拖拽 pressY/scrollOffset 时刻不一致 | **已修**：`todo_module.cpp:555-557` 注释自陈"tech-debt B：必须用按下时刻的快照"，`drag_.pressScrollOffset` 与 `pressY` 同帧快照（`:2345-2347`） | 已闭环 |
| C | `md.tree.overlay` 布局语义 | **查无此项**：全仓 grep `md.tree.overlay` 零命中；该 id 或其语义已被重构掉 | 待验证（§9 V2） |
| D | `keychain_posix deleteSecret` 吞错 | **已修**：`keychain_posix.cpp:61-67` 注释自陈"tech-debt D：原先 ec 被吞"，已对齐 `keychain_win` 语义 | 已闭环 |
| E | mtime 秒级粒度 | 成立且是设计边界：`markdown_service.cpp:106-116` 转 unix 秒；但已有事件监听缓解（`:136`、`:182-183` 注释"事件确证文件动过 → force 跳过 (size,mtime) 短路"） | 重构不做（§7） |
| F | 带重音符文本轻微跨行 | EUI-NEO 文本排版（上游 `KNOWN_LIMITATIONS.md` #8） | 上游/不属重构 |
| G | EUI-NEO `SegmentedBuilder` 无 position API | 上游能力缺失 | 上游/不属重构 |
| H | segmented 只支持纯文本（三态标签去符号偏离 spec §5.7） | 上游能力缺失；现状见 `markdown_module.cpp:358-359` 注释"segmented 只支持纯文本，故整体去符号" | 上游/不属重构 |
| I | toast 单实例接续 | 上游 toast 队列限制；现状在 `markdown_module.cpp:1776/2184/2801` 以状态位节流 | 属 D1 里程碑的**约束**（重构必须保留节流语义） |
| J | 桌宠整窗 bbox 命中（M3 逐像素） | `pet_window.cpp:333` 注释"MVP 命中 = 整窗（O1 逐像素 alpha 是 M3）" | 功能债，非重构项 |

### 3.5 现状中"已经对了、不要动"的部分（避免重构扩大化）

- 数据层：SQL 全部收在 `*_service.cpp`（`markdown_service.cpp` 41 处 / `todo_service.cpp` 39 / `claude_service.cpp` 31），UI 模块 `.cpp` 内**零 SQL**（grep `"(SELECT|INSERT|UPDATE|DELETE` 对 `*module.cpp` 零命中）
- 平台层：`keychain/paths/time/file_open/file_watcher` 已按 `*_win.cpp`/`*_macos.cpp`/`*_posix.cpp` 拆完，业务代码零平台宏
- markdown 纯函数层：`md_block_model` / `md_keyboard` / `md_highlight` / `md_sync_map` / `md_tree` / `md_view_state` / `md_line_scanner` / `md_preview_highlight` / `md_block_interact` 共 9 个单元已是"可单测纯函数"形态 —— **这是本重构在其他模块复制的模板**
- settings 卡片：类内 `cardFrame`（`settings_module.cpp:715`）已被 9 处复用，是模块内去重的正面样例

---

## 4. 目标形态

### 4.1 关键架构决策：新增 `src/ui_kit/`（presentation 能力层）

**问题**：D1/D2/D4 的重复跨 3~4 个模块，而 R2（模块互不依赖）禁止放任一 `module_*`，R3（core 不可调 EUI 运行时）禁止放 `core`。

**裁决**：新增 `src/ui_kit/`，`add_library(devdesk_ui_kit STATIC)`，依赖 `devdesk_core` + `eui_neo`，被 `module_*` 与 `app_sidebar` 链接；`core`/`platform` **不得**反向依赖它。

| 层 | 关系（新增后） |
|----|----------------|
| `module_*` / `app_sidebar` / `main` | 可依赖 `core` + `platform` + **`ui_kit`** |
| `ui_kit` | 可依赖 `core`（`Result`/`Tokens`/`ThemeColorTokens`）+ EUI-NEO 组件；**不可依赖任何 `module_*`** |
| `core` / `platform` | 不可依赖 `ui_kit`（保持 spec §4.2.1 边界不动） |

**为什么不放 core / 不放 EUI-NEO 上游**：
- 不放 core：R3 明令禁止（`components::dialog` 是需要 eui_neo 实体符号的运行时能力）
- 不直接提 EUI-NEO 上游：`ui_kit` 封装的是 **DevDesk 的设计 token 组合**（28px 内边距 / 460px 标准弹窗宽 / `w-56` 正文宽 / 方案 B 色板），CLAUDE.md 明列"设计 token / 配色方案（DevDesk 自己的事）"**不属**上游范畴。真正属上游的通用能力缺失已在 §3.4 F/G/H 单列

**落什么**（只收真正跨模块重复的，不收单模块专用）：
| 单元 | 收编对象 | 依据 |
|------|----------|------|
| `ui_kit/toast.h/.cpp` — `ToastState`（`open/title/body/isError` + `show()`）+ `drawToast(ui, id, state, tokens)` | D1 的 4 份 | 单实例节流语义由状态位保留（对齐 §3.4 I） |
| `ui_kit/dialog_form.h/.cpp` — `DialogFrame`（标准宽 460/420/560/720/760 具名枚举 + 标题行 + 正文行 + 按钮对 + 校验错误行） | D2 的 13 处 | 保留每个对话框现有的尺寸与文案（R6） |
| `ui_kit/card.h/.cpp` — `cardFrame` 提升为公共（现 `settings_module.cpp:715`） | D2 的卡片侧 + settings 9 处改调公共版 | 现成实现，零行为变化 |
| `ui_kit/chip.h/.cpp` — `drawChip` + **单一**宽度估算 `estimateTextWidth(text, fontSize)` | D4 全部 | 3 套启发式收敛为 1；取值以最保守者为准（宽度只影响留白，见 §9 V3） |
| `ui_kit/metrics.h` — 具名尺寸常量：`kHeaderH=64`、`kDialogInsetX=28`、`kDialogBodyW=w-56`、`kNavItemH=40`… | D3、D5 的常量侧 | R5：纯 header/常量，无新依赖 |

**明确不收**：`drawHeader`（各模块标题区结构差异大，强行统一 = 改行为）、几何计算（`BoardGeom`/`MdSyncMap` 属模块领域）、`fs()` 字号缩放（settings 私有的全局缩放语义，见 §9 V4）。

### 4.2 各模块改造后形态

统一目标形状（照抄 markdown 已验证的模板）：

```
module_X/
  X_module.cpp        只留：Module 接口实现（pageId/displayName/compose 骨架）、
                      状态字段、事件回调接线 —— 目标 ≤600 行，
                      compose ≤120 行（只做分区委派，不做内联装配）
  X_ui_<area>.cpp     每个可视区域一个文件（面板/对话框/表格/卡片），
                      全部是 (ui, screen, tokens, 只读状态) → void 的自由函数或小类
  X_ui_logic.h/.cpp   不碰 Ui、不碰 DB 的纯函数（几何计算/格式化/排序/命中测试）
  X_service.*         不动
```

| 模块 | 拆分切法（file:line — 动作） | 预计形态 |
|------|------------------------------|----------|
| `claude_module.cpp` | `:131 compose`(859) → compose 骨架 + `claude_ui_profile.cpp`（`:1112 drawProfileCard`/`:131` 内列表段）+ `claude_ui_form.cpp`（`:1479/1555/1636/1839` 四个 form tab/dialog）+ `claude_ui_logs.cpp`（`:1995 reloadCallsPage`/`:2034 drawCallsDialog`）+ `claude_ui_logic.h`（`:51/61/79/98` 四个格式化/推导纯函数） | 2454 → 5~6 文件，最大 ≤600 |
| `todo_module.cpp` | `:2016 drawDetailDialog`(310)/`:1827 drawOverdueDialog`/`:1249 drawNewTaskDialog`/`:1379 drawNewColumnDialog`/`:1632 drawRenameDialog`/`:1712 drawConfirmDeleteDialog` → `todo_ui_dialogs.cpp`；`:700 drawHeader`/`:845 drawBoard`/`:934 drawColumnContent`/`:1063 drawCard` → `todo_ui_board.cpp`；`:89 boardGeom`/`:101 cardLeft`/`:105 cardTop`/`:143 chipWidth`/`:2339 DragState` 命中测试 → `todo_ui_logic.h`（与既有 `todo_ui_logic.h` 合并） | 2432 → 5 文件 |
| `settings_module.cpp` | `:494 compose`(163) → 卡片注册表驱动（`:729/880/966/1078/1144/1310/1431/1530/1597` 共 9 张 `draw*Card`）；`cardFrame:715` 迁 `ui_kit`；`:177 nearestIndex`/`:661 fs` → `settings_ui_logic.h` | 1704 → 4 文件 |
| `markdown_module.cpp` | `:1253 drawPreviewColumn`(212)/`:430 drawTreeColumn`(192)/`:762 drawOutlinePanel`(120)/`:885 drawEditorColumn`(98) → `md_ui_*.cpp`；`:1596/1685/2567` 对话框 → `md_ui_dialogs.cpp`；`:170 estimateTextWidth` 迁 `ui_kit`；`:303 drawHeader`(98) → `md_ui_header.cpp` | 2847 → 7~8 文件 |
| `home_module.cpp` | `:188 compose`(369) 拆 4 张统计卡 + 最近编辑表 + 快捷操作（`:235-560`），`:166 refreshStats` 保留 | 581 → 3 文件 |
| `app_sidebar.cpp` + `main.cpp` | `main.cpp:359 trayPageDisplayName` 与 `Module::displayName()` 双命名源合一（D8）：`Module` 加 `shortName()` 默认回落 `displayName()`；`:350-353 Action→PageId` / `:425-434` 托盘分支 / `:188-217` 注册块 → 一张 `{PageId, 短名, 图标, 工厂, 门控键}` 表驱动（D8 的根因消除） | 表驱动，`main.cpp` 687 → ≤550 |
| `module_pet` | 最健康（子窗/皮肤/帧/气泡已分层）；只做 `:308 petCompose`(177) 的区域拆 | 可选，最低优先 |

### 4.3 模式选用 / 不选用

**真正需要（全部是"已经在用、只是没推广"的）**：

| 模式 | 用在哪 | 为什么必须 |
|------|--------|-----------|
| 组合 + 单一职责切分 | 全模块 | 唯一能同时满足 C2 与 C3 的手段：移动代码不改行为，测试护栏即可证伪 |
| 值类型承载几何/尺寸（struct + 具名常量） | `ui_kit/metrics.h`、`X_ui_logic.h` | 消除 D3/D5 的裸字面量，编译器可查 |
| 自由函数 + 显式入参（"函数"而非"类"） | 全部 `*_ui_*.cpp` / `*_ui_logic.h` | 项目既有约定（`md_*` 9 个单元全是自由函数），可无头单测 |
| Facade（`ui_kit` 一个门面收编 dialog/toast/card/chip） | D1/D2/D4 | 3 个模块共用，不用 Facade 就得三份 include 编排 |
| Strategy 注入（既有先例：`LineCountFn`，phase3 spec §4.7） | 需要"测量函数注入"的新纯函数 | 保持纯函数可单测，不把 EUI-NEO 依赖拖进 logic 层 |

**明确用不上（防止为模式而模式）**：

| 不用的模式 | 为什么不用 |
|-----------|-----------|
| 抽象工厂 / 工厂方法 | 模块创建已是 `extern "C" create_X_module()` 一行（`main.cpp:37-41`）；再包一层无任何可变点 |
| 观察者 / 事件总线 | 模块间通知已由 `Router::navigateTo` + 单槽 payload（`router.h:55`）解决；总线引入隐式耦合 + 生命周期风险，违反 R2 精神 |
| 单例（**新增**） | 现存 `ClaudeService::global()`（`claude_service.h:246-247`）与 `core::appRouter()` 是**待消解的债**（§5 R0），不是模板。重构目标是减少单例 |
| 继承层次（除既有 `core::Module`） | 模块之间无 is-a 关系；用组合 |
| 模板元编程 / CRTP / 编译期多态 | CLAUDE.md §2.3 明列"避免高级 C++ 黑魔法"，用户不熟 C++；可读性优先 |
| Pimpl / ABI 隔离 | 单 exe 全静态链接，无 ABI 边界需求 |
| Visitor / 双分派 | 无此形状的领域模型（节点层次固定在 markdown 块模型，且已是扁平 struct 数组） |
| Command 模式做 undo | markdown phase3 已选定**快照栈**（`MdLiveUndoStack`，phase3 spec §4.5），够用且更简单 |
| State 模式做模式机 | `ViewMode` 是 4 态线性 enum + switch（phase3 spec §3.1）；桌宠四态机已存在，两者都不动 |
| 装饰器链 / 中间件 | `proxy_router` 的转发链是单层直调，无嵌套需求 |
| DI 容器 | 现有 main 手工装配（`main.cpp:188-217`）即够；容器属过度设计 |

---

## 5. 里程碑拆分

### 5.1 前置条件（必须先满足，否则不许开工）

| # | 前置 | 理由 |
|---|------|------|
| P1 | 工作树 37 项在飞改动全部 commit 并合 master，`git status` 干净 | 当前 `feat/ui-batch2-2026-09-25` 有 30 个已改文件 + 7 个 untracked；重构必须在干净基线上做单 commit 可 revert |
| P2 | `2026-09-27-markdown-phase3-block-render-design.md`、`2026-09-27-profile-reverse-sync-and-copy-design.md` 两条在飞 spec **实施完毕并合 master** | 两者分别大改 `markdown_module.cpp`（+430 行）与 `module_claude_proxy/*` 6 文件；与 R4/R7 直接撞车 |
| P3 | 全量基线记录：`bash scripts/build-test-run.sh --no-run` 输出 `Total/Passed/Failed` 三行存档 | C3 的护栏基线；当前静态计数 494 处 `REGISTER_TEST`（§9 V1） |
| P4 | 每个里程碑开 worktree（`git worktree add`），独立 build 目录（`-DEUI_NEO_DIR=D:/project/gui/EUI-NEO`） | 编排板既有约定；避免撞 build（同一时刻只一个 agent 持 build） |

### 5.2 里程碑总表

顺序 = R0 → R1 → {R2, R3} → R4 → {R5, R6} → {R7, R8} → R9。括号内可 worktree 并行。

| # | 里程碑 | 范围（file:line — 动作） | 为什么独立可验收 | 风险 | 验证方式 | 依赖 |
|---|--------|--------------------------|------------------|------|----------|------|
| **R0** | 跨模块耦合消解（D6/D7） | `settings_module.cpp:48/55-57/78-83` 删 `#include "claude_service.h"` 与 4 个 `module::pet` forward-decl；改为 `core` 新增只读服务定位器（`core/service_locator.h`，模块 onStart 时自注册，`Result<IFoo*>` 取用）；`:413/514/1150/971-1045/1263` 6 处调用点改走定位器。D7：端口常量单一来源定义在 `core`（`core/proxy_port.h`，纯常量头，不违反 R3），两处各自包含 | 只动引用方式不动 UI/业务逻辑；`shared_ptr` 生命周期由 main 拥有（现状已是 static unique_ptr）。定位器缺失时返回 fail 而非空指针 | 低（无 UI 改动） | 定向：`test_settings`（53）+ `test_settings_sync` + `test_module_gating`（11）；全量全绿；**不需肉眼** | P1-P4 |
| **R1** | `ui_kit` 基座 + Toast 统一（D1） | 新增 `src/ui_kit/{CMakeLists.txt,toast.h,toast.cpp}`；顶层 `CMakeLists.txt` 加 `add_subdirectory(src/ui_kit)`；`claude_module.cpp:2249/2265`、`markdown_module.cpp:1782/1815`、`todo_module.cpp:616`、`app_sidebar.cpp:294` 四处改调 `ui_kit::drawToast(ui, id, state, tokens)` | 基座 + 一类横切，4 处改动互相独立；`ui_kit` 只有 1 个功能，可单独回退且不影响其他模块 | 中（toast 单实例节流语义 `markdown_module.cpp:1776/2184/2801` 必须逐字保留） | 定向：`test_markdown`（64）、`test_todo_ui`（50 中的 toast 相关）、`test_claude_ui`、`test_app_sidebar`（5）；全量全绿；**需肉眼**：四个模块各触发一次 toast（成功/失败两态） | R0 |
| **R2** | 对话框表单脚手架统一（D2） | `ui_kit/dialog_form.{h,cpp}`（标准宽具名枚举 + 标题行 + 正文 + 按钮对 + 校验错误行）；13 处调用点替换：`claude_module.cpp:1839/1928/2034`、`markdown_module.cpp:1596/1685/2513/2571`、`todo_module.cpp:1249/1379/1632/1712/1827/2016`。**按模块分 3 批**（todo 6 → markdown 4 → claude 3），每批全绿才进下一批 | 每批 = 一个模块的对话框，可单独 revert；批间零共享状态 | 中高（13 个对话框的尺寸/文案/按钮序必须逐字不变；R6 最易在此破） | 定向：该模块 `test_*_ui.cpp` 全部；全量全绿；**需肉眼**：每批逐对话框开/关一次（含 Esc、错误行、层叠弹窗 zIndex，见 `todo_module.cpp:1737-1739` 先例） | R1 |
| **R3** | 设计数值收口（D3/D5） | `ui_kit/metrics.h`（`kHeaderH`、`kDialogInsetX`、`kDialogStandardWidths`、`kNavItemH`…）；`claude_module.cpp:138/154`、`settings_module.cpp:502/563`、`todo_module.cpp:97/640`、`markdown_module.cpp:322` 的表头几何改用具名常量；后续逐模块把 `40.0f/24.0f/16.0f` 等**布局类**字面量换 `t.sizes.spacing*`。**按模块分 4 批** | 纯数值替换，无控制流变化；每批只碰一个模块 | 中（视觉回归密集，且"看起来没变"最难自证） | 定向：该模块全部用例；全量全绿；**必须肉眼 + 截屏**：每批前后同窗口尺寸各截一张逐像素比对 | R1（用 `ui_kit/metrics.h`） |
| **R4** | `claude_module.cpp` 拆分 | 见 §4.2 第一行；`:131 compose`(859) 拆为骨架 + 4 个 `claude_ui_*.cpp` | 单模块闭环；`test_claude_ui` 覆盖该模块全部可视区域 | 中高（859 行 compose 是最大单点；闭包捕获变量众多，搬迁时漏捕获 = 编译错而非静默错，风险可控） | 定向：`test_claude_ui`（该文件全部）+ `test_claude*`（32）；全量全绿；**需肉眼**：Profile 列表/详情/表单三 tab/日志弹窗逐页点验 | R1,R2,R3;P2 |
| **R5** | `todo_module.cpp` 拆分 | 见 §4.2 第二行；6 个对话框 → `todo_ui_dialogs.cpp`，看板 → `todo_ui_board.cpp`，几何/命中 → `todo_ui_logic.h` | 单模块闭环；`test_todo_ui`（50）覆盖 | 中（拖拽状态机 `:2339 DragState` 搬迁时 `pressX/pressY/pressScrollOffset` 同帧快照语义必须保持，见 §3.4 B） | 定向：`test_todo_ui` + `test_todo_service`；全量全绿；**需肉眼**：拖拽跨列 + 6 个对话框 + 搜索/逾期视图 | R2,R3（可与 R4 并行） |
| **R6** | `settings_module.cpp` 拆分 + 卡片表驱动 | 见 §4.2 第三行；9 张卡片改注册表遍历；`:715 cardFrame` 迁 `ui_kit/card.h`；`:661 fs`/`:177 nearestIndex` → `settings_ui_logic.h` | 单模块闭环；`test_settings`（53）+ `test_editor_font` + `test_font_em_layout` 覆盖 | 中（卡片高度常量 `:171-173` 与 openUp 几何 `:145-160` 耦合，搬迁时须保持常量同源） | 定向：`test_settings`+`test_font_em_layout`+`test_editor_font`；全量全绿；**需肉眼**：9 张卡片 + 3 个下拉的 openUp 上下展开 | R1,R3;P2(profile-sync 不碰 settings，但 R0 已改此文件) |
| **R7** | `markdown_module.cpp` 拆分 | 见 §4.2 第四行；`:170 estimateTextWidth` 迁 `ui_kit`（并入 D4 统一估算器），调用点 `:503/1102` 改调 | 单模块闭环；`test_markdown`（64）+ 8 个 `test_md_*`（64）覆盖，护栏最厚 | 中（phase3 刚改过此文件，须在 P2 之后；`drawPreviewColumn` 212 行含预览/实时共用的逐块渲染路径） | 定向：`test_markdown` + 全部 `test_md_*`；全量全绿；**需肉眼**：文件树/大纲/编辑器/预览四区 + 4 个对话框 + chip 宽度变化（§9 V3） | R2,R3;P2（可与 R6 并行） |
| **R8** | `home_module` + `app_sidebar` + `main.cpp` 收口（D8） | `home_module.cpp:188 compose`(369) 拆 4 区；`main.cpp:359-368` 双命名源合一（`Module::shortName()` 默认回落 `displayName()`）；`:350-353`/`:425-434`/`:188-217` 合为一张 `{PageId, 短名, 图标, 工厂, 门控键}` 表 | 应用级装配，不碰模块内部；`test_home`（5）+ `test_app_sidebar`（5）+ `test_module_gating`（11）+ `test_tray_dynamic`（2）覆盖 | 中（托盘短名与侧边栏长名**当前确实不同**，合一时必须保留两套取值，只是改为单一声明点——R6 行为不变） | 定向：`test_home`+`test_app_sidebar`+`test_module_gating`+`test_tray_dynamic`+`test_menu_beautify`（15）+ `test_module_icons`；全量全绿；**需肉眼**：托盘菜单四项 + 侧边栏四项 + 模块开关插拔 | R1,R3（可与 R7 并行） |
| **R9** | `module_pet` 区域拆 + chip/卡片收编 | `pet_window.cpp:308 petCompose`(177) 拆区域；`:333 bbox` 注释保留不动；`pet_skin.cpp` 无 UI 重复，不动 | 单模块；`test_pet`（33）覆盖；**优先级最低**，可在任意时点插入或整体放弃 | 低 | 定向：`test_pet`；全量全绿；**需肉眼**：桌宠四态 + 气泡 + 换肤（子窗截图受限，见 backlog 桌宠条目） | 任意（建议最后） |

### 5.3 可并行组合（每个 worktree 独立 build 目录）

| 并行组 | 成员 | 冲突面 |
|--------|------|--------|
| 组 1 | R4 ‖ R5 | 分属 `module_claude_proxy` / `module_todo`，零文件重叠；但**同触 `ui_kit`**（只读复用），故需 R1-R3 已合 master |
| 组 2 | R6 ‖ R7 | 分属 `module_settings` / `module_markdown`，零重叠 |
| 组 3 | R8 ‖ R9 | 分属 `main/app_sidebar/home` / `module_pet`，零重叠 |

**串行强制**：R0 → R1 必须先单独验证（R1 是全项目第一次引入第 5 个库，构建图变更要单独一次闭环）；R2/R3 可并行但**同触 13 个对话框文件**，故 **R2 与 R3 串行**（建议 R2 先，因 R2 会把 `28.0f, 24.0f` 等字面量集中到 `ui_kit`，R3 的替换面因此变小）。

### 5.4 顺序一览（一行）

**R0 跨模块耦合 → R1 ui_kit+toast → R2 对话框（3 批）→ R3 数值收口（4 批）→ R4 claude ‖ R5 todo → R6 settings ‖ R7 markdown → R8 应用装配 ‖ R9 pet**。

---

## 6. 回退策略

| # | 场景 | 回退动作 |
|---|------|----------|
| B1 | 单里程碑失败 | 每里程碑 = 1 个原子 commit（R2/R3 为每批 1 个 commit）；`git revert <sha>` 即可，无跨里程碑状态残留 |
| B2 | R0 失败（服务定位器不成熟） | 回退到 forward-declare 现状。**注意**：R0 是唯一"改变链接期契约"的里程碑——其定位器取代了 4 个 `module::pet` 自由函数与 3 处 `ClaudeService::global()`，回退必须整块 revert，不允许只回退一半（半套定位器 + 半套 extern 会变成两套真值） |
| B3 | R1 失败（ui_kit 引入构建图问题） | 保留 `ui_kit` 目录但先从 `top CMakeLists.txt` 摘除 `add_subdirectory`，各模块回自有 `drawToast`；基座与消费方**分 commit**便于此操作 |
| B4 | R2/R3 中途某批失败 | 只 revert 该批；已通过批次保留（批间零共享状态是 R2/R3 拆批的**唯一目的**） |
| B5 | R4-R9 单模块失败 | 单模块 revert；`ui_kit` 与已完成的 R0-R3 不受影响 |
| **不可回退项** | `Module::shortName()` 新虚函数（R8） | 一旦合入即成为 `core::Module` 接口的一部分，其他模块的 `Module` 实现都需保持可编译；回退需同时删虚函数与所有 override → **建议 R8 把"加虚函数"与"改用虚函数"拆成 2 个 commit** |
| **不可回退项** | D7 常量单一来源（R0） | 唯一来源若写错值，两处同错（现状是"两处同值"，错也是同错）——非回退问题，但需在 R0 验收加一条：断言 `claude_service.h` 与 `settings_config.h` 的端口取值仍为 8888/1/65535 |

**通用守护**：
- 每个里程碑合入前 `git diff --stat` 必须只含**预期文件**；出现预期外文件 = 立即 STOP（对齐 CLAUDE.md 集成纪律）
- R2 的风险点是文案/尺寸被"顺手优化"——review 时对 13 个对话框逐个 diff 文案字符串与 `.size()` 数值
- R3 的风险点是字面量替换时取错 token 档位（如 `spacingLarge=24` 误取 `spacing=8`）——review 时逐一对照 `core/theme.h:469-485` 的 `SizeTokens` 定义

---

## 7. 不做清单（明确划界）

| # | 不做 | 理由 |
|---|------|------|
| N1 | 不动 EUI-NEO 的公开 API / 不改 `../EUI-NEO/` 任何代码 | R4 上游优先 ≠ 本次重构范围；上游项（F/G/H）单独排期 |
| N2 | 不重写 SQLite 层 / `core::data::SqliteDb` / `Database` / `migrations.h` | 数据层当前健康（§3.5）；spec §6.3 的设计未变 |
| N3 | 不重写 service 层（`*_service.cpp`） | SQL 已收口且单测最厚；重构收益低于风险 |
| N4 | 不引入任何新第三方依赖（含 boost / fmt / ranges-v3 / DI 容器） | R5 + 单 exe 零外部 DLL |
| N5 | 不改 `HomeService` 直查他模块表的既有设计 | spec §8.4 **明确sanctioned**（"直接通过 SqliteDb 查询聚合数据"）；改动 = 违背权威 spec |
| N6 | 不改平台适配层文件划分（`*_win/_macos/_posix`） | 已完成且正确 |
| N7 | 不改任何产品行为：文案、数值、层级顺序、快捷键、持久化格式、日志文本 | R6/C2；发现缺陷只登记入 backlog 不修 |
| N8 | 不删 `KNOWN_LIMITATIONS.md` 已登记的边界行为（LRU 8 / mtime 秒 / toFRB 等） | 属已接受的设计边界 |
| N9 | 不为重构补写新功能测试；只在**抽出的纯函数**上补等价断言 | C3；纯函数抽离后加断言是"证明搬迁等价"，不是新功能 |
| N10 | 不动 `module_pet` 的 M3 逐像素命中（`:333`） | 功能债非重构项（§3.4 J） |
| N11 | 不做跨模块"统一 UI 风格走查"（空状态文案等 D8 之外的次要重复） | 统一文案 = 改产品行为，违反 R6 |

---

## 8. 需用户拍板的开放问题（≤3 条）

| # | 问题 | 选项 | 影响 |
|---|------|------|------|
| Q1 | **R2/R3 的 13 个对话框 + 数值替换，是否接受"逐批肉眼点验"？** | A 接受（推荐）：每批截屏 + 手工开关，共 7 批 | 选项 A 可确保 R6 不被破坏；B 只跑测试不点验——toast/dialog 是 EUI-NEO 组件，headless 用例无法覆盖真实渲染，视觉回归会漏 |
| | | B 只跑测试 + 全量全绿，不点验 | |
| Q2 | **R9（`module_pet` 拆分）做不做？** | A 做（R9 保留）| 桌宠是当前最"新"的模块、代码最干净、子窗截图受企业水印层限制无法自动验证（backlog 已记）——做它的收益最低、验证最难 |
| | | B 整体放弃（推荐）：`module_pet` 只在 D1/D4 消费点改调 `ui_kit`，内部不拆 | |
| Q3 | **`Module::shortName()`（R8 的双命名源合一）是否接受改 `core::Module` 接口？** | A 接受（推荐）：加带默认实现的虚函数，D8 根因消除 | B 选项下 D8 保留为"已知双源"，只在 `main.cpp` 加注释约束；A 是唯一能真正消除 D8 的做法，代价是 `core::Module` 接口变更（§6 不可回退项） |
| | | B 不改接口，D8 保留登记 | |

### 8.1 已定决策（2026-09-27，用户拍板）

| # | 决策项 | 结论 | 含义 |
|---|--------|------|------|
| D-R1 | R2/R3 的 toast/对话框批次（共 7 批） | **接受逐批肉眼点验** | 每批改完由用户点验一次；理由：toast/dialog 是 EUI-NEO 组件，headless 用例覆盖不到真实渲染 |
| D-R2 | R9（`module_pet` 内部拆分） | **放弃** | 该模块最干净、收益最低；子窗截图受企业水印层限制无法自动验证 |
| D-R3 | 给 `core::Module` 加 `shortName()` | **接受** | 消除「页面名双来源」；代价是接口变更（已列为不可回退项，建议拆 2 个 commit 落地） |

以上三条为实施前置约束。R9 从里程碑清单中移除后，可并行组相应变为 {R4,R5}、{R6,R7}、{R8}（原 {R8,R9} 中的 R9 不做，R8 单独一组）。

---

## 9. 待验证项（实现前须先取证，附验证方法）

| # | 待验证 | 为什么关键 | 验证方法 |
|---|--------|-----------|----------|
| V1 | **测试基线到底是 485 还是 494** | C3 的准入门槛数字。需求给 485；静态扫描 `tests/**/*.cpp`（52 个编入 `tests/CMakeLists.txt` 的文件）得 **494** 处 `REGISTER_TEST(`，差 9 处来源未查明（`DEVDESK_SOAK=1` 门控 1 处已知） | 跑 `bash scripts/build-test-run.sh --no-run`，读输出末三行 `Total/Passed/Failed`；与 494 的差额逐一定位（可能：静态注册被 `#if` 排除 / 计数器语义差异） |
| V2 | backlog §四「md.tree.overlay 布局语义」是否仍存在 | 决定该债是"已闭环"还是"被遗漏" | 全仓 `grep -rn 'tree.overlay\|md.tree' src/`；再查 `git log --all -S'md.tree.overlay' --oneline` 看是否已被删除 |
| V3 | D4 收敛后的单一宽度估算器是否会改变现有 chip 间距 | 3 套启发式取值不同（markdown `0.55/3.0`、todo `5.5/4.0`），收敛到一套必然有一侧留白变化 = **可能违反 R6** | 收敛前对 `markdown_module.cpp:503/1102`、`todo_module.cpp:1171/1181/2178` 五个调用点的当前 chip 宽做回归快照；收敛后逐点比对。若差异 >2px 则改为**保留两套入参**、只统一函数签名（"同一实现 + 两套系数"） |
| V4 | `settings_module.cpp:661 fs()` 全局字号缩放的语义边界 | `ui_kit` 若收编 `fs` 会让其他模块获得全局缩放能力 = **改行为**（R6） | 读 `settings_module.cpp:661` 上下文 + `test_font_em_layout` / `test_editor_font` 断言；确认它是 settings 私有语义后**明确不收编**（§4.1 已按此裁决，此处只作留痕） |
| V5 | `ui_kit` 的 `PUBLIC` include 传播是否会让 `core`/`platform` 意外获得 EUI-NEO 组件头路径 | R3 边界靠"取不到"来强制，取得到就会被误用 | R1 落地后：`cmake --build` 通过 + 检查 `devdesk_core` 的 `INTERFACE_INCLUDE_DIRECTORIES` 不含 `ui_kit`；加一条 grep 断言 `src/core/` 与 `src/platform/` 下零 `ui_kit/` include |
| V6 | **R0 的边界必须精确到 R2 违规处，不许扩大** | 全仓 `ClaudeService::global()` 共 **17 处调用**：`main.cpp` 2（装配层，合法）、`claude_module.cpp` **11**（同模块内读自己的 service，属「单例债」但**不构成 R2 违规**）、`settings_module.cpp` **3**（跨模块，**违规**）；另 `claude_service.cpp:115` 是定义。R0 只许动 settings 那 3 处 + 4 个 `module::pet` forward-decl | 先 `grep -rn 'ClaudeService::global()' src/` 取准数并逐条定性（同模块 / 跨模块）；R0 验收断言：`settings_module.cpp` 零 `module::claude` / `module::pet` 引用（现为 `:78-83`、`:413`、`:511`、`:1154`、`:971-1045`、`:1263`——按 V7 重新定位）；`claude_module.cpp` 的 11 处**不在 R0 范围**，若要一并收敛须单开里程碑 |
| V7 | **所有 file:line 会漂移**（设计期已实测：`settings_module.cpp` 里 `ClaudeService::global()` 的调用点在一次设计会话内从 `413/514/1150` 漂到 `413/511/1154` —— 在飞 agent 正在改此文件） | R4/R5/R6/R7 的范围表全部基于 2026-09-27 快照行号 | R0 开工前重跑一次 §3.2 的函数规模扫描 + §3.3 的重复模式取证，刷新全部行号；**禁止**直接沿用本 spec 的行号盲改。行号只作「定位线索」，动手前必须按符号名重新定位 |

---

## 附：记录（实施期回填）

| 里程碑 | 状态 | commit / 证据 |
|--------|------|---------------|
| （全部） | 未启动（本文件为设计，未 commit） | — |

---

**Spec 版本**：1.0（2026-09-27，仅设计）
**上游依据**：`2026-09-20-devdesk-design.md` §4.2/§4.2.1（依赖规则与 core 边界）
