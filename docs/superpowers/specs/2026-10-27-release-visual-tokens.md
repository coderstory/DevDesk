# 上线模块视觉 Token 清单（浅色）

> **用途**：作为 C++ 实现与 HTML 原型的唯一共同视觉基线。原型按本文改，C++ 按本文取值；双方都不再各自引用旧设计文档。
>
> **来源纪律**：本文全部数值从当前源码抽取并标注 `file:line`。`2026-10-21-devdesk-design-system-v1.md`、`wechat-ui-demo.html`、`release-module-demo.html` 的 CSS 变量一律**不是**事实来源（前两者已被代码覆盖，原型仍带旧 Claude Clay 配色，可能正被另一任务重写）。
>
> **单位**：逻辑 px（与窗口逻辑坐标同单位）。`1 CSS px = 1 逻辑 px`，换算见第 8 节。
>
> **注释陷阱**：`src/core/theme.h` 的结构体注释是旧口径（例如注释写 `fontSizeBody = 15`、`radius = 10`），**运行期真值以 `src/core/theme.cpp` 为准**（body = 14、radius = 8）。本文只引 `theme.cpp`。

---

## 1. 颜色（浅色）

| C++ token | 值 | 出处 | 典型用途 |
|---|---|---|---|
| `colors.background` | `#FAFAF9` | `src/core/theme.cpp:73` | 页面底、列容器底 |
| `colors.surface` | `#FFFFFF` | `src/core/theme.cpp:74` | 卡片/输入框/弹窗底 |
| `colors.surfaceElevated` | `#FFFFFF` | `src/core/theme.cpp:75` | 浅色下与 surface 同值（仅深色分层） |
| `colors.sidebarBg` | `#F0FDF7` | `src/core/theme.cpp:96` | 常驻侧边栏底（淡绿） |
| `colors.hover` | `#F3F4F6` | `src/core/theme.cpp:76` | 悬停底、次级按钮底、弱分割 |
| `colors.active` | `#E5E7EB` | `src/core/theme.cpp:77` | 按下底（弱化档） |
| `colors.primary` | `#36D399` | `src/core/theme.cpp:78` | 主色：主按钮/进度/选中竖线/焦点边框 |
| `colors.primaryHover` | `#2EC48F` | `src/core/theme.cpp:79` | 主色悬停 |
| `colors.primaryPressed` | `#23B07F` | `src/core/theme.cpp:80` | 主色按下 |
| `colors.primarySoftBg` | `#E6FBF2` | `src/core/theme.cpp:81` | 主色浅底（选中/chip/提示条/拖拽悬停） |
| `colors.primaryText` | `#157E4E` | `src/core/theme.cpp:82` | **主色系文字**（小字，≈4.87:1） |
| `colors.accentText` | `#0C7E58` | `src/core/theme.cpp:83` | Ghost/Text 按钮字色（≈4.86:1） |
| `colors.onPrimary` | `#FFFFFF` | `src/core/theme.cpp:84` | 主色填充上的前景字/图标 |
| `colors.textPrimary` | `#1F2937` | `src/core/theme.cpp:85` | 标题、正文主色 |
| `colors.textSecondary` | `#4B5563` | `src/core/theme.cpp:86` | 次级文字、导航未选中 |
| `colors.textTertiary` | `#6B7280` | `src/core/theme.cpp:87` | 三级文字、占位、弱提示 |
| `colors.textDisabled` | `#6B7280` | `src/core/theme.cpp:88` | 禁用文字（浅色下与三级同值） |
| `colors.borderDefault` | `#E5E7EB` | `src/core/theme.cpp:89` | 卡片/控件默认描边、侧栏分割线 |
| `colors.borderMuted` | `#F3F4F6` | `src/core/theme.cpp:90` | 弱分割（浅色下与 hover 同值） |
| `colors.success` | `#10B981` | `src/core/theme.cpp:91` | 成功、已完成勾选底、低优先级条 |
| `colors.warning` | `#F59E0B` | `src/core/theme.cpp:92` | 警告/待办/中优先级条 |
| `colors.error` | `#EF4444` | `src/core/theme.cpp:93` | 错误底/边框/圆点/高优先级条 |
| `colors.errorText` | `#B91C1C` | `src/core/theme.cpp:94` | **错误文字**（浅底上用 text 专用档） |
| `colors.info` | `#3B82F6` | `src/core/theme.cpp:95` | 信息/待执行态 |

