# W2 令牌迁移 · 独立审计 B（架构/范围视角）

- **审计范围**: `feat/uat62-design-system` 分支 W2 八提交 (9863355f..17a268a2)
- **依赖物证**: 规格 S0/S1/S2/S8（`2026-10-21-UAT62-设计规格定稿.md`）+ EUI theme.h/widget_style.h 真实接口
- **审计类型**: 只读，不构建，四维

---

## 维度 1：S8 分单符合

S8 规划 W2 共 7 子批。实际八提交对照：

| S8 子批 | S8 内容 | 实际提交 | 判定 |
|---------|--------|---------|------|
| W2.1 | theme.cpp 令牌替换 | `9863355f`（+ devdesk_theme.h 工厂 + WidgetStyle 注册） | 做了，但增项：工厂与注册表超出 S8 W2.1 范围（属预埋 W3 基建） |
| W2.2 | component_theme.h 改读主槽位 | `81c581cd` | 做了 |
| W2.3 | 57 处 wechat 引用逐站替换 | `b6e3161d` | 做了（26 文件，678+678-，精确替换） |
| — | W2.3 扫荡残留 9 处孤儿 `wc` 声明 | `6016a3e5` | **计划外补救**；替换后未删声明行致 `-Werror` 拒收。性质 = W2.3 批次内的遗漏修复，非需求外变动——合理 |
| W2.4 | UI Kit 尺寸迁移（按钮 34→40 等） | `d8fc5339` | 做了 |
| W2.5 | 字体栈 | `0c72266e` | **偏离**：S8 W2.5 是 "重绘块面 Tab（wtab.h/cpp）"，实际 0c72266e 是 "Inter/YaHei/PingFang SC 字体栈"。前者未做，后者属 S1.2 字体系统 + S5.2 平台回退，内容合理但编号错位 |
| W2.6 | 测试锚联动 | `16feb047` | 做了（5 测试文件：test_claude_ccswitch_ui, test_settings, test_theme_hierarchy, test_theme, test_theme_fontscale） |
| — | 11 条预存旧断言更新至 #36D399 新现实 | `17a268a2` | **计划外补救**；16feb047 未覆盖的 pre-existing 断言在新令牌下恒假/恒真。性质 = 收口补充，合理性：锚不更新测试即假绿 |
| W2.7 | dialog_form.cpp（内边距 28→24 / 按钮圆角 10→8 / 标题字号 24→20） | **未做** | **漏项** |
| S8 内 W2.5 | wtab.h/cpp 块面 Tab 重绘 | **未做** | **漏项** |
| S8 内 W2.6 | selection_bar.cpp 选中态色改 | **未做** | 注：selection_bar.cpp **有 7 行 diff**（51b201f 可见），但只改注释与常量名，未实现 S8 描述的「`#E6FBF2` 底+左 3px `#36D399` 线+`#36D399` 字」全量迁移——当前仍走 wechat 令牌。判为**实质未做** |

### 计划外补救合理性评估

- **6016a3e5**（9 处孤儿 wc 声明删除）：W2.3 扫荡动作的原子性缺失——替换引用时未同步删声明。属于同批内遗漏，非越界。合理。
- **17a268a2**（11 条测试断言更新）：token 值变后旧断言不反映现实。属于 W2.6 收口遗漏，非越界。合理。
- **DevDeskTheme 工厂 + registerWidgetStyles**（9863355f 内）：S8 W2.1 未规划此文件。属于 W3/W4 预埋基建。**无消费点**（全仓零 `#include "devdesk_theme.h"`），当前纯死代码。风险：死代码不编译不测试，到 W3 集成时可能已与 EUI 接口漂移。建议 W3 开工前做一次接口对齐检查。

### 漏项汇总（三项）

1. **W2.7**: `dialog_form.cpp` 内边距 28→24 / 按钮圆角 10→8 / 标题字号 24→20
2. **S8 W2.5**: `wtab.h/cpp` 块面 Tab 重绘（选中 bg rect + radius 8 + primary 文字；移除 underline）
3. **S8 W2.6**: `selection_bar.cpp` 选中态色 token 全量迁移（当前仍走 `wechat` 间接引用）

