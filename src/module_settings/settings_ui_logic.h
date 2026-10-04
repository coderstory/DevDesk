// src/module_settings/settings_ui_logic.h
// settings 模块的「不碰 Ui、不碰 DB」纯件与共享常量（档位表 / 几何常量 / 下拉持有者
// 取值语义 / 各纯函数）。
//
// 常量在本头单一定义：卡片高度与下拉 openUp 几何同源——两处各写一份会让「卡片变高」
// 与「弹层方向判定」脱钩。消费方 = settings_module.cpp + settings_ui_cards.cpp。
#pragma once

#include "settings_config.h"

#include <core/theme.h>

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace module::settings {

// 字号缩放档位（%）
inline constexpr int kFontPresets[] = {85, 100, 115, 130};
inline constexpr int kFontPresetCount = 4;
inline constexpr const char* kFontPresetLabels[] = {
    "小 85%", "标准 100%", "大 115%", "特大 130%"};

// 日志保留档位（天；0 = 永久）
inline constexpr int kRetentionPresets[] = {0, 7, 30, 90, 180, 365};
inline constexpr int kRetentionPresetCount = 6;

// 窗口效果档位（磨砂 Phase D；与 settings_config 的三档字符串一一对应）
inline constexpr const char* kWindowEffectPresets[] = {
    kWindowEffectOff, kWindowEffectTranslucent, kWindowEffectFrosted};
inline constexpr int kWindowEffectPresetCount = 3;
inline constexpr const char* kWindowEffectPresetLabels[] = {
    "关闭（默认）", "半透", "磨砂（Acrylic）"};

// 主题模式三档（与 settings_config 的 kThemeMode* 一一对应）
inline constexpr const char* kThemeModePresets[] = {kThemeModeAuto,
                                                    kThemeModeLight,
                                                    kThemeModeDark};
inline constexpr int kThemeModePresetCount = 3;
inline constexpr const char* kThemeModePresetLabels[] = {"跟随系统", "浅色",
                                                         "深色"};

// 编辑器字号档位（px；15 = fontSizeBody token 基准）
inline constexpr int kEditorFontPresets[] = {13, 15, 17, 20};
inline constexpr int kEditorFontPresetCount = 4;
inline constexpr const char* kEditorFontPresetLabels[] = {
    "小 13px", "标准 15px", "大 17px", "特大 20px"};

// 日志级别四档（与 settings_config 的 kLogLevel* 一一对应；裸大写词对齐 levelToString）
inline constexpr const char* kLogLevelPresets[] = {
    kLogLevelDebug, kLogLevelInfo, kLogLevelWarn, kLogLevelError};
inline constexpr int kLogLevelPresetCount = 4;
inline constexpr const char* kLogLevelPresetLabels[] = {
    "DEBUG（详尽）", "INFO（默认）", "WARN", "ERROR"};

// 设置页 3 tab：通用 = 外观 + 编辑器字号；模块 = 插拔开关 + 桌宠 + 代理 + 目录；
// 日志 = 保留天数 + 级别 + tail 查看（关于 tab 已拆成独立模块 module_about）。
inline constexpr const char* kSettingTabLabels[] = {"通用", "模块", "日志"};
inline constexpr int kSettingTabCount = 3;
inline constexpr int kTabGeneral = 0;
inline constexpr int kTabModules = 1;
inline constexpr int kTabLog = 2;
inline constexpr float kSettingsTabHeight = 40.0f;  // tab 栏高
inline constexpr int kLogTailMaxLines = 500;        // 日志查看 tail 行数上限
inline constexpr long kLogTailReadWindow = 256 * 1024;  // tail 只读末尾 256KB

// ---- 卡片高度（唯一来源：注册表卡高与卡内几何同源）----
// 内边距四周 ≥16px（顶标题 y≥16、左右 28、底 = 卡高 − 内容底沿 ≥16）；改动须同时满足
//「内容底沿 + 16」。这些是 100% 档基准，注册表运行期取 roundFs(常量)——行盒派生后内容
// 底沿增长，卡高按同一缩放放大才不挤出尾行。
inline constexpr float kAppearanceCardHeight = 172.0f;  // 外观卡（通用；降级提示行底 156 + 16）
inline constexpr float kEditorFontCardHeight = 108.0f;  // 编辑器字号卡（通用）
inline constexpr float kModulesCardHeight = 344.0f;     // 模块插拔卡（模块）
// 5 行开关：尾行 y=68+4*56=292、盒底 328 + 16 = 344
// 桌宠卡：皮肤行 y=64、显示大小行 y=112（各 40），底沿 152 + 16 = 168
inline constexpr float kPetCardHeight = 168.0f;
inline constexpr float kDirectoryCardHeight = 176.0f;   // 目录卡（模块）
// +「数据根忽略规则」路径行（y=126 高 34，盒底 160）+ 16 = 176
inline constexpr float kLogSettingsCardHeight = 148.0f; // 日志卡（日志；编辑器字号迁出后收窄）

// 行盒 / 间距随生效字号派生：行盒 = round(基准 × fontScale)，系数恒包住最坏 CJK 墨迹。
// 100% 档 round() 后与旧字面常量逐一相等（零回归锚）；字号本体仍走 fs()（不取整）。
inline float roundFs(float base) {
    return std::round(base * ::core::theme::fontScale());
}

// ---- 下拉 openUp 越界判定几何 ----
// 日志卡 / 编辑器字号卡的下拉贴近窗体底边，向下展开可能被裁剪，用同一套几何预先判断、
// 出界则置 openUp 向上。弹层三项必须与 EUI-NEO dropdown 实际渲染一致（来源
// components/theme.h 默认 metrics，主题覆写 metrics 时需同步）。
//
// 日志卡两行统一「标签列 + 控件列 + 说明列」同列口径（下列常量即单一来源）：标签 x=28 /
// 控件 x=124 / 说明 x=388，行 y 取 56、104。行 y / 标签行高 / 下拉控件高走 roundFs 派生
//（85% 档字面值会溢出卡外）；水平几何（x / 宽）与弹层三项保持字面（组件 chrome 不缩放）。
inline constexpr float kLogLabelX = 28.0f;       // 标签列左边缘（卡内 padding）
inline constexpr float kLogLabelWidth = 84.0f;   // 标签列宽（"日志保留" 4 字 ×12px = 48）
inline float logLabelHeight() { return roundFs(20.0f); }  // 标签行盒高（100% = 20，单行不折行）
inline constexpr float kLogFieldX = 124.0f;      // 控件列左边缘 = 28 + 84 + 12（一档间距）
inline constexpr float kLogFieldWidth = 240.0f;  // 两个下拉同宽（同一列口径）
inline float logFieldHeight() { return roundFs(36.0f); }  // 两个下拉同高（100% = 36）
inline float logRow1LabelY() { return roundFs(56.0f); }   // 行 1 标签 y（100% = 56，标题行 16..38 之下）
inline float logRow2LabelY() { return roundFs(104.0f); }  // 行 2 标签 y（100% = 104）
inline constexpr float kLogHintX = 388.0f;       // 说明列 x = 124 + 240 + 24
// 控件相对标签行的 y 偏移：控件比标签高一档，取垂直居中（100% = (20-36)/2 = -8）
inline float logFieldDy() { return (logLabelHeight() - logFieldHeight()) * 0.5f; }
inline float logDropdownOffsetY() { return logRow1LabelY() + logFieldDy(); }  // 100% = 48
inline float logLevelOffsetY() { return logRow2LabelY() + logFieldDy(); }     // 100% = 96
// 编辑器字号卡内 dropdown y = 字面锚（标题盒底 38）+ 派生一档间距 roundFs(4)——整段
// 圆整会在 85% 档越过标题盒底造成行盒叠印，故只让间距入派生池。
inline float editorFontDdOffsetY() { return 38.0f + roundFs(4.0f); }  // 100% = 42
inline constexpr float kDropdownPopupGap = 8.0f;       // 字段底边与弹层间距（spacing.compact）
inline constexpr float kDropdownItemHeight = 34.0f;    // 单个菜单项高（control.menuItem）
inline constexpr float kDropdownPopupPadding = 12.0f;  // 弹层上下 padding（spacing.small × 2）
inline constexpr float kWindowBottomMargin = 16.0f;    // 窗口底边安全留白，越界 = 有裁剪风险

// 下拉文案宽度：EUI-NEO text 默认不折行，超宽直接画出控件右缘，故「不换行」等价于
//「文本宽 ≤ 可用宽」——用保守估算 + 截断。chrome 常量对齐 components/dropdown.h 内部取数
//（字段 = spacing.content(12)+micro(2)+overlay(48)；弹层项 = small(6)×2+panel(24)）。
inline constexpr float kDropdownFieldChrome = 62.0f;  // 12 + 2 + 48
inline constexpr float kDropdownItemChrome = 36.0f;   // 6 × 2 + 24
// 估算口径：ASCII 0.62em / 全角 1.0em（实测拉丁约 0.49em，取 0.62 偏保守，宁可早截）。
inline constexpr float kAsciiWidthEm = 0.62f;
inline constexpr float kFullWidthEm = 1.0f;

// 皮肤下拉宽度：下限 240（与其余控件同宽），按最长皮肤名放大、上限 420（超出由截断兜底）。
inline constexpr float kPetSkinFieldMinWidth = 240.0f;
inline constexpr float kPetSkinFieldMaxWidth = 420.0f;

// 下拉「谁打开着」的单一持有者：一个索引代替 7 个 bool——打开任一项自动关掉先前那个，
// -1 = 全关。新增下拉只需在 Dropdown 加一项 + 传自己的枚举值。
enum class Dropdown {
    Theme,
    Font,
    Effect,
    Retention,
    LogLevel,
    EditorFont,
    PetSkin,
    PetSize,  // 桌宠显示大小三档（2026-09-30）
};

inline bool isDropdownOpen(int openIndex, Dropdown d) {
    return openIndex == static_cast<int>(d);
}

// 组件 onOpenChange 通道的等价语义：v=true → 独占打开（自动关掉先前那个）；v=false →
// 只有当前打开的是自己才关（避免迟到回调误关别人）。返回新的持有者索引。
inline int applyDropdownOpen(int openIndex, Dropdown d, bool v) {
    if (v) {
        return static_cast<int>(d);
    }
    return isDropdownOpen(openIndex, d) ? -1 : openIndex;
}

// 档位最近邻索引
int nearestIndex(int value, const int* presets, int count);

// 非 token 的硬编码字号跟随全局缩放（token 字号已在 theme 层缩放，勿再包一层）。
float fs(float base);

// 代理卡接管 / 警告行几何（drawProxyCard 与 proxyCardHeightFor 单一来源）：接管行 y=98
// 锚在字面控件链上，警告行 y 从接管行派生盒底起算（100% 档 = 118）。
inline float proxyTakeoverRowHeight() { return roundFs(18.0f); }  // 100% = 18
inline float proxyWarnRowHeight() { return roundFs(16.0f); }      // 100% = 16
inline float proxyWarnRowTop() {
    return 98.0f + proxyTakeoverRowHeight() + roundFs(2.0f);      // 100% = 118
}
// 代理卡高 = 内容底沿 + roundFs(16) 下边距（100% 档：无警告 132 / n 条 118+16n+16）。
inline float proxyCardHeightFor(std::size_t warnLines) {
    const float bottom =
        warnLines > 0
            ? proxyWarnRowTop() +
                  static_cast<float>(warnLines) * proxyWarnRowHeight()
            : 98.0f + proxyTakeoverRowHeight();
    return bottom + roundFs(16.0f);
}

// 下拉弹层向下展开是否越出窗体底边（越界 → 调用方置 openUp 向上）。cardTopY = 卡顶页面
// 绝对 y；fieldOffsetY/fieldHeight = 字段在卡内的 y 与高；itemCount = 项数；screenH = 页面逻辑高。
bool dropdownPopupOverflows(float cardTopY, float fieldOffsetY,
                            float fieldHeight, int itemCount, float screenH);

// 日志保留档文案：0 = 永久保留，其余 "<n> 天"
std::string retentionLabel(int days);

// 文本宽度估算（保守、不依赖字体度量）：ASCII 计 kAsciiWidthEm、其余计 kFullWidthEm。
float estimatedTextWidth(const std::string& text, float fontSize);

// 单行文案适配：放得进就原样返回，放不进则从左截断补 '…'（含省略号后仍不超宽）。
// 只改显示串，不改取值（调用方回传的仍是原始下标 / 名字）。
std::string fitControlLabel(const std::string& text, float availableWidth,
                            float fontSize);

// 皮肤下拉宽度：按最长皮肤名估算放大，clamp 到 [kPetSkinFieldMinWidth, kPetSkinFieldMaxWidth]。
float petSkinFieldWidth(const std::vector<std::string>& skinNames,
                        float fontSize);

} // namespace module::settings
