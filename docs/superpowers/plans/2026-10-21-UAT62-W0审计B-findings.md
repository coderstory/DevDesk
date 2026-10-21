# UAT6.2 W0 审计 B：架构视角四维审计 findings

- **审计者**: 独立审计 B（架构视角）
- **审计对象**: `docs/superpowers/plans/2026-10-21-UAT62-设计规格定稿.md`（commit `a3550dfb`）
- **对照源**: `specs/2026-10-21-devdesk-design-system-v1.md`、`specs/2026-10-21-devdesk-design-system-eui-落地方案.md`、EUI-NEO 源码 `D:/project/gui/EUI-NEO/components/`、DevDesk 主检出 `D:/project/gui/DevDesk/src/`
- **日期**: 2026-10-21

---

## 维度 1：规格符合度（S0 API 映射逐条核）

**结论**：16 条映射中 13 条成立，3 条有事实偏差。Fork 扩展清单 6 项中 1 项完全冗余（Fork #2 四字段全与现有字段值重复）、1 项有字段名冲突（Fork #1 `inputHeight` vs 现有 `control.input`）。

### Findings

| # | Severity | 位置 | 问题 | 建议 |
|---|----------|------|------|------|
| 1 | **HIGH** | S0 Fork 表第 2 行 + `EUI-NEO/components/theme.h:50-63` | Fork #2 提议新增 `button=8.0f / input=6.0f / dialog=12.0f / tab=8.0f`，但 EUI RadiusTokens **已存在完全相同值的字段**：`control=8.0f`(:54)、`small=6.0f`(:53)、`card=12.0f`(:57)。四字段全为语义别名重复——新增零能力、纯增维护面。违反「不瞎改 EUI」原则。 | 删除 Fork #2；S3 迁移表已正确引用现有字段名（`radius.control=8`、`radius.small=6`、`radius.card=12`），不改 EUI 即可满足所有需求。 |
| 2 | **MED** | S0 Fork 表第 1 行 + `EUI-NEO/components/theme.h:73` | Fork #1 提议新增 `inputHeight = 40.0f`，但 `ControlSizeTokens` 已有 `input = 40.0f`(:73)。两字段同名同值——名称冲突会编译失败、或造出两套 `tokens.metrics.control.input` 歧义源。 | 删除 `inputHeight` 新增项，改用现有 `control.input`。其余 `buttonHeight`/`buttonLargeHeight`/`tabHeight`/`tabSmallHeight` 因 EUI 无同义字段，保留。 |
| 3 | **MED** | S1.1 功能语义色表「EUI Token 字段」列 | S1.1 标注 `tokens.success`/`tokens.warning`/`tokens.error`/`tokens.info` 为 "EUI Token 字段"，但 `EUI-NEO/components/theme.h:88-98` 中 `ThemeColorTokens` **仅有 8 个字段**（background/primary/surface/surfaceHover/surfaceActive/text/border/dark/metrics），**没有**任何功能语义色字段。Fork 清单也未列此项扩展。 | (a) 在 Fork 清单中追加 `ThemeColorTokens` 新增字段（success/warning/error/info + textSecondary/textTertiary/textDisabled + primaryHover/primaryActive/primarySoftBg + cardGreen 等辅助色）；或 (b) 明确这些 token 仅存在于 DevDesk `core::theme::ColorTokens`，通过 `componentTokens()` 映射子集传给 EUI。S1 表中的 "EUI Token 字段" 列需按实际归属改写。 |
| 4 | **LOW** | S5.4 真机复验清单 | 写 "开关 **24×24pt** 在 macOS 下动画流畅"，但 S2.6 开关规范与 S0#15 均为 **24×44px**（24 高 × 44 宽）。疑为笔误（"24pt" 只描述了高度档，漏了宽度）。 | 修正为 "24×44pt"。 |
| 5 | **LOW** | S0 entry #12 | 标注 "Fork 否（构造器）"——认为 `.colors()+textColor()+border()` 组合足够。但 entry #8 标注 "Fork 是" 需要 `ButtonVariant` 枚举。两条目对同一问题的结论冲突：要不要做变体枚举？ | 统一口径。推荐保留 Fork #3（ButtonVariant 枚举）——组合式 API 虽然可达，但 40+ 调用点每次写 `.colors()+border()+textColor()` 三行组合比 `button(...).variant(Filled)` 更易出错（漏设 hover/pressed 即静默不一致）。 |
| 6 | **INFO** | S0 entry #9 | "不存在全局 Apply" 正确，但未提及 EUI `DialogBuilder::theme(tokens)` 可单次注入整棵子树的主题——这是比逐站 `.theme(tokens)` 更轻的替代路径，可用于弹窗族统一改造（S3.10）。 | 在 S0 补充说明：弹窗类可通过 `dialog(ui,id).theme(tokens)` 一次性完成子树注入，不需逐控件传。 |

