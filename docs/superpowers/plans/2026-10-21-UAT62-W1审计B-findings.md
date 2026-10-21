# W1 EUI 审计 B（架构视角）—— findings

- **审计对象**: `/d/wt-uat62-eui`，分支 `feat/uat62-theme`（3 文件 5 commits）
- **依据**: S0 Fork 清单 + S1/S2 `/d/project/gui/DevDesk/docs/superpowers/plans/2026-10-21-UAT62-设计规格定稿.md`
- **日期**: 2026-10-21

---

## 四维总评

| 维度 | 结果 |
|------|------|
| 1. S0 Fork 逐条符合 | 5/5 条目已实现，commit 全带 S0 编号（S0#1~S0#6，S0#5 是 DevDesk 侧故意不在此工作树实现） |
| 2. 防瞎改反向核查 | 1 处注释删除 + 1 处工具函数前移（后者为必要适配） |
| 3. W2 接口契约 | **1 处数据矛盾**：`listItemHeight`=44 与 S2 全量 40px 冲突 |
| 4. 可维护性 | 单例无 ODR 风险，键命名可扩展，指定初始化器前向兼容 |

**Finding 数: 1 HIGH / 2 MED / 1 LOW**

---

## 维度 1 · S0 Fork 逐条符合

| S0# | 条目 | Commit | 文件 | 状态 |
|-----|------|--------|------|------|
| 1 | ControlSizeTokens 新增 buttonHeight/buttonLargeHeight/tabHeight/tabSmallHeight | `0d3d3c6` | `theme.h` | OK |
| 2 | ThemeColorTokens 新增 16 字段 | `7c44425` | `theme.h` | OK |
| 3 | ButtonVariant 枚举 + ButtonStyle 变体构造 + ButtonBuilder::theme(tokens,variant) | `13901e9` | `button.h` | OK |
| 4 | WidgetStyleRegistry 单例 | `a477f43` | `widget_style.h`(新建) | OK |
| 5 | DevDesk 主题工厂 (DevDesk/src/core/devdesk_theme.h) | 未提交 | — | 非 EUI 范畴，正确排除 |
| 6 | PageVisualTokens 扩 buttonHeight/tabHeight/inputRadius/listItemHeight | `1d8f216` | `theme.h` | OK |

**结论**: 5 条 EUI Fork 全实现，commit 可追溯性完整。无漏做。

### MED-C1: `button(ui, id, variant)` 重载接受 variant 但未使用

`button.h:293-295` 新增三参数 `button()` 重载，签名为 `(Ui&, const string&, ButtonVariant)`，但函数体与双参数版完全相同——variant 被接受后丢弃：

```cpp
inline ButtonBuilder button(core::dsl::Ui& ui, const std::string& id, ButtonVariant variant) {
    return ButtonBuilder(ui, id);  // variant 未传给 Builder
}
```

S0#3 commit message 声称 "ButtonBuilder::theme(tokens,variant) 重载"——该重载确实存在（`ButtonBuilder::theme(const ThemeColorTokens&, ButtonVariant)`），但本 `button()` 自由函数重载未列入 commit message 也未接链到 Builder。当前调用者需额外链式 `.theme(tokens, variant)`，使此重载成为**死 API 面**。若意图是便利入口，应改为 `ButtonBuilder(ui, id).theme(默认tokens, variant)`（但默认 tokens 从何来？）——建议移除此重载，或文档声明其等价于双参数版。

---

## 维度 2 · 防瞎改反向核查

对 `git diff main..feat/uat62-theme` 中与 S0 无关的每一行逐行审计：

| 改动 | 文件:行域 | 分类 |
|------|----------|------|
| `color()`/`defaultPrimary()`/`withAlpha()`/`withOpacity()` 四函数从 struct 之后（旧 L137-155）移至 struct 之前（新 L9-26） | `theme.h` | **必要技术适配** —— S0#2 新增字段用 `color(...)` 作默认成员初始化器，必须在 struct 定义前可见 |
| `pageVisuals()` 从位置聚合初始化改为指定初始化器 | `theme.h:208-233` | **必要技术适配** —— S0#6 新增 4 字段在末尾，指定初始化器防未来字段插入打乱位置 |
| UAT6 阴影注释块（6 行）在 `ButtonStyle(tokens, primary)` 构造器中删除 | `button.h:28` | **LOW-B1: 范围外注释清除**（见下） |
| 无其他与 S0 无关行 | — | 3 文件外零改动 |

### LOW-B1: ButtonStyle 默认构造器删除 UAT6 阴影注释

`button.h` 的 `ButtonStyle(tokens, primary)` 构造器中，删除了解释 `shadow = {}` 设计意图的 6 行中文注释（"UAT6 阴影统一（WeChat 纯平体系）：按钮默认无阴影…"）。该注释记录了为何按钮无阴影，删除后未来维护者看到 `shadow = {}` 将不明白此决策。

