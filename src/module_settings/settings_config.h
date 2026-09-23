// src/module_settings/settings_config.h
// P13 设置页：配置模型 + 持久化（spec §8.5 / 任务 T11）
//
// 设计决策：
//   - Config 结构放 module 内部（任务允许"core 或 module 内部皆可，选最小改动"），
//     core 不感知具体设置项
//   - 持久化：core::paths::configFile() + platform::json（yyjson 薄封装）
//   - SettingsStore 的文件路径由构造注入，便于单测用临时文件做读写往返
//
// 字段说明：
//   - theme: "dark" | "light"（默认 dark = 方案 B Dark）
//   - fontScalePercent: 字号缩放百分比，load 时钳制到
//     [kMinFontScalePercent, kMaxFontScalePercent]。
//     生效范围：core::theme::setFontScale 在 token 读取层全局施加——main.cpp
//     启动时恢复、settings_module 切换时调用，作用于全站字号，并非仅设置页自身。
//     （早期"SizeTokens 不支持运行时缩放、该值仅作用于设置页自身、持久化先行"
//     的限制已在 W5 消除，见 KNOWN_LIMITATIONS「已解决」。）
//   - logRetentionDays: 调用日志保留天数，0 = 永久保留
//     集成点：与 w3-t10-home 并行分支的"30 天日志清理任务"联动——
//     T10 的清理逻辑应读取本字段（config.json 的 logRetentionDays），
//     接线留给集成阶段
//   - windowEffect: 窗口效果三档 "off"|"translucent"|"frosted"（磨砂设计
//     Phase D）。档位→参数映射（clearColor alpha / backdrop 请求 / 降级文案）
//     是纯函数，供启动接线（main.cpp）、设置页切换（settings_module）与
//     单测三处共用；EUI-NEO 侧能力见其 docs/DSL.md「窗口效果」
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/platform/window_effect.h"
#include "core/result.h"