---

## 维度 2：可维护性（命名一致性 + 迁移杠杆）

**结论**：S1 令牌命名与 DevDesk `ColorTokens` 现有字段名一致（`primaryHover`/`primaryPressed`/`primarySoftBg` 沿用现有 snake_case 风格）。47 条迁移集中改 `theme.cpp` 常量（杠杆点正确），但 S3 多处指示页面"接玻璃"与 S6.1 退役玻璃的决定直接矛盾，且 S1 token 归属（EUI vs DevDesk）模糊会拖慢 W1 落地。

### Findings

| # | Severity | 位置 | 问题 | 建议 |
|---|----------|------|------|------|
| 7 | **HIGH** | S3.2#8 / S3.3#8 / S3.5#6 / S3.7#7 vs S6.1 | **四对直接矛盾**：S3.2#8 让 markdown 「接玻璃/半透底」、S3.3#8 让 todo 用「玻璃帮手」、S3.5#6 让 agent 「接玻璃（B3 台账）」、S3.7#7 让 settings 「补半透底面」。但 S6.1 裁决 "玻璃体系退役→实底卡片"，明确 (1) `glass_card.h` 归档停用、(2) 所有卡片 `rect.color(#FFFFFF)`、(3) 移除 `drawGlassCardLayers`。**实现者照 S3 做会造出 S6 说要删的代码**——四页改法必须与 S6.1 对齐：用 `#FFFFFF` 实底 + Sm 阴影替换所有「接玻璃」指令。 | S3 的 #8/#8/#6/#7 四处「接玻璃」条目全部改为 "实底 #FFFFFF + Sm 阴影"（对齐 S6.1 落地口径）。同时在 S6.1 影响面清单补充这四处的具体 file:line。 |
| 8 | **MED** | S1.1 全表「EUI Token 字段」列 | 该列混用两个不同 struct 的字段名：(a) EUI `ThemeColorTokens`（8 字段：primary/surface/border 等）vs (b) DevDesk `core::theme::ColorTokens`（含 wechat 子结构体、success/warning/error/info 等 20+ 字段）。S1 写 "tokens.textSecondary" 时，W1 实现者去 EUI theme.h 找字段会扑空——它在 DevDesk ColorTokens、不在 EUI ThemeColorTokens。 | 拆为两列或加注：标注每个 token 在哪个 struct。例如 `tokens.textSecondary`→ DevDesk ColorTokens.textSecondary（非 EUI 字段），`tokens.primary`→ 两 struct 均有。Fork 清单补上 EUI ThemeColorTokens 需新增的字段（同 finding #3）。 |
| 9 | **LOW** | S1.1 间距表 + `EUI-NEO/components/theme.h:37` | S1 最小间距档是 `spacing.tiny=4`，但 EUI `SpacingTokens` 有更小档：`hairline=1`(:35)、`micro=2`(:36)、`tiny=4`(:37)。S1 直接跳过了 1 和 2 档——是否刻意？当前实现中 `dialog.h` 多处用 `metrics.spacing.hairline` 做边框宽，若 v1.0 设计规范没有 1px 档，未来新增组件可能被迫用 4px 做最小边框。 | 在 S1 补充一句：1px/2px 档保留给边框/微调专用（hairline/micro），常规间距从 tiny=4 起。不改值。 |
| 10 | **LOW** | S1.1 字体表「typography.heading 改 24」 | EUI `TypographyTokens.heading` 当前已是 `24.0f`(`theme.h:21`)，不需要改。S1 的实际动作是把 DevDesk 的 `fontSizePage` 从 32→24（`theme.cpp:74/148`），与 EUI heading 对齐。列名写 "改 24" 容易让 W1 实现者误以为需要改 EUI heading 的值。 | 该行 "改 24" 改为 "不变(已 24)"，迁移动作栏保留 "theme.cpp:74/148 32→24"。 |