**硬规则**：

- 主色 = `#36D399`（薄荷绿），**不是**原型里的 Claude Clay `#D97757`。原型旧配色必须整体替换。
- `primary` 浅色下作小字对比度不足（1.84:1），文字一律用 `primaryText` / `accentText`；`error` 作文字用 `errorText`。
- 有主色强调档 `purple #71639B` / `pink #C05E86` / `blue #12B7F5`（`src/core/theme.cpp:97-99`），只作图标/标签/装饰，禁作小字正文色。

---

## 2. 字号

`core::theme::applyFontScale` 会把下列 token 全部乘用户字号档（`src/core/theme.cpp:208-218`）。**硬编码字面字号必须包 `core::theme::scaled()`**（`src/core/theme.h:103`），否则不随字号档缩放。

| C++ token | 100% 值 | 典型用途（代码例） |
|---|---|---|
| `sizes.fontSizeSmall` | 12 | 次级说明/表格表头/统计标签（`src/module_home/home_module.cpp:97`）；chip 字 = 此值 − 1 |
| `sizes.fontSizeBody` | 14 | 导航项图标与文字（`src/app_sidebar.cpp:451`）、标题栏品牌（`src/app_sidebar.cpp:313`）、按钮字（`src/ui_kit/button.h:35`） |
| `sizes.fontSizeLarge` | 18 | 设置卡标题（`src/module_settings/settings_ui_cards.cpp:63`） |
| `sizes.fontSizeTitle` | 24 | 页面主标题（todo：`src/module_todo/todo_ui_board.cpp:110`） |
| `sizes.fontSizePage` | 24 | 设置页主标题（`src/module_settings/settings_module.cpp:1390`） |
| `sizes.fontSizeStat` | 28 | 首页统计数字（`src/module_home/home_module.cpp:109`） |
| `sizes.fontSizeDisplay` | 48 | **当前无消费方**（全仓 grep 仅定义/测试）；保留给超大数值，不要为上线模块新开用法 |
| `sizes.fontSizeWeChat` | 16 | 微信风格正文档，todo 列标题在用（`src/module_todo/todo_ui_board.cpp:413`） |

> 字号 token 的全部字段定义：`src/core/theme.cpp:112-120`（浅色）与 `:57-65`（深色）。

---

## 3. 圆角

| C++ token | 值 | 出处 | 典型用途 |
|---|---|---|---|
| `sizes.radiusSmall` | 6 | `src/core/theme.cpp:108` | 小控件、hover 块、拖拽占位条 |
| `sizes.radius`（base） | 8 | `src/core/theme.cpp:107` | 按钮（`src/ui_kit/button.cpp:81`）、todo 卡（`src/module_todo/todo_ui_board.cpp:728`） |
| `sizes.radiusLarge` | 12 | `src/core/theme.cpp:109` | 设置卡/列容器（`src/module_todo/todo_ui_board.cpp:87`） |
| `sizes.radiusOverlay` | 16 | `src/core/theme.cpp:110` | 模态/浮层/大卡片 |
| `sizes.radiusPill` | 999 | `src/core/theme.cpp:111` | chip/胶囊/开关轨道 |
| （EUI-NEO 输入框） | 10 | `../EUI-NEO/components/input.h:29,40` | 输入框默认 `radius.popup` |

---

## 4. 间距

| C++ token | 值 | 出处 |
|---|---|---|
| `sizes.spacingSmall` | 4 | `src/core/theme.cpp:103` |
| `sizes.spacing` | 8 | `src/core/theme.cpp:102` |
| `sizes.spacingLarge` | 16 | `src/core/theme.cpp:104` |
| `sizes.spacingXLarge` | 24 | `src/core/theme.cpp:105` |
| `sizes.spacingXXLarge` | 40 | `src/core/theme.cpp:106` |
| `sizes.spacingPage` | 12 | `src/core/theme.cpp:119`（**token 定义，但现有页面主 padding 不用它**） |

**页面主 padding 的实际口径（以代码为准）**：

- todo 页与设置页：左右 `32`（`spacingLarge × 2`），上 `0`，下 `32`；顶部由 40 高页头 + 16 间距接内容。出处 `src/module_todo/todo_module.cpp:824,867`、`src/module_settings/settings_module.cpp:1026,1058`。
- 侧栏顶部留白 `32`（`spacingLarge × 2`，`src/app_sidebar.cpp:360`）；导航项间距 `4`（`:354`）；品牌块到导航 `28`（`:365`，无 token 档）。
- 卡片间距：设置页 `16`（`src/module_settings/settings_module.cpp:1059`）；todo 看板卡片 `8`（`src/module_todo/todo_ui_logic.h:29`）。

