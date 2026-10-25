# ADR 0005：接受低对比度选中态色对（#36D399 on #E6FBF2）

- **日期**：2026-10-23
- **状态**：Accepted（当前接受，附重审触发条件）
- **决策方**：用户拍板（2026-10-23，H5/H10 整改轮）+ 主会话记录
- **相关**：台账 H5/H10 行；审计 `2026-10-23-UAT62-W3.5-audit-A.md`（H10 对比度门禁、H11/H12 块面 Tab 验证）

## 背景

v1.0 设计系统规定选中态色对为**前景 `#36D399`（primary）on 背景 `#E6FBF2`（primarySoftBg）**，用于块面 Tab 选中、列表选中、按钮 hover 等强调态。该色对的 WCAG 2.1 对比度比约 **1.63:1**，远低于 AA 文本对比度下限 4.5:1（亦低于非文本 3:1），属 WCAG 失败。

W3.5 修复轮 H10 引入对比度门禁断言（WCAG 2.1 相对亮度公式，亮度差 ≥0.05 或存在 1px `borderDefault` 描边，OR 门）。H5 把左侧导航栏选中态改为白色 elevated 卡 + 描边（因侧栏底 `#F0FDF7` 上 `#E6FBF2` 亮度差不足），但**其余选中面仍保留 `#E6FBF2` 底**口径。

## 5 对位置（抄自组件状态字典——单一真相源）

5 对 `#36D399 on #E6FBF2` 的权威定义位置在 DevDesk 组件样式注册表 `src/core/devdesk_theme.h`（`reg.set(...)`，`WidgetStyleEntry{bg, text, border, radius, font, height}`），逐条如下：

| # | 组件·状态 | 位置 | bg | text | border | 三要素 |
|---|-----------|------|----|------|--------|--------|
| 1 | `Button:Outline:hover` | `devdesk_theme.h:140` | `primarySoftBg` #E6FBF2 | `primary` #36D399 | `primary` #36D399（1px） | 底色块 + 字色 + 描边 |
| 2 | `Button:Ghost:hover` | `devdesk_theme.h:151` | `primarySoftBg` #E6FBF2 | `primary` #36D399 | 透明 | 底色块 + 字色 |
| 3 | `Tab:Page:selected` | `devdesk_theme.h:188` | `primarySoftBg` #E6FBF2 | `primary` #36D399 | 透明 | 底色块 + 字色 + 圆角块面 |
| 4 | `Tab:Small:selected` | `devdesk_theme.h:196` | `primarySoftBg` #E6FBF2 | `primary` #36D399 | 透明 | 底色块 + 字色 + 圆角块面 |
| 5 | `ListItem:default:selected` | `devdesk_theme.h:204` | `primarySoftBg` #E6FBF2 | `primary` #36D399 | 透明 | 底色块 + 字色（列表另加左 3px 竖线，见 S2.5） |

> **审计交叉引用**：审计 A（`2026-10-23-UAT62-W3.5-audit-A.md`）H11/H12 验证块面 Tab 三参数齐全（primarySoftBg/8px 圆角/primary 字，审计 A 第 108 行），H10 对比度门禁断言非恒真、去描边即红（第 111/129 行）。审计 B F1（`W3.5-audit-B.md`）记录「全 app 三种并存选中视觉」并已核销回写 S2.5/S6.7。本 ADR 的 5 对位置取自样式注册表（状态字典的单一真相源），与审计 A 的选中面验证一致；审计 A 未逐行列出注册表行号，故位置以代码注册表为准、审计 A 作旁证。

## 决策

**当前接受这 5 对为 WCAG 失败但不修复**，理由 = **三要素识别**：

- **底色块**：`#E6FBF2` 浅绿底块本身与未选中态（透明/`surface`）形成面块边界，提供「这是选中区」的视觉锚；
- **左竖线**：列表选中态（#5）另有左侧 3px `#36D399` 实线竖条作硬标识（S2.5 规定）；
- **字色**：选中文字 `#36D399` 与未选中 `#4B5563`（textSecondary）色相不同，提供字色区分。

三要素叠加（底色块 + 左竖线 + 字色）使选中态在**功能上可识别**，即便色对比度本身不达 WCAG 文本下限。审计 A H10 门禁的 OR 门（`hasBorder || enoughContrast`）使带描边的对（#1）通过亮度校验；不带描边的对（#2/#3/#4/#5）靠块面 + 字色 +（列表）竖线三要素接受。

## 未来触发重审的条件

满足任一即重新评估本 ADR（是否补描边/加深底色/换字色）：

1. **用户目检说看不清**：用户真机反馈选中态与未选中态不可分辨（复现 H5/H10 原症状）；
2. **设计规范升版**：v1.0 之后的设计规范修订选中态色对或对比度要求；
3. **对比度门禁升级**：H10 门禁从「亮度差 ≥0.05 或有描边」收紧为强制 WCAG AA 4.5:1（去掉 OR 门的描边旁路）；
4. **移除三要素之一**：如列表选中态取消左 3px 竖线、或 Tab 取消圆角块面改下划线，导致三要素识别失效。

## 影响

- 5 对保持现状，不追加描边/换色（#2/#3/#4/#5 仍透明 border）；#1 已有 `primary` 描边。
- 后续新增选中/hover 态若沿用 `primarySoftBg + primary` 组合，自动落入本 ADR 接受范围；若改用其他色对则不受本 ADR 覆盖。
- 无障碍合规口径：本 ADR 是「功能可识别但 WCAG 失败」的有意取舍，对外宣称无障碍合规时须如实标注此例外。
