// src/module_settings/settings_config.cpp
#include "settings_config.h"

#include "window_effect_hint.h"

#include <algorithm>

#include <fstream>
#include <sstream>
#include <utility>

#include "core/atomic_write.h"
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
// 日志级别四档字符串键（spec 缩水 D；值 = kLogLevel* 常量）
constexpr const char* kKeyLogLevel = "logLevel";
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
    kModuleIdClaudeProxy, kModuleIdSettings, kModuleIdAgent,
    kModuleIdPet};

bool isKnownModuleId(const std::string& id) {
    for (const char* known : kKnownModuleIds) {
        if (id == known) return true;
    }
    return false;
}
} // namespace

std::vector<std::string> defaultEnabledModules() {
    // agent（智能体页，agent M1）**不在默认清单**（final-review Fix 2，
    // 2026-10-01）——spec §8 L4 明文「默认关闭（agent 跑 LLM 有成本，用户
    // 主动启用）」，语义同 pet：用户经设置页模块开关写入 enabledModules 后
    // 生效（sanitized 保留该 ID，重启装配门控按清单注册）
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
    // 日志级别（spec 缩水 D）：只认四档大写字符串，未知值收敛 INFO
    //（与 parseLogLevel 同口径；大小写敏感——配置键值即字面档位）
    if (out.logLevel != kLogLevelDebug && out.logLevel != kLogLevelWarn &&
        out.logLevel != kLogLevelError) {
        out.logLevel = kLogLevelInfo;
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

// 临时标定探针已删（标定方式见 windowEffectAlpha 注释）

// 标定记录（表内数字全部真机实测，算式为事后归纳；两轮口径逐位可对比）。
// 两层结构（**两层 alpha 必须分开**，这是首轮 P1/P2 的共同前提）：
//   窗口背景色（theme × titleAlpha）一层  ← 标题栏整条就是它
//   clearColor（theme × contentAlpha）一层 ← 内容区盖在材质/窗口背景之上
//   · 半透档：windowEffectRequest 返回 None → macOS 侧把材质视图整条摘除
//     （EUI_FX_DEBUG=1 实测 material=0x0）→ 内容区 = clearColor 纱叠窗口背景色
//     = 250 × [1 − (1−titleAlpha)(1−contentAlpha)]，与实测逐位对得上。
//   · 磨砂档：内容区还要过一层 NSVisualEffectView 材质（GL 视图的父视图），
//     内容区 = M + contentAlpha × (250 − M)，M = 材质输出（实测曲线 ≈ 107 + 132×titleAlpha）。
// 首轮旧值为什么错：两处 alpha 由 EUI-NEO 从同一个 clearColor **推导**成同一个值
//（glfw_app_main.cpp applyTitleBarAppearanceToWindow 的兜底），于是
//   · 半透档 0.94 双层同 α → 有效不透明 0.9964 → 内容区 249.1，与关闭档肉眼无差（P1）；
//   · 磨砂档 0.30 时标题栏只有 75（单层）而内容区 180（三层）→ 差 105/255（P2）。
// 首轮 DevDesk 显式给 TitleBarAppearance.customColor（两处 alpha 解耦），并且
// EUI-NEO 补了标题栏条材质（见 window_effect_macos.mm attachMaterialView）。
//
// 2026-09-29 二轮标定（用户第 4 次反馈「半透的透明度再低一点，目前有点看不清内容」）。
// 先做的是**定位「用户感知的透」来自哪一层**，而不是再抬一档常量。真机取证（浅色档 +
// 黑壁纸 + 窗口捕获 `screencapture -l -o`：窗口自身 alpha 通道合成到黑底 = 黑壁纸口径；
// 同法复算关闭档恒 250,249,245、半透档标题栏 225,224,221，与首轮整屏口径逐位同值 →
// 两轮数字可直接对比）：
//   · 内容区自绘面板色**不透明**（alpha 255 = surface #F5F2EC；下拉框底 / tab 底同为
//     255）→ 卡片上的正文从来不随窗口效果变化 ⇒ 候选「面板色其实是透的」不成立
//   · 侧边栏空区 alpha 241 == 内容区页面底色 alpha 241 → 同一层、无第二套 α ⇒ 候选不成立
//   · 半透档 windowEffectRequest=None，材质视图整条摘除 ⇒ 候选「材质层残留」不成立
//   · 唯一劈叉是标题栏 alpha 229（0.90）vs 内容区 241（0.945）——标题栏是全窗最透的
//     一条，但只占 28pt 高 ⇒ 候选「标题栏与内容区不一致」成立、量级有限
//   · 真正承载「透」的是**整窗那层纱**：内容区与侧边栏同为有效 α 0.945（黑壁纸
//     236.3/250），用户视线大面积落在它上面；同时落在纱上的文字（页标题 / tab 文案 /
//     表单标签 / 提示行）对比度同步下降——同一段正文 关闭档 15.96:1 → 半透档 14.10:1
//     （= 关闭档的 88%）。这正是「数值上比 ghostty 0.85 更不透明却仍看不清」的答案：
//     ghostty 的 0.85 是**带 blur(30) 的模糊底**（高频细节被抹平），半透档没有材质，
//     透出来的 5.5% 是**原样清晰**的（叠黑壁纸是整片灰，叠别的窗口能读出背后文字重影）。
// 故本轮两层一起收紧：(1−titleAlpha)(1−contentAlpha) 0.055 → 0.015，落点按判据反解：
//   · 内容区 ≥ 244（黑壁纸口径，向关闭档 250 靠）⇒ (1−ta)(1−ca) ≤ 0.024
//   · 标题栏 − 内容区 ≤ 20 ⇒ 同时抬标题栏 α，顺带收掉「全窗最透的一条」（11.5 → 8.75）
//   · 落在纱上的文字对比度 ≥ 关闭档 95%（实测 15.96:1 → 15.41:1 = 96.6%）
//   · 半透 0.70/0.95：内容区 246.25、侧边栏 246.25、标题栏 237.5（差值算式 8.75；
//     实测 246.0 / 246.0 / 237.0，差 9.0；±0.5 为取样区域差）✓ ≥244
//   · 磨砂 0.15/0.65（**本轮未动**，改动前后窗口捕获逐像素 max|Δ|=0）：内容区 200.7、
//     侧边栏 200.0、标题栏 192（差 8.7）✓（沿用首轮整屏口径——材质是独立视图，
//     窗口捕获不含它）
//   · 关闭档不变：三区恒 250,249,245（alpha=1 实色）
float windowEffectAlpha(const std::string& mode) {
    if (mode == kWindowEffectTranslucent) return 0.60f;
    if (mode == kWindowEffectFrosted) return 0.15f;
    return 1.0f;  // off / 未知值：实色
}

float windowEffectTitleBarAlpha(const std::string& mode) {
    // 标题栏那层的 alpha（= 窗口背景色）**也就是窗口整体的透出上限**：它比
    // windowEffectAlpha 更关键——两层是串联的，wallpaper 要先穿过这层再穿过
    // clearColor 纱，故半透档抬到 0.95（透出量 5%）：单靠 contentAlpha 顶不到
    // 「内容区 ≥ 244」且会让标题栏那条比内容区暗一大截（差 ≤ 20 的判据）。
    // 磨砂档取 0.65 而不是更低：材质输出 M(0.65) ≈ 193 使标题栏与内容区都落在
    //「浅灰、深色文字仍清晰」的区间（黑壁纸实机截图复核，文字可读）；再低
    //（如 0.30 → M=149）标题栏会发暗、观感偏「玻璃」。
    if (mode == kWindowEffectTranslucent) return 0.90f;
    if (mode == kWindowEffectFrosted) return 0.65f;
    return 1.0f;
}

float windowEffectThemeOpacity(const std::string& mode) {
    // 两层同色叠加后主题色的**有效不透明度**：1 − (1−标题栏α)(1−内容区α)。
    // 对无材质的档（off / translucent）精确成立（黑壁纸实测逐位对上），供单测锁住
    // 「≥244 且 <250 目标带」而不是只锁常量值；磨砂档多一层材质，本式不适用（见上注释）。
    const float content = windowEffectAlpha(mode);
    const float bar = windowEffectTitleBarAlpha(mode);
    return 1.0f - (1.0f - bar) * (1.0f - content);
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
            // 降级文案平台化（macos-three-features 设计 §5.3）：磨砂在 Windows
            // 要求 Win11 22H2+、在 macOS 要求 macOS 11+ 系统磨砂——按平台各写各的，
            // 避免在另一平台显示误导性系统要求。平台差异走 window_effect_hint_*.cpp，
            // 业务代码零平台宏（见该头文件）
            return frostedDegradedHint();
        }
        return "当前系统不支持窗口效果，已回退为不透明";
    }
    // off：alpha=1 下任何 active 都视觉等价（透明 hint 开着也不可见）
    return {};
}