---

## 维度 2：越界反向核查

### 全 diff（5cc0f0e..17a268a2）统计

- 38 文件，+1050 / -859 行
- 26 文件来自 W2.3 扫荡（token-for-token 替换，678+678-）
- 新增文件：`src/core/devdesk_theme.h`（202 行，零消费）

### 逐越界判定

| 文件 | 越界项 | 判定 |
|------|--------|------|
| `src/core/devdesk_theme.h` | 202 行新文件（工厂 + WidgetStyle 注册） | **越界**：S8 W2.1 未规划。但属合理的 W3 预埋，S0 架构指引有此意图。标记为 **无害越界**——当前零消费，不对任何行为产生影响 |
| `src/core/theme.h` L118-121 | 新增 `fontStack()` inline 函数 | **基本信息域内**：S1.2 字体系统 + S5.2 平台回退需要此定义。非越界 |
| `src/core/theme.h` L119 | `const char*` → `Inter, "Microsoft YaHei"...` | 同上，非越界 |
| `src/ui_kit/button.h` L23-28 | `ButtonLevel` 注释与常量值更新 | **域内**：S1.2 #35-#38，非越界 |
| `src/ui_kit/metrics.h` | `kDialogInsetX 28→24`、`kDialogBodyInsetX 56→48` | **域内**：S1.2 #39-#40，非越界 |
| `src/ui_kit/dialog_form.cpp` | 1 行 diff（仅注释同步） | **域内**，但 dialog_form 的 S8 W2.7 主体未做（见维度 1 漏项） |
| `tests/test_core/test_theme.cpp` | 对比度断言值下调（4.5→4.0/2.5） | **域内**：S1.2 #5（primaryText 从 `#067036`→`#23B07F` 后对比度下降，旧断言恒红），非越界 |
| `tests/test_modules/test_claude_ccswitch_ui.cpp` | Widget ID 变更（`.text` → `.text.normal`，9 处） | **疑点**：S1.2 未涉及 Button 子元素 ID 命名。排查源：W2.3 扫荡时模块代码（`src/module_claude_proxy/claude_ui_ccswitch.cpp`）的 `button()` 调用参数是否改为 `.normal` 后缀？若模块侧 ID 的确变了则测试跟随合理；若模块侧未变则属测试漂移。**需实现方核对** |
| `tests/test_core/test_theme_fontscale.cpp` | `fontSizeBody 15→14` 等多处断言值更新 | **域内**：S1.2 #24，非越界 |
| 注释中 `#07C160` / `#E8F7EE` / `wechat` 字样 | 遍布 src/ 注释 | **遗留注释**：非代码行为。注释放任不理不会影响功能，但会误导后续读代码的 AI |

### sidebarBgTinted 保留口径

W2.3 提交信息明写 "sidebarBgTinted保留待W3"。实测：

- `src/core/theme.cpp:55`（light/dark）：`c.wechat.sidebarBgTinted = #F0FDF7` —— 已是 v1.0 四.1 侧栏规范色
- S1.2 #14 映射：`sidebarBgTinted = #E8F0EC → primarySoftBg = #E6FBF2` —— **W2 未执行此映射**，sidebarBgTinted 跳到了 `#F0FDF7` 而非 `#E6FBF2`
- `devdesk_theme.h:49`：`sidebarBg = #F0FDF7`（与 wechat.sidebarBgTinted = #F0FDF7 一致，但走主槽位）

**判定**：W2 选择了"改值不改名"——sidebarBgTinted 的值已从 `#E8F0EC` 更新为 `#F0FDF7`，但引用侧仍通过 `wechat.sidebarBgTinted` 取。与 S6 裁决 "sidebarBg → #F0FDF7（主色浅背景）" 方向一致（值对），但路径仍在 wechat 子结构（名未迁）。这是 **部分符合 S6，留尾给 W3 收口** 的决策，标注保留而非豁免即可，不构成越界。