**建议**: 用一行精简注释替换（如 `// flat — no shadow`），而非全删。

---

## 维度 3 · W2 接口契约

### 16 新字段名 vs S1 令牌命名 —— OK

| S1 令牌 | EUI 字段名 | 匹配 |
|---------|-----------|------|
| success | `ThemeColorTokens::success` | OK |
| warning | `ThemeColorTokens::warning` | OK |
| error | `ThemeColorTokens::error` | OK |
| info | `ThemeColorTokens::info` | OK |
| textSecondary | `ThemeColorTokens::textSecondary` | OK |
| textTertiary | `ThemeColorTokens::textTertiary` | OK |
| textDisabled | `ThemeColorTokens::textDisabled` | OK |
| primaryHover | `ThemeColorTokens::primaryHover` | OK |
| primaryActive | `ThemeColorTokens::primaryActive` | OK |
| primarySoftBg | `ThemeColorTokens::primarySoftBg` | OK |
| sidebarBg | `ThemeColorTokens::sidebarBg` | OK |
| cardGreen | `ThemeColorTokens::cardGreen` | OK |
| cardBlue | `ThemeColorTokens::cardBlue` | OK |
| cardOrange | `ThemeColorTokens::cardOrange` | OK |
| cardPurple | `ThemeColorTokens::cardPurple` | OK |
| cardCyan | `ThemeColorTokens::cardCyan` | OK |

16/16 命名与 S1 完全对齐。

### `inputRadius` — OK

`PageVisualTokens::inputRadius = metrics.radius.small = 6.0f`，与 S2 输入框圆角 6px 一致。

### HIGH-D1: `listItemHeight = 44` 与 S2 全量 40px 冲突

**证据链**：

1. `theme.h:231`: `.listItemHeight = metrics.control.control`
2. `theme.h:93`: `float control = 44.0f;`（ControlSizeTokens，预存值）
3. **S2.5**（列表选中态表）: 高度列统一 **40px**
4. **S2.2**（Tab 组件，页面级）: 高度 **40px**
5. **S1.2 #42**: `kNavItemH = 40.0f` → "保留 40.0f | v1.0 列表项高度同 40"
6. **S0 Fork #6**: 新增 `listItemHeight` 字段——意图承接列表项 40px 默认
7. **S0 #13**: 按钮 40px 走 `buttonHeight = 40.0f`——同为 40px 值却有独立字段

`ControlSizeTokens` 无 `listItemHeight` 字段（Fork #1 只加了 button 和 tab 系列），实现者就近映射到 `control = 44.0f`，但该值与 spec 矛盾。W2 消费 `PageVisualTokens::listItemHeight` 时将得到 **44px** 而非 spec 要求的 **40px**——所有列表项渲染偏高 4px。

**修复方案（二选一）**：

- A: 在 `ControlSizeTokens` 新增 `float listItemHeight = 40.0f;`，`pageVisuals()` 改为 `.listItemHeight = metrics.control.listItemHeight`
- B: 直接硬编码 `.listItemHeight = 40.0f`（不经过 metrics 间接层，与其他 PageVisualTokens 值的来源风格一致——现有 `titleColor`/`cardColor` 等都是 `tokens.*` 直接字段，不是 metrics 间接值）

推荐 A——保持度量统一来源。若选 A，同时检查 Fork #1 是否需要补此项。

### MED-D2: `ThemeColorTokens` 新增 16 字段中 12 个零初始化为 `{}`

以下 12 个字段默认初始化为 `{}`（即 `Color{0,0,0,0}` 透明黑）：

`textSecondary`, `textTertiary`, `textDisabled`, `primaryHover`, `primaryActive`, `primarySoftBg`, `sidebarBg`, `cardGreen`, `cardBlue`, `cardOrange`, `cardPurple`, `cardCyan`

仅 `success`/`warning`/`error`/`info` 4 个有非零默认值（从 S1 色值直接编入）。EUI 内置 `light()`/`dark()` 工厂函数均不设置这些字段（仅聚合初始化 8 个旧字段）。这意味着 EUI 侧消费者通过默认值得到的是**透明/黑色**而非 spec 色值。

**这不是 W1 的 bug**——S0#5（DevDesk 主题工厂）将在 DevDesk 侧 `devdesk_theme.h` 中填充这些字段到 spec 值，EUI 默认值是占位。但需在 W2 集成时验证：DevDesk 侧 `DevDeskTheme::light()` 是否确实覆盖了全部 16 新字段到 S1 值。若漏填，UI 将渲染为透明/黑色。

---

## 维度 4 · 可维护性

### 键命名方案 `widget:variant:state` — OK

`WidgetStyleRegistry` 用 `"Button:Filled:default"` 格式，分隔符明确，新增控件类型/变体/状态只需追加新键，无需改注册表结构。`query()` 的 wildcard fallback（`widget:*:state`）提供合理降级。

### 单例 ODR 安全 — OK