---

## 维度 3：测试有效性（G1+G4 断言点推演）

**结论**：G1 与 G4 的流程设计合理，场景覆盖完整。但以下缺陷会让实现回退时测试不红（假阴性）。

### Findings

| # | Severity | 位置 | 问题 | 建议 |
|---|----------|------|------|------|
| 11 | **MED** | G1 step 3+4 | Step 3 断言「下一步按钮 disabled 直到名称非空」，step 4 操作「输入无效名(纯空格)点下一步」。**disabled 的按钮不可点击**——若测试用指针注入强行点击 disabled 按钮，需要先确认 EUI 的 disabled 实现是否仍分发 onClick（若处理链在 disabled 时静默吞事件，点击即无效果、测试通过但原因错误）。若实现者把 validator 删了但忘了设 disabled（按钮可点、点了直接进 step 2），step 4 的点击会"成功"进入 step 2——测试变绿但功能坏了。 | 断言拆两条：① `button.disabled == true`（状态断言，挡「忘了设 disabled」）；② 指针注入点击后 `currentStep 仍为 1`（行为断言，挡「disabled 不生效」）。两条缺一不可。 |
| 12 | **MED** | G1 step 6 | "AI 起草按钮 Outline 样式（白底+绿边）"——若按钮样式从 Outline 被改成 Ghost（纯文字、无边框），按钮仍可见、仍可点击、后端仍返回草稿填入文本域——功能完全正常但样式回归。断言只验证了功能（"点击应调后端返回草稿填入文本域"），未验证样式（bg=white, border=primary）。 | 追加一条视觉断言：按钮 bg 色 = `#FFFFFF` 或 surface、border color = `#36D399`。若无 probe（像素读取），至少断言 builder 参数的 colors/border 调用值。 |
| 13 | **LOW** | G4 step 9 | "0.3s 后 tooltip 显示全名"——固定延迟断言在 CI 负载波动下天然 flaky。tooltip 出现依赖 hover 状态 + 定时器，若实现改用 CSS transition delay 而非 timer，0.3s 的精确断言可能恒绿（tooltip 在 0.31s 出现，测试等 0.3s 刚超时判失败——但这对用户无差别）。 | 放宽为「hover 后 ≤0.5s 内可见」，或用事件信号（tooltip 渲染完成事件）替代时间等待。 |
| 14 | **LOW** | G1 step 13 | Step 13 要求「真实后端」实测——这是 live test 层需求，但 G1 header 声明 "每条流程完整跑通并核对结果"，未区分 probe 层（step 1-12）与 live 层（step 13）的执行边界。 | 将 step 13 独立为 G1-LIVE 子流程，注明执行环境（需真实网关+token）与 G1 probe 层（step 1-12）分属不同测试套件。 |

---

## 维度 4：文档口径一致（S1↔S2↔S3↔S4↔S6 交叉核）