void applyWindowVisuals(const std::string& mode) {
    // 底色跟随当前主题 token，alpha 按档位；setClearColor 覆盖主窗口 + 全部
    // 存活子窗口并触发全量重绘（EUI-NEO Phase B），主题切换底色即时跟随
    const core::theme::Tokens& theme = core::theme::current();
    const eui::Color& bg = theme.colors.background;
    app::setClearColor(eui::Color{bg.r, bg.g, bg.b, windowEffectAlpha(mode)});
    // 标题栏底：同主题色、**独立 alpha**（见 windowEffectTitleBarAlpha 注释）。
    // 显式给 customColor → 不再走 EUI-NEO 的「按 clearColor 推导」兜底
    //（那条推导把两处 alpha 绑成同一个值，正是标题栏/内容区劈叉的成因）
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
    if (parsed.isFail() || !parsed.value().isObject()) {
        // M3（2026-09-27）：解析失败仍回落默认（不阻断启动、不引入新失败态），
        // 但先把坏内容另存一份 config.json.bak —— 否则截断 / 手改坏的 config 被
        // 静默换成全默认值（用户关掉的模块"复活"），原始内容无处可查
        std::filesystem::path bak = file_;
        bak += ".bak";
        std::error_code cpEc;
        std::filesystem::copy_file(
            file_, bak, std::filesystem::copy_options::overwrite_existing, cpEc);
        if (cpEc) {
            LOG_WARN("Settings: 配置文件解析失败，使用默认值（备份失败: " +
                     cpEc.message() + "）: " + file_.string());
        } else {
            LOG_WARN("Settings: 配置文件解析失败，使用默认值（原内容已备份: " +
                     bak.string() + "）");
        }
        return cfg;
    }
    platform::json::JsonObject obj = parsed.value().asObject();

    if (obj.has(kKeyTheme) && obj.get(kKeyTheme).isString()) {
        // 只认 "light" 为浅色，其余一律深色（默认值安全）
        cfg.darkTheme = (obj.get(kKeyTheme).asString() != "light");
    }
    // themeMode（spec 缩水 A）：只认 themeMode 键的三档字面值；键缺失/非法 →
    // 回落默认 auto。auto→深浅的实际解析在
    // SettingsModule::applyThemeRuntime（load/sanitized 保持纯函数，
    // 不经 platform，单测机器无关）
    if (obj.has(kKeyThemeMode) && obj.get(kKeyThemeMode).isString()) {
        const std::string mode = obj.get(kKeyThemeMode).asString();
        if (mode == kThemeModeAuto || mode == kThemeModeLight ||
            mode == kThemeModeDark) {
            cfg.themeMode = mode;
        }
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
    if (obj.has(kKeyLogLevel) && obj.get(kKeyLogLevel).isString()) {
        cfg.logLevel = obj.get(kKeyLogLevel).asString();
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
    obj.set(kKeyLogLevel, platform::json::makeString(cfg.logLevel));
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

    // 原子写（tmp → remove → rename，core/atomic_write.h 唯一实现）：
    // 原地 trunc 写在写窗口内崩溃会让 config.json 半截，load 静默回落全默认
    // （被用户关掉的模块"复活"，比"保存失败"更难察觉）
    return core::atomicWriteFile(file_, text.value() + "\n", "配置文件");
}

} // namespace module::settings
