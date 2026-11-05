// src/module_terminal/terminal_settings.h
// 终端设置模型 + 持久化（need250 设计 §4.3 字段面；落库口径 = md_view_state 同款：
// module 自管 app_config 单行 JSON（键 terminal_settings），不经 SettingsStore——
// module 间零依赖，settings_config 零接触）。空 shell = 平台默认、空 workDir = 用户
// 主目录、空 fontFamily = 内置等宽，是三条持久化契约的「空值语义」。
// 纯函数（钳制/sanitize/色板解析）doctest 直测。
#pragma once

#include <string>
#include <vector>

#include "core/result.h"
#include "core/sqlite_db.h"

namespace terminal {

enum class TerminalColorMode {
    FollowApp,    // 网格 ANSI 表随应用主题切（现状行为）
    Independent,  // 锁定所选色板不随主题
};

enum class TerminalColorPalette { Light, Dark };

enum class TerminalCursorStyle { Block, Bar };

// 设计契约值（§4.2/§4.3）
constexpr int kFontSizeMin = 10;
constexpr int kFontSizeMax = 24;
constexpr int kFontSizeDefault = 13;
constexpr int kScrollbackMin = 100;
constexpr int kScrollbackMax = 100000;
constexpr int kScrollbackDefault = 1000;
// 回滚行数超大值警示阈值（行下微字「较大回滚占用内存」）
constexpr int kScrollbackWarnThreshold = 50000;

struct TerminalSettings {
    std::string fontFamily = "";  // "" = 内置等宽（v2 字体栈方案 A）
    int fontSize = kFontSizeDefault;
    TerminalColorMode colorMode = TerminalColorMode::FollowApp;
    TerminalColorPalette colorPalette = TerminalColorPalette::Light;
    std::string shell = "";       // "" = 平台默认（win=pwsh 链 / mac=zsh 链）
    std::string workDir = "";     // "" = 用户主目录
    int scrollback = kScrollbackDefault;
    TerminalCursorStyle cursorStyle = TerminalCursorStyle::Block;
    bool cursorBlink = true;
};

// ---- 纯函数（doctest 直测） ----

int clampedFontSize(int value);
int clampedScrollback(int value);

// 网格 ANSI 表取哪套：跟随应用=随主题；独立=锁所选色板。供 grid compose 的
// darkTheme 参数（ansiIndexedColor 双表选择），渲染零新面。
bool gridPaletteDark(bool appDark, TerminalColorMode mode, TerminalColorPalette palette);

// 新会话 spawn 前的目录有效性：空 = 主目录语义（有效）；非空须存在（无效 = false，
// 调用方静默回退主目录，设计 §4.2⑤ 容错）。
bool workDirUsableForSpawn(const std::string& dir);

// 越界收敛 + 枚举外值回默认 + 字体/shell 枚举外回空（平台默认语义）。load 与 UI 写入共用。
TerminalSettings sanitizedTerminalSettings(const TerminalSettings& settings);

// ---- 平台选项表（terminal_settings_platform_win.cpp / _posix.cpp 各一套） ----

// 字体下拉取值表（首项恒 ""=内置等宽）。
std::vector<std::string> terminalFontOptionValues();

// shell 下拉取值表（首项恒 ""=平台默认）。
std::vector<std::string> terminalShellOptionValues();

// 平台默认 shell 名（展示「默认（pwsh）」/「默认（zsh）」用）。
std::string terminalDefaultShellName();

// ---- 持久化（app_config 单行 JSON，键 terminal_settings） ----

// db 为空 / 键缺失 / JSON 损坏 → 默认值（损坏记 LOG_ERROR）。
TerminalSettings loadTerminalSettings(core::data::SqliteDb* db);

// db 为空 → fail；写入前 sanitize（落库值恒合法）。
core::Result<void> saveTerminalSettings(core::data::SqliteDb* db,
                                        const TerminalSettings& settings);

} // namespace terminal
