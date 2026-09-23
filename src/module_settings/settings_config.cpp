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
// themeMode 三档字符串键（spec 缩水 A；值 = kThemeMode* 常量）
constexpr const char* kKeyThemeMode = "themeMode";
// Markdown 编辑器字号（px）键（spec 缩水 B）
constexpr const char* kKeyEditorFontSize = "editorFontSize";
// W9 换键一次性重置：EUI-NEO 0.7.0 修了字号 em 语义后文字放大约 33%，
// fontScalePercent/fontScalePercentV2 下的存量值（85/115/130）都是在
// 0.75 缩水语义下选的，修正后失去意义 → 一律忽略、回落默认 100，
// 用户在新语义下重选。保存只写 V3 键；V3 下任何值按字面生效。
constexpr const char* kKeyFontScale = "fontScalePercentV3";
constexpr const char* kKeyLogRetention = "logRetentionDays";
constexpr const char* kKeyProxyAutoStart = "claudeProxyAutoStart";
constexpr const char* kKeyProxyPort = "claudeProxyPort";
constexpr const char* kKeyWindowEffect = "windowEffect";
constexpr const char* kKeyEnabledModules = "enabledModules";
constexpr const char* kKeySidebarCollapsed = "sidebarCollapsed";

// 合法模块 ID 清单（sanitized 过滤未知项；与 kModuleId* 一一对应）
const char* const kKnownModuleIds[] = {
    kModuleIdHome, kModuleIdMarkdown,     kModuleIdTodo,
    kModuleIdClaudeProxy, kModuleIdSettings, kModuleIdPet};

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

bool parseProxyPortInput(const std::string& text, int& out) {
    // 纯数字约束：任一非数字字符即拒绝（不静默截取，避免 "88ab" 悄悄变 88）
    if (text.empty() || text.size() > 5) return false;  // 5 位封顶（65535）
    int value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    if (value < kMinProxyPort || value > kMaxProxyPort) return false;
    out = value;
    return true;
}

std::string maskProxyToken(const std::string& token) {
    if (token.empty()) return {};
    // 短 token（<8 位）整体遮蔽：前后缀会暴露大半内容，且固定 "****"
    // 不泄露真实长度（32 hex 的常规代理 token 恒走下面的前后缀分支）
    if (token.size() < 8) return "****";
    return token.substr(0, 4) + "***" + token.substr(token.size() - 4);
}

bool resolveThemeDark(const std::string& themeMode, bool systemPrefersDark) {
    if (themeMode == kThemeModeLight) return false;
    if (themeMode == kThemeModeDark) return true;
    // auto（含未知值，sanitized 后理论不可达）：跟随系统检测
    return systemPrefersDark;
}

// ---- 关于卡文案（spec 缩水 C）。宏缺失回退（手工编译/其他 target）----
#ifndef DEVDESK_VERSION
#define DEVDESK_VERSION "dev"
#endif
#ifndef DEVDESK_BUILD_TYPE
#define DEVDESK_BUILD_TYPE "?"
#endif
#ifndef DEVDESK_EUI_NEO_REV
#define DEVDESK_EUI_NEO_REV "unknown"
#endif
#ifndef DEVDESK_EUI_NEO_MIN_REV
#define DEVDESK_EUI_NEO_MIN_REV "?"
#endif

std::string aboutVersionLine() {
    // 版本号 = 顶层 CMake project(VERSION)；构建类型 + 编译时间戳
    //（__DATE__/__TIME__ 为本 TU 编译时刻，Debug 增量构建后随之刷新）
    return std::string("DevDesk ") + DEVDESK_VERSION + " · " +
           DEVDESK_BUILD_TYPE + " · 构建 " __DATE__ " " __TIME__;
}

