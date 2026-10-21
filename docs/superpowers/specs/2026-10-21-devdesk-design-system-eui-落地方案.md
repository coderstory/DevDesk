# 基于 EUI-NEO 的 DevDesk 统一设计规范落地方案（原文存档，2026-10-21）

> 用户交付的 C++ 落地方案示例，逐字保留。定位 = **架构意图与结构指引**：全局自定义主题 + 控件样式覆盖注册。示例中的 API 名（`EUI::NEO::Theme::Create` / `SetWidgetStyle` / `Button.Filled` 等）为示意形态——实现批必须映射到 EUI-NEO **真实**主题系统（`ThemeColorTokens`、组件 Style 构造器、现有 token 注入链），EUI 缺的全局控件样式注册能力在本 fork 内扩展实现（用户已授权：EUI 是定制 fork 可改、但不瞎改）。

## 一、核心思路：全局自定义主题

EUI-NEO 原生支持**主题系统**，可以从全局统一管控颜色、边框、阴影、圆角、控件状态等所有视觉属性。不需要逐个页面、逐个控件修改样式，只需要定义一套 `DevDesk 专属主题`，全局注入后所有控件自动继承统一风格，从根源解决每页样式各异的问题。

EUI-NEO 默认是蓝白主题，将其完整替换为薄荷绿轻量化主题，完全匹配设计规范。

## 二、主题层：设计令牌映射

```
// DevDeskTheme.h  主题头文件，纯C++常量定义
#pragma once
#include "EUI/NEO/Theme.h"

namespace DevDesk::Theme
{
    // 品牌主色体系
    constexpr Color Primary        = { 0x36, 0xD3, 0x99, 0xFF }; // 默认态
    constexpr Color PrimaryHover   = { 0x2E, 0xC4, 0x8F, 0xFF }; // 悬浮态
    constexpr Color PrimaryActive  = { 0x23, 0xB0, 0x7F, 0xFF }; // 点击态
    constexpr Color PrimaryBg      = { 0xE6, 0xFB, 0xF2, 0xFF }; // 浅背景/选中态

    // 数据卡片辅助色（统一饱和度20%、明度95%，保证视觉重量均衡）
    constexpr Color CardGreen  = { 0xE6, 0xFB, 0xF2, 0xFF };
    constexpr Color CardBlue   = { 0xE0, 0xF4, 0xFF, 0xFF };
    constexpr Color CardOrange = { 0xFF, 0xF1, 0xE0, 0xFF };
    constexpr Color CardPurple = { 0xF3, 0xE8, 0xFF, 0xFF };
    constexpr Color CardCyan   = { 0xDF, 0xFA, 0xF6, 0xFF };

    // 功能语义色（低饱和适配柔和风格）
    constexpr Color Success = { 0x10, 0xB9, 0x81, 0xFF };
    constexpr Color Warning = { 0xF5, 0x9E, 0x0B, 0xFF };
    constexpr Color Danger  = { 0xEF, 0x44, 0x44, 0xFF };
    constexpr Color Info    = { 0x3B, 0x82, 0xF6, 0xFF };

    // 中性色（文字/边框/背景）
    constexpr Color TextPrimary   = { 0x1F, 0x29, 0x37, 0xFF }; // 一级文字
    constexpr Color TextSecondary = { 0x4B, 0x55, 0x63, 0xFF }; // 二级文字
    constexpr Color TextTertiary  = { 0x6B, 0x72, 0x80, 0xFF }; // 三级文字
    constexpr Color TextDisabled  = { 0x9C, 0xA3, 0xAF, 0xFF }; // 禁用文字
    constexpr Color BorderBase    = { 0xE5, 0xE7, 0xEB, 0xFF }; // 默认边框
    constexpr Color BgPage        = { 0xFA, 0xFA, 0xF9, 0xFF }; // 页面底色
    constexpr Color BgContainer   = { 0xFF, 0xFF, 0xFF, 0xFF }; // 容器/卡片底色
    constexpr Color BgHover       = { 0xF3, 0xF4, 0xF6, 0xFF }; // 悬浮背景
}
```

### 全局样式令牌

