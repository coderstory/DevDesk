# W1 EUI 独立审计 A -- 系统开发视角

- **审计分支**: feat/uat62-theme (EUI-NEO worktree `/d/wt-uat62-eui`)
- **5 提交**: 0d3d3c6 / 7c44425 / 13901e9 / a477f43 / 1d8f216
- **依据**: S0/S2 of `/d/wt-uat62-ds/docs/superpowers/plans/2026-10-21-UAT62-设计规格定稿.md`

---

## D1 · 值正确性

| ID | 级别 | 位置 | 问题 | 证据 |
|----|------|------|------|------|
| **H1** | HIGH | `button.h:63` Danger 构造 | 默认 bg 用 `withAlpha(err, 0.12f)`，S2.1 规定 `#FEE2E2` = EF4444 **20%** 透明度（非 12%）。12% 在白色底合成后 g/b 通道与 `#FEE2E2` 偏约 6-7/255，可见差异。 | S2.1 原文: "`#FEE2E2`（浅红，派生：error 20% 透明度）"；代码写死 `0.12f`。注：fallback 路径 `{0.996f, 0.886f, 0.886f}` 就是 `#FEE2E2`，但 `err.a > 0` 时不走 fallback。 |
| **H2** | HIGH | `button.h:68` Danger text | S2.1 Danger 规定 hover 文字 `#FFFFFF`（白字红底），但 ButtonStyle 只有单一 `text` 字段，代码在所有状态统一用 `err`（红字）。hover 将出现红字红底不可读。 | S2.1 Danger: hover "`#EF4444` | 无 | `#FFFFFF`"；ButtonStyle 无 per-state textColor 槽位。 |
| **M1** | MED | `button.h:66-67` Danger pressed | S2.1 规定 pressed `#DC2626`，代码用 `mixColor(hover, black, 0.15f)` 得约 #CB3A3A——红色通道偏暗 ~17/255，绿色偏亮 ~20/255。S2 标注"派生"（非精确值），差异属可接受范围。 | 代码因 W1 不含渲染，此处为静态计算。 |

**补充说明**：Filled 变体委托旧 `ButtonStyle(tokens, true)` 构造，hover/pressed 走 `buttonHover`/`buttonPressed` 混合函数，**未读**新增 `primaryHover`/`primaryActive` 字段。Outline 则显式检查 `primarySoftBg.a > 0` / `primaryActive.a > 0`。两变体对待新字段的一致性缺统一规则，留 W2 评估是否为设计意图。

---

## D2 · API 兼容 / 回归面

| ID | 级别 | 位置 | 问题 | 证据 |
|----|------|------|------|------|
| **L1** | LOW | `button.h:293-295` | `button(ui, id, ButtonVariant)` 重载**接收 variant 参数后丢弃**——函数体仅 `return ButtonBuilder(ui, id)`，variant 未传入构造也未存成员。调用方若写 `button(ui, "x", ButtonVariant::Outline).text("...")` 会得到默认样式（非 Outline）。 | 三个现有调用点 `button(ui, "id")` 不受影响（匹配旧两参数重载）；新重载不会有误用风险。 |
| **OK** | -- | `button.h:106` | `theme(tokens, bool)` 旧重载**完整保留**（`bool primary = true` 默认值未变）。所有现有 `.theme(tokens)` / `.theme(tokens, true)` / `.theme(tokens, false)` 编译通过、行为不变。 | grep 确认 ~60 处存量调用点签名兼容。 |
| **OK** | -- | `theme.h:208-232` | `pageVisuals()` 从位置初始化改为 designated init——字段名显式映射到**完全相同的来源表达式**，行为等价。新增 4 字段用 `.buttonHeight = metrics.control.buttonHeight` 等显式赋值。现存调用方（grep: 无 DevDesk `src/` 引用）不受影响。 | 逐字段行对行比对：16 旧字段的值源未变，designated init 仅做语义强化。 |
| **OK** | -- | `theme.h:9-26` | `color()`/`withAlpha()`/`withOpacity()` 从 struct 后移到前——因新增字段默认初始化器 `= color(...)` 需要 `color()` 先声明。`inline` 函数定义顺序不影响链接行为。 | 编译期需求，非行为变更。 |

---

## D3 · 并发 / 静默失败

| ID | 级别 | 位置 | 问题 | 证据 |
|----|------|------|------|------|
| **M2** | MED | `widget_style.h:30-56` | `WidgetStyleRegistry` 单例的 `entries_`（`std::unordered_map`）**无同步原语**。`set()` 写 + `query()` 读如并发执行 = 数据竞争 (UB)。Meyer 单例保证 `instance()` 初始化线程安全，但不保护后续 map 访问。风险：DevDesk agent 后台线程可能在主线程填充注册表期间 query。 | 若使用模式为"主线程一次性全量填充后只读"，风险降为 LOW。当前代码无任何注释或 `assert` 声明此并发契约。 |
| **M3** | MED | `button.h:41-56` Outline/Ghost/Danger 构造 | 所有 `primarySoftBg.a > 0.0f` / `primaryActive.a > 0.0f` 条件分支：**若 W2 忘记填充这些字段**（默认初始 `{}` = alpha 0），hover/pressed 会静默回退到 `withAlpha(tokens.primary, 0.12f/0.22f)` 混合色——视觉与 S2 规范明显不同但**不报错、不崩溃**。 | 依赖 W2 正确赋值；W1 侧无防护（如 `static_assert` 或运行时 CHECK）。 |
| **OK** | -- | `widget_style.h:40` | `query()` 未命中返回 `std::nullopt`（显式空值），非静默空 `WidgetStyleEntry`。调用方有义务检查，失败模式明确。 | 按 W2 调用方职责设计——W1 侧 fallback 语义正确。 |

