// src/module_terminal/terminal_settings.cpp
#include "terminal_settings.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include "core/app_config.h"
#include "core/logging.h"
#include "platform/json.h"

namespace terminal {

namespace {

constexpr const char* kSettingsKey = "terminal_settings";

constexpr const char* kKeyFontFamily = "fontFamily";
constexpr const char* kKeyFontSize = "fontSize";
constexpr const char* kKeyColorMode = "colorMode";
constexpr const char* kKeyColorPalette = "colorPalette";
constexpr const char* kKeyShell = "shell";
// PRD P-14：新键 startupDir；旧 workDir 键不读（C-4 已砍时的历史残留不复活）。
constexpr const char* kKeyStartupDir = "startupDir";
constexpr const char* kKeyRecentDirs = "recentStartupDirs"; // #383
constexpr const char* kKeyWebPort = "webPort";
constexpr int kWebPortMin = 1024;
constexpr int kWebPortMax = 65535;

constexpr const char* kColorModeFollow = "followApp";
constexpr const char* kColorModeIndependent = "independent";
constexpr const char* kPaletteLight = "light";
constexpr const char* kPaletteDark = "dark";

bool contains(const std::vector<std::string>& values, const std::string& v) {
    return std::find(values.begin(), values.end(), v) != values.end();
}

} // namespace

std::string serializeRecentDirs(const std::vector<std::string>& dirs) {
    std::string out;
    for (const auto& d : dirs) {
        if (d.empty()) continue;
        if (!out.empty()) out.push_back('\n');
        out += d;
    }
    return out;
}

std::vector<std::string> parseRecentDirs(const std::string& packed) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : packed) {
        if (c == '\n') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

void pushRecentDir(std::vector<std::string>& dirs, const std::string& dir) {
    if (dir.empty()) return;
    for (auto it = dirs.begin(); it != dirs.end(); ++it) {
        if (*it == dir) {
            dirs.erase(it);
            break;
        }
    }
    dirs.insert(dirs.begin(), dir);
    if (static_cast<int>(dirs.size()) > TerminalSettings::kRecentDirLimit) {
        dirs.resize(static_cast<size_t>(TerminalSettings::kRecentDirLimit));
    }
}

int clampedFontSize(int value) {
    return std::clamp(value, kFontSizeMin, kFontSizeMax);
}

bool gridPaletteDark(bool appDark, TerminalColorMode mode, TerminalColorPalette palette) {
    if (mode == TerminalColorMode::Independent) return palette == TerminalColorPalette::Dark;
    return appDark;
}

bool workDirUsableForSpawn(const std::string& dir) {
    if (dir.empty()) return true; // 空 = 主目录语义
    std::error_code ec;
    const std::filesystem::path p(dir);
    if (!std::filesystem::exists(p, ec) || ec) return false;
    if (!std::filesystem::is_directory(p, ec) || ec) return false;
    return canEnterDirectory(dir);
}

TerminalSettings sanitizedTerminalSettings(const TerminalSettings& settings) {
    TerminalSettings out;
    // 枚举外字体/shell → 空 = 平台默认语义（设计 §4.3 约束列）
    const std::string font =
        contains(terminalFontOptionValues(), settings.fontFamily) ? settings.fontFamily : "";
    const std::string shell =
        contains(terminalShellOptionValues(), settings.shell) ? settings.shell : "";
    out.fontFamily = font;
    out.shell = shell;
    out.startupDir = settings.startupDir;  // 有效性在 writeSettings 入口校验，非 sanitize 职责
    out.recentStartupDirs = settings.recentStartupDirs;  // #383（去重/上限由 pushRecentDir 保证）
    out.fontSize = clampedFontSize(settings.fontSize);
    out.colorMode = settings.colorMode;
    out.colorPalette = settings.colorPalette;
    out.webPort = (settings.webPort >= kWebPortMin && settings.webPort <= kWebPortMax)
                      ? settings.webPort
                      : 0;
    return out;
}

TerminalSettings loadTerminalSettings(core::data::SqliteDb* db) {
    TerminalSettings defaults;
    if (db == nullptr) return defaults;
    auto got = core::appconfig::get(*db, kSettingsKey);
    if (got.isFail()) return defaults; // 键缺失 = 首次使用，默认值非错误
    auto parsed = platform::json::parse(got.value());
    if (parsed.isFail() || !parsed.value().isObject()) {
        LOG_ERROR("Terminal: 设置 JSON 损坏（" +
                  (parsed.isFail() ? parsed.errorMessage() : std::string("顶层非对象")) +
                  "），回退默认值");
        return defaults;
    }
    const platform::json::JsonObject obj = parsed.value().asObject();
    TerminalSettings s;
    if (auto v = platform::json::getIfString(obj, kKeyFontFamily))
        s.fontFamily = *v;
    if (auto v = platform::json::getIfInt(obj, kKeyFontSize))
        s.fontSize = static_cast<int>(*v);
    if (auto v = platform::json::getIfString(obj, kKeyColorMode)) {
        if (*v == kColorModeIndependent) s.colorMode = TerminalColorMode::Independent;
    }
    if (auto v = platform::json::getIfString(obj, kKeyColorPalette)) {
        if (*v == kPaletteDark) s.colorPalette = TerminalColorPalette::Dark;
    }
    if (auto v = platform::json::getIfString(obj, kKeyShell))
        s.shell = *v;
    if (auto v = platform::json::getIfString(obj, kKeyStartupDir))
        s.startupDir = *v;
    if (auto v = platform::json::getIfString(obj, kKeyRecentDirs))
        s.recentStartupDirs = parseRecentDirs(*v);
    if (auto v = platform::json::getIfInt(obj, kKeyWebPort))
        s.webPort = static_cast<int>(*v);
    // C-4（need380）：workDir/scrollback/cursorStyle/cursorBlink 旧键不再读取
    //（行为固定默认；旧 JSON 里残留键在下次 save 整体重写时消失 = 一次性归一）。
    return sanitizedTerminalSettings(s);
}

core::Result<void> saveTerminalSettings(core::data::SqliteDb* db,
                                        const TerminalSettings& settingsRaw) {
    if (db == nullptr) {
        return core::Result<void>::fail(
            core::ErrorCode::NotFound,
            "应用数据库未就绪（core::appDatabase() 为空），终端设置未保存");
    }
    const TerminalSettings s = sanitizedTerminalSettings(settingsRaw);
    platform::json::JsonObject obj;
    obj.set(kKeyFontFamily, platform::json::makeString(s.fontFamily));
    obj.set(kKeyFontSize, platform::json::makeInt(s.fontSize));
    obj.set(kKeyColorMode, platform::json::makeString(
                               s.colorMode == TerminalColorMode::Independent
                                   ? kColorModeIndependent
                                   : kColorModeFollow));
    obj.set(kKeyColorPalette, platform::json::makeString(
                                  s.colorPalette == TerminalColorPalette::Dark ? kPaletteDark
                                                                               : kPaletteLight));
    obj.set(kKeyShell, platform::json::makeString(s.shell));
    obj.set(kKeyStartupDir, platform::json::makeString(s.startupDir));
    obj.set(kKeyRecentDirs, platform::json::makeString(serializeRecentDirs(s.recentStartupDirs)));
    if (s.webPort > 0) obj.set(kKeyWebPort, platform::json::makeInt(s.webPort));
    auto text = platform::json::stringify(platform::json::JsonValue(obj), /*pretty=*/false);
    if (text.isFail()) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "终端设置序列化失败: " + text.errorMessage());
    }
    return core::appconfig::set(*db, kSettingsKey, text.value());
}

} // namespace terminal