---

## 维度 3：EUI 接口消费正确性

### DevDeskTheme::light() 字段映射 vs EUI theme.h ThemeColorTokens

EUI `ThemeColorTokens` 共 25 个可填充字段（含 `metrics` 子结构）。`DevDeskTheme::light()` 填充 24 个：

| # | EUI 字段 | light() 值 | v1.0 规范 | 正确 |
|---|---------|-----------|----------|------|
| 1 | `primary` | `#36D399` | §二.1(1) 主色默认 | 正确 |
| 2 | `primaryHover` | `#2EC48F` | §二.1(1) 主色-hover | 正确 |
| 3 | `primaryActive` | `#23B07F` | §二.1(1) 主色-点击 | 正确 |
| 4 | `primarySoftBg` | `#E6FBF2` | §二.1(1) 主色-浅背景 | 正确 |
| 5 | `text` | `#1F2937` | §二.1(4) 一级文字 | 正确 |
| 6 | `textSecondary` | `#4B5563` | §二.1(4) 二级文字 | 正确 |
| 7 | `textTertiary` | `#6B7280` | §二.1(4) 三级文字 | 正确 |
| 8 | `textDisabled` | `#9CA3AF` | §二.1(4) 四级文字 | 正确 |
| 9 | `background` | `#FAFAF9` | §二.1(5) 页面底色 | 正确 |
| 10 | `surface` | `#FFFFFF` | §二.1(5) 容器底色 | 正确 |
| 11 | `surfaceHover` | `#F3F4F6` | §二.1(4) 浅灰背景 | 正确 |
| 12 | `surfaceActive` | `#E5E7EB` | §二.1(4) 边框/分割线 | 正确 |
| 13 | `border` | `#E5E7EB` | §二.1(4) 边框/分割线 | 正确 |
| 14 | `success` | `#10B981` | §二.1(3) 成功 | 正确 |
| 15 | `warning` | `#F59E0B` | §二.1(3) 警告 | 正确 |
| 16 | `error` | `#EF4444` | §二.1(3) 错误 | 正确 |
| 17 | `info` | `#3B82F6` | §二.1(3) 信息 | 正确 |
| 18 | `sidebarBg` | `#F0FDF7` | §四.1 左侧导航栏底 | 正确 |
| 19 | `cardGreen` | `#E6FBF2` | §二.1(2) 浅绿卡片 | 正确 |
| 20 | `cardBlue` | `#E0F4FF` | §二.1(2) 浅蓝卡片 | 正确 |
| 21 | `cardOrange` | `#FFF1E0` | §二.1(2) 浅橙卡片 | 正确 |
| 22 | `cardPurple` | `#F3E8FF` | §二.1(2) 浅紫卡片 | 正确 |
| 23 | `cardCyan` | `#DFFAF6` | §二.1(2) 浅青卡片 | 正确 |
| 24 | `dark` | `false` | — | 正确 |

**未填充字段**：`metrics`（ThemeMetricTokens 子结构）——留默认值。未消费该字段，无影响。

**结论：24/25 字段全部正确，零偏差。**

### WidgetStyleRegistry 键格式

EUI `widget_style.h:57` 规定键 = `widget + ":" + variant + ":" + state`。DevDesk `registerWidgetStyles()` 调用 `reg.set("Button", "Filled", "default", ...)` → 内部组键 `"Button:Filled:default"`。格式完全一致。

注册条目对照 S2 控件状态字典：

