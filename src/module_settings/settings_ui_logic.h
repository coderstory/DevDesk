// src/module_settings/settings_ui_logic.h
// settings 模块的「不碰 Ui、不碰 DB」纯件与共享常量（档位表 / 几何常量 / 下拉持有者
// 取值语义 / 各纯函数）。常量在本头单一定义：卡片高度与下拉 openUp 几何同源，
// 两处各写一份会让「卡片变高」与「弹层方向判定」脱钩。
#pragma once

#include "settings_config.h"

#include <core/theme.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace module::settings {

// 设置页页头高：UAT5 全局顶部收紧后共享常量 ui_kit::kHeaderH 已收至 40（标题行盒
// 34 + 6 呼吸空隙），与设置页专用档同值——kSettingsHeaderH 已删除并入共享源，不再双源。

// 字号缩放档位（%）
// UAT6.2 无级调节后本表不再驱动 UI（字号行改 ui_kit::slider 连续取值）；
// 数组保留仅供历史档位/文案参考与既有单测锚定。
inline constexpr int kFontPresets[] = {85, 100, 115, 130};
inline constexpr int kFontPresetCount = 4;
inline constexpr const char* kFontPresetLabels[] = {
    "小 85%", "标准 100%", "大 115%", "特大 130%"};

// 滑杆归一化值 → 字号百分比：1% 分辨率取整后钳制进 [kMin, kMax]（slider
// onChange 的连续值一律经本函数量化，cfg.fontScalePercent 仍是 int 百分比）。
inline int fontScalePercentFromRatio(float ratio) {
    const int percent = kMinFontScalePercent +
                        static_cast<int>(std::lround(ratio * static_cast<float>(
                                                   kMaxFontScalePercent - kMinFontScalePercent)));
    return std::clamp(percent, kMinFontScalePercent, kMaxFontScalePercent);
}

// 日志保留档位（天；0 = 永久）
inline constexpr int kRetentionPresets[] = {0, 7, 30, 90, 180, 365};
inline constexpr int kRetentionPresetCount = 6;

// 窗口效果档位（与 settings_config 的三档字符串一一对应）
inline constexpr const char* kWindowEffectPresets[] = {
    kWindowEffectOff, kWindowEffectTranslucent, kWindowEffectFrosted};