> **上线模块必须用 32 主 padding**，不要用 `spacingPage = 12`；`spacingPage` 在当前代码里没有消费方承担页面外框。

---

## 5. 控件尺寸

### 5.1 按钮

| 项 | 值 | 出处 |
|---|---|---|
| 高度 | 40 | `src/ui_kit/button.h:33` |
| 左右内距 | 16 | `src/ui_kit/button.h:34` |
| 圆角 | 8（`sizes.radius`） | `src/ui_kit/button.cpp:81` |
| 字号 | 14 | `src/ui_kit/button.h:35` |
| 四级 | Primary / Secondary / Text / Danger | `src/ui_kit/button.h:24-29` |

### 5.2 输入框

| 项 | 值 | 出处 |
|---|---|---|
| 标准高 | 40 | `../EUI-NEO/components/theme.h:92`（`control.input`） |
| todo 搜索框 | 240 × 40 | `src/module_todo/todo_ui_board.cpp:79,125` |
| 设置页表单控件行高 | 36 | `src/module_settings/settings_ui_logic.h:140` |
| 表单 label 宽 | 110 | `src/ui_kit/form_row.h:17` |
| label→control 间距 | 8（`sizes.spacing`） | `src/ui_kit/form_row.h:25` |
| 输入框圆角 | 10 | `../EUI-NEO/components/input.h:29,40` |

### 5.3 列表行 / 导航行

| 项 | 值 | 出处 |
|---|---|---|
| 导航项高 | 40 | `src/ui_kit/metrics.h:15`（`kNavItemH`） |
| EUI-NEO 列表行高 | 40 | `../EUI-NEO/components/theme.h:102`（`control.listItemHeight`） |
| 逾期/任务选择列表行 | 44 | `src/module_todo/todo_ui_dialogs.cpp:579` |
| 导航项间距 | 4 | `src/app_sidebar.cpp:354` |
| 导航水平内距 | 16 | `src/app_sidebar.cpp:355` |
| 图标宽 | 28 | `src/app_sidebar.cpp:356` |
| 内容距高亮条左缘 | 12 | `src/app_sidebar.cpp:357` |

### 5.4 侧边栏

| 项 | 值 | 出处 |
|---|---|---|
| 展开宽 | 208 | `src/app_sidebar.h:14` |
| 折叠宽 | 72 | `src/app_sidebar.h:15` |
| 品牌行高 | 56 | `src/app_sidebar.cpp:363` |
| logo 边长 | 48 | `src/app_sidebar.cpp:403` |
| 品牌行顶 y | 32 | `src/app_sidebar.cpp:360` |
| 首个导航 y | 116（32+56+28） | `src/app_sidebar.cpp:411`（注释） |
| 折叠开关 | 36 × 36，距底 56 | `src/app_sidebar.cpp:512-513` |
| 组间分割线 | 1px，宽 = 栏宽 − 2×16 | `src/app_sidebar.cpp:496-500` |

### 5.5 Chip（状态/环境/类型）

| 项 | 值 | 出处 |
|---|---|---|
| 标准 chip 高 | 22 | `src/module_todo/todo_ui_logic.h:602`（`kChipHeight`） |
| 标准 chip 字号 | 11（`fontSizeSmall 12 − 1`） | `src/module_todo/todo_ui_logic.h:603` |
| 标准 chip 圆角 | pill（999） | `src/module_todo/todo_ui_common.h:31` |
| 页头动作 chip（逾期） | 88 × 28，字号 12，圆角 14 | `src/module_todo/todo_ui_board.cpp:154-176` |
| 完成角标宽 | 16 | `src/module_todo/todo_ui_board.cpp:48` |

> 原型当前 chip 高 24 / 字号 12 / 圆角 999。**以代码 22 / 11 / pill 为准**，原型需改。

### 5.6 卡片内边距（两种既有风格）

**A. 设置风格（推荐用于上线模块的配置卡、前置项卡）**

