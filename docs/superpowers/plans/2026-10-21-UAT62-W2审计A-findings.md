# W2 令牌迁移独立审计 A（迁移保真视角）— findings

- **对象**: feat/uat62-design-system (8 commit) vs 规格 2026-10-21-UAT62-设计规格定稿.md (S1/S2/S6)
- **日期**: 2026-10-22

---

## 1. 映射保真

**一行**: S1.2 46 条逐条值与 diff 对齐（componentTokensWeChat 内 `surfaceActive` 语义从灰变薄荷系连锁效应外全匹配）；57 处扫荡是 wechat.* 引用站点计数（15 种字段 × 多文件复用），非 47 条映射计数——差值 +10 来自同一 S1 token 被多处代码引用（如 `textTertiary` 73 处、`primary` 61 处），不是映射外越界。

### F1.1 — 非缺陷 · 计数口径偏差
- 47 是 S1.2 迁移表条目数（#1-#46 ≈ 46-47 条），57 是 `b6e3161d` 中 wechat.* 引用替换的总引用站点数（623 行 `-wechat.*` 中非重复字段类型 15 种）。
- 两者维度不同：映射数 vs 引用点数。差值不是「多改了 10 个映射外的站」，而是同一映射在 26 文件的多处引用。
- 验证：15 种 wechat 字段全部在 S1.2 的 #1-#17 覆盖范围内，没有发现映射表外的 wechat 字段被改。

### F1.2 — 注意 · componentTokensWeChat 连锁语义变
- `81c581cd` 把 `componentTokensWeChat()` 从 `wechat.*` 子槽位改读主槽位后，`tokens.surfaceActive = t.colors.primarySoftBg`（#E6FBF2 薄荷绿），旧值 `wechat.selectedBg` 是灰（#E5E5E5）。
- 这不是 bug——S1.2 #4 把 `selectedBg` 定义为 `primarySoftBg = #E6FBF2`，映射链路忠实执行。但 `surfaceActive` 语义从「纯灰有源底色」变成「薄荷绿软底」，影响所有用 `componentTokensWeChat` 的组件（`surfaceActive` 用于组件 third-state 背景）。
- 规格级注意点：S1 未显式声明 surfaceActive 应保持灰色而非联动 primarySoftBg。当前行为忠实于 S1.2 令牌替换，保守接受；若后续 UI audit 发现组件显色不合预期，根因在此。

### F1.3 — 注意 · 57 处扫荡统计线残余
- `b6e3161d` 后 src/ 无 `t.colors.wechat.*` 生产代码残留（全部通过）。测试文件 `test_claude_ui.cpp` 仍有 ~30 处 `t.colors.wechat.*` 直接引用——非 W2 范围（W2 只迁 src/），测试锚清账 lagging。
- `test_settings.cpp:1391` 也有 1 处 `t.colors.wechat.textPrimary` 残留。

### F1.4 — 非缺陷 · d8fc5339 UI Kit 尺寸迁移映射完整
- S1.2 #35 (按钮 34→40)、#37 (内边距 10→16)、#39 (弹窗 inset 28→24)、#40 (bodyInset 56→48)、#45 (按钮圆角 10→8) 全部到位。`metrics.h` 的 `kDialogInsetX 28→24` 与 `kDialogBodyInsetX 56→48` 同步更新，`button.h` 的 `kButtonHeight 34→40` 和 `kButtonPaddingX 10→16` 已改。`dialog_form.cpp` 圆角 `10→8` 已改。

---

## 2. 工厂/注册正确性

**一行**: DevDeskTheme light/dark 全部 16+ 新字段值与 S1.1 令牌总表逐条匹配；WidgetStyleRegistry 4 按钮变体 × 4 状态 + Tab 双尺寸 + ListItem 的值全部对应 S2 状态字典；注册时机 inline（工厂调用时注册），无初始化序问题。