`WidgetStyleRegistry::instance()` 是 Meyer's 单例（函数内 `static` 局部变量），所在函数为 `inline`。C++17 [dcl.inline]/6 保证所有翻译单元共享同一实例。整个类是 header-only，无 ODR 风险。非线程安全（`unordered_map` 无锁），但 EUI 是单线程 GUI 框架，非问题。

### PageVisualTokens 指定初始化器前向兼容 — OK

`pageVisuals()` 用 C++20 指定初始化器显式命名每个字段。未来新增 `PageVisualTokens` 字段时：
- 若有默认成员初始化器 → `pageVisuals()` 无需修改，自动取默认值
- 若需从 `ThemeColorTokens` 派生 → 在 `pageVisuals()` return 末尾追加一行

比位置聚合初始化更安全（后者在字段插入时静默偏移）。

### `button()` 三参数重载（复述 MED-C1 可维护性面）

除功能面外，此重载的 API 契约模糊：调用者看到 `button(ui, id, variant)` 会期待 variant 被应用到返回的 Builder，但实际不是。若未来任何人使用此重载，将得到无 variant 的 Builder 并产生 bug。建议要么实现完整（需 tokens 参数），要么删除以消除误用面。

---

## Finding 汇总

| ID | 严重度 | 维度 | 简述 | 涉及文件:行 |
|----|--------|------|------|-----------|
| HIGH-D1 | HIGH | 3. W2 接口契约 | `listItemHeight = 44` 与 S2 全量 40px 冲突 | `theme.h:231` |
| MED-C1 | MED | 1. S0 符合 | `button(ui, id, variant)` 重载接受但未使用 variant 参数 | `button.h:293-295` |
| MED-D2 | MED | 3. W2 接口契约 | 12 个新颜色字段零初始化，EUI `light()`/`dark()` 不填充，依赖 W2 DevDesk 侧覆盖 | `theme.h:126-137` |
| LOW-B1 | LOW | 2. 防瞎改 | ButtonStyle 构造器删除 UAT6 阴影设计意图注释 | `button.h`（旧 L27-32） |

**建议优先级**: 先修 HIGH-D1（修改 `theme.h:231` 的映射源或新增 `ControlSizeTokens::listItemHeight`），再处理 MED-C1（移除或实现 `button()` 三参数重载），LOW-B1 可延后。

---

## 复查结论 (2026-10-21)

复查目标: `/d/wt-uat62-eui` 分支 `feat/uat62-theme`，追加 commit d886b14 / c6bf5fc / ce4501f。

| ID | 处置 | 证据 | 状态 |
|----|------|------|------|
| HIGH-D1 | ControlSizeTokens 新增 `listItemHeight=40.0f`，pageVisuals() 改读 `metrics.control.listItemHeight` | `theme.h:102` 新增字段 + `theme.h:232` 映射修正 | **通过** |
| MED-C1 | `button(ui,id,variant)` 改为 `return ButtonBuilder(ui,id).variant(variant)` —— 链入 `defaultVariant_`，后续 `theme(tokens)` 检测 `hasExplicitVariant_` 后走变体构造 | `button.h:347` 新链 + `button.h:328-329` 新私有字段 + `button.h:112-115` theme() 分支 | **通过** |
| MED-D2 | 不修——理由: 12 零初始化字段由 W2 DevDesk 主题工厂（S0#5）填充 EUI 默认值仅作占位 | 已在 commit message 与 W2 合约注释中声明（button.h Outline 构造器注释: "W2 合约: primarySoftBg/primaryActive 需由 DevDesk 主题工厂填充"） | **接受不修** |
| LOW-B1 | 恢复一行注释 `// flat — no shadow (WeChat flat design)` | `button.h:27` 注释恢复 | **通过** |

**无新冲突引入**: d886b14 的 ControlSizeTokens 新字段为默认成员初始化器（非侵入）；c6bf5fc 的 `theme(tokens)` 分支逻辑仅在 `hasExplicitVariant_=true` 时走变体路径，旧单参数 `theme(tokens, primary)` 调用链行为不变。

### W2 接口契约段同步检查

| 检查项 | 当前值 | S2 要求 | 一致 |
|--------|--------|---------|------|
| `PageVisualTokens::listItemHeight` | `metrics.control.listItemHeight = 40.0f` | 40px（S2.2/S2.5） | OK |
| `PageVisualTokens::buttonHeight` | `metrics.control.buttonHeight = 40.0f` | 40px（S2.1） | OK |
| `PageVisualTokens::tabHeight` | `metrics.control.tabHeight = 40.0f` | 40px（S2.2 页面级） | OK |
| `PageVisualTokens::inputRadius` | `metrics.radius.small = 6.0f` | 6px（S2.3） | OK |

16 新颜色字段命名与 S1 一致（未变）。接口文档（批报）与实现已对齐，无需标出偏差。

**复查结果: 通过 4 / 未通过 0**