inline constexpr int kWindowEffectPresetCount = 3;
inline constexpr const char* kWindowEffectPresetLabels[] = {
    "关闭（默认）", "半透", "磨砂（Mica）"};

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
// UAT6 动态背景开关（活体流质）落位外观卡**标题带右贴**（代理卡「随应用启动代理」
// 同款先例，settings.card.appearance.fluid），不是卡内新行——通用 tab 零滚动预算
// 逐值保持的硬约束：0/1/2 条代理警告 = 内容 338/356/372 对可用 372，任何 +30~40
// 的开关行都会把 1/2 警告档顶破 372（加行方案已在预算表排除），标题带方案
// 行盒零增量、卡高 184 全字号档不动（四档 85/100/115/130 卡高 156/184/212/239
// 逐档保持，见 settings_card_rows_scale_derived 的 appearance 卡高钉值）。
inline constexpr float kAppearanceCardHeight = 184.0f;  // 外观卡（通用 | 半宽配对左列；降级提示行底 168 + 16，E 批布局统一后行序不变仍 184）
// 编辑器字号卡（通用 | 半宽配对右列；UAT6 起卡内第二段加「开机自启动」开关行）：
// 字号段行底 hintRowBelowY(68)+roundFs(16)=126，自启动行顶 126+roundFs(6)=132、
// 行盒 roundFs(30)=30 → 行底 162，+ 边距 16 → 184，与外观卡同高。**配对行高
// 184 全档不变**（max(184,184)，外观卡继续掌行高）→ 通用 tab 内容高
// 338/356/372（0/1/2 条代理警告）对可用 372 的零滚动预算逐值保持。
// 85% 档卡高 156（自启动行底 139，下边距 17）/130% 档 239（行底 211，下边距 28），
// 下边距全档 ≥16（卡内边距契约）。历史：UAT5 时 144（hint 行底 126+16）。
inline constexpr float kEditorFontCardHeight = 184.0f;
inline constexpr float kModulesCardHeight = 198.0f;     // 模块插拔卡（模块 | 全宽卡内 2×2 开关格，UAT4 按用户拍板撑满；次行块底 68+roundFs(56)+roundFs(36)=160，+16 → 198 给字号档富余；85% 档 168 / 130% 档 257）
// 壁纸卡（模块 | 全宽单卡：标题 16/22 + 说明 40/roundFs(16) + 开关行 y=roundFs(68)、
// 行盒 roundFs(44)、44×24 开关行内垂直居中）。卡高 = roundFs(68) + roundFs(44) +
// roundFs(16) 下边距 = 128 基准：三档逐值和 = roundFs(128)（85% 109 / 100% 128 /
// 130% 166），130% 档不裁切且下边距 ≥16 契约保持。
inline constexpr float kWallpaperCardHeight = 128.0f;
inline constexpr float kLogSettingsCardHeight = 144.0f; // 日志卡（日志；两下拉同排双列，hint 并一行下移后行底 126，+16 → 144；85% 档 122 / 130% 档 187）
// GitHub Sync 卡（同步，UAT5 方案 A 三段式 + UAT6 开关行 + UAT6 零滚动收口）：
// 卡高由最坏行序决定——头部（标题 20+1+摘要 16 + 行距 6 + 启用行 28 + 6 +
// 自动推送行 28 = 105）+ 分段 5+1+5 + 配置组（URL 行 36+6 + 分支|Token 行 36
// = 78）+ 分段 11 + 操作行 36 + 6 + 条件信息行 5×16+4×2（分支提示/public/
// 本地 git/未同步/删跳过五条全显的最坏组合，仅行间无行尾距）+ 下边距 8 →
// 100% 档需高 351（内容底 343 + 边距 8）。开关尺寸 52×28/44×24 是字面值不随
// 字号缩放（行盒随 roundFs，85% 档 24 贴自动推送 24、启用行 28 超行盒由
// max(行盒/文本/开关) 记账兜住）：85% 档需高 307、115% 档 397、130% 档 456。
//
// UAT6 零滚动收口（用户：「同步 tab 出滚动条完全没必要」）：Windows 自绘壳下
// 最小窗 1000×640 的页面高 = 600（标题条 40，app_sidebar.cpp bodyTop），卡区
// 可用高 cardsH = 600 − cardsTopY 152 − pad 40 − hint 20 − gap 16 = 372。卡高
// > 372 同步 tab 即出滚动条，故常量 496 → 372 取可用高**恰好值**：100% 档
// roundFs(372)=372 ≤ 372 不滚（内容需高 351 有 21 富余）、85% 档
// roundFs(372)=316 ≤ 375 不滚（需高 307 有 9 富余）、115%/130% 档（428/484）
// 仍超可用高滚动（大字号档设计内，与通用/日志 tab 同口径）。瘦身来源见
// github_sync_card.cpp 行几何常量注释（−124：说明行+行盒 36→28、组题行、
// 分段留白 12→5、信息行 20→16/行距 4→2 去行尾距、行距 8→6、标题/摘要距、
// 卡上下留白 16→8、信息行前距 8→6）。卡高恒定不按启用态折叠（UAT4 口径）：
// 未启用态全字段显示，只差表单可交互性。
inline constexpr float kGithubSyncCardHeight = 362.0f; // N 项: 372→362 (主开关 52×28→44×24, toggleRowH 28→24 节省 ~10px@130% 档)

// 行盒 / 间距随生效字号派生：行盒 = round(基准 × fontScale)，系数恒包住最坏 CJK 墨迹；字号本体仍走 fs()（不取整）。
inline float roundFs(float base) {
    return std::round(base * ::core::theme::fontScale());
}