### F2.1 — 缺陷 · 深色主题 primaryText 缺位
- `DevDeskTheme::light()` 未显式设 `primaryText` 字段（依赖结构体默认值），而 `DevDeskTheme::dark()` 同样未设。light 路径的实际 primaryText 值由 `theme.cpp` 的 `setLight()` 函数中 `c.primaryText = eui::Color("#23B07F")` 提供，工厂不覆盖。
- `DevDeskTheme::dark()` 同样不设 primaryText，依赖 `setDark()` 中 `c.primaryText = eui::Color("#36D399")`。
- 影响：工厂 l/d 与 setLight/setDark 的 primaryText 值同源（theme.cpp），但工厂返回的 ThemeColorTokens 仅部分字段（primary/primaryHover/primaryActive 等已显式设），若未来直接使用 `DevDeskTheme::light()` 返回的 tokens 而不再经 `setLight()` 路径，primaryText 等字段会零初始化——**工厂与旧路径不完全对等**。
- 评级：中等。当前两条路径都调 `setLight()`/`setDark()`（含完整字段），暂无功能断裂，但工厂作为「单出口」的完整性有缺口。

### F2.2 — 注意 · WidgetStyle 注册的 Ghost pressed 背景硬编码
- `devdesk_theme.h:161-163`：`Ghost.pressed` 背景色 `#D1F5E4` 是硬编码字面量（注释写 `primarySoftBg ~10% darker`），非从 primarySoftBg 计算派生。
- S2.1 Ghost pressed 说明「#E6FBF2 加深」，加深到什么程度未量化。当前实现色 `#D1F5E4` 视觉上可接受但不精确可追溯。
- 低风险；建议改为 `withOpacity(t.primary, 0.12f)` 之类的派生方式，或至少在规格里量化加深值。

---

## 3. 测试改动合法性

**一行**: 字号/圆角断言（16feb047）全部来自 S1 令牌迁移（15→14、32→24、10→8 等）——锚点有效、非为过测试调阈值；三个降阈值中 lumadiff 0.4→0.35（合理，primary 变亮不可避）、wcag 4.5→4.0（可接受，值真 4.19:1）、wcag 4.5→2.5（**严重退化，结论见下**）。

### F3.1 — 合格 · 16feb047 字号/圆角锚点迁移
- `test_theme.cpp:29` fontSizeBody 15→14 ← S1.2 #24
- `test_theme.cpp:31` radius 10→8 ← S1.2 #30
- `test_theme_fontscale.cpp:9-18` 全 7 处字号/圆角更新 ← S1.2 #23 #24 #25 #30 #31
- 逐条验证通过：每条新锚点值在 S1.2 迁移表有直接出处。

### F3.2 — 合格 · lumadiff 0.4→0.35（onPrimary vs primary）
- S1 #17: onPrimary = #FFFFFF 不变。primary #36D399 比旧 #D97757 亮约 15%（luminance 0.468 vs 0.303），lumadiff 自然收窄。
- #FFFFFF vs #36D399 = ~0.53 luminance diff > 0.35，断言仍过。阈值被动调整，锚点保护力基本保留。

### F3.3 — 注意 · wcag 4.5→4.0（textTertiary on primarySoftBg）
- S1 值：textTertiary #6B7280 / primarySoftBg #E6FBF2。实测 ~4.19:1，低于 WCAG AA 4.5:1（正常文本）。
- 阈值降到 4.0 反映新值现实。4.0 > WCAG AAA 大文本 3:1 阈值，仍在可读范围。
- 根源：primarySoftBg #E6FBF2 是薄荷浅绿而非纯灰，比旧 selectedBg #E5E5E5 略亮（luminance 0.927 vs 0.892），textTertiary #6B7280 也比旧 #999 略亮——组合恰在 4.5 线下。
- 可接受但应记录：该组合不满足 WCAG AA 正常文本，设计系统应在文档中标注此已知差异。

### F3.4 — **缺陷 · wcag 4.5→2.5（primaryText on background）← 三阈值裁决核心**
- S1.2 #5: `wechat.primaryText = #067036 → primaryText #36D399（文字用法）或新增 #23B07F 加深档`。
- 实现选择：#23B07F 作为 primaryText。
- 实测：**#23B07F on #FAFAF9 = ~2.64:1，远低于 WCAG AA 4.5:1**（正常文本）和 WCAG AA 3:1（大文本）。
- 旧值 #067036 on #FAF9F5 = ~5.8:1（通过 AA）。

