// src/module_settings/settings_ui_logic.h
// settings 模块的「不碰 Ui、不碰 DB」纯件与共享常量（档位表 / 几何常量 / 下拉持有者
// 取值语义 / 各纯函数）。常量在本头单一定义：卡片高度与下拉 openUp 几何同源，
// 两处各写一份会让「卡片变高」与「弹层方向判定」脱钩。
#pragma once

#include "settings_config.h"

#include <core/theme.h>

#include <cmath>
#include <cstddef>
#include <optional>
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

// 窗口效果档位（与 settings_config 的三档字符串一一对应）
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

// 强调色四档（与 settings_config 的 "clay"/"purple"/"pink"/"blue" 一一对应）
inline constexpr const char* kAccentPresetValues[] = {"clay", "purple", "pink", "blue"};
inline constexpr int kAccentPresetCount = 4;

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

inline constexpr const char* kSettingTabLabels[] = {"通用", "模块", "日志", "同步"};
inline constexpr int kSettingTabCount = 4;
inline constexpr int kTabGeneral = 0;
inline constexpr int kTabModules = 1;
inline constexpr int kTabLog = 2;
inline constexpr int kTabSync = 3;
inline constexpr float kSettingsTabHeight = 40.0f;  // tab 栏高
inline constexpr int kLogTailMaxLines = 500;        // 日志查看 tail 行数上限
inline constexpr long kLogTailReadWindow = 256 * 1024;  // tail 只读末尾 256KB

// 卡片高度（唯一来源：注册表卡高与卡内几何同源）
// 内边距四周 ≥16px，改动须同时满足「内容底沿 + 16」；这些是 100% 档基准，注册表运行期取 roundFs(常量)，卡高与行盒同缩放才不挤出尾行。
inline constexpr float kAppearanceCardHeight = 248.0f;  // 外观卡（通用；强调色行底 232 + 16）
inline constexpr float kEditorFontCardHeight = 108.0f;  // 编辑器字号卡（通用）
inline constexpr float kModulesCardHeight = 344.0f;     // 模块插拔卡（模块）
inline constexpr float kDirectoryCardHeight = 176.0f;   // 目录卡（模块）
inline constexpr float kLogSettingsCardHeight = 148.0f; // 日志卡（日志）
// GitHub Sync 卡（同步）：启用态满高（最坏行序 = 按钮行 + 进度 + 状态 + 分支
// 提示 + public 提醒 + 本地 git 灰字，内容底沿 450；错误行与进度行互斥、错误行
// 显示时状态行让位，故 460 包住所有组合）。未启用时卡片早退，改用折叠高。
inline constexpr float kGithubSyncCardHeight = 460.0f;
inline constexpr float kGithubSyncCollapsedHeight = 132.0f;  // 未启用（标题+描述+开关）

// 行盒 / 间距随生效字号派生：行盒 = round(基准 × fontScale)，系数恒包住最坏 CJK 墨迹；字号本体仍走 fs()（不取整）。
inline float roundFs(float base) {
    return std::round(base * ::core::theme::fontScale());
}

// 下拉贴近窗体底边向下展开可能被裁剪，用同一套几何预判、出界则置 openUp 向上；弹层三项必须与 EUI-NEO dropdown 实际渲染一致（主题覆写 metrics 时需同步）。
// 日志卡两行「标签列 + 控件列 + 说明列」同列口径（下列常量即单一来源）；行 y / 行高 / 控件高走 roundFs 派生（85% 档字面值会溢出卡外），水平几何保持字面。
inline constexpr float kLogLabelX = 28.0f;       // 标签列左边缘（卡内 padding）
inline constexpr float kLogLabelWidth = 84.0f;   // 标签列宽（"日志保留" 4 字 ×12px = 48）
inline float logLabelHeight() { return roundFs(20.0f); }  // 标签行盒高（100% = 20，单行不折行）
inline constexpr float kLogFieldX = 124.0f;      // 控件列左边缘 = 28 + 84 + 12（一档间距）
inline constexpr float kLogFieldWidth = 240.0f;  // 两个下拉同宽（同一列口径）
inline float logFieldHeight() { return roundFs(36.0f); }  // 两个下拉同高（100% = 36）
inline float logRow1LabelY() { return roundFs(56.0f); }   // 行 1 标签 y（100% = 56，标题行 16..38 之下）
inline float logRow2LabelY() { return roundFs(104.0f); }  // 行 2 标签 y（100% = 104）
inline constexpr float kLogHintX = 388.0f;       // 说明列 x = 124 + 240 + 24
inline float logFieldDy() { return (logLabelHeight() - logFieldHeight()) * 0.5f; }
inline float logDropdownOffsetY() { return logRow1LabelY() + logFieldDy(); }  // 100% = 48
inline float logLevelOffsetY() { return logRow2LabelY() + logFieldDy(); }     // 100% = 96
// 编辑器字号卡内 dropdown y = 字面锚（标题盒底 38）+ 派生间距——整段圆整会在 85% 档越过标题盒底造成行盒叠印。
inline float editorFontDdOffsetY() { return 38.0f + roundFs(4.0f); }  // 100% = 42
inline constexpr float kDropdownPopupGap = 8.0f;       // 字段底边与弹层间距（spacing.compact）
inline constexpr float kDropdownItemHeight = 34.0f;    // 单个菜单项高（control.menuItem）
inline constexpr float kDropdownPopupPadding = 12.0f;  // 弹层上下 padding（spacing.small × 2）
inline constexpr float kWindowBottomMargin = 16.0f;    // 窗口底边安全留白，越界 = 有裁剪风险

