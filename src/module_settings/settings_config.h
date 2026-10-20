// src/module_settings/settings_config.h
// 设置页配置模型 + 持久化（SQLite app_config 表）；关键契约：load() 返回值已 sanitized
//（越界钳制 / 未知枚举收敛 / 模块 ID 过滤）；themeMode 是用户意图、darkTheme 是生效档；
// logRetentionDays 0 = 永久。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/platform/window_effect.h"
#include "core/logging.h"
#include "core/proxy_port.h"
#include "core/result.h"
#include "core/sqlite_db.h"

namespace module::settings {

// 编辑器字号范围（px；与 core::theme::setEditorFontSize 钳制区间一致）
constexpr int kDefaultEditorFontSize = 15;
constexpr int kMinEditorFontSize = 10;
constexpr int kMaxEditorFontSize = 32;

constexpr int kMinFontScalePercent = 85;
constexpr int kMaxFontScalePercent = 130;
// 日志保留天数上限（0 = 永久）
constexpr int kMaxLogRetentionDays = 3650;
// Claude 代理端口：取值唯一来源 = core/proxy_port.h（避免各处自定义同值常量）。
constexpr int kDefaultProxyPort = ::core::kDefaultProxyPort;
constexpr int kMinProxyPort = ::core::kMinProxyPort;
constexpr int kMaxProxyPort = ::core::kMaxProxyPort;

// 模块开关 ID：与 pageIdToString 的小写对齐；pet 不是 Router 页面模块，同经门控。
inline constexpr const char* kModuleIdHome = "home";
inline constexpr const char* kModuleIdMarkdown = "markdown";
inline constexpr const char* kModuleIdTodo = "todo";
inline constexpr const char* kModuleIdClaudeProxy = "claude_proxy";
inline constexpr const char* kModuleIdSettings = "settings";
// agent：默认关闭（跑 LLM 有成本，用户主动启用）；开启 = 整页注册进 Router。
inline constexpr const char* kModuleIdAgent = "agent";

// 默认全启用（字段缺失/解析失败回退）
std::vector<std::string> defaultEnabledModules();

// 窗口效果三档（档位字符串直接持久化进 app_config）
inline constexpr const char* kWindowEffectOff = "off";
inline constexpr const char* kWindowEffectTranslucent = "translucent";
inline constexpr const char* kWindowEffectFrosted = "frosted";

// 主题模式三档："auto"（默认，跟随系统）|"light"|"dark"；字符串直接持久化。
inline constexpr const char* kThemeModeAuto = "auto";
inline constexpr const char* kThemeModeLight = "light";
inline constexpr const char* kThemeModeDark = "dark";

// 日志级别四档：大写字符串直接持久化；Trace/Fatal 不开放。
inline constexpr const char* kLogLevelDebug = "DEBUG";
inline constexpr const char* kLogLevelInfo = "INFO";
inline constexpr const char* kLogLevelWarn = "WARN";
inline constexpr const char* kLogLevelError = "ERROR";

struct SettingsConfig {
    bool darkTheme = true;         // 实际生效的深浅档（auto 模式下 = 解析结果）
    // 主题模式："auto"|"light"|"dark"。themeMode 是用户意图、darkTheme 是生效档；
    // save 双写 themeMode + theme（theme 供 main.cpp 启动路径读到上次解析值）。
    std::string themeMode = kThemeModeAuto;
    // Markdown 编辑器字号（px），生效走 core::theme::setEditorFontSize；sanitized 钳制到 [min,max]。
    int editorFontSize = kDefaultEditorFontSize;
    // 日志级别："DEBUG"|"INFO"(默认)|"WARN"|"ERROR"，未知值收敛 INFO。
    std::string logLevel = kLogLevelInfo;
    int fontScalePercent = 100;    // 100 = 设计字号原样（正文 15px token）
    int logRetentionDays = 30;     // 0 = 永久保留
    // false 时 main 只注册单例、不拉起 8888。
    bool claudeProxyAutoStart = false;
    // false 时供应商余量整组功能对用户不可见（首页卡隐藏、配置入口消失、后台刷新不启动）。
    // 默认关闭：用户主动开启才启用余量查询。关闭不删数据库里的配额表/数据/已配供应商引用。
    bool quotaEnabled = false;
    // Claude 代理监听端口：非法值回落默认 8888；运行中改端口自动 stop→start 重启。
    int proxyPort = kDefaultProxyPort;
    // 窗口效果："off"|"translucent"|"frosted"，未知值收敛 off。
    std::string windowEffect = kWindowEffectOff;
    // 强调色："clay"|"purple"|"pink"|"blue"
    std::string accentTheme = "clay";
    // true = 启动恢复 72px 图标栏；运行时切换在 app_sidebar，经 SettingsStore 持久化到这里。
    bool sidebarCollapsed = false;
    // 模块开关：home/settings 恒启用（sanitized 强制补齐）；agent 与 pet 默认关闭。
    std::vector<std::string> enabledModules = defaultEnabledModules();
};

// 模块开关判定（home/settings 恒 true，未知 ID 恒 false）；装配门控与卡片显隐共用。
bool moduleEnabled(const SettingsConfig& cfg, const std::string& moduleId);

// 代理端口输入解析（纯函数）：只接受范围内纯数字，越界/带符号一律 false。
bool parseProxyPortInput(const std::string& text, int& out);

// 主题模式解析（纯函数）：core 不可依赖 platform，系统深浅由 module 层探测后传入；未知 mode 按 auto。
bool resolveThemeDark(const std::string& themeMode, bool systemPrefersDark);

// 日志级别解析（纯函数）：只认四档大写字符串，未知值（含 Trace/Fatal）→ Info。
core::log::Level parseLogLevel(const std::string& mode);

// 关于卡文案（纯函数）：版本/构建信息来自 CMake 注入宏，宏缺失回退 "dev"/"unknown"。
std::string aboutVersionLine();
std::string aboutEuiNeoLine();

// 窗口效果档位 → 参数映射（纯函数，供单测）

// 档位 → clearColor alpha（内容区「主题色纱」）：off=1.0 实色；取值依据见 .cpp。
float windowEffectAlpha(const std::string& mode);

// 档位 → 标题栏底 alpha。与 windowEffectAlpha 分开：两层串联，同 α 喂两层必然劈叉。
float windowEffectTitleBarAlpha(const std::string& mode);

// 无材质档（off/translucent）主题色有效不透明度 = 1−(1−标题栏α)(1−内容区α)，供单测锁目标带；磨砂档不适用。
float windowEffectThemeOpacity(const std::string& mode);

// 档位 → EUI-NEO backdrop 请求：off/translucent 显式关，frosted 请求 Acrylic。
core::platform::WindowEffect windowEffectRequest(const std::string& mode);

// 降级提示文案（activeWindowEffect 回查 vs 所选档；一致返回空串）。
std::string windowEffectDegradedHint(const std::string& mode,
                                     core::platform::WindowEffect active);

// 按档位 + 当前主题应用窗口视觉，启动恢复 / 切档 / 主题切换三处共用。
// 同时把档位写入 core::window_effect_state 单值（sidebar / about 页铺色按档带
// alpha 的读口，UAT2 B 批）。
void applyWindowVisuals(const std::string& mode);

// 应用配置读写（不抛异常）。落点 = SQLite app_config 表，键前缀 "config.json:"，按顶层键一行一个——整份单行会让多个写者互相覆盖。
// load 是一次前缀查询，save 是一事务内 UPSERT + 清掉不再写出的旧键；库里没有键 = 默认值。
class SettingsStore {
public:
    // db：应用共享连接（生产传 ::core::appDatabase()，测试传临时库）；
    // nullptr = 数据库没就绪：save() 返回 fail，load() 回退默认值 + LOG_ERROR。
    explicit SettingsStore(core::data::SqliteDb* db);

    // 键缺失 → 默认值；库未就绪 / 表不存在 → 默认值 + LOG_ERROR。
    SettingsConfig load() const;

    // 与 load() 同源，但把「库未就绪 / 表不存在 / 单键读失败」如实报出（ErrorCode + 原因）。
    core::Result<SettingsConfig> loadChecked() const;

    // 写入配置：11 个顶层键在一个事务里 UPSERT，并清掉本轮不再写出的旧键。
    core::Result<void> save(const SettingsConfig& cfg) const;

    // 数值越界收敛到合法范围（load 时应用；也供单测直接验证）
    static SettingsConfig sanitized(const SettingsConfig& cfg);

    // 键的命名空间前缀（供测试与工具层拼键名；实现与 save/load 同一来源）
    static const char* keyNamespace();

private:
    core::data::SqliteDb* db_ = nullptr;
};

} // namespace module::settings