| 项 | 值 | 出处 |
|---|---|---|
| 左右内边距 | 28 | `src/module_settings/settings_ui_cards.cpp:147-148`（`w - 56`） |
| 标题顶距 | 16 | `src/module_settings/settings_ui_cards.cpp:126` |
| 标题行高 | 22 | 同上 |
| 说明行 y | 40 | `src/module_settings/settings_ui_cards.cpp:128` |
| 内容起始 y | 68 | `src/module_settings/settings_ui_cards.cpp:145` |
| 卡圆角 | 12（`radiusLarge`） | `src/module_settings/settings_ui_cards.cpp:121` |
| 卡间垂直间距 | 16 | `src/module_settings/settings_module.cpp:1059` |
| 卡底最小留白 | 16 | `src/module_settings/settings_ui_logic.h:87`（注释契约） |

**B. Todo 风格（推荐用于紧凑任务卡/步骤格）**

| 项 | 值 | 出处 |
|---|---|---|
| 卡高 | 84 | `src/module_todo/todo_ui_logic.h:28` |
| 卡圆角 | 8 | `src/module_todo/todo_ui_board.cpp:728` |
| 卡内左右内距 | 12 | `src/module_todo/todo_ui_board.cpp:45-47` |
| 标题行高 | 18 | `src/module_todo/todo_ui_logic.h:615` |
| 标题→chip 行间距 | 12 | `src/module_todo/todo_ui_logic.h:616` |
| 列头高 | 56 | `src/module_todo/todo_ui_logic.h:30` |
| 列内容左右内距 | 16 | `src/module_todo/todo_ui_logic.h:39` |
| 列容器圆角 | 12 | `src/module_todo/todo_ui_board.cpp:87` |
| 卡间距 | 8 | `src/module_todo/todo_ui_logic.h:29` |

### 5.7 进度条（缺口，需实现前定档）

| 来源 | 高 | 出处 |
|---|---|---|
| DevDesk 现有进度条（同步卡） | 4 | `src/module_settings/github_sync_card.cpp:84` |
| EUI-NEO 组件默认 | 14 | `../EUI-NEO/components/theme.h:85`（`control.progress`） |
| 上线原型 | 6 | `docs/superpowers/demo/release-module-demo.html`（非代码事实源） |

> **建议**：复用 DevDesk 既有 4px 档，原型从 6 改 4；不要为上线模块新开 6px 档。若产品坚持 6px，必须把它登记为 `module_release` 局部常量并在原型同步，不能悄悄用第三个值。

---

## 6. 阴影

EUI-NEO 的阴影 helper 签名是 `shadow(tokens, blur, offsetY, darkAlpha, lightAlpha)`；浅色档颜色固定为 `rgba(0.10, 0.14, 0.22, lightAlpha)`，无水平偏移（`../EUI-NEO/components/theme.h:281-293`）。

| 场景 | 偏移 | 模糊 | 暗色 α | 浅色 α | 出处 |
|---|---|---|---|---|---|
| 设置卡 | `(0, 1)` | 3 | 0.04 | 0.02 | `src/module_settings/settings_ui_cards.cpp:122` |
| Todo 卡 | `(0, 1)` | 3 | 0.06 | 0.03 | `src/module_todo/todo_ui_board.cpp:726` |
| `ui_kit::cardFrame` / `panelShadow` | `(0, 8)` | 24 | 0.28 | 0.12 | `../EUI-NEO/components/theme.h:299-301` |
| `buttonShadow`（未用于上线模块） | `(0, 4)` | 14 | 0.22 | 0.10 | `../EUI-NEO/components/theme.h:295-297` |

> 上线模块卡片**用设置卡档 `(0,1)/3/0.02`** 与既有页面一致；不要在原型用 `--shadow-md`（4/12）或 `--shadow-lg`（10/25）代替。浮层/弹窗才可升到 panelShadow 档。

---

## 7. 字体

### 7.1 实际字体链（EUI-NEO 运行时）

EUI-NEO 的 `resolveDefaultUiFontPath()` 顺序：**显式 override → 系统 CJK → 打包兜底 → 系统 UI**（`../EUI-NEO/core/render/text_shaping.cpp:371-391`）。