**裁决**：
1. **`#23B07F` 用错场景**。S1.2 #3 中 #23B07F 是 `primaryActive`——按钮按下态的背景色，不是文字色。S1.2 #5 说 `primaryText` 应优先用 `#36D399`（虽然在浅背景上也不达标），或「新增 #23B07F 加深档」，但此处的「加深」应是相对 #36D399 再加深一档用于文字（即比 pressed 色更深），而非直接复用 `primaryActive` 值。
2. **primaryText 必须深于 primaryActive**（文字在白色背景需要更高对比度——这是 WCAG 的基本前提）。建议值 ~`#1B8C5C`（约 4.5:1 on #FAFAF9）或提请改规格。
3. **阈值 4.5→2.5 构成护栏退化**。2.5:1 低于任何 WCAG 等级（正常文本 AA 4.5、大文本 AA 3.0、大文本 AAA 4.5）。测试从「极可能有可读性问题才失败」降为「完全不可读才失败」——已失去保护力。此阈值必须在 primaryText 值修复后**回滚到 ≥4.5**。

---

## 4. 遗留完整性

**一行**: `6016a3e5` 实际删除 14 行 `const auto& wc = t.colors.wechat;`（12 文件，非 commit message 声称的 9 处），src/ 残余 2 处（sidebarBgTinted 仍使用中 → 有意保留待 W3），tests/ 残余 5 文件（各有 2-12 个 `wc.*` 引用 → 非 W2 范围）。

### F4.1 — 非缺陷 · 计数偏差
- Commit `6016a3e5` 消息说「删除 9 处」，实际 diff 显示 14 处。经由 `wc.` 使用引用计数验证：被删的 14 处 `wc` 变量在对应函数内**无任何 `wc.xxx` 引用**（否则 -Werror 会报 unused-variable，commit 也不会声称「致 -Werror 拒收」），删除正确。

### F4.2 — 非缺陷 · src/ 残余 2 处 sidebarBgTinted
- `app_sidebar.cpp:335` 和 `splash.cpp:97` 的 `const auto& wc = t.colors.wechat;` 仍被 `wc.sidebarBgTinted` 使用（各 1-2 次引用），不是孤儿。S6.7 裁决 sidebarBgTinted → sidebarBg #F0FDF7 且 `b6e3161d` commit message 明确「sidebarBgTinted保留待W3」——这是有意的分批复用，不是遗落。

### F4.3 — 注意 · tests/ 残余 5 文件
- `test_agent_ui.cpp:4814`（2 个 wc.* 引用）、`test_app_sidebar.cpp:655`（3）、`test_home.cpp:325`（4）、`test_markdown.cpp:1240`（12）、`test_ui_modern.cpp:1303`（3）——5 文件共 24 个 wc.* 引用。
- 这些文件不在 W2 迁移范围（W2 只迁 src/ 生产代码），但 test 锚清账 commit（`17a268a2`）只更新了 4 个测试文件中的断言值，未触达这 5 个。它们持有旧 wechat.* 断言，会随 wechat 子槽值变自动通过（因为 `setLight()` 中 `wechat.*` 已被赋 mint 新值，不是旧微信绿色），不会挂测试。
- W3 逐页改造时应清理这些残留——当前不构成功能缺陷。

---

## 汇总

| 维 | 结论 | findings |
|----|------|---------|
| 1 映射保真 | S1 值 100% 落 diff；+10 是引用点计数差异非越界 | 无缺陷，3 注意，1 非缺陷 |
| 2 工厂/注册 | 令牌值与 S1/S2 匹配；factory dark 缺 primaryText 字段显式初始化 | 1 缺陷(MED)，1 注意 |
| 3 测试改动 | lumadiff/wcag-4.0 合理；**wcag-2.5 是严重设计退化** | 1 缺陷(HIGH)，1 注意，2 合格 |
| 4 遗留完整性 | 14 行删除到位；src/ 残余有因（W3 分批）；tests/ 残余待 W3 | 无缺陷，2 非缺陷，1 注意 |

**三阈值裁决结论**：lumadiff 0.4→0.35 合理（primary 变亮不可避），wcag 4.5→4.0 可接受（值真 4.19:1），wcag 4.5→2.5 **不可接受**——`#23B07F` 是 primaryActive（pressed 态色）不是文字色，应改用 ≥`#1B8C5C` 级暗绿使 primaryText on background 恢复到 ≥4.5:1，阈值回滚到 `wcagContrast(primaryText, background) >= 4.5`。当前 2.5 阈值已将 WCAG 护栏降为摆设。