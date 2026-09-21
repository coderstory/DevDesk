// src/module_settings/settings_config.cpp
#include "settings_config.h"

#include <fstream>
#include <sstream>
#include <utility>

#include "core/logging.h"
#include "platform/json.h"

namespace module::settings {

namespace {
constexpr const char* kKeyTheme = "theme";
constexpr const char* kKeyFontScale = "fontScalePercent";
constexpr const char* kKeyLogRetention = "logRetentionDays";
} // namespace

SettingsStore::SettingsStore(std::filesystem::path file)
    : file_(std::move(file)) {}

SettingsConfig SettingsStore::sanitized(const SettingsConfig& cfg) {
    SettingsConfig out = cfg;
    if (out.fontScalePercent < kMinFontScalePercent) {
        out.fontScalePercent = kMinFontScalePercent;
    }
    if (out.fontScalePercent > kMaxFontScalePercent) {
        out.fontScalePercent = kMaxFontScalePercent;
    }
    if (out.logRetentionDays < 0) out.logRetentionDays = 0;
    if (out.logRetentionDays > kMaxLogRetentionDays) {
        out.logRetentionDays = kMaxLogRetentionDays;
    }
    return out;
}

SettingsConfig SettingsStore::load() const {
    SettingsConfig cfg;  // 全默认值兜底

    std::error_code ec;
    if (!std::filesystem::exists(file_, ec) || ec) {
        return cfg;  // 首次启动：无配置文件，正常路径
    }

    std::ifstream f(file_);
    if (!f) {
        LOG_WARN("Settings: 无法读取配置文件，使用默认值: " + file_.string());
        return cfg;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    f.close();

    auto parsed = platform::json::parse(ss.str());
    if (parsed.isFail() || !parsed.unsafeValue().isObject()) {
        LOG_WARN("Settings: 配置文件解析失败，使用默认值: " + file_.string());
        return cfg;
    }
    platform::json::JsonObject obj = parsed.unsafeValue().asObject();

    if (obj.has(kKeyTheme) && obj.get(kKeyTheme).isString()) {
        // 只认 "light" 为浅色，其余一律深色（默认值安全）
        cfg.darkTheme = (obj.get(kKeyTheme).asString() != "light");
    }
    if (obj.has(kKeyFontScale) && obj.get(kKeyFontScale).isNumber()) {
        cfg.fontScalePercent =
            static_cast<int>(obj.get(kKeyFontScale).asInt());
    }
    if (obj.has(kKeyLogRetention) && obj.get(kKeyLogRetention).isNumber()) {
        cfg.logRetentionDays =
            static_cast<int>(obj.get(kKeyLogRetention).asInt());
    }
    return sanitized(cfg);
}

core::Result<void> SettingsStore::save(const SettingsConfig& cfgRaw) const {
    const SettingsConfig cfg = sanitized(cfgRaw);

    platform::json::JsonObject obj;
    obj.set(kKeyTheme, platform::json::makeString(cfg.darkTheme ? "dark" : "light"));
    obj.set(kKeyFontScale, platform::json::makeInt(cfg.fontScalePercent));
    obj.set(kKeyLogRetention, platform::json::makeInt(cfg.logRetentionDays));

    auto text = platform::json::stringify(
        platform::json::JsonValue(obj), /*pretty=*/true);
    if (text.isFail()) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "配置序列化失败: " + text.errorMessage());
    }

    // 父目录确保存在（appDataDir 理论上已建好，这里兜底）
    std::error_code ec;
    const auto parent = file_.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent, ec) && !ec) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return core::Result<void>::fail(core::ErrorCode::Internal,
                                            "配置目录创建失败: " + ec.message());
        }
    }

    std::ofstream f(file_, std::ios::trunc);
    if (!f) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "配置文件写入失败: " + file_.string());
    }
    f << text.unsafeValue() << "\n";
    f.close();
    if (!f) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "配置文件写入未完成: " + file_.string());
    }
    return core::Result<void>::ok();
}

} // namespace module::settings