| EUI-NEO 主题字段 | 配置值 | 解决的核心问题 |
| --- | --- | --- |
| `Radius.Small` | 6px | 输入框、开关统一圆角 |
| `Radius.Default` | 8px | 按钮、小卡片统一圆角 |
| `Radius.Medium` | 12px | 弹窗、任务卡片统一圆角 |
| `Radius.Large` | 16px | 首页数据卡片统一圆角 |
| `Shadow.Sm` | `0 1px 3px rgba(0,0,0,0.04)` | 普通卡片统一轻阴影 |
| `Shadow.Md` | `0 4px 12px rgba(0,0,0,0.06)` | 弹窗统一中阴影 |
| `FontSize.Display` | 24px | 页面大标题统一字号 |
| `FontSize.Heading` | 20px | 弹窗/区块标题统一 |
| `FontSize.Title` | 16px | 二级标题/卡片标题统一 |
| `FontSize.Body` | 14px | 正文/按钮文字统一 |
| `FontSize.Caption` | 12px | 辅助文字/时间统一 |
| `Spacing.Base` | 8px | 全局8px栅格间距 |

## 三、组件级统一规范

### 1. 按钮组件（Filled / Outline / Ghost 三变体）
- **主按钮（Filled）**：默认 Primary 色填充，hover 变 PrimaryHover，点击变 PrimaryActive，圆角 8px，文字白色 14px/600，高度统一 40px
- **次按钮（Outline）**：白色底 + Primary 色边框，hover 变 Primary 浅背景，圆角 8px，文字 Primary 色 14px/600，高度 40px
- **文字按钮（Ghost）**：透明底，hover 变 BgHover，文字 Primary 色，用于三级操作

修复：首页底部三按钮统一高度、按优先级分配「主+次+次」；零散按钮全部归入变体；弹窗底部按钮统一右对齐、主按钮在最右、间距 8px。

### 2. Tab（块面选中式，全局统一）
- 未选中：透明背景 + TextSecondary 文字
- 选中：PrimaryBg 背景 + Primary 色文字 + 圆角 8px
- 内边距：水平 16px，高度 40px；Tab 间距 4px
- 应用范围：笔记本视图切换、切换器详情 Tab、设置页 Tab、编辑弹窗 Tab——全部自动统一

### 3. 列表与侧边栏（共用列表主题令牌）
- 未选中：透明背景 + TextSecondary 文字
- 选中：PrimaryBg 背景 + 左侧 3px Primary 色竖线 + Primary 色文字
- 悬浮：BgHover 背景
- 列表项高度统一 40px，水平内边距 16px
- 修复：左侧导航、笔记目录、Profile 列表选中态全部统一

### 4. 输入框与表单
- 单行输入框高度 40px，圆角 6px，BorderBase 边框，聚焦时 Primary 边框
- 文本域仅高度变化，边框圆角与单行完全一致
- 错误状态：Danger 色边框 + 下方 8px 处 Danger 色提示文字
- 占位符统一 TextDisabled 色

### 5. 弹窗组件
- 白色背景 + 12px 圆角 + Md 阴影
- 标题 20px/600 左对齐，内容区内边距 24px
- 底部操作栏固定在底部，按钮右对齐，主按钮在最右侧
- 内部分隔线统一 BorderBase 色

### 6. 数据卡片 & 任务卡片
- **首页数据卡片**：对应辅助色背景、无描边、16px 圆角，内部文字层级统一（标题 12px 三级 + 数据 28px 一级 + 说明 12px 三级），等宽等高
- **Todo 任务卡片**：白色背景 + 8px 圆角 + Sm 阴影，**仅左侧色条区分优先级**，禁止整卡涂色

## 四、9 个页面的具体修复点