// 下拉贴近窗体底边向下展开可能被裁剪，用同一套几何预判、出界则置 openUp 向上；弹层三项必须与 EUI-NEO dropdown 实际渲染一致（主题覆写 metrics 时需同步）。
// E 批布局统一：label+control 行一律 ui_kit::formRow（label 定宽 110、行高 = logFieldHeight 36、
// hint 作控件区内行内尾注）；行 y 走 roundFs 派生（85% 档字面值会溢出卡外），水平几何由 formRow 给。
inline float logFieldHeight() { return roundFs(36.0f); }  // 两个下拉同高（100% = 36，行高单一来源）
inline float logRow1LabelY() { return roundFs(68.0f); }   // 行 y（100% = 68，内容起始统一口径；UAT3 双列后保留/级别两行同排共用本锚）
// 迁 FormRow 后行高即控件高（垂直居中差恒 0），行 y 与控件 y 合一：
inline float logDropdownOffsetY() { return logRow1LabelY(); }  // 100% = 68
// 编辑器字号卡 FormRow 行 y：与日志卡同口径的内容起始锚（100% = 68）。
inline float editorFontRowY() { return roundFs(68.0f); }  // 100% = 68
// 行内 hint 迁「控件行下方第二行」的行 y（UAT3 多列分栏：窄列行内尾注放不下）。
// 编辑器字号卡与日志卡同口径：hint 顶 = 控件行 y + 行高 + 行距（100% = 68+36+6 = 110）。
inline float hintRowBelowY(float rowY) {
    return rowY + logFieldHeight() + roundFs(6.0f);
}
// ── 开机自启动行（编辑器字号卡内第二段，UAT6；几何与卡高同源推导见
// kEditorFontCardHeight 注释）──
// 行 y = hint 行底（hintH 恒 roundFs(16)）+ 行距 6：100% = 110+16+6 = 132。
inline float autostartRowBelowY(float hintY) {
    return hintY + roundFs(16.0f) + roundFs(6.0f);
}
// 行盒高：roundFs(30)（85% 档 26 仍包住 44×24 字面开关——开关是字面值不随字号缩放）
inline float autostartRowHeight() { return roundFs(30.0f); }
// 开关尺寸（§4.4 微信开关口径，与模块插拔卡/同步卡同款字面值）
inline constexpr float kAutostartSwitchW = 44.0f;
inline constexpr float kAutostartSwitchH = 24.0f;
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

// 代理卡行几何（drawProxyCard 与 proxyCardHeightFor 单一来源）：E 批统一——端口行迁
// FormRow（行高 = logFieldHeight 36），行 y 从 desc 盒底（40+16）+ gap 12 派生 = 68；
// 接管行从端口行盒底顺延，警告行从接管行盒底派生。
inline float proxyPortRowY() {
    return 40.0f + roundFs(16.0f) + roundFs(12.0f);               // 100% = 68
}
inline float proxyTakeoverRowY() {
    return proxyPortRowY() + logFieldHeight();                    // 100% = 104
}
inline float proxyTakeoverRowHeight() { return roundFs(18.0f); }  // 100% = 18
inline float proxyWarnRowHeight() { return roundFs(16.0f); }      // 100% = 16
inline float proxyWarnRowTop() {
    return proxyTakeoverRowY() + proxyTakeoverRowHeight() + roundFs(2.0f);  // 100% = 124
}
// 代理卡高 = 内容底沿 + roundFs(16) 下边距（100% 档：无警告 138 / n 条 124+16n+16）。
inline float proxyCardHeightFor(std::size_t warnLines) {
    const float bottom =
        warnLines > 0
            ? proxyWarnRowTop() +
                  static_cast<float>(warnLines) * proxyWarnRowHeight()
            : proxyTakeoverRowY() + proxyTakeoverRowHeight();
    return bottom + roundFs(16.0f);
}

// MCP 配置卡行几何（drawMcpCard 与 mcpCardHeightFor 单一来源）：端口行 = FormRow
//（行 y = proxyPortRowY、行高 = logFieldHeight），状态行 / 命令行依次从行盒底派生，
// 卡高不写死——85% 档 roundFs(16)=14 不满足下边距 ≥16，须 std::max 抬底。
inline float mcpPortRowY() { return proxyPortRowY(); }            // 100% = 68
inline float mcpStatusRowHeight() { return roundFs(18.0f); }      // 100% = 18
inline float mcpCommandRowY() {
    return mcpPortRowY() + logFieldHeight() + mcpStatusRowHeight() +
           roundFs(6.0f);
}                                                                 // 100% = 128
inline float mcpCommandRowHeight() { return roundFs(16.0f); }     // 100% = 16
inline float mcpCardHeightFor() {                                 // 100% = 160
    // 下边距下限 16：85% 档 roundFs(16) = 14，不能直接 roundFs(16)
    return mcpCommandRowY() + mcpCommandRowHeight() +
           std::max(16.0f, roundFs(16.0f));
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
