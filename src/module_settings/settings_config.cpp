// src/module_settings/settings_config.cpp
#include "settings_config.h"

#include <algorithm>

#include <fstream>
#include <sstream>
#include <utility>

#include "core/logging.h"
#include "core/theme.h"
#include "platform/json.h"

#include <eui/app.h>
#include <eui/types.h>

namespace module::settings {

namespace {
constexpr const char* kKeyTheme = "theme";
// W9 换键一次性重置：EUI-NEO 0.7.0 修了字号 em 语义后文字放大约 33%，
// fontScalePercent/fontScalePercentV2 下的存量值（85/115/130）都是在
// 0.75 缩水语义下选的，修正后失去意义 → 一律忽略、回落默认 100，
// 用户在新语义下重选。保存只写 V3 键；V3 下任何值按字面生效。
constexpr const char* kKeyFontScale = "fontScalePercentV3";
constexpr const char* kKeyLogRetention = "logRetentionDays";
constexpr const char* kKeyProxyAutoStart = "claudeProxyAutoStart";
constexpr const char* kKeyWindowEffect = "windowEffect";
constexpr const char* kKeyEnabledModules = "enabledModules";

// 合法模块 ID 清单（sanitized 过滤未知项；与 kModuleId* 一一对应）
const char* const kKnownModuleIds[] = {
    kModuleIdHome, kModuleIdMarkdown,     kModuleIdTodo,
    kModuleIdClaudeProxy, kModuleIdSettings};

bool isKnownModuleId(const std::string& id) {
    for (const char* known : kKnownModuleIds) {
        if (id == known) return true;
    }
    return false;
}
} // namespace

std::vector<std::string> defaultEnabledModules() {
    return {kModuleIdHome, kModuleIdMarkdown, kModuleIdTodo,
            kModuleIdClaudeProxy, kModuleIdSettings};
}

bool moduleEnabled(const SettingsConfig& cfg, const std::string& moduleId) {
    // home/settings 不可关闭（设计 §5.1：落地页 + 开关 UI 宿主）
    if (moduleId == kModuleIdHome || moduleId == kModuleIdSettings) return true;
    return std::find(cfg.enabledModules.begin(), cfg.enabledModules.end(),
                     moduleId) != cfg.enabledModules.end();
}

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
    // 窗口效果：只认三档，未知值收敛为 off（照抄 fontScale 的钳制模式）
    if (out.windowEffect != kWindowEffectTranslucent &&
        out.windowEffect != kWindowEffectFrosted) {
        out.windowEffect = kWindowEffectOff;
    }
    // 模块开关：未知 ID 过滤（防手改 config）+ 去重 + 强制补齐恒启用项
    //（home/settings，设计 §5.1）。静默过滤——load 对文件里的未知 ID 才 WARN
    std::vector<std::string> modules;
    modules.reserve(out.enabledModules.size());
    for (const auto& id : out.enabledModules) {
        if (isKnownModuleId(id) &&
            std::find(modules.begin(), modules.end(), id) == modules.end()) {
            modules.push_back(id);
        }
    }
    for (const char* alwaysOn : {kModuleIdHome, kModuleIdSettings}) {
        if (std::find(modules.begin(), modules.end(), alwaysOn) ==
            modules.end()) {
            modules.push_back(alwaysOn);
        }
    }
    out.enabledModules = std::move(modules);
    return out;
}

float windowEffectAlpha(const std::string& mode) {
    if (mode == kWindowEffectTranslucent) return 0.85f;
    if (mode == kWindowEffectFrosted) return 0.55f;
    return 1.0f;  // off / 未知值：实色
}

core::platform::WindowEffect windowEffectRequest(const std::string& mode) {
    if (mode == kWindowEffectFrosted) {
        return core::platform::WindowEffect::Acrylic;
    }
    // off / translucent（含未知值）：显式关 backdrop，视觉由 clearColor alpha 决定
    return core::platform::WindowEffect::None;
}