namespace module::settings {

// 编辑器字号范围（px；spec 缩水 B；默认 15 = fontSizeBody token 基准，
// 与 core::theme::setEditorFontSize 的钳制区间保持一致）
constexpr int kDefaultEditorFontSize = 15;
constexpr int kMinEditorFontSize = 10;
constexpr int kMaxEditorFontSize = 32;

// 字号缩放范围（%）
constexpr int kMinFontScalePercent = 85;
constexpr int kMaxFontScalePercent = 130;
// 日志保留天数上限（0 = 永久）
constexpr int kMaxLogRetentionDays = 3650;
// Claude 代理端口（总设计 §8.5 第 5 条「数字输入框（默认 8888；改后自动重启
// 代理）」，产品审计 2026-09-23 [14]；范围/默认值与 ClaudeService 的
// k*ProxyPort 常量保持字面一致——模块隔离不互相 include）
constexpr int kDefaultProxyPort = 8888;
constexpr int kMinProxyPort = 1;
constexpr int kMaxProxyPort = 65535;

// ---- 模块开关 ID（模块插拔方案 A，设计 2026-09-22 §5.1）----
// 字符串与 pageIdToString 的小写对齐；config.json 的 enabledModules 数组用。
// pet 是桌宠（2026-09-22-desktop-pet-design.md §4.1）：不是 Router 页面模块
//（无 PageId/侧边栏入口），但同样经 enabledModules 门控（关闭 = 不开窗口零执行）；
// 默认不启用（氛围功能，用户主动开）。
inline constexpr const char* kModuleIdHome = "home";
inline constexpr const char* kModuleIdMarkdown = "markdown";
inline constexpr const char* kModuleIdTodo = "todo";
inline constexpr const char* kModuleIdClaudeProxy = "claude_proxy";
inline constexpr const char* kModuleIdSettings = "settings";
inline constexpr const char* kModuleIdPet = "pet";

// 默认全启用（字段缺失/解析失败回退；home/settings 恒启用见 kAlwaysOn）
std::vector<std::string> defaultEnabledModules();

// 窗口效果三档（磨砂设计 §3.4；档位字符串直接持久化进 config.json）
inline constexpr const char* kWindowEffectOff = "off";
inline constexpr const char* kWindowEffectTranslucent = "translucent";
inline constexpr const char* kWindowEffectFrosted = "frosted";

// 主题模式三档（spec §8.5「跟随系统主题」，spec 缩水任务 A）：
//   auto（默认，跟随系统浅/深，检测见 platform::prefersDarkTheme）
//   light / dark（固定档）。字符串直接持久化进 config.json 的 themeMode 键
inline constexpr const char* kThemeModeAuto = "auto";
inline constexpr const char* kThemeModeLight = "light";
inline constexpr const char* kThemeModeDark = "dark";

struct SettingsConfig {
    bool darkTheme = true;         // 实际生效的深浅档（auto 模式下 = 解析结果）
    // 主题模式（spec §8.5，spec 缩水 A）："auto"（默认，跟随系统）|"light"|"dark"。
    // 与 darkTheme 的关系：darkTheme 恒为「当前应生效的档」，themeMode 是用户
    // 意图；auto 时由调用方经 resolveThemeDark(mode, platform::prefersDarkTheme())
    // 解析（SettingsModule onStart/onPageEnter/切档三处），core 不依赖 platform。
    // 兼容：老 config.json 只有 theme 键 → load 派生为显式 light/dark（不把
    // 存量用户静默切到 auto）；save 双写 themeMode + theme（theme 供未改动的
    // main.cpp 启动路径读到「上次解析值」，settings onStart 随即幂等纠正）。
    std::string themeMode = kThemeModeAuto;
    // Markdown 编辑器字号（px，spec §8.5「编辑器字体大小」，spec 缩水 B）。
    // 生效通道：SettingsModule onStart/切档写 core::theme::setEditorFontSize
    // （值下沉 core，markdown 不 include 本模块，模块隔离不破）；sanitized
    // 钳制 [kMinEditorFontSize, kMaxEditorFontSize]
    int editorFontSize = kDefaultEditorFontSize;
    // W9 默认 100：EUI-NEO 0.7.0 已修字号 CSS em 语义（fix/font-em-size，
    // scalable 字体 fontSize 直接作 em 尺寸），此前 hhea 缩水 ~0.75 不复存在。
    // 存量用户值通过换键 fontScalePercentV3 一次性重置回 100（旧值都是
    // 缩水语义下选的，修正后失去意义）。
    int fontScalePercent = 100;    // 100 = 设计字号原样（正文 15px token）
    int logRetentionDays = 30;     // 0 = 永久保留
    // Claude 代理随应用启动（provider-config Phase B 硬性需求 1：默认 **不**
    // 启动，代理由用户手动启动；false 时 main.cpp 只创建 ClaudeService
    // 注册全局单例，不拉起 127.0.0.1:8888）
    bool claudeProxyAutoStart = false;
    // Claude 代理监听端口（审计 [14]；sanitized 钳制 [kMinProxyPort,
    // kMaxProxyPort]，非法 0/负数/越界一律回落默认 8888）。生效路径：
    // main 启动注入 ClaudeService::setConfiguredProxyPort，设置页改端口时
    // 代理运行中自动 stop→start 重启（§8.5 原话语义）
    int proxyPort = kDefaultProxyPort;
    // 窗口效果（磨砂设计 Phase D）："off"（默认实色）| "translucent"（整体
    // 半透）| "frosted"（Acrylic 磨砂）。sanitized 把未知值收敛为 "off"
    std::string windowEffect = kWindowEffectOff;
    // 侧边栏折叠（2026-09-23 侧边栏折叠/展开）：true = 启动恢复 72px 图标栏，
    // false = 常规 260px。运行时切换在 app_sidebar（按钮/Ctrl+B），经
    // setSidebarCollapsed 走 SettingsStore 持久化到这里
    bool sidebarCollapsed = false;
    // 模块开关（模块插拔方案 A）：可关的是三个工具模块（markdown/todo/
    // claude_proxy）；home 是默认落地页、settings 是开关 UI 宿主，恒启用
    //（sanitized 强制补齐）。缺省 = 全启用，老 config.json 零迁移兼容。
    // pet（桌宠）是已知 ID 但**不在默认清单**——默认关闭，用户在设置页开启
    std::vector<std::string> enabledModules = defaultEnabledModules();
};

// 模块开关判定（sanitized 后的 config 恒有效；home/settings 恒 true，未知
// ID 恒 false）。main.cpp 装配门控与设置页卡片显隐共用这一个谓词
bool moduleEnabled(const SettingsConfig& cfg, const std::string& moduleId);

// ---- 代理端口输入解析（纯函数，供设置页与单测；审计 2026-09-23 [14]）----
// 只接受纯数字且在 [kMinProxyPort, kMaxProxyPort]：空格 / 符号 / 超长 /
// 0 / 越界一律 false。out 仅在 true 时写入
bool parseProxyPortInput(const std::string& text, int& out);

// ---- 代理 token 脱敏（纯函数，供设置页与单测；审计 2026-09-23 [16]）----
// provider-config §7 安全第 2 条：展示默认掩码（前 4 + "***" + 后 4，
// 形如 a3f8***c21e）。短于 8 位整体替换为固定 "****"（不输出任何真实
// 字符、不泄露长度）；空串原样返回（调用方按「代理未运行」分支处理）。
std::string maskProxyToken(const std::string& token);

// ---- 主题模式解析（纯函数，供 SettingsModule 与单测；spec 缩水 A）----
// themeMode + 系统深浅检测 → 实际生效的深色档。core 不可依赖 platform，
// 所以「系统是否深色」由调用方（module 层可依赖 platform）探测后传入。
// 未知 mode 按 auto 语义处理（sanitized 保证正常路径只会是三档之一）
bool resolveThemeDark(const std::string& themeMode, bool systemPrefersDark);

// ---- 窗口效果档位 → 参数映射（纯函数，供单测）----

// 档位 → clearColor alpha：off=1.0（实色）/ translucent≈0.85 / frosted≈0.55
//（Acrylic 材质自身还有一层遮蔽，alpha 偏小才透得出磨砂；观感调参定）
float windowEffectAlpha(const std::string& mode);

// 档位 → EUI-NEO backdrop 请求：off/translucent 显式关 backdrop（视觉由
// clearColor alpha 决定），frosted 请求 Acrylic
core::platform::WindowEffect windowEffectRequest(const std::string& mode);

// 降级提示文案（app::activeWindowEffect 回查 vs 用户所选档；一致返回空串）：
// translucent 拿不到透明 → 提示回退不透明；frosted 拿不到 Acrylic → 按
// 实际值提示回退半透/不透明；off 档任何 active 都视觉等价（alpha=1），不提示
std::string windowEffectDegradedHint(const std::string& mode,
                                     core::platform::WindowEffect active);

// 按档位 + 当前主题应用窗口视觉（clearColor alpha + backdrop 请求），启动
// 恢复 / 设置页切档 / 主题切换三处共用一条路径（EUI-NEO Phase B/C 运行时 API）
void applyWindowVisuals(const std::string& mode);

// config.json 读写（不抛异常；文件不存在/损坏一律回退默认值）
class SettingsStore {
public:
    // file：配置文件完整路径（生产传 core::paths::configFile()，测试传临时文件）
    explicit SettingsStore(std::filesystem::path file);

    // 读取配置；文件不存在 / 解析失败 / 字段缺失 → 对应默认值
    SettingsConfig load() const;

    // 序列化写入 JSON（pretty）；失败返回 fail Result
    core::Result<void> save(const SettingsConfig& cfg) const;

    // 数值越界收敛到合法范围（load 时应用；也供单测直接验证）
    static SettingsConfig sanitized(const SettingsConfig& cfg);

private:
    std::filesystem::path file_;
};

} // namespace module::settings
