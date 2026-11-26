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
// need#452：常用目录列表键；startupDir 复用为默认镜像（旧读取方零改动）。
constexpr const char* kKeyStartupDirs = "startupDirs";
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

std::string trimCopy(const std::string& in) {
    const auto isSpace = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    size_t b = 0, e = in.size();
    while (b < e && isSpace(static_cast<unsigned char>(in[b]))) b++;
    while (e > b && isSpace(static_cast<unsigned char>(in[e - 1]))) e--;
    return in.substr(b, e - b);
}

} // namespace

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

StartupDirConvergence convergeStartupDirs(std::vector<std::string> dirs,
                                          std::string defaultDir) {
    StartupDirConvergence out;
    out.defaultDir = trimCopy(defaultDir);
    for (const std::string& raw : dirs) {
        const std::string d = trimCopy(raw);
        if (d.empty()) continue;
        bool dup = false;
        for (const std::string& e : out.dirs) {
            if (startupDirPathEqual(e, d)) {
                dup = true;
                break;
            }
        }
        if (!dup) out.dirs.push_back(d);
        if (static_cast<int>(out.dirs.size()) >= kStartupDirsMax) break;
    }
    if (!out.defaultDir.empty()) {
        bool listed = false;
        for (const std::string& e : out.dirs) {
            if (startupDirPathEqual(e, out.defaultDir)) {
                out.defaultDir = e;  // 镜像取列表内原写法（保 ∈ 列表字面值）
                listed = true;
                break;
            }
        }
        if (!listed) {
            out.dirs.insert(out.dirs.begin(), out.defaultDir);
            if (static_cast<int>(out.dirs.size()) > kStartupDirsMax) out.dirs.pop_back();
        }
    } else if (!out.dirs.empty()) {
        out.defaultDir = out.dirs.front();
    }
    return out;
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
    // need#452：目录列表与默认镜像同一收敛点（迁移/旧值并入/网页设默认都走这里）
    const StartupDirConvergence conv =
        convergeStartupDirs(settings.startupDirs, settings.startupDir);
    out.startupDirs = conv.dirs;
    out.startupDir = conv.defaultDir;  // 有效性在启动时校验，非 sanitize 职责（设计 §9）
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
    // need#452：列表缺失 = 旧结构（迁移交由 sanitize 用 startupDir 并入）
    {
        const platform::json::JsonValue arr = obj.get(kKeyStartupDirs);
        if (arr.isArray()) {
            for (int i = 0; i < arr.size(); i++) {
                const platform::json::JsonValue item = arr[i];
                if (item.isString()) s.startupDirs.push_back(item.asString());
            }
        }
    }
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
    obj.set(kKeyStartupDirs, platform::json::makeStringArray(s.startupDirs));
    if (s.webPort > 0) obj.set(kKeyWebPort, platform::json::makeInt(s.webPort));
    auto text = platform::json::stringify(platform::json::JsonValue(obj), /*pretty=*/false);
    if (text.isFail()) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "终端设置序列化失败: " + text.errorMessage());
    }
    return core::appconfig::set(*db, kSettingsKey, text.value());
}

} // namespace terminal