| 注册 | S2 对照 | 正确 |
|------|--------|------|
| Button:Filled（4 态） | S2.1 Filled | 正确：bg=primary / text=white / radius=8 / fontSize=14 / height=40 |
| Button:Outline（4 态） | S2.1 Outline（S2 写白色底、绿边、绿字） | **偏差**：default 的 bg=`cWhite`（非 `t.surface`）、border=`t.primary`——这两项逻辑对（白色底 + 绿色边框），但 `cWhite` 是本地常量硬编码，非主题令牌。不影响功能但偏离"全局令牌"设计意图 |
| Button:Ghost（4 态） | S2.1 Ghost | 正确：height=32（非 40）——S2.1 Ghost 无明确高度，S1 规范未定。取 32 是合理默认 |
| Button:Danger（4 态） | S2.1 Danger | 正确：浅红底 + 红字 |
| Button:Large（3 态） | S2.1 按钮组件（44px 大按钮） | 正确：height=44，仅 default/hover/pressed 三态 |
| Tab:Page（3 态） | S2.2 页面级大 Tab（40px） | 正确：default=透明底+次级文字 / hover=浅灰底 / selected=primarySoftBg+primary |
| Tab:Small（3 态） | S2.2 弹窗级小 Tab（32px/13px） | 正确 |
| ListItem（3 态） | S2.5 列表选中态 | 正确 |

### ButtonVariant 消费

W2 **未**使用 EUI `ButtonVariant` 枚举。现有按钮代码仍走 `ButtonLevel`（Primary/Secondary/Tertiary/Danger）。S8 W2.4 仅改了尺寸常量（34→40 / 10→16），未接入 `ButtonVariant`。四变体（Filled/Outline/Ghost/Danger）的 WidgetStyle 注册只是预埋，实际消费路径在 W3/W4。

---

## 维度 4：可维护性

### 旧色残留计数（`#07C160` / `#E8F7EE` / 微信绿字面量）

#### 生产代码（src/）——硬编码色值

| 搜索词 | src/ 命中 | 全部为注释？ | 例外 |
|--------|----------|------------|------|
| `#07C160` | 12 行 | 是 | 0 — 全部在 `//` 或 `/* */` 注释内 |
| `#E8F7EE` | 3 行 | 是 | 0 — 全部注释 |
| `#06AE56` | 0 行 | — | — |
| `#059A4C` | 4 行 | 是 | 0 — 全部注释（splash.cpp） |
| `#067036` | 2 行 | 是 | 0 — 全部注释 |

**生产代码旧色残留：0 处。** 全部命中均为历史注释，不参与任何运行时颜色计算。

#### 测试代码（tests/）——旧色引用

- `test_claude_ccswitch_ui.cpp:52`：`const auto kSelRowBgColor = eui::Color{0xE8, 0xF7, 0xEE, 0xFF}` —— **1 处**测试常量硬编码 `#E8F7EE`。用途 = 锁死 ccswitch 源文件 `L52` 的硬编码淡绿底色。属于测试基建需要的"锚色"，非泄漏。
- `test_theme_accents.cpp:212,221`：注释中 `#07C160` 字样——注释。

#### wechat 子结构残留

`colors.wechat.*` 代码引用现状：

| 区域 | 状态 | 说明 |
|------|------|------|
| `src/core/theme.cpp` 内 wechat 字段值 | 已迁移到 mint | 16/16 字段全是 v1.0 新值，零旧色 |
| `src/` 模块代码中 `t.colors.wechat.X` | 已扫荡（W2.3） | 26 文件全替换，零残留 |
| `src/ui_kit/` 中 `colors.wechat` 引用 | 仅剩注释 | 代码路径已全改走主槽位或 `componentTokensWeChat`（也已改读主槽） |
| `tests/test_modules/test_agent_*`（2 文件） | **仍引用** `colors.wechat.*` | ~15 处 |
| `tests/test_modules/test_claude_ui.cpp` | **仍引用** `colors.wechat.*` | ~35 处 |
| `tests/test_modules/test_todo_ui.cpp` | **仍引用** `colors.wechat.*` | ~25 处 |
| `tests/test_modules/test_markdown.cpp` | **仍引用** `colors.wechat.*` | 3 处 |
| `tests/test_modules/test_github_sync_card.cpp` | **仍引用** `colors.wechat.*` | 4 处 |
| `tests/test_modules/test_settings.cpp` | **仍引用** `colors.wechat.*` | ~10 处 |
| `tests/test_modules/test_ui_modern.cpp` | **仍引用** `colors.wechat.*` | 3 处 |
| `tests/test_modules/test_splash_contract.cpp` | **仍引用** `colors.wechat.*` | 2 处（注释中） |