| 页面 | 问题 | EUI-NEO 修复方式 |
| --- | --- | --- |
| 工作台首页 | 卡片色彩不均、按钮不等宽、刷新样式怪、空状态空白 | 1. 卡片用 5 个辅助色变量 2. 底部按钮 Row 均分宽度按主/次分配 3. 刷新改 Outline 次按钮 4. 空状态用 Empty 组件（图标+文字+引导按钮） |
| Markdown 笔记本 | Tab 弱、选中态不统一、文字看不清 | 1. Tab 用全局块面主题 2. 目录列表复用全局列表主题 3. 辅助文字统一 TextTertiary |
| Todo 看板 | 卡片色彩乱、归档文字看不清、列按钮怪 | 1. 任务卡只留左侧色条、主体白色 2. 时间文字 TextSecondary 3. 列头按钮改 Ghost 文字按钮 |
| Claude 切换器 | 分栏失衡、Tab 不统一、按钮组混乱 | 1. 左右分栏比例 1:2 2. 详情 Tab 套全局主题 3. 底部按钮按主/次变体重排 |
| 智能体页面 | 列表层级模糊、空状态空白 | 1. 工具列表用标题+描述双行样式、字号区分 2. 空状态用 Empty 组件 |
| 新建任务弹窗 | 步骤条违和、表单不统一、AI 起草突兀 | 1. 步骤条用主题主色、柔和圆角样式 2. 表单复用全局输入框主题 3. AI 起草改 Outline 次按钮 |
| 设置同步页 | Tab 特殊、表单乱、提示突兀 | 1. 顶部 Tab 套全局主题 2. 表单用 FormGrid、输入框高度统一 3. 提示文字用 Warning 色+提示图标组件 |
| 关于页面 | 信息层级乱、留白多 | 1. 描述列表组件、标签和值右对齐 2. 复制按钮改 Ghost 图标按钮 |
| 编辑 Profile 弹窗 | Tab 又不同、删除按钮太红 | 1. Tab 套全局小 Tab 规格 2. 删除按钮改 Danger 色 Ghost 按钮（非实心红块） |

## 五、落地代码结构示例

### 注册并应用全局主题
```
// main.cpp  程序入口处注册自定义主题
#include "DevDeskTheme.h"
#include "EUI/NEO/Application.h"

int main()
{
    EUI::NEO::Application app;

    auto devDeskTheme = EUI::NEO::Theme::Create("devdesk");
    devDeskTheme->SetColors(DevDesk::Theme::GetColorMap());
    devDeskTheme->SetRadius(DevDesk::Theme::GetRadiusMap());
    devDeskTheme->SetShadows(DevDesk::Theme::GetShadowMap());
    devDeskTheme->SetTypography(DevDesk::Theme::GetTypographyMap());
    devDeskTheme->SetSpacing(DevDesk::Theme::GetSpacingMap());

    DevDesk::Theme::OverrideWidgetStyles(devDeskTheme);

    app.ApplyTheme(devDeskTheme);
    app.Run();
    return 0;
}
```

### 控件样式覆盖示例
```
void DevDesk::Theme::OverrideWidgetStyles(EUI::NEO::Theme::Ptr theme)
{
    // ========== 按钮通用规则 ==========
    theme->SetWidgetStyle("Button", "height", 40);
    theme->SetWidgetStyle("Button", "radius", 8);
    theme->SetWidgetStyle("Button", "font-weight", 600);

    theme->SetWidgetStyle("Button.Filled", "bg-color", Primary);
    theme->SetWidgetStyle("Button.Filled:hover", "bg-color", PrimaryHover);
    theme->SetWidgetStyle("Button.Filled:active", "bg-color", PrimaryActive);
    theme->SetWidgetStyle("Button.Filled", "text-color", Color::White);

    theme->SetWidgetStyle("Button.Outline", "bg-color", Color::White);
    theme->SetWidgetStyle("Button.Outline", "border-color", Primary);
    theme->SetWidgetStyle("Button.Outline", "text-color", Primary);
    theme->SetWidgetStyle("Button.Outline:hover", "bg-color", PrimaryBg);

    // ========== Tab 全局样式 ==========
    theme->SetWidgetStyle("Tabs.Item", "height", 40);
    theme->SetWidgetStyle("Tabs.Item", "padding-x", 16);
    theme->SetWidgetStyle("Tabs.Item:selected", "bg-color", PrimaryBg);
    theme->SetWidgetStyle("Tabs.Item:selected", "text-color", Primary);
    theme->SetWidgetStyle("Tabs.Item:selected", "radius", 8);
    theme->SetWidgetStyle("Tabs.Item", "margin-x", 2);

    // ========== 列表选中态 ==========
    theme->SetWidgetStyle("ListItem:selected", "bg-color", PrimaryBg);
    theme->SetWidgetStyle("ListItem:selected", "text-color", Primary);
    theme->SetWidgetStyle("ListItem:selected", "border-left", "3px solid Primary");
}
```

## 六、落地验证

1. 所有页面按钮、Tab、列表选中态自动统一
2. 文字清晰可见，无浅灰配浅底
3. 首页卡片色彩均衡，无突兀色块
4. 所有弹窗结构、按钮位置一致

EUI-NEO 的主题系统支持局部覆盖：个别页面需要特殊调整时在页面级单独覆盖某个 token，不影响全局。
