// src/module_settings/settings_ui_logic.h
// R6（spec 2026-09-27-refactor-incremental-design §5.2）：settings 模块的
// 「不碰 Ui、不碰 DB」纯件与共享常量 —— 拆自 settings_module.cpp 的文件内匿名
// namespace（档位表 / 几何常量 / nearestIndex）与 SettingsModule 的私有纯成员
// （fs / retentionLabel / 下拉唯一持有者取值语义）。
//
// 常量在本头**单一定义**：卡片高度与下拉 openUp 几何同源（spec §5.2 R6 的风险
// 项——两处各写一份会让「卡片变高」与「弹层方向判定」脱钩）。
//
// 消费方：settings_module.cpp（卡片注册表：卡高 / 门控 / openUp 几何）与
// settings_ui_cards.cpp（9 张卡）。
#pragma once

#include "settings_config.h"

#include <core/theme.h>

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace module::settings {

// ============ 档位表（值 + 标签）============

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

// 主题模式三档（spec §8.5「跟随系统主题」，spec 缩水 A；与 settings_config
// 的 kThemeMode* 一一对应）
inline constexpr const char* kThemeModePresets[] = {kThemeModeAuto,
                                                    kThemeModeLight,
                                                    kThemeModeDark};
inline constexpr int kThemeModePresetCount = 3;
inline constexpr const char* kThemeModePresetLabels[] = {"跟随系统", "浅色",
                                                         "深色"};

// 编辑器字号档位（px；spec 缩水 B；15 = fontSizeBody token 基准 = 旧硬编值）
inline constexpr int kEditorFontPresets[] = {13, 15, 17, 20};
inline constexpr int kEditorFontPresetCount = 4;
inline constexpr const char* kEditorFontPresetLabels[] = {
    "小 13px", "标准 15px", "大 17px", "特大 20px"};

// 日志级别四档（spec 缩水 D；与 settings_config 的 kLogLevel* 一一对应，
// 核心档用裸大写词 = 与 core::log::levelToString 输出对齐）
inline constexpr const char* kLogLevelPresets[] = {
    kLogLevelDebug, kLogLevelInfo, kLogLevelWarn, kLogLevelError};
inline constexpr int kLogLevelPresetCount = 4;
inline constexpr const char* kLogLevelPresetLabels[] = {
    "DEBUG（详尽）", "INFO（默认）", "WARN", "ERROR"};

// ============ 设置页 3 tab（Task B：页面太长拆 tab + 日志查看）============
// 通用 = 外观 + 编辑器字号；模块 = 插拔开关 + 桌宠 + 代理 + 目录；
// 日志 = 保留天数 + 级别 + tail 查看。关于 tab 已拆成独立模块
//（module_about，2026-09-28 about 页设计——不保留僵尸 tab）
inline constexpr const char* kSettingTabLabels[] = {"通用", "模块", "日志"};
inline constexpr int kSettingTabCount = 3;
inline constexpr int kTabGeneral = 0;
inline constexpr int kTabModules = 1;
inline constexpr int kTabLog = 2;
inline constexpr float kSettingsTabHeight = 40.0f;  // tab 栏高
inline constexpr int kLogTailMaxLines = 500;        // 日志查看 tail 行数上限
inline constexpr long kLogTailReadWindow = 256 * 1024;  // tail 只读末尾 256KB

// ============ 卡片高度（唯一来源：注册表卡高与卡内几何同源）============
// 注册表（settings_module.cpp kCardSpecs）按这些常量摆卡；改动必须同时满足卡内
// 内容底沿 + 16px 下边距的既有公式，否则卡片内容会被裁或贴底边。卡内容内边距
// 口径（2026-09-28 系统性修复）：四周 ≥16px——顶（标题 y≥16）、左右 28（对齐
// kDialogInsetX 语系）、底 = 卡高 - 内容底沿 ≥ 16。
//
// 2026-09-30 缩放派生审查（spec 2026-09-30-batch-fix-design.md §2.3 本期修）：
// 这些常量是 **100% 档基准值**；注册表运行期取 roundFs(常量)（cardHeight 的
// Fixed 分支）——行盒/行距随生效字号派生后内容底沿同步增长，卡高按同一缩放
// 放大才不会把尾行挤出卡外。100% 档 roundFs(x) == x，与旧字面逐一相等。
inline constexpr float kAppearanceCardHeight = 172.0f;  // 外观卡（通用；降级提示行底 156 + 16）
inline constexpr float kEditorFontCardHeight = 108.0f;  // 编辑器字号卡（通用）
inline constexpr float kModulesCardHeight = 288.0f;     // 模块插拔卡（模块）
// 桌宠卡（模块）：两行控件——皮肤行 y=64、显示大小行 y=112（各高 40），
// 内容底沿 152 + 16 下边距 = 168（2026-09-30 加显示大小行后由 120 抬高）
inline constexpr float kPetCardHeight = 168.0f;
inline constexpr float kDirectoryCardHeight = 116.0f;   // 目录卡（模块；hint 底 100 + 16）
inline constexpr float kLogSettingsCardHeight = 148.0f; // 日志卡（日志；编辑器字号迁出后收窄）

// ---- 行盒/间距随生效字号派生（2026-09-30 spec §2.3 本期修）----
// 根因（与详情页 env pane 同一台病，真机 130% 档墨迹越盒）：全局字号缩放只放大
// 字号，行盒/行距按 100% 写死 → 文本 advance 超出盒贴边/叠印。派生口径与 env
// pane 修复（1ca87b0）一致：**行盒 = round(基准 × fontScale)**，系数（基准/行
// 字号）> 最坏 CJK 墨迹 1.25×，恒包住墨迹；**100% 档 round() 后与旧字面常量
// 逐一相等**（零回归锚——既有布局断言、常量钉值用例全部不动）。字号本体仍走
// fs()（不取整，与 env pane 同款），取整只发生在盒/间距上（像素对齐稳定）。
inline float roundFs(float base) {
    return std::round(base * ::core::theme::fontScale());
}

// ============ 下拉 openUp 越界判定几何 ============
// 日志卡 / 编辑器字号卡的下拉贴近窗体底边，向下展开可能被裁剪；用同一套几何
// 预先判断「向下展开是否出界」，出界则置 openUp 让 dropdown 向上展开。
// 弹层三项必须与 EUI-NEO dropdown 实际渲染一致，否则判定失真：取值来源是
// EUI-NEO components/theme.h 的默认 metrics（control.menuItem=34 /
// spacing.compact=8 / spacing.small=6），若将来主题覆写 metrics 需同步。
//
// 2026-09-29（用户真机反馈「日志和日志级别这 2 个条件 错位了啊」）后，日志卡
// 两行统一为「标签列 + 控件列 + 说明列」同列口径，下列常量即该口径的单一来源：
//   行 1（日志保留）：标签 x=28 y=56 / 控件 x=124 y=48 / 说明 x=388
//   行 2（日志级别）：标签 x=28 y=104 / 控件 x=124 y=96 / 说明 x=388
// 改前行 1 没有标签、控件左边缘 = 卡内 padding 28，行 2 有 84 宽标签、控件左
// 边缘 = 124 —— 两行控件左边缘差 96px 且行 1 缺标签列，就是「错位」的全部来源。
// 行 y 取 56 / 104（标签中心与控件中心对齐 + 控件底边 132 = 卡高 148 − 16 下边距
// 刚好用满）：行 1 控件底边 84 也是 openUp 几何的既有锚点（短窗向上展开时
// 弹层顶边不越出窗体顶）。
//
// 2026-09-30 merge-review R1 本期修：行 y / 标签行高 / 下拉控件高原为字面
// constexpr，不在 roundFs 派生池 —— 85% 字号档（fontScale=0.85）日志卡行 2
// 下拉底沿 96+36=132 > 卡高 roundFs(148)=126，控件溢出卡外与 tail 卡叠印
//（编辑器字号卡同池共享 kEditorFontDdOffsetY / 下拉高）。改走 roundFs 派生
//（口径与 settings_ui_cards.cpp 已派生行一致）：行盒/控件随生效字号缩放，
// 100% 档 roundFs(x)==x，y=56/104、高 20/36、偏移 48/96 与旧字面逐值相等
//（零回归锚）。水平几何（x / 宽）不随字号缩放，保持字面常量。弹层三项
//（kDropdownItemHeight 等）是 EUI-NEO 组件 chrome、不经 DevDesk 字号缩放，
// 同样保持字面。
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
// 编辑器字号卡内 dropdown y（Task B 独立卡；下拉高与日志卡同源 logFieldHeight）。
// y 拆两段：字面锚（标题盒 16..38，标题盒高 22 字面、字号 token 化）+ 派生
// 一档间距 roundFs(4)——若整段 roundFs(42)，85% 档缩到 36 会越过字面标题盒
// 底 38（compose 实测 editor.title × editor.ddwrap 行盒叠印），故间距才入
// 派生池、锚保持字面（与代理卡接管行 y=98 字面锚同款口径）
inline float editorFontDdOffsetY() { return 38.0f + roundFs(4.0f); }  // 100% = 42
inline constexpr float kDropdownPopupGap = 8.0f;       // 字段底边与弹层间距（spacing.compact）
inline constexpr float kDropdownItemHeight = 34.0f;    // 单个菜单项高（control.menuItem）
inline constexpr float kDropdownPopupPadding = 12.0f;  // 弹层上下 padding（spacing.small × 2）
inline constexpr float kWindowBottomMargin = 16.0f;    // 窗口底边安全留白，越界 = 有裁剪风险

// ============ 下拉文案宽度：单行 + 不溢出控件（③ 桌宠下拉「不应该换行」）====
// EUI-NEO 的 text 元素默认 wrap=false（core/dsl.h:149）：超宽文本**不折行**，
// 而是直接画出控件右边缘（真机复现：皮肤名 "super-long-skin-name-here" 在
// 240 宽的皮肤下拉里文本压过 chevron 跑出控件外）。故「不换行」在实现上等价于
// 「文本宽度 ≤ 可用宽度」，这里给一个不依赖字体度量的保守估算 + 截断。
// chrome 常量与 EUI-NEO dropdown 内部取数一致（components/dropdown.h:122-124,
// 178/197）：字段文案 = spacing.content(12) + spacing.micro(2) + spacing.overlay(48)；
// 弹层项 = spacing.small(6) × 2 + spacing.panel(24)。
inline constexpr float kDropdownFieldChrome = 62.0f;  // 12 + 2 + 48
inline constexpr float kDropdownItemChrome = 36.0f;   // 6 × 2 + 24
// 估算口径：ASCII 0.62em / 其余（CJK 等全角）1.0em。实测本机 UI 字体下拉丁
// 字符约 0.49em（"super-long-skin-name-here" 25 字符 × 16px ≈ 196px），取 0.62
// 是**偏保守**：宁可早截一点，也不许溢出（溢出 = 压 chevron / 跑出控件）。
inline constexpr float kAsciiWidthEm = 0.62f;
inline constexpr float kFullWidthEm = 1.0f;

// 桌宠皮肤下拉宽度：下限 240（与其余设置控件同宽），按最长皮肤名放大，
// 上限 420（再长的名字靠 fitControlLabel 截断显示，取值不受影响）
inline constexpr float kPetSkinFieldMinWidth = 240.0f;
inline constexpr float kPetSkinFieldMaxWidth = 420.0f;

// ============ 下拉「谁打开着」的单一持有者（审计 THS-05）============
// 此前 7 个下拉各持一个 bool：开一个不会关掉先前那个，判断/维护要逐个点名。
// 现在只有一个索引：打开任一项时其它自动关（同时最多一个下拉展开），
// -1 = 全关。新增下拉只需在 Dropdown 里加一项 + 传自己的枚举值。
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

// 组件 onOpenChange 通道的等价语义（原 SettingsModule::setDropdownOpen）：
// v=true → 独占打开（自动关掉先前那个）；v=false → 只有当前打开的是自己才关
//（避免迟到回调误关别人的下拉）。返回新的持有者索引。
inline int applyDropdownOpen(int openIndex, Dropdown d, bool v) {
    if (v) {
        return static_cast<int>(d);
    }
    return isDropdownOpen(openIndex, d) ? -1 : openIndex;
}

// ============ 纯函数 ============

// 档位最近邻索引（原 settings_module.cpp 文件内匿名 ns 的同名函数）
int nearestIndex(int value, const int* presets, int count);

// 非 token 的硬编码字号跟随全局缩放（单一来源 core::theme::fontScale；
// token 字号已在 theme 层缩放，不要再经本函数包一层，避免双重缩放）。
float fs(float base);

// 代理卡接管/警告行几何（drawProxyCard 与 proxyCardHeightFor 单一来源——
// compose 的卡高公式与卡内绘制各写一份必然漂移，同本头「卡高与卡内几何同源」
// 口径）。上方端口行是字面高控件（input 28），故接管行 y=98 锚在字面链上；
// 警告行 y 从接管行**派生盒底**起算（100% 档 = 118，与旧字面一致）。
inline float proxyTakeoverRowHeight() { return roundFs(18.0f); }  // 100% = 18
inline float proxyWarnRowHeight() { return roundFs(16.0f); }      // 100% = 16
inline float proxyWarnRowTop() {
    return 98.0f + proxyTakeoverRowHeight() + roundFs(2.0f);      // 100% = 118
}
// 代理卡高 = 内容底沿 + roundFs(16) 下边距（compose 顶部算 proxyH 唯一入口）。
// 100% 档：无警告 132 / n 条警告 118 + 16n + 16 —— 与旧字面公式逐一相等。
inline float proxyCardHeightFor(std::size_t warnLines) {
    const float bottom =
        warnLines > 0
            ? proxyWarnRowTop() +
                  static_cast<float>(warnLines) * proxyWarnRowHeight()
            : 98.0f + proxyTakeoverRowHeight();
    return bottom + roundFs(16.0f);
}

// 下拉弹层向下展开是否越出窗体底边（越界 → 调用方置 openUp 向上展开）。
// 原三处（日志保留 / 日志级别 / 编辑器字号）各写一遍的同一几何，R6 收成一个
// 纯函数；算术与拆分前逐字一致（同样的小括号顺序 → 浮点结果逐位相同）。
// cardTopY = 卡顶页面绝对 y；fieldOffsetY / fieldHeight = 字段在卡内的 y 与高；
// itemCount = 弹层项数；screenH = 页面逻辑高。
bool dropdownPopupOverflows(float cardTopY, float fieldOffsetY,
                            float fieldHeight, int itemCount, float screenH);

// 日志保留档文案：0 = 永久保留，其余 "<n> 天"（原 SettingsModule::retentionLabel）
std::string retentionLabel(int days);

// 文本宽度估算（保守，不依赖字体度量）：ASCII 计 kAsciiWidthEm、其余计
// kFullWidthEm，乘 fontSize。用于「文案是否放得进控件」的判定与截断。
float estimatedTextWidth(const std::string& text, float fontSize);

// 单行文案适配：放得进 availableWidth 就原样返回；放不进则从左截断并在末尾
// 补 '…'，保证**含省略号后仍不超宽**（至少保留 1 个字符 + 省略号）。
// 只改显示串，不改取值（调用方回传的仍是原始下标 / 名字）。
std::string fitControlLabel(const std::string& text, float availableWidth,
                            float fontSize);

// 桌宠皮肤下拉宽度：按最长皮肤名的估算宽度放大，clamp 到
// [kPetSkinFieldMinWidth, kPetSkinFieldMaxWidth]（皮肤清单为空时返回下限）。
float petSkinFieldWidth(const std::vector<std::string>& skinNames,
                        float fontSize);

} // namespace module::settings