std::string aboutEuiNeoLine() {
    // EUI-NEO 以同级源码集成（非 submodule），rev 取 configure 期
    // git rev-parse（顶层 CMakeLists「EUI-NEO 版本诊断」同一变量）
    return std::string("EUI-NEO rev ") + DEVDESK_EUI_NEO_REV +
           "（最低依赖 rev " + DEVDESK_EUI_NEO_MIN_REV + "）";
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
    // 编辑器字号（spec 缩水 B）：钳制区间与 core::theme::setEditorFontSize
    // 同口径（load 回退默认、save 前收敛，双向不产生越界值）
    if (out.editorFontSize < kMinEditorFontSize) {
        out.editorFontSize = kMinEditorFontSize;
    }
    if (out.editorFontSize > kMaxEditorFontSize) {
        out.editorFontSize = kMaxEditorFontSize;
    }
    // 代理端口（审计 [14]）：越界/0/负数不钳到边界而是回落默认——与
    // ClaudeService::setConfiguredProxyPort 同语义（0 钳到 1 会得到一个
    // 必然 bind 失败/被特权的端口，回落 8888 更可预期）
    if (out.proxyPort < kMinProxyPort || out.proxyPort > kMaxProxyPort) {
        out.proxyPort = kDefaultProxyPort;
    }
    // 主题模式（spec 缩水 A）：只认三档，未知值收敛为 auto（与新人默认同
    // 语义；sanitized 保持纯函数，不在此处解析 auto→darkTheme——解析需要
    // platform 检测，收在 SettingsModule::applyThemeRuntime）
    if (out.themeMode != kThemeModeLight && out.themeMode != kThemeModeDark) {
        out.themeMode = kThemeModeAuto;
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
    // themeMode（spec 缩水 A）：新键优先（只认三档字面值）；键缺失/非法时
    // 按 legacy "theme" 派生为显式档（存量用户的明确选择不能被静默改成
    // auto）；两键都没有 = 首启用默认 auto。auto→深浅的实际解析在
    // SettingsModule::applyThemeRuntime（load/sanitized 保持纯函数，
    // 不经 platform，单测机器无关）
    if (obj.has(kKeyThemeMode) && obj.get(kKeyThemeMode).isString()) {
        const std::string mode = obj.get(kKeyThemeMode).asString();
        if (mode == kThemeModeAuto || mode == kThemeModeLight ||
            mode == kThemeModeDark) {
            cfg.themeMode = mode;
        } else if (obj.has(kKeyTheme) && obj.get(kKeyTheme).isString()) {
            cfg.themeMode = (obj.get(kKeyTheme).asString() == "light")
                                ? kThemeModeLight
                                : kThemeModeDark;
        }
    } else if (obj.has(kKeyTheme) && obj.get(kKeyTheme).isString()) {
        cfg.themeMode = (obj.get(kKeyTheme).asString() == "light")
                            ? kThemeModeLight
                            : kThemeModeDark;
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
    if (obj.has(kKeyEditorFontSize) && obj.get(kKeyEditorFontSize).isNumber()) {
        cfg.editorFontSize =
            static_cast<int>(obj.get(kKeyEditorFontSize).asInt());
    }
    if (obj.has(kKeyProxyAutoStart) && obj.get(kKeyProxyAutoStart).isBool()) {
        cfg.claudeProxyAutoStart = obj.get(kKeyProxyAutoStart).asBool();
    }
    if (obj.has(kKeyProxyPort) && obj.get(kKeyProxyPort).isNumber()) {
        cfg.proxyPort = static_cast<int>(obj.get(kKeyProxyPort).asInt());
    }
    if (obj.has(kKeyWindowEffect) && obj.get(kKeyWindowEffect).isString()) {
        cfg.windowEffect = obj.get(kKeyWindowEffect).asString();
    }
    if (obj.has(kKeySidebarCollapsed) &&
        obj.get(kKeySidebarCollapsed).isBool()) {
        cfg.sidebarCollapsed = obj.get(kKeySidebarCollapsed).asBool();
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
    // themeMode 与 theme 双写：themeMode = 用户意图（auto/light/dark），
    // theme = 当前解析档（main.cpp 启动路径只认 theme，settings onStart
    // 随即按 themeMode 幂等纠正——main.cpp 零改动）
    obj.set(kKeyThemeMode, platform::json::makeString(cfg.themeMode));
    obj.set(kKeyEditorFontSize, platform::json::makeInt(cfg.editorFontSize));
    obj.set(kKeyFontScale, platform::json::makeInt(cfg.fontScalePercent));
    obj.set(kKeyLogRetention, platform::json::makeInt(cfg.logRetentionDays));
    obj.set(kKeyProxyAutoStart, platform::json::makeBool(cfg.claudeProxyAutoStart));
    obj.set(kKeyProxyPort, platform::json::makeInt(cfg.proxyPort));
    obj.set(kKeyWindowEffect, platform::json::makeString(cfg.windowEffect));
    obj.set(kKeySidebarCollapsed,
            platform::json::makeBool(cfg.sidebarCollapsed));
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