| 平台 | 实际首选与回退顺序 | 出处 |
|---|---|---|
| Windows | `C:/Windows/Fonts/msyh.ttc`（微软雅黑）→ `msyhl.ttc` → `simhei.ttf` → 打包 `JingNanJunJunTi-JinNanJunJunTi-Bold-2.ttf` → `segoeui.ttf` → `msyh.ttc` → `arial.ttf` | `text_shaping.cpp:348-369,371-391` |
| macOS | `/System/Library/Fonts/PingFang.ttc`（苹方）→ `Hiragino Sans GB.ttc` → `STHeiti Light.ttc` → `SFNS.ttf` → `Helvetica.ttc` | 同上 |
| 等宽（`fontFamily("monospace")`） | Win：`CascadiaMono.ttf` → `CascadiaCode.ttf` → `consola.ttf` → `cour.ttf`；macOS：`SFNSMono.ttf` → `Menlo.ttc` → `Monaco.ttf` | `text_shaping.cpp:249-283,539-542` |
| 图标 | `assets/Font Awesome 7 Free-Solid-900.otf` | `text_shaping.cpp:35` |

每段文本的首个可用 face 之外，还会懒加载 default/icon/system/emoji/对侧字重作为缺字回退（`text_shaping.cpp:834-857`）。

**关键事实**：`core::theme::fontStack()`（`src/core/theme.h:112`）返回 `"Inter, Microsoft YaHei, PingFang SC, sans-serif"`，但**全仓无消费方**；DevDesk 也没有调用 `setDefaultFontFiles`（`src/main.cpp:639` 明确注释不调）。所以实际渲染字体是上面的系统 CJK 链，**Inter 根本不会生效**。

### 7.2 行高差异的解释

- EUI-NEO 未显式设 `lineHeight` 时，行高由字体自身的 ascent/descent/lineGap 决定（`../EUI-NEO/core/render/text.cpp:578-580`），不是 CSS 的 `line-height: 1.7`。
- DevDesk 大量文本显式给 `lineHeight`（如标题 22/24、卡标题 18、页头 20），这些值才是原型应模仿的行高。
- **给原型的结论**：字体族写成 `"Microsoft YaHei","PingFang SC",sans-serif`；行高按各元素代码里的实际 `lineHeight`（见第 2、5 节）写死，不要全站 `1.7`。否则原型一定比 C++ 松。

---

## 8. HTML 原型映射约定

### 8.1 CSS 变量对照（浅色）

| 原型 CSS 变量 | C++ token | **代码权威值** | 原型当前值 / 处理 |
|---|---|---|---|
| `--bg-base` | `colors.background` | `#FAFAF9` | `#FAF9F5` → 改 |
| `--bg-surface` | `colors.surface` | `#FFFFFF` | 一致 |
| `--bg-elevated` | `colors.surfaceElevated` | `#FFFFFF` | 一致（浅色不分层） |
| `--bg-hover` | `colors.hover` | `#F3F4F6` | `#F5F2EC` → 改 |
| `--bg-active` | `colors.active` | `#E5E7EB` | `#EBE7DF` → 改 |
| `--accent` | `colors.primary` | `#36D399` | `#D97757` → **整体换薄荷绿** |
| `--accent-hover` | `colors.primaryHover` | `#2EC48F` | `#E0896B` → 改 |
| `--accent-pressed` | `colors.primaryPressed` | `#23B07F` | `#B85F3F` → 改 |
| `--accent-soft` | `colors.primarySoftBg` | `#E6FBF2` | `#F5E6DE` → 改 |
| （链接/小字主色） | `colors.primaryText` | `#157E4E` | 无对应，新增 |
| （Ghost 按钮字） | `colors.accentText` | `#0C7E58` | 无对应，新增 |
| `--text-primary` | `colors.textPrimary` | `#1F2937` | `#1F1D1A` → 改 |
| `--text-secondary` | `colors.textSecondary` | `#4B5563` | `#6B6660` → 改 |
| `--text-tertiary` | `colors.textTertiary` | `#6B7280` | `#9A948A` → 改 |
| `--text-disabled` | `colors.textDisabled` | `#6B7280` | `#B5B0A8` → 改 |
| `--border-default` | `colors.borderDefault` | `#E5E7EB` | `#E8E6DC` → 改 |
| `--border-muted` | `colors.borderMuted` | `#F3F4F6` | `#F0EEE6` → 改 |
| `--success` | `colors.success` | `#10B981` | `#5E7048` → 改 |
| `--warning` | `colors.warning` | `#F59E0B` | `#9A6B30` → 改（`--yellow` 也并入本值） |
| `--error` | `colors.error` | `#EF4444` | `#B82850` → 改；错误文字另用 `#B91C1C` |
| `--info` | `colors.info` | `#3B82F6` | `#4A7AAB` → 改 |
| `--radius-btn` | `sizes.radius` | `8px` | `10px` → 改 |
| `--radius-card` | `sizes.radius`（todo 卡）或 `radiusLarge`（设置卡） | `8 / 12px` | 原注释模糊；上线模块按用途二选一，不要 14 |
| `--radius-large` | `sizes.radiusLarge` | `12px` | `14px` → 改 |
| `--shadow-sm` | 设置卡阴影 | `0 1px 3px rgba(25.5,35.7,56.1,0.02)` | 改用第 6 节参数，不要 `rgba(0,0,0,.04)` |
| `--shadow-md` | 无对应（浮层档 panelShadow） | `0 8px 24px rgba(25.5,35.7,56.1,0.12)` | 卡片禁用；只在浮层 |
| `--shadow-lg` | 无对应（浮层档） | 同上或更强 | 卡片禁用 |

