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
constexpr const char* kKeyWorkDir = "workDir";
constexpr const char* kKeyScrollback = "scrollback";
constexpr const char* kKeyCursorStyle = "cursorStyle";
constexpr const char* kKeyCursorBlink = "cursorBlink";

constexpr const char* kColorModeFollow = "followApp";
constexpr const char* kColorModeIndependent = "independent";
constexpr const char* kPaletteLight = "light";
constexpr const char* kPaletteDark = "dark";
constexpr const char* kCursorBlock = "block";
constexpr const char* kCursorBar = "bar";

bool contains(const std::vector<std::string>& values, const std::string& v) {
    return std::find(values.begin(), values.end(), v) != values.end();
}

} // namespace

int clampedFontSize(int value) {
    return std::clamp(value, kFontSizeMin, kFontSizeMax);
}

int clampedScrollback(int value) {
    return std::clamp(value, kScrollbackMin, kScrollbackMax);
}

bool gridPaletteDark(bool appDark, TerminalColorMode mode, TerminalColorPalette palette) {
    if (mode == TerminalColorMode::Independent) return palette == TerminalColorPalette::Dark;
    return appDark;
}

bool workDirUsableForSpawn(const std::string& dir) {
    if (dir.empty()) return true; // 空 = 主目录语义
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(dir), ec) && !ec;
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
    out.fontSize = clampedFontSize(settings.fontSize);
    out.colorMode = settings.colorMode;
    out.colorPalette = settings.colorPalette;
    out.workDir = settings.workDir; // workDir 不在此校验存在性（开新会话时才回退，§4.2⑤）
    out.scrollback = clampedScrollback(settings.scrollback);
    out.cursorStyle = settings.cursorStyle;
    out.cursorBlink = settings.cursorBlink;
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
    if (obj.has(kKeyFontFamily) && obj.get(kKeyFontFamily).isString())
        s.fontFamily = obj.get(kKeyFontFamily).asString();
    if (obj.has(kKeyFontSize) && obj.get(kKeyFontSize).isNumber())
        s.fontSize = static_cast<int>(obj.get(kKeyFontSize).asInt());
    if (obj.has(kKeyColorMode) && obj.get(kKeyColorMode).isString()) {
        const std::string m = obj.get(kKeyColorMode).asString();
        if (m == kColorModeIndependent) s.colorMode = TerminalColorMode::Independent;
    }
    if (obj.has(kKeyColorPalette) && obj.get(kKeyColorPalette).isString()) {
        const std::string p = obj.get(kKeyColorPalette).asString();
        if (p == kPaletteDark) s.colorPalette = TerminalColorPalette::Dark;
    }
    if (obj.has(kKeyShell) && obj.get(kKeyShell).isString())
        s.shell = obj.get(kKeyShell).asString();
    if (obj.has(kKeyWorkDir) && obj.get(kKeyWorkDir).isString())
        s.workDir = obj.get(kKeyWorkDir).asString();
    if (obj.has(kKeyScrollback) && obj.get(kKeyScrollback).isNumber())
        s.scrollback = static_cast<int>(obj.get(kKeyScrollback).asInt());
    if (obj.has(kKeyCursorStyle) && obj.get(kKeyCursorStyle).isString()) {
        const std::string c = obj.get(kKeyCursorStyle).asString();
        if (c == kCursorBar) s.cursorStyle = TerminalCursorStyle::Bar;
    }
    if (obj.has(kKeyCursorBlink) && obj.get(kKeyCursorBlink).isBool())
        s.cursorBlink = obj.get(kKeyCursorBlink).asBool();
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
    obj.set(kKeyWorkDir, platform::json::makeString(s.workDir));
    obj.set(kKeyScrollback, platform::json::makeInt(s.scrollback));
    obj.set(kKeyCursorStyle, platform::json::makeString(
                                 s.cursorStyle == TerminalCursorStyle::Bar ? kCursorBar
                                                                           : kCursorBlock));
    obj.set(kKeyCursorBlink, platform::json::makeBool(s.cursorBlink));
    auto text = platform::json::stringify(platform::json::JsonValue(obj), /*pretty=*/false);
    if (text.isFail()) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "终端设置序列化失败: " + text.errorMessage());
    }
    return core::appconfig::set(*db, kSettingsKey, text.value());
}

} // namespace terminal