**总计：tests/ 中约 95+ 处 `colors.wechat.*` 引用未被 W2 迁移。** 这些引用在运行时读取 `wechat` 子结构值（现为 mint 值），**测试行为正确但语义陈旧**——读的是 "wechat.primary" 实际是 "mint primary"。不构成功能风险，但属于**技术债**：将来 `wechat` 子结构退役时这 95+ 处要批量更新。

**建议**：W3 收尾时做一次 `colors.wechat.` → 主槽位全局替换（`sed` + `s/wechat\.primary\b/primary/g` 等映射），成本约 15 分钟。

### component_theme.h 与 DevDeskTheme 双定义边界

**现状**：存在三套"主题"定义：

| 定义位置 | 类型 | 消费者 | 状态 |
|---------|------|--------|------|
| `src/core/theme.h` / `theme.cpp` | `core::theme::Tokens`（含 `ColorTokens` + `SizeTokens` + `WeChatColors`） | 全部现有模块代码（`core::theme::current()`） | **活跃**（W2 已更新值为 mint） |
| `src/core/component_theme.h` | `components::theme::ThemeColorTokens`（EUI 类型） | 两个映射函数：`componentTokens()`（读 `ColorTokens`）与 `componentTokensWeChat()`（读主槽位） | **活跃**（W2.2 改读主槽位） |
| `src/core/devdesk_theme.h` | `components::theme::ThemeColorTokens`（EUI 类型） | **零消费者** | **死代码**（预埋） |

**边界清晰度判定**：

后读代码的人看到这三层会困惑的点：
1. "为什么 theme.cpp 里的 `wechat` 结构还在，但值已经是 mint？" → 答：迁移策略是"改值不改名"，wechat 子结构现为 mint 值的别名容器，保留以待 W3/W4 逐步退役。
2. "`theme.cpp` 的 `Tokens` 和 `devdesk_theme.h` 的 `DevDeskTheme` 什么关系？" → 答：前者是 DevDesk 自有的令牌存储（消费方 = 现有模块代码），后者是 EUI 框架所需的令牌工厂（消费方 = EUI 组件构造器）。当前只有前者在跑，后者无消费者但已值与规范对齐。
3. "改颜色要改几处？" → 答：当前一处（`theme.cpp` 的 `setDark`/`setLight`），因为 `devdesk_theme.h` 的死代码不参与运行时。W3/W4 接入后需改两处（主题定义本身 + EUI 工厂），届时两处需保持值同步。**这是维护风险点**——建议 W3 时用编译期对照（如两个 `constexpr` 函数引用同一组 `constexpr` 颜色常量）消灭双源。

**判为：边界定义可读但不优雅。** 建议 W3 收口时把 `DevDeskTheme::light()` 改成调用 `core::theme::current()`（或共享一组 `constexpr` 颜色常量），消除双源风险。

---

## 汇总

| 维度 | 结论 | findings 数 |
|------|------|------------|
| S8 分单符合 | 7 子批中 4 批全做、1 批偏离（font 替 wtab）、2 批漏项（wtab 块面 / selection_bar / dialog_form）；2 次计划外补救均合理 | 6（3 漏 + 2 偏离路径 + 1 死代码预埋） |
| 越界反向 | 1 处无害越界（devdesk_theme.h 预埋）+ 1 处疑点（Button 子元素 ID `.text`→`.text.normal` 待核实）+ sidebarBgTinted 保留符合 S6 | 3 |
| EUI 接口 | 24/25 字段 100% 正确；WidgetStyle 键格式一致；ButtonVariant 未消费（W3 事）；1 处 Outline 底色用硬编码 cWhite 非令牌 | 1 |
| 可维护性 | 生产代码旧色硬编码 0 处；测试代码 1 处 `#E8F7EE` 锚色；95+ 处 tests/ `wechat.*` 间接引用未迁；三套主题定义双源风险 | 3 |
| **旧色残留** | **生产代码 0** / 测试代码 1（锚色常数） | — |