---

## D4 · 测试声称复核

| ID | 级别 | 位置 | 问题 | 证据 |
|----|------|------|------|------|
| **OK** | -- | `test_theme_hierarchy.cpp:156` | 批报称 31/32、1 失败 `md.tree.bg != wechat.cardBg` 属 W2 范围——**确认属实**。该断言检查 DevDesk 侧 `core::theme::Tokens::wechat.cardBg` 渲染值，不受 EUI-NEO 以下任何改动影响：(a) ThemeColorTokens 新增 16 字段（在现有字段之后，aggregate init of `light()`/`dark()` 不触及）；(b) pageVisuals() designated init（值源等价）；(c) ControlSizeTokens 新增字段；(d) ButtonVariant 枚举和构造器（不改变既有渲染路径）。EUI-NEO W1 改动对此测试断言是**零效应**的，失败原因在 DevDesk `theme.cpp` 令牌值（W2 域）。 | `wechat.cardBg` 值由 DevDesk `theme.cpp:50/124` 定义；EUI-NEO theme.h 不含 `wechat` 概念。 |

---

## 汇总

| 维 | Findings |
|---|---------|
| D1 值正确性 | **2 HIGH** (H1 Danger bg alpha 12%→20%, H2 Danger hover 文字不可变白) + **1 MED** (M1 pressed 色近似) |
| D2 API 兼容 | **1 LOW** (L1 button(variant) 重载吞参数) + 3 OK |
| D3 并发/静默 | **2 MED** (M2 无锁, M3 fallback 无声) + 1 OK |
| D4 测试 | 1 OK（确认 W2 范围） |

**总检察**: 5 提交 / 3 文件 / 184 行新增 / 38 行删减。结构正确、API 向后兼容。需修 2 HIGH（值错误）+ 2 MED（并发/静默风险），LOW 和其余 MED 可延至 W2 评估。

---

# 复查结论（2026-10-21，修复批 c6bf5fc + ce4501f）

**修复提交**: c6bf5fc (button.h 值修正+逐态文字+工厂接链+合约注释) / ce4501f (widget_style.h 线程合约注释)

## 逐条核对

| Finding | 处置 | 证据 | 结论 |
|---------|------|------|------|
| **H1** Danger bg alpha 12%→20% | 改 `0.12f` → `0.20f` + 注释 `S2.1: #FEE2E2 = error 20% alpha` | `button.h:65` | **通过** |
| **H2** Danger hover 文字无法切白 | 新增 `textHover`/`textPressed` 字段 (line 83-84)；Danger 构造设 `textHover = textPressed = #FFFFFF` (line 70-71)；build() 双栈 text + hoverOpacityFrom 交叉淡入淡出 (line 257-292)——hover/pressed 时正常文字渐隐、白字渐显 | `button.h:70-71 + 83-84 + 257-292` | **通过**。S2 Danger hover+pressed 均为白字，双状态栈满足。textPressed 字段未在 render 中独占消费（与 textHover 共用 hoverClr），但 Danger 两态同白色故无实际差异。 |
| **M1** Danger pressed `#DC2626` | `mixColor(...)` → 硬编码 `core::Color{0.863f, 0.149f, 0.149f, 1.0f}` = 220/38/38 = #DC2626 | `button.h:68` | **通过** |
| **L1** `button(ui, id, variant)` 吞参数 | 新增 `hasExplicitVariant_`/`defaultVariant_` 成员 (line 336-337) + `.variant(ButtonVariant)` 方法 (line 126-130)；`.theme(tokens, bool)` 检查 flag 走 variant 构造 (line 111-117)；工厂 `button(ui, id, v)` 改为 `return ButtonBuilder(ui, id).variant(v)` (line 348) | `button.h:111-117 + 126-130 + 336-337 + 348` | **通过**。端到端链路：`button(ui,"x",Outline)` → 存储 variant → `.theme(tokens)` 检查 flag → `ButtonStyle(tokens, Outline)` → 正确样式。 |
| **M3** Outline/Ghost/Danger fallback 无声 | 加 W2 合约注释说明 fallback 语义 (line 38-39) | `button.h:38-39` | **通过**。行为未改（fallback 仍存在，W2 填充为消除风险的唯一路径），但合约已显式声明。 |
| **M2** WidgetStyleRegistry 无锁 | 加线程安全合约注释：声明 EUI 单线程框架、多线程使用者外部同步 (line 22-23) | `widget_style.h:22-23` | **通过**。合约注释明确责任边界——EUI 框架侧不做内部锁、多线程使用者自己上锁。 |

## 总评

**6/6 全部通过**。值修正精确（H1/M1 逐位对照 S2 hex 值），逐态文字机制可见可验（H2 双栈 + hoverOpacityFrom 交叉过渡），工厂接链端到端闭合（L1 variant→theme 全路径），两处合约注释位置准确（M2/M3 边界声明）。

**副作用审计**: 新增 `textHover`/`textPressed` 字段默认 `{}` (零 alpha)，旧代码不设则为 0 → render 走旧单 text 分支，零回归。`hasExplicitVariant_` 默认 false → `.theme(tokens, bool)` 走旧路径，旧 `.theme(tokens, true/false)` 调用点不受影响。