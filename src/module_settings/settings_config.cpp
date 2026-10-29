// src/module_settings/settings_config.cpp
#include "settings_config.h"

#include <algorithm>

#include <utility>

#include "core/app_config.h"
#include "core/logging.h"
#include "core/theme.h"
#include "core/window_effect_state.h"  // 档位单值写口（sidebar/about 铺色 alpha 读）
#include "platform/json.h"

#include <eui/app.h>
#include <eui/types.h>

namespace module::settings {

namespace {
constexpr const char* kKeyTheme = "theme";
constexpr const char* kKeyThemeMode = "themeMode";
constexpr const char* kKeyEditorFontSize = "editorFontSize";
constexpr const char* kKeyLogLevel = "logLevel";
// 字号键用 V3（fontScalePercent/V2 旧键一律不读，回落默认 100）
constexpr const char* kKeyFontScale = "fontScalePercentV3";
constexpr const char* kKeyLogRetention = "logRetentionDays";
constexpr const char* kKeyProxyAutoStart = "claudeProxyAutoStart";
constexpr const char* kKeyAutostart = "autostartEnabled";
// 动态背景（UAT6 主窗活体流质）：bool 直存；键缺失（存量库/旧版本升级）→ 默认开
constexpr const char* kKeyFluidBackground = "fluidBackgroundEnabled";
constexpr const char* kKeyProxyPort = "claudeProxyPort";
constexpr const char* kKeyMcpPort = "mcpPort";
constexpr const char* kKeyWindowEffect = "windowEffect";
constexpr const char* kKeyEnabledModules = "enabledModules";
// 默认模块清单迁移标记：键缺失 = 旧版本配置（见 kModuleDefaultsVersion）
constexpr const char* kKeyModuleDefaultsVersion = "moduleDefaultsVersion";
constexpr const char* kKeySidebarCollapsed = "sidebarCollapsed";
constexpr const char* kKeyAccentTheme = "accentTheme";
// 窗体记忆（UAT6）：还原态尺寸 0 = 无记录；sanitized 负值/越此上限归 0
//（巨值/微值的可用性由恢复侧钳制链兜底，这里只防脏数据落账）
constexpr const char* kKeyWindowRestoreWidth = "windowRestoreWidth";
constexpr const char* kKeyWindowRestoreHeight = "windowRestoreHeight";
constexpr const char* kKeyWindowMaximized = "windowMaximized";
constexpr int kMaxWindowRestoreSide = 65535;

// 合法模块 ID 清单（sanitized 过滤未知项；与 kModuleId* 一一对应）
const char* const kKnownModuleIds[] = {
    kModuleIdHome, kModuleIdMarkdown,     kModuleIdTodo,
    kModuleIdSchedule, kModuleIdClaudeProxy, kModuleIdSettings,
    kModuleIdAgent, kModuleIdMemory, kModuleIdWallpaper, kModuleIdMcp,
    kModuleIdRelease};

bool isKnownModuleId(const std::string& id) {
    for (const char* known : kKnownModuleIds) {
        if (id == known) return true;
    }
    return false;
}

// 版本化默认模块迁移表：一行 = 「该版本起新增的默认启用模块」。
// 只并入这些 ID、绝不整体重置清单——存量的缺席按版本判定为「从未存在」，
// 用户显式关闭的模块（配置已带当前版本标记）不会被重新打开。
struct ModuleDefaultMigration {
    int version;
    const char* moduleId;
};
constexpr ModuleDefaultMigration kModuleDefaultMigrations[] = {
    {1, kModuleIdRelease},
    {2, kModuleIdSchedule},
};

void mergeDefaultModulesSince(std::vector<std::string>& ids, int storedVersion) {
    for (const auto& m : kModuleDefaultMigrations) {
        if (m.version <= storedVersion) continue;
        if (std::find(ids.begin(), ids.end(), m.moduleId) == ids.end()) {
            ids.push_back(m.moduleId);
        }
    }
}
} // namespace

std::vector<std::string> defaultEnabledModules() {
    // agent 不在默认清单（跑 LLM 有成本，用户主动启用）；wallpaper 同默认关闭；
    // mcp 后台监听端口，同样默认关闭（开关在设置 → 模块卡）。
    // memory 默认开启：引擎未安装时页面只显引导态，零后台成本。
    // release 默认启用（任务/基础配置两页共用该键）；schedule 默认启用
    //（此前恒注册，开关卡补齐后存量配置经迁移 v2 并入）。
    return {kModuleIdHome, kModuleIdMarkdown, kModuleIdTodo,
            kModuleIdSchedule, kModuleIdClaudeProxy, kModuleIdMemory,
            kModuleIdSettings, kModuleIdRelease};
}

bool moduleEnabled(const SettingsConfig& cfg, const std::string& moduleId) {
    // home/settings 不可关闭（落地页 + 开关 UI 宿主）
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

bool parseMcpPortInput(const std::string& text, int& out) {
    // 与 parseProxyPortInput 同口径：只接受范围内纯数字
    if (text.empty() || text.size() > 5) return false;
    int value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    if (value < kMinMcpPort || value > kMaxMcpPort) return false;
    out = value;
    return true;
}

bool resolveThemeDark(const std::string& themeMode, bool systemPrefersDark) {
    if (themeMode == kThemeModeLight) return false;
    if (themeMode == kThemeModeDark) return true;
    // auto（含未知值，sanitized 后理论不可达）：跟随系统检测
    return systemPrefersDark;
}

core::log::Level parseLogLevel(const std::string& mode) {
    if (mode == kLogLevelDebug) return core::log::Level::Debug;
    if (mode == kLogLevelWarn) return core::log::Level::Warn;
    if (mode == kLogLevelError) return core::log::Level::Error;
    // INFO 与一切未知值（sanitized 保证正常路径只会是四档之一）
    return core::log::Level::Info;
}

// 关于卡文案。宏缺失回退（手工编译/其他 target）
#ifndef DEVDESK_VERSION
#define DEVDESK_VERSION "dev"
#endif
#ifndef DEVDESK_BUILD_TYPE
#define DEVDESK_BUILD_TYPE "?"
#endif
#ifndef DEVDESK_EUI_NEO_REV
#define DEVDESK_EUI_NEO_REV "unknown"
#endif

std::string aboutVersionLine() {
    // 版本号 = CMake project(VERSION)；__DATE__/__TIME__ 为本 TU 编译时刻。
    return std::string("DevDesk ") + DEVDESK_VERSION + " · " +
           DEVDESK_BUILD_TYPE + " · 构建 " __DATE__ " " __TIME__;
}

std::string aboutEuiNeoLine() {
    // EUI-NEO 同级源码集成，rev 取 configure 期 git rev-parse。
    // 消费政策（ADR 0006）：不锁版本、每次用最新源码——无「最低依赖 rev」；
    // 版本匹配由 scripts/check-eui-neo-version.sh 内容级守卫把关。
    return std::string("EUI-NEO rev ") + DEVDESK_EUI_NEO_REV;
}

SettingsStore::SettingsStore(core::data::SqliteDb* db) : db_(db) {}

const char* SettingsStore::keyNamespace() { return "config.json:"; }

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
    if (out.logLevel != kLogLevelDebug && out.logLevel != kLogLevelWarn &&
        out.logLevel != kLogLevelError) {
        out.logLevel = kLogLevelInfo;
    }
    // 钳制区间与 core::theme::setEditorFontSize 同口径。
    if (out.editorFontSize < kMinEditorFontSize) {
        out.editorFontSize = kMinEditorFontSize;
    }
    if (out.editorFontSize > kMaxEditorFontSize) {
        out.editorFontSize = kMaxEditorFontSize;
    }
    // 越界/0/负数不钳边界、回落默认 8888（0 钳到 1 会得到必然 bind 失败/被特权的端口）。
    if (out.proxyPort < kMinProxyPort || out.proxyPort > kMaxProxyPort) {
        out.proxyPort = kDefaultProxyPort;
    }
    // mcpPort 同口径：越界/0/负回落 8931（不钳边界）。
    if (out.mcpPort < kMinMcpPort || out.mcpPort > kMaxMcpPort) {
        out.mcpPort = kDefaultMcpPort;
    }
    // 只认三档，未知值收敛 auto；不在此解析 auto→darkTheme（保持纯函数）。
    if (out.themeMode != kThemeModeLight && out.themeMode != kThemeModeDark) {
        out.themeMode = kThemeModeAuto;
    }
    // 窗体记忆：负值/越上限归 0 = 无记录（0 是「无记录」哨兵，不能钳到别的合法值——
    // 负值钳 1 会把脏数据变成 1px 窗口记录；巨值由恢复侧收缩链兜底，这里只防落账）
    if (out.windowRestoreWidth < 0 || out.windowRestoreWidth > kMaxWindowRestoreSide) {
        out.windowRestoreWidth = 0;
    }
    if (out.windowRestoreHeight < 0 || out.windowRestoreHeight > kMaxWindowRestoreSide) {
        out.windowRestoreHeight = 0;
    }
    if (out.windowEffect != kWindowEffectTranslucent &&
        out.windowEffect != kWindowEffectFrosted) {
        out.windowEffect = kWindowEffectOff;
    }
    if (out.accentTheme != "clay" && out.accentTheme != "purple" &&
        out.accentTheme != "pink" && out.accentTheme != "blue") {
        out.accentTheme = "clay";
    }
    // 未知 ID 过滤（防手改 config）+ 去重 + 强制补齐恒启用项（home/settings）。
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

// 两层 alpha 串联叠加（窗口背景色层 + clearColor 层），同值推导会让半透/磨砂档视觉无法区分。
// UAT2 B 批（半透档"太高"第一轮）：translucent 0.60/0.90 → 0.80/0.85。UAT3 C1 批
// （round3 用户仍反馈半透透明度太高）：内容 0.80→0.82。UAT6 真机两连（第 4 轮，用户
// 「win下 半透的透明度太高了 看不清内容」）：内容 0.82→0.85、标题栏 0.85 不动（两值
// 拉平），壁纸透出率 18%→15%——正文（textPrimary 叠乘后）极端壁纸最坏 WCAG 对比度
// 两主题 ≥10.6:1（护栏：test_settings settings_window_effect_contrast_guard）。
// 自绘标题栏（D-R3-1）落地后 Windows 无系统标题栏，标题栏 alpha 只实际作用于 macOS。
float windowEffectAlpha(const std::string& mode) {
    if (mode == kWindowEffectTranslucent) return 0.85f;
    if (mode == kWindowEffectFrosted) return 0.15f;
    return 1.0f;  // off / 未知值：实色
}

float windowEffectTitleBarAlpha(const std::string& mode) {
    // 标题栏层 = 窗口背景色，是透出上限（wallpaper 先穿这层再穿 clearColor 纱）。
    if (mode == kWindowEffectTranslucent) return 0.85f;
    if (mode == kWindowEffectFrosted) return 0.65f;
    return 1.0f;
}

float windowEffectThemeOpacity(const std::string& mode) {
    // 有效不透明度 = 1 − (1−标题栏α)(1−内容区α)；对无材质档精确成立，磨砂档不适用。
    const float content = windowEffectAlpha(mode);
    const float bar = windowEffectTitleBarAlpha(mode);
    return 1.0f - (1.0f - bar) * (1.0f - content);
}

core::platform::WindowEffect windowEffectRequest(const std::string& mode) {
    if (mode == kWindowEffectFrosted) {
        // UAT6 真机两连「磨砂直接没半透效果了，底色变成灰色」：Acrylic→Mica。
        // DWMSBT_TRANSIENTWINDOW 是系统给菜单/浮层的 transient 材质，用于常驻主窗
        // 时落进系统的回退策略高发区（最大化/失焦态常见表现 = 材质不渲、窗底成实
        // 色灰——真机症状同形；本机 1920×1080@125% 工作区 1680×860，主窗自 9-27 起
        // 「放不下即启动最大化」恒常驻最大化态，e554acde 又记忆最大化标志）。
        // Mica（DWMSBT_MAINWINDOW）是主窗材质，无 transient 回退类；最大化态窗后
        // 无活物，blur 实时性差异视觉不可辨。M-3 门控桥已把 Mica 映射 "frosted"
        //（main.cpp activeWindowEffectTier），降级 UX 不变；macOS Mica 族挂同一
        // NSVisualEffectView 材质视图（window_effect_macos.mm），不受影响。若换
        // Mica 后真机仍灰底，根因收敛到 EUI-NEO 自绘壳系列（逐像素透明/backdrop
        // 渲染）——见本批报告取证项，非本层请求。
        return core::platform::WindowEffect::Mica;
    }
    // off / translucent（含未知值）：显式关 backdrop，视觉由 clearColor alpha 决定
    return core::platform::WindowEffect::None;
}

std::string windowEffectDegradedHint(const std::string& mode,
                                     core::platform::WindowEffect active) {
    // UAT3 #10（用户拍板删除）：「当前系统不支持窗口半透/磨砂」提示行不再显示——
    // 恒返回空串，设置页外观卡的提示行按 empty 条件渲染整行不建。自动降级行为原样
    // 保留：降级判定在 applyWindowVisuals（按请求档落 clearColor/backdrop）与
    // EUI-NEO 主循环 activeWindowEffect 回写，与提示文案互不依赖。签名保留（调用方
    // settings_ui_cards.cpp 仍按此取行文案；mode/active 照传，空串即不渲染）。
    (void)mode;
    (void)active;
    return {};
}

void applyWindowVisuals(const std::string& mode) {
    // 档位单值先落 core（sidebar/about 的磨砂铺色按档带 alpha 读它；
    // 启动恢复 / 切档 / 主题切换三处都走本漏斗，写这里即全覆盖）
    core::window_effect_state::setCurrent(mode);
    const core::theme::Tokens& theme = core::theme::current();
    const eui::Color& bg = theme.colors.background;
    app::setClearColor(eui::Color{bg.r, bg.g, bg.b, windowEffectAlpha(mode)});
    // 标题栏底：同主题色独立 alpha；显式 customColor 避开 EUI-NEO 按 clearColor 推导的兜底（那条把两处 alpha 绑成同值）。
    const auto channel = [](float v) -> unsigned {
        const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<unsigned>(c * 255.0f + 0.5f);
    };
    core::platform::TitleBarAppearance appearance{theme.isDark};
    appearance.customColor = true;
    appearance.colorAbgr = (channel(bg.b) << 16) | (channel(bg.g) << 8) |
                           channel(bg.r);   // COLORREF 0x00BBGGRR
    appearance.colorAlpha = windowEffectTitleBarAlpha(mode);
    app::setTitleBarAppearance(appearance);
    // backdrop 请求下一帧应用；实际生效值经 activeWindowEffect() 回查、降级由设置页提示。
    app::setWindowEffect(windowEffectRequest(mode));
}

namespace {

// app_config 表里「某顶层键对应的行」：键名 = 命名空间前缀 + 顶层键名。
inline std::string configKey(const char* topKey) {
    return std::string(SettingsStore::keyNamespace()) + topKey;
}

// 按 key 取值；键缺失 → fail(NotFound)。「键缺失」用默认值，「值为空串」按字面生效。
core::Result<std::string> rowValue(
    const std::vector<core::appconfig::Entry>& rows, const std::string& key) {
    for (const auto& r : rows) {
        if (r.key == key) return core::Result<std::string>::ok(r.value);
    }
    return core::Result<std::string>::fail(
        core::ErrorCode::NotFound, "app_config 无此键: " + key);
}

// 行文本 → JsonValue；解析失败报错（不静默当默认值，否则坏写会悄悄换掉用户设置）。
core::Result<platform::json::JsonValue> parseRow(const std::string& key,
                                                  const std::string& text) {
    auto parsed = platform::json::parse(text);
    if (parsed.isFail()) {
        return core::Result<platform::json::JsonValue>::fail(
            parsed.errorCode(), "app_config[" + key + "] 值不是合法 JSON: " +
                                    parsed.errorMessage());
    }
    return parsed;
}

// JsonValue → 行文本。失败（序列化异常）如实报错。
core::Result<std::string> encodeRow(const char* topKey,
                                    const platform::json::JsonValue& v) {
    auto text = platform::json::stringify(v, /*pretty=*/false);
    if (text.isFail()) {
        return core::Result<std::string>::fail(
            text.errorCode(),
            std::string("配置字段序列化失败（") + topKey + "): " +
                text.errorMessage());
    }
    return text;
}

} // namespace

SettingsConfig SettingsStore::load() const {
    auto loaded = loadChecked();
    if (loaded.isFail()) {
        // load 不能返回 Result，故把「读不到」升级成 ERROR 日志：配置已回退默认。
        LOG_ERROR("Settings: 读取应用配置失败，回退默认值（本次改动将无法保存）: " +
                  loaded.errorMessage());
        return sanitized(SettingsConfig{});
    }
    return loaded.value();
}

core::Result<SettingsConfig> SettingsStore::loadChecked() const {
    SettingsConfig cfg;  // 全默认值兜底
    if (db_ == nullptr) {
        return core::Result<SettingsConfig>::fail(
            core::ErrorCode::NotFound,
            "应用数据库未就绪（core::appDatabase() 为空：装配段漏 "
            "registerAppDatabase，或已注销）");
    }
    auto rows = core::appconfig::listWithPrefix(*db_, keyNamespace());
    if (rows.isFail()) {
        return core::Result<SettingsConfig>::fail(
            rows.errorCode(),
            "读取 app_config 失败（表不存在或 SQL 失败）: " + rows.errorMessage());
    }
    const std::vector<core::appconfig::Entry>& all = rows.value();

    const auto readRow = [&](const char* topKey)
        -> core::Result<std::string> {
        return rowValue(all, configKey(topKey));
    };

    if (auto raw = readRow(kKeyTheme); raw.isOk()) {
        auto v = parseRow(configKey(kKeyTheme), raw.value());
        if (v.isFail()) return core::Result<SettingsConfig>::fail(
            v.errorCode(), v.errorMessage());
        if (v.value().isString()) {
            // 只认 "light" 为浅色，其余一律深色（默认值安全）
            cfg.darkTheme = (v.value().asString() != "light");
        }
    }
    if (auto raw = readRow(kKeyThemeMode); raw.isOk()) {
        auto v = parseRow(configKey(kKeyThemeMode), raw.value());
        if (v.isFail()) return core::Result<SettingsConfig>::fail(
            v.errorCode(), v.errorMessage());
        if (v.value().isString()) {
            const std::string mode = v.value().asString();
            if (mode == kThemeModeAuto || mode == kThemeModeLight ||
                mode == kThemeModeDark) {
                cfg.themeMode = mode;
            }
        }
    }
    if (auto raw = readRow(kKeyFontScale); raw.isOk()) {
        auto v = parseRow(configKey(kKeyFontScale), raw.value());
        if (v.isFail()) return core::Result<SettingsConfig>::fail(
            v.errorCode(), v.errorMessage());
        if (v.value().isNumber()) cfg.fontScalePercent = static_cast<int>(v.value().asInt());
    }
    if (auto raw = readRow(kKeyLogRetention); raw.isOk()) {
        auto v = parseRow(configKey(kKeyLogRetention), raw.value());
        if (v.isFail()) return core::Result<SettingsConfig>::fail(
            v.errorCode(), v.errorMessage());
        if (v.value().isNumber()) {
            cfg.logRetentionDays = static_cast<int>(v.value().asInt());
        }
    }
    if (auto raw = readRow(kKeyEditorFontSize); raw.isOk()) {
        auto v = parseRow(configKey(kKeyEditorFontSize), raw.value());
        if (v.isFail()) return core::Result<SettingsConfig>::fail(
            v.errorCode(), v.errorMessage());
        if (v.value().isNumber()) {
            cfg.editorFontSize = static_cast<int>(v.value().asInt());
        }
    }
    if (auto raw = readRow(kKeyLogLevel); raw.isOk()) {
        auto v = parseRow(configKey(kKeyLogLevel), raw.value());
        if (v.isFail()) return core::Result<SettingsConfig>::fail(
            v.errorCode(), v.errorMessage());
        if (v.value().isString()) cfg.logLevel = v.value().asString();
    }
    if (auto raw = readRow(kKeyProxyAutoStart); raw.isOk()) {
        auto v = parseRow(configKey(kKeyProxyAutoStart), raw.value());
        if (v.isFail()) return core::Result<SettingsConfig>::fail(
            v.errorCode(), v.errorMessage());
        if (v.value().isBool()) cfg.claudeProxyAutoStart = v.value().asBool();
    }
    if (auto raw = readRow(kKeyAutostart); raw.isOk()) {
        auto v = parseRow(configKey(kKeyAutostart), raw.value());
        if (v.isFail()) return core::Result<SettingsConfig>::fail(
            v.errorCode(), v.errorMessage());
        if (v.value().isBool()) cfg.autostartEnabled = v.value().asBool();
    }
    if (auto raw = readRow(kKeyFluidBackground); raw.isOk()) {
        auto v = parseRow(configKey(kKeyFluidBackground), raw.value());
        if (v.isFail()) return core::Result<SettingsConfig>::fail(
            v.errorCode(), v.errorMessage());
        if (v.value().isBool()) cfg.fluidBackgroundEnabled = v.value().asBool();
    }
    if (auto raw = readRow(kKeyProxyPort); raw.isOk()) {
        auto v = parseRow(configKey(kKeyProxyPort), raw.value());
        if (v.isFail()) return core::Result<SettingsConfig>::fail(
            v.errorCode(), v.errorMessage());
        if (v.value().isNumber()) cfg.proxyPort = static_cast<int>(v.value().asInt());
    }
    // MCP 端口：键缺失（存量库/旧版本升级）→ 默认 8931；越界由 sanitized 回落。
    if (auto raw = readRow(kKeyMcpPort); raw.isOk()) {
        auto v = parseRow(configKey(kKeyMcpPort), raw.value());
        if (v.isFail()) return core::Result<SettingsConfig>::fail(
            v.errorCode(), v.errorMessage());
        if (v.value().isNumber()) cfg.mcpPort = static_cast<int>(v.value().asInt());
    }
    if (auto raw = readRow(kKeyWindowEffect); raw.isOk()) {
        auto v = parseRow(configKey(kKeyWindowEffect), raw.value());
        if (v.isFail()) return core::Result<SettingsConfig>::fail(
            v.errorCode(), v.errorMessage());
        if (v.value().isString()) cfg.windowEffect = v.value().asString();
    }
    if (auto raw = readRow(kKeySidebarCollapsed); raw.isOk()) {
        auto v = parseRow(configKey(kKeySidebarCollapsed), raw.value());
        if (v.isFail()) return core::Result<SettingsConfig>::fail(
            v.errorCode(), v.errorMessage());
        if (v.value().isBool()) cfg.sidebarCollapsed = v.value().asBool();
    }
    // 窗体记忆（UAT6）：键缺失（首启动/旧版本升级）→ 默认 0/0/false = 无记录
    if (auto raw = readRow(kKeyWindowRestoreWidth); raw.isOk()) {
        auto v = parseRow(configKey(kKeyWindowRestoreWidth), raw.value());
        if (v.isFail()) return core::Result<SettingsConfig>::fail(
            v.errorCode(), v.errorMessage());
        if (v.value().isNumber()) cfg.windowRestoreWidth = static_cast<int>(v.value().asInt());
    }
    if (auto raw = readRow(kKeyWindowRestoreHeight); raw.isOk()) {
        auto v = parseRow(configKey(kKeyWindowRestoreHeight), raw.value());
        if (v.isFail()) return core::Result<SettingsConfig>::fail(
            v.errorCode(), v.errorMessage());
        if (v.value().isNumber()) cfg.windowRestoreHeight = static_cast<int>(v.value().asInt());
    }
    if (auto raw = readRow(kKeyWindowMaximized); raw.isOk()) {
        auto v = parseRow(configKey(kKeyWindowMaximized), raw.value());
        if (v.isFail()) return core::Result<SettingsConfig>::fail(
            v.errorCode(), v.errorMessage());
        if (v.value().isBool()) cfg.windowMaximized = v.value().asBool();
    }
    if (auto raw = readRow(kKeyAccentTheme); raw.isOk()) {
        auto v = parseRow(configKey(kKeyAccentTheme), raw.value());
        if (v.isFail()) return core::Result<SettingsConfig>::fail(
            v.errorCode(), v.errorMessage());
        if (v.value().isString()) cfg.accentTheme = v.value().asString();
    }
    // 字符串数组；未知 ID 忽略 + WARN（防脏数据打死模块）；键缺失 → 默认全启用。
    bool enabledModulesStored = false;
    if (auto raw = readRow(kKeyEnabledModules); raw.isOk()) {
        auto v = parseRow(configKey(kKeyEnabledModules), raw.value());
        if (v.isFail()) return core::Result<SettingsConfig>::fail(
            v.errorCode(), v.errorMessage());
        if (v.value().isArray()) {
            std::vector<std::string> ids;
            for (int i = 0; i < v.value().size(); ++i) {
                platform::json::JsonValue item = v.value()[i];
                if (!item.isString()) continue;
                const std::string id = item.asString();
                if (!isKnownModuleId(id)) {
                    LOG_WARN("Settings: enabledModules 含未知模块 ID \"" + id +
                             "\"，已忽略");
                    continue;
                }
                ids.push_back(id);
            }
            cfg.enabledModules = std::move(ids);
            enabledModulesStored = true;
        }
    }
    // 默认模块清单一次性迁移（B1：存量配置升级后 release 不启用）：
    // 标记缺失/落后 + 存储清单存在 → 并入该版本新增的默认 ID，只回写受影响的
    // 两行（不借 load 重写整份配置）。已到位则原样保留，绝不重开用户关掉的模块。
    int storedDefaultsVersion = 0;
    if (auto raw = readRow(kKeyModuleDefaultsVersion); raw.isOk()) {
        auto v = parseRow(configKey(kKeyModuleDefaultsVersion), raw.value());
        if (v.isFail()) return core::Result<SettingsConfig>::fail(
            v.errorCode(), v.errorMessage());
        if (v.value().isNumber()) {
            storedDefaultsVersion = static_cast<int>(v.value().asInt());
        }
    }
    cfg.moduleDefaultsVersion = std::max(storedDefaultsVersion, kModuleDefaultsVersion);
    if (enabledModulesStored && storedDefaultsVersion < kModuleDefaultsVersion) {
        mergeDefaultModulesSince(cfg.enabledModules, storedDefaultsVersion);
        const auto modsText = encodeRow(
            kKeyEnabledModules, platform::json::makeStringArray(cfg.enabledModules));
        const auto versionText = encodeRow(
            kKeyModuleDefaultsVersion, platform::json::makeInt(kModuleDefaultsVersion));
        if (modsText.isFail() || versionText.isFail()) {
            LOG_WARN("Settings: 模块默认清单迁移文本编码失败（本次会话内存态已生效，"
                     "下次启动重试）");
        } else {
            std::vector<core::appconfig::Entry> entries = {
                {configKey(kKeyEnabledModules), modsText.value(), 0},
                {configKey(kKeyModuleDefaultsVersion), versionText.value(), 0}};
            auto wrote = db_->transaction([&]() -> core::Result<void> {
                return core::appconfig::setMany(*db_, entries);
            });
            if (wrote.isFail()) {
                LOG_WARN("Settings: 模块默认清单迁移回写失败（本次会话内存态已生效，"
                         "下次启动重试）: " + wrote.errorMessage());
            }
        }
    }
    return core::Result<SettingsConfig>::ok(sanitized(cfg));
}

core::Result<void> SettingsStore::save(const SettingsConfig& cfgRaw) const {
    const SettingsConfig cfg = sanitized(cfgRaw);
    if (db_ == nullptr) {
        // 绝不静默成功：调用方刚改的设置会被丢掉，用户却看不到任何报错
        return core::Result<void>::fail(
            core::ErrorCode::NotFound,
            "应用数据库未就绪（core::appDatabase() 为空），配置未保存");
    }

    struct Row { const char* key; platform::json::JsonValue value; };
    const Row rows[] = {
        {kKeyTheme,
         platform::json::makeString(cfg.darkTheme ? "dark" : "light")},
        // themeMode 与 theme 双写：前者 = 用户意图，后者 = 当前解析档（main.cpp 启动路径只认 theme）。
        {kKeyThemeMode, platform::json::makeString(cfg.themeMode)},
        {kKeyEditorFontSize, platform::json::makeInt(cfg.editorFontSize)},
        {kKeyLogLevel, platform::json::makeString(cfg.logLevel)},
        {kKeyFontScale, platform::json::makeInt(cfg.fontScalePercent)},
        {kKeyLogRetention, platform::json::makeInt(cfg.logRetentionDays)},
        {kKeyProxyAutoStart, platform::json::makeBool(cfg.claudeProxyAutoStart)},
        {kKeyAutostart, platform::json::makeBool(cfg.autostartEnabled)},
        {kKeyFluidBackground, platform::json::makeBool(cfg.fluidBackgroundEnabled)},
        {kKeyProxyPort, platform::json::makeInt(cfg.proxyPort)},
        {kKeyMcpPort, platform::json::makeInt(cfg.mcpPort)},
        {kKeyWindowEffect, platform::json::makeString(cfg.windowEffect)},
        {kKeySidebarCollapsed, platform::json::makeBool(cfg.sidebarCollapsed)},
        {kKeyAccentTheme, platform::json::makeString(cfg.accentTheme)},
        {kKeyWindowRestoreWidth, platform::json::makeInt(cfg.windowRestoreWidth)},
        {kKeyWindowRestoreHeight, platform::json::makeInt(cfg.windowRestoreHeight)},
        {kKeyWindowMaximized, platform::json::makeBool(cfg.windowMaximized)},
        {kKeyEnabledModules,
         platform::json::makeStringArray(cfg.enabledModules)},
        {kKeyModuleDefaultsVersion, platform::json::makeInt(cfg.moduleDefaultsVersion)},
    };

    std::vector<core::appconfig::Entry> entries;
    std::vector<std::string> keep;
    entries.reserve(sizeof(rows) / sizeof(rows[0]));
    keep.reserve(sizeof(rows) / sizeof(rows[0]));
    for (const auto& r : rows) {
        auto text = encodeRow(r.key, r.value);
        if (text.isFail()) return core::Result<void>::fail(
            text.errorCode(), text.errorMessage());
        entries.push_back(core::appconfig::Entry{configKey(r.key),
                                                 text.value(), 0});
        keep.push_back(configKey(r.key));
    }

    // 写入与清除非本前缀键放同一个事务：避免中间态被别线程读到。
    return db_->transaction([&]() -> core::Result<void> {
        auto wrote = core::appconfig::setMany(*db_, entries);
        if (wrote.isFail()) return wrote;
        return core::appconfig::eraseWithPrefixExcept(*db_, keyNamespace(), keep);
    });
}

} // namespace module::settings