### 8.2 px → 逻辑单位

- HTML/CSS 的 `px` 直接当**逻辑 px** 用，不做 ×2 或 ÷1.25 的换算。
- C++ 侧：窗口默认 1600×1000 逻辑、最小 1000×640 逻辑；物理像素 = 逻辑 × `contentScale`（Windows 125% 缩放时 contentScale=1.25）。设计稿只写逻辑值。
- 字号：token 值已含用户字号档缩放；原型固定展示 100% 档，不要模拟缩放。
- 间距/圆角不随字号档缩放（`applyFontScale` 只乘 `fontSize*`，`src/core/theme.cpp:208-218`）；原型也不缩放。

### 8.3 原型必须同步的结构度量

以下差异会直接导致 C++ 与原型对不上，原型重写时一并改：

| 项 | 原型现值 | 代码权威值 | 出处 |
|---|---|---|---|
| 页面主 padding | `32px 40px 64px` | 左右 32 / 上 0 / 下 32 | `src/module_todo/todo_module.cpp:824,867` |
| 页头高 | 未固定（内容撑） | 40 | `src/ui_kit/metrics.h:12` |
| 导航项间距 | `gap:2px` | 4 | `src/app_sidebar.cpp:354` |
| 侧栏水平内距 | `12px` | 16 | `src/app_sidebar.cpp:355` |
| 侧栏顶部留白 | `24px` | 32 | `src/app_sidebar.cpp:360` |
| 品牌 logo | 34px | 48px（行高 56） | `src/app_sidebar.cpp:363,403` |
| chip 高 / 字 | 24 / 12 | 22 / 11 | `src/module_todo/todo_ui_logic.h:602-603` |
| 主按钮 | 40 / 16 / 8 / 15 | 40 / 16 / 8 / 14 | `src/ui_kit/button.h:33-35` |
| 输入框高 | 40 | 40（标准）/36（设置表单行） | `../EUI-NEO/components/theme.h:92` / `src/module_settings/settings_ui_logic.h:140` |
| 进度条高 | 6 | 4（建议复用） | `src/module_settings/github_sync_card.cpp:84` |
| 卡片圆角 | 12 / 14 | 8（todo 紧凑卡）/12（设置卡） | 第 5.6 节 |
| 卡片阴影 | `0 1px 3px rgba(0,0,0,.04)` | 见第 6 节（浅色带蓝调） | 第 6 节 |

### 8.4 组件级映射速查

| 原型元素 | 实现位置 |
|---|---|
| 页面根 | `ui.column(...).padding(32,0,32,32).gap(16)` |
| 页头标题 | `fontSize(sizes.fontSizePage=24)`；行高按本页既有先例（todo 24 / 设置 34），同一页内一致 |
| 页头动作 | `ui_kit::button`（40 高）或 chip（28 高） |
| 卡片 | 设置风格用 `ui_kit::cardSurface` + `radiusLarge(12)` + 第 6 节阴影；紧凑卡用 todo 风格 |
| 状态 chip | 高 22、字 11、pill、底=功能色 10-12% 混合、字=功能色 text 档 |
| 进度条 | 高 4、轨道 `hover`、填充 `primary`、两端 pill |
| 侧栏选中 | 白卡 + 1px `borderDefault` + 左 3px `primary` 竖线 + `primaryText` 文字（`src/ui_kit/selection_bar.cpp:16-38`） |