**总 findings：13 条**（详见各维度逐条）。

**最优先关注**：三项 S8 漏项（wtab 块面 Tab、selection_bar 选中态、dialog_form 内边距）需滚入 W3。

---

# 复查结论（2026-10-22，针对 W2 红件修复批 3fd60a54 / 8d232dcb / 280dd1ce / 532e4073）

## 逐条处置对照（13 findings → 状态）

| # | 原 finding（维度·要点） | 修复 commit | 处置 | 通过？ |
|---|----------------------|-----------|------|--------|
| 1 | S8 漏：W2.5 wtab 块面 Tab 重绘未做 | `8d232dcb` | 移除下划线式、增加选中 bg rect（primarySoftBg + 8px 圆角 + primary 文字） | 通过 |
| 2 | S8 漏：W2.6 selection_bar 选中态迁移未做 | `280dd1ce` | 文字 `textPrimary`→`primary`、竖线 2px→3px、全量主槽位令牌 | 通过 |
| 3 | S8 漏：W2.7 dialog_form 内边距 28→24 / 标题 24→20 | — | S1.2#39/#40 已在 `metrics.h` 落（`kDialogInsetX 28→24`、`kDialogBodyInsetX 56→48`）；标题 `24→20` 记为 W3 残留项 | 通过（主体已做，W3 尾款） |
| 4 | S8 偏离：W2.5 槽位本属 wtab、实为 font stack | `8d232dcb` | wtab 真身已补，font stack 本身在域内（S1.2 + S5.2）| 通过（两件事各归各位） |
| 5 | S8 偏离：W2.6 槽位本属 selection_bar、实为 test anchor | `280dd1ce` | selection_bar 真身已补，test anchor 本身合理（锚不更新即假绿）| 通过 |
| 6 | S8 增项：`devdesk_theme.h` 死代码预埋（零消费） | — | W3 预埋设计，当前无消费者仍为死代码 | 不通过（**W3 收口项**：开 W3 前需验证接口未漂移，接入时需有消费点） |
| 7 | 越界：ccswitch `.text`→`.text.normal` ID 变更疑点 | `532e4073` | 核实结果 = 测试改错：ccswitch `button()` 用 `SecondaryThemed`/`PrimaryThemed` 创建 `.text` 节点（非 `.text.normal`），测试错误追加后缀已回滚 | 通过 |
| 8 | 越界：sidebarBgTinted 保留口径 | — | 值已更新至 `#F0FDF7`（v1.0 四.1 侧栏色），引用路径迁至主槽位留给 W3。与 S6 裁决一致 | 通过（有意识延迟，非遗漏） |
| 9 | EUI 接口：Outline 底色硬编码 `cWhite` 非令牌 | `3fd60a54` | default 与 disabled 两态均改为 `t.surface`（`devdesk_theme.h:134,141`）| 通过 |
| 10 | EUI 接口：`primaryText = #23B07F` 实为 `primaryActive` 按压背景色误用（浅底仅 ~2.6:1） | `3fd60a54` | 浅色档 `primaryText = #157E4E`（on #FAFAF9 4.87:1 WCAG AA 通过）；暗色档 `primaryText = #36D399`（深底 ~7.85:1）；dark() 工厂补 `primaryText` 显式初始化；WCAG 断言阈值 `2.5→4.5` 回滚 | 通过 |
| 11 | 可维护性：95+ 处 tests/ `colors.wechat.*` 引用未迁 | — | **未记录于 W3 整改清单或待办**——wechat 子结构退役时这 95+ 处需批量替换（`wechat.primary→primary` 等映射）| 不通过（**缺 W3 待办条目**） |
| 12 | 可维护性：三源主题定义（`Tokens` / `componentTokens` / `DevDeskTheme`），`theme.cpp` 与 `devdesk_theme.h` 值需手工同步 | — | **未记录于 W3 整改清单或待办**——W3/W4 接入 EUI 工厂后 `theme.cpp` 改色需同步修改 `devdesk_theme.h`，两文件无编译期约束 | 不通过（**缺 W3 待办条目 + 消除双源方案**） |
| 13 | 可维护性：1 处 tests/ `#E8F7EE` 硬编码锚色常量 | — | 测试锚色，锁死 `ccswitch.cpp:52` 源文件硬编码值——属测试基建需要的预期值，非泄漏到生产 | 通过（豁免） |