std::string windowEffectDegradedHint(const std::string& mode,
                                     core::platform::WindowEffect active) {
    using core::platform::WindowEffect;
    if (mode == kWindowEffectTranslucent) {
        if (active != WindowEffect::Transparent) {
            return "当前系统不支持窗口半透，已回退为不透明";
        }
        return {};
    }
    if (mode == kWindowEffectFrosted) {
        if (active == WindowEffect::Acrylic) {
            return {};
        }
        if (active == WindowEffect::Transparent) {
            return "当前系统不支持磨砂（需 Windows 11 22H2+），已回退为半透";
        }
        return "当前系统不支持窗口效果，已回退为不透明";
    }
    // off：alpha=1 下任何 active 都视觉等价（透明 hint 开着也不可见）
    return {};
}

void applyWindowVisuals(const std::string& mode) {
    // 底色跟随当前主题 token，alpha 按档位；setClearColor 覆盖主窗口 + 全部
    // 存活子窗口并触发全量重绘（EUI-NEO Phase B），主题切换底色即时跟随
    const eui::Color& bg = core::theme::current().colors.background;
    app::setClearColor(eui::Color{bg.r, bg.g, bg.b, windowEffectAlpha(mode)});
    // backdrop 请求（EUI-NEO Phase C）：下一帧应用，实际生效值经
    // app::activeWindowEffect() 回查，降级由设置页提示
    app::setWindowEffect(windowEffectRequest(mode));
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
    // 旧键 fontScalePercent / fontScalePercentV2 刻意不读（见文件头注释）
    if (obj.has(kKeyLogRetention) && obj.get(kKeyLogRetention).isNumber()) {
        cfg.logRetentionDays =
            static_cast<int>(obj.get(kKeyLogRetention).asInt());
    }
    if (obj.has(kKeyProxyAutoStart) && obj.get(kKeyProxyAutoStart).isBool()) {
        cfg.claudeProxyAutoStart = obj.get(kKeyProxyAutoStart).asBool();
    }
    if (obj.has(kKeyWindowEffect) && obj.get(kKeyWindowEffect).isString()) {
        cfg.windowEffect = obj.get(kKeyWindowEffect).asString();
    }
    // 模块开关：字符串数组；未知 ID 忽略 + WARN（防手改 config 打死模块，
    // 设计 §5.1/§8）。字段缺失 → 保持默认全启用
    if (obj.has(kKeyEnabledModules) && obj.get(kKeyEnabledModules).isArray()) {
        const platform::json::JsonValue arr = obj.get(kKeyEnabledModules);
        std::vector<std::string> ids;
        for (int i = 0; i < arr.size(); ++i) {
            if (!arr[i].isString()) continue;
            const std::string id = arr[i].asString();
            if (!isKnownModuleId(id)) {
                LOG_WARN("Settings: enabledModules 含未知模块 ID \"" + id +
                         "\"，已忽略");
                continue;
            }
            ids.push_back(id);
        }
        cfg.enabledModules = std::move(ids);
    }
    return sanitized(cfg);
}

core::Result<void> SettingsStore::save(const SettingsConfig& cfgRaw) const {
    const SettingsConfig cfg = sanitized(cfgRaw);

    platform::json::JsonObject obj;
    obj.set(kKeyTheme, platform::json::makeString(cfg.darkTheme ? "dark" : "light"));
    obj.set(kKeyFontScale, platform::json::makeInt(cfg.fontScalePercent));
    obj.set(kKeyLogRetention, platform::json::makeInt(cfg.logRetentionDays));
    obj.set(kKeyProxyAutoStart, platform::json::makeBool(cfg.claudeProxyAutoStart));
    obj.set(kKeyWindowEffect, platform::json::makeString(cfg.windowEffect));
    obj.set(kKeyEnabledModules,
            platform::json::makeStringArray(cfg.enabledModules));

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
