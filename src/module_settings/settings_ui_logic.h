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

#include <string>

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

// ============ 设置页 4 tab（Task B：页面太长拆 tab + 日志查看）============
// 通用 = 外观 + 编辑器字号；模块 = 插拔开关 + 桌宠 + 代理 + 目录；
// 日志 = 保留天数 + 级别 + tail 查看；关于 = 版本/路径
inline constexpr const char* kSettingTabLabels[] = {"通用", "模块", "日志",
                                                    "关于"};
inline constexpr int kSettingTabCount = 4;
inline constexpr int kTabGeneral = 0;
inline constexpr int kTabModules = 1;
inline constexpr int kTabLog = 2;
inline constexpr int kTabAbout = 3;
inline constexpr float kSettingsTabHeight = 40.0f;  // tab 栏高
inline constexpr int kLogTailMaxLines = 500;        // 日志查看 tail 行数上限
inline constexpr long kLogTailReadWindow = 256 * 1024;  // tail 只读末尾 256KB

// ============ 卡片高度（唯一来源：注册表卡高与卡内几何同源）============
// 注册表（settings_module.cpp kCardSpecs）按这些常量摆卡；改动必须同时满足卡内
// 内容底沿 + 16px 下边距的既有公式，否则卡片内容会被裁或贴底边。卡内容内边距
// 口径（2026-09-28 系统性修复）：四周 ≥16px——顶（标题 y≥16）、左右 28（对齐
// kDialogInsetX 语系）、底 = 卡高 - 内容底沿 ≥ 16。
inline constexpr float kAppearanceCardHeight = 172.0f;  // 外观卡（通用；降级提示行底 156 + 16）
inline constexpr float kEditorFontCardHeight = 108.0f;  // 编辑器字号卡（通用）
inline constexpr float kModulesCardHeight = 288.0f;     // 模块插拔卡（模块）
inline constexpr float kPetCardHeight = 120.0f;         // 桌宠卡（模块）
inline constexpr float kDirectoryCardHeight = 116.0f;   // 目录卡（模块；hint 底 100 + 16）
inline constexpr float kLogSettingsCardHeight = 148.0f; // 日志卡（日志；编辑器字号迁出后收窄）
inline constexpr float kAboutCardHeight = 150.0f;       // 关于卡（关于）

// ============ 下拉 openUp 越界判定几何 ============
// 日志卡 / 编辑器字号卡的下拉贴近窗体底边，向下展开可能被裁剪；用同一套几何
// 预先判断「向下展开是否出界」，出界则置 openUp 让 dropdown 向上展开。
// 弹层三项必须与 EUI-NEO dropdown 实际渲染一致，否则判定失真：取值来源是
// EUI-NEO components/theme.h 的默认 metrics（control.menuItem=34 /
// spacing.compact=8 / spacing.small=6），若将来主题覆写 metrics 需同步。
inline constexpr float kLogDropdownOffsetY = 48.0f;    // ddwrap 在日志卡内 y 偏移
inline constexpr float kLogDropdownHeight = 40.0f;     // dropdown 控件高度
inline constexpr float kEditorFontDdOffsetY = 42.0f;   // 编辑器字号卡内 dropdown y（Task B 独立卡）
inline constexpr float kLogEditorFontHeight = 36.0f;   // 编辑器字号 dropdown 高
inline constexpr float kLogLevelOffsetY = 92.0f;       // 日志级别行 y 偏移（spec 缩水 D；编辑器字号行迁出通用 tab 后上移）
inline constexpr float kLogLevelHeight = 36.0f;        // 日志级别 dropdown 高
inline constexpr float kDropdownPopupGap = 8.0f;       // 字段底边与弹层间距（spacing.compact）
inline constexpr float kDropdownItemHeight = 34.0f;    // 单个菜单项高（control.menuItem）
inline constexpr float kDropdownPopupPadding = 12.0f;  // 弹层上下 padding（spacing.small × 2）
inline constexpr float kWindowBottomMargin = 16.0f;    // 窗口底边安全留白，越界 = 有裁剪风险

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

// 下拉弹层向下展开是否越出窗体底边（越界 → 调用方置 openUp 向上展开）。
// 原三处（日志保留 / 日志级别 / 编辑器字号）各写一遍的同一几何，R6 收成一个
// 纯函数；算术与拆分前逐字一致（同样的小括号顺序 → 浮点结果逐位相同）。
// cardTopY = 卡顶页面绝对 y；fieldOffsetY / fieldHeight = 字段在卡内的 y 与高；
// itemCount = 弹层项数；screenH = 页面逻辑高。
bool dropdownPopupOverflows(float cardTopY, float fieldOffsetY,
                            float fieldHeight, int itemCount, float screenH);

// 日志保留档文案：0 = 永久保留，其余 "<n> 天"（原 SettingsModule::retentionLabel）
std::string retentionLabel(int days);

} // namespace module::settings