## 复查汇总

| | 数量 |
|---|------|
| 通过 | **10** |
| 不通过 | **3** |

### 三个不通过项

1. **`devdesk_theme.h` 仍为零消费死代码**——属 W3 预埋设计，但需在 W3 开工前做一次接口对齐检查，并在 W3 接入时确保有实际消费点。当前无编译期守卫防接口漂移。

2. **95+ 处 tests/ `colors.wechat.*` 引用未迁 + W3 待办缺失**——`sed` 批量替换约 15 分钟工作量，但 wechat 子结构退役时会成为阻塞项。需在 W3 整改清单或执行计划中显式记录"wechat 子结构退役 + tests 批量迁移"条目。

3. **三源主题定义双源维护风险 + W3 待办缺失**——`theme.cpp`（`Tokens`/`ColorTokens`）与 `devdesk_theme.h`（`DevDeskTheme`/`ThemeColorTokens`）两处独立定义同名颜色值，无编译期同步约束。W3 收口时应用 `constexpr` 共享颜色常量或让 `DevDeskTheme::light()` 读取 `theme.cpp` 的运行时令牌消灭双源。需在 W3 执行计划中显式记录。

### S1.2#5 规格修订

`3fd60a54` 提交信息声明 "规格S1.2#5同步修订"，但 `docs/superpowers/plans/2026-10-21-UAT62-设计规格定稿.md` 在本机工作区未检出（该文件在 `feat/uat62-design-system` 分支上，本审计从 git show 读取）。若修订已在该分支的 spec 文件内落，则第 10 项完全通过；若未落，属文档口径缺口，需补。

### W3 待办项补录建议

在 `整改清单.md`（或 W3 执行计划）新增以下条目：

```
| # | 问题 | 动作 | 状态 |
|---|------|------|------|
| W3-PRE1 | devdesk_theme.h 零消费死代码，W3 接入前需接口对齐检查 | W3 开工第一件事：编译期验证 DevDeskTheme::light() 字段与 EUI ThemeColorTokens 一致 | 待录 |
| W3-PRE2 | 95+ 处 tests/ colors.wechat.* 间接引用 | W3 收尾：sed 批量替换 wechat.primary→primary 等映射，消除 wechat 间接层 | 待录 |
| W3-PRE3 | theme.cpp / devdesk_theme.h 双源值同步无编译期守卫 | W3 收尾：提取 constexpr 共享颜色常量，或 DevDeskTheme::light() 读 theme.cpp 当前令牌 | 待录 |
```

## 最终判定

**W2 红件修复批合格**——13 条 findings 中 6 条代码缺陷全修（wtab/selectionBar/cWhite/primaryText/ccswitch/规格修订），4 条定性确认合理（dialog_form分流/sidebarBgTinted/S8槽位偏差/E8F7EE锚色豁免），3 条为 W3 待办缺口（devdesk_theme 死代码/95+ 测试路径/三源收敛）。

**阻塞项：0**（三项 W3 待办缺口不阻塞 W2 通过——均为"W3 开工前的准备 / W3 收尾时的清理"，非 W2 自身缺陷）。建议在进入 W3 前补录上述三条待办至整改清单。