// 下拉文案宽度：EUI-NEO text 默认不折行、超宽画出控件右缘，故保守估算 + 截断；chrome 常量对齐 components/dropdown.h 内部取数。
inline constexpr float kDropdownFieldChrome = 62.0f;  // 12 + 2 + 48
inline constexpr float kDropdownItemChrome = 36.0f;   // 6 × 2 + 24
// 估算口径：ASCII 0.62em / 全角 1.0em（偏保守，宁可早截）。
inline constexpr float kAsciiWidthEm = 0.62f;
inline constexpr float kFullWidthEm = 1.0f;

// 下拉「谁打开着」的单一持有者：一个索引代替 7 个 bool（打开任一项自动关掉先前那个，-1 = 全关）；新增下拉在 Dropdown 加一项即可。
enum class Dropdown {
    Theme,
    Font,
    Effect,
    Retention,
    LogLevel,
    EditorFont,
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

int nearestIndex(int value, const int* presets, int count);

// 非 token 的硬编码字号跟随全局缩放（token 字号已在 theme 层缩放，勿再包一层）。
float fs(float base);

// 代理卡接管 / 警告行几何（drawProxyCard 与 proxyCardHeightFor 单一来源）：接管行 y=98 锚字面控件链，警告行 y 从接管行盒底派生。
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

// 下拉弹层向下展开是否越出窗体底边（越界 → 调用方置 openUp 向上）；cardTopY = 卡顶页面绝对 y，fieldOffsetY/fieldHeight = 字段卡内 y 与高。
bool dropdownPopupOverflows(float cardTopY, float fieldOffsetY,
                            float fieldHeight, int itemCount, float screenH);

// 日志保留档文案：0 = 永久保留，其余 "<n> 天"
std::string retentionLabel(int days);

// 文本宽度估算（保守、不依赖字体度量）：ASCII 计 kAsciiWidthEm、其余计 kFullWidthEm。
float estimatedTextWidth(const std::string& text, float fontSize);

// 单行文案适配：放不进则从左截断补 '…'（含省略号后仍不超宽）；只改显示串不改取值。
std::string fitControlLabel(const std::string& text, float availableWidth,
                            float fontSize);

// 日志行级别解析（纯函数）：从 "[timestamp] [LEVEL] ..." 格式提取第二个方括号内的级别 token，映射到 core::log::Level。levelToString 对 INFO/WARN 末尾补空格对齐宽度，token 需 trim 后再匹配。解析不出返回 nullopt。
inline std::optional<core::log::Level> logLineLevel(const std::string& line) {
    size_t first = line.find('[');
    if (first == std::string::npos) return std::nullopt;
    size_t second = line.find('[', first + 1);
    if (second == std::string::npos) return std::nullopt;
    size_t close = line.find(']', second + 1);
    if (close == std::string::npos) return std::nullopt;

    std::string token = line.substr(second + 1, close - second - 1);
    // levelToString 对 INFO/WARN 末尾补空格对齐宽度，需 trim 以防不匹配
    while (!token.empty() && token.back() == ' ') token.pop_back();
    while (!token.empty() && token.front() == ' ') token.erase(token.begin());

    if (token == "TRACE") return core::log::Level::Trace;
    if (token == "DEBUG") return core::log::Level::Debug;
    if (token == "INFO")  return core::log::Level::Info;
    if (token == "WARN")  return core::log::Level::Warn;
    if (token == "ERROR") return core::log::Level::Error;
    if (token == "FATAL") return core::log::Level::Fatal;
    return std::nullopt;
}

// 日志 tail 按最低级别过滤（纯函数）：只保留级别 ≥ minLevel 的行。解析不出级别的行保留不吞（fail-open）——日志视图吞内容比显示杂行危害大；空行与格式异常行因此一律通过。
inline std::vector<std::string> filterLogTailByLevel(
    const std::vector<std::string>& lines, core::log::Level minLevel) {
    std::vector<std::string> out;
    out.reserve(lines.size());
    for (const auto& line : lines) {
        auto lvl = logLineLevel(line);
        if (!lvl || static_cast<int>(*lvl) >= static_cast<int>(minLevel)) {
            out.push_back(line);
        }
    }
    return out;
}

} // namespace module::settings