**结论**：S1↔S2 值互一致（逐对抽样核验通过）。S3↔S6 存在前述矛盾（finding #7）。S4 弹性公式有一处真值错误。

### Findings

| # | Severity | 位置 | 问题 | 建议 |
|---|----------|------|------|------|
| 15 | **MED** | S4.3 弹窗高度上限公式 | "工作区可用高度 = 窗口客户区高度（640 最小档）; 弹窗最大高度 = min(工作区可用高度 - 48, 内容自然高度); 最小档：弹窗最高 = 640 - 48 = 592px"。**640 是窗口最小尺寸（含非客户区外框），不是客户区高度**。按 S5.3：macOS 非客户区 `{0,28}` → 最小客户区高 = 640-28 = 612pt、最大弹窗高 = 612-48 = **564pt**。Windows 外框 `{14.4, 37.6}@96dpi` 差距更大。按当前公式写 592，最小窗口下 592 > 564 会溢出工作区——弹窗底被截。 | 修正公式：`弹窗最大高度 = min(窗口客户区实际高度 - 48, 内容自然高度)`。客户区高度 = 窗口高 - 非客户区外框高（平台值取 `window_layout_*.cpp`）。640 只是设计下限不是计算常量。 |
| 16 | **LOW** | S3.4 #1 "左 240px : 右 480px" vs S4.4 公式 | S3.4 写 "1:2（左 240px : 右 480px）"。但 S4.4 公式 "右 = 可用宽 - 240"——最小档（1000）：主区 = 1000-200-64=736，右 = 736-240=**496**。240:496 ≈ 1:2.06。480 是窗口约 960px 时的值，不是最小档。 | 改为 "左 240px（固定）：右 flex（主区宽 - 240），最小档下右 = 496px，居中窗口时 ≈480px"。 |
| 17 | **LOW** | S1.2 entry #16 | `wechat.linkBlue = #576B95` → `info = #3B82F6`。语义迁移：linkBlue 专用于链接文字，info 是通用信息色。改后所有原用 linkBlue 的地方（链接、可复制文本）会变成 info 蓝——视觉上没问题（都是蓝色），但若以后 info 用于非链接场景（如 info toast 图标），改 info token 值会把链接色也带走（耦合）。 | 要么保留独立 `link` 字段（不合并到 info），要么在接受耦合的前提下文档明写。 |
| 18 | **INFO** | S6.3 豁免清单 #3 | "primarySoftBg 始终为 #E6FBF2（mint 淡底，不与强调色联动）"——当用户切换到 Purple 强调色（`setAccent(Purple)`）时，primary 变紫但选中态背景仍是薄荷绿，视觉割裂。 | 如是有意设计（用户已确认）则保持；否则考虑 primarySoftBg 跟随 accent 联动（四强调色各有对应的 softBg 值，见 `theme.cpp:160-221` 已有实现）。 |

---

## 汇总

| 维度 | 结论 | HIGH | MED | LOW | INFO |
|------|------|------|-----|-----|------|
| 1. 规格符合度 | 13/16 映射正确；Fork #2 四字段全冗余（最需纠正） | 1 | 2 | 2 | 1 |
| 2. 可维护性 | S3 四处「接玻璃」与 S6.1 退役决策硬冲突（最需纠正） | 1 | 1 | 2 | 0 |
| 3. 测试有效性 | G1 step 3+4 disabled 断言缺状态验证（假阴风险） | 0 | 2 | 2 | 0 |
| 4. 文档口径一致 | S4.3 弹窗高度公式使用窗口高而非客户区高（真值错误） | 0 | 1 | 2 | 1 |
| **合计** | — | **2** | **6** | **8** | **2** |

**排序建议**（按落地修复优先度）：Fork #2 删除 → S3 玻璃条目修正 → S4.3 弹窗公式修正 → G1 disabled 断言补强 → ThemeColorTokens 字段归属澄清 → 其余 LOW 项。