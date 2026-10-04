// src/module_settings/settings_config.h
// 设置页配置模型 + 持久化（SQLite app_config 表 + platform::json）。Config 结构放
// module 内部（core 不感知具体设置项）；SettingsStore 的 DB 连接由构造注入。
//
// 关键契约：load() 返回值已 sanitized（越界钳制 / 未知枚举收敛 / 模块 ID 过滤）；
// themeMode 是用户意图、darkTheme 是当前生效档（auto 由模块层解析，core 不依赖
// platform）；logRetentionDays 0 = 永久；windowEffect 档位→参数映射是纯函数，
// 启动接线 / 设置页 / 单测三处共用。
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

// 编辑器字号范围（px；默认 15 = fontSizeBody token 基准，与
// core::theme::setEditorFontSize 钳制区间一致）
constexpr int kDefaultEditorFontSize = 15;
constexpr int kMinEditorFontSize = 10;
constexpr int kMaxEditorFontSize = 32;

// 字号缩放范围（%）
constexpr int kMinFontScalePercent = 85;
constexpr int kMaxFontScalePercent = 130;
// 日志保留天数上限（0 = 永久）
constexpr int kMaxLogRetentionDays = 3650;
// Claude 代理端口：取值唯一来源 = core/proxy_port.h（避免与 ClaudeService 各自
// 定义同值常量）；默认 8888，改后自动重启代理。
constexpr int kDefaultProxyPort = ::core::kDefaultProxyPort;
constexpr int kMinProxyPort = ::core::kMinProxyPort;
constexpr int kMaxProxyPort = ::core::kMaxProxyPort;

// 模块开关 ID：字符串与 pageIdToString 的小写对齐，app_config 的 enabledModules 键用。
// pet 不是 Router 页面模块（无 PageId/侧边栏入口），同经 enabledModules 门控，默认不启用。
inline constexpr const char* kModuleIdHome = "home";
inline constexpr const char* kModuleIdMarkdown = "markdown";
inline constexpr const char* kModuleIdTodo = "todo";
inline constexpr const char* kModuleIdClaudeProxy = "claude_proxy";
inline constexpr const char* kModuleIdSettings = "settings";
// agent：Router 页面模块，默认关闭（跑 LLM 有成本，用户主动启用；语义同 pet）；
// 开启 = 整页注册进 Router。
inline constexpr const char* kModuleIdAgent = "agent";
inline constexpr const char* kModuleIdPet = "pet";

// 默认全启用（字段缺失/解析失败回退；home/settings 恒启用见 kAlwaysOn）
std::vector<std::string> defaultEnabledModules();

// 窗口效果三档（磨砂设计 §3.4；档位字符串直接持久化进 app_config）
inline constexpr const char* kWindowEffectOff = "off";
inline constexpr const char* kWindowEffectTranslucent = "translucent";
inline constexpr const char* kWindowEffectFrosted = "frosted";

// 主题模式三档："auto"（默认，跟随系统，检测见 platform::prefersDarkTheme）|
// "light"|"dark"；字符串直接持久化进 app_config 的 themeMode 键。
inline constexpr const char* kThemeModeAuto = "auto";
inline constexpr const char* kThemeModeLight = "light";
inline constexpr const char* kThemeModeDark = "dark";

// 日志级别四档：大写字符串直接持久化，与 core::log::levelToString 档位对齐。
// Trace/Fatal 不开放（Trace 在 Debug 下已够详尽，Fatal 作输出级别无意义）。
inline constexpr const char* kLogLevelDebug = "DEBUG";
inline constexpr const char* kLogLevelInfo = "INFO";
inline constexpr const char* kLogLevelWarn = "WARN";
inline constexpr const char* kLogLevelError = "ERROR";

struct SettingsConfig {
    bool darkTheme = true;         // 实际生效的深浅档（auto 模式下 = 解析结果）
    // 主题模式："auto"|"light"|"dark"。darkTheme 恒为当前生效档、themeMode 是用户
    // 意图；auto 时由模块层经 resolveThemeDark 解析（core 不依赖 platform）。
    // save 双写 themeMode + theme（theme 供 main.cpp 启动路径读到上次解析值）。
    std::string themeMode = kThemeModeAuto;
    // Markdown 编辑器字号（px）。生效通道：模块层写 core::theme::setEditorFontSize
    // （值下沉 core，markdown 不 include 本模块）；sanitized 钳制到 [min,max]。
    int editorFontSize = kDefaultEditorFontSize;
    // 日志级别："DEBUG"|"INFO"(默认)|"WARN"|"ERROR"。sanitized 把未知值收敛为
    // INFO；生效路径 = core::log::setLevel(parseLogLevel(...))，即时生效。
    std::string logLevel = kLogLevelInfo;
    // 默认 100：EUI-NEO 0.7.0 已修字号 em 语义，旧缩水语义下的存量值经换键
    // fontScalePercentV3 一次性重置回 100。
    int fontScalePercent = 100;    // 100 = 设计字号原样（正文 15px token）
    int logRetentionDays = 30;     // 0 = 永久保留
    // Claude 代理随应用启动，默认不启动（false 时 main 只注册单例，不拉起 8888）。
    bool claudeProxyAutoStart = false;
    // Claude 代理监听端口：非法值回落默认 8888；生效路径 = main 注入
    // ClaudeService::setConfiguredProxyPort，运行中改端口自动 stop→start 重启。
    int proxyPort = kDefaultProxyPort;
    // 窗口效果："off"（默认实色）|"translucent"（半透）|"frosted"（Acrylic）；未知值收敛 off。
    std::string windowEffect = kWindowEffectOff;
    // 侧边栏折叠：true = 启动恢复 72px 图标栏，false = 常规 260px；运行时切换在
    // app_sidebar（按钮/Ctrl+B），经 SettingsStore 持久化到这里。
    bool sidebarCollapsed = false;
    // 模块开关：可关四个工具模块（markdown/todo/claude_proxy/agent）；home/settings
    // 恒启用（sanitized 强制补齐）。缺省 = 常规工具模块全启用；agent 与 pet 默认关闭。
    std::vector<std::string> enabledModules = defaultEnabledModules();
};

// 模块开关判定（home/settings 恒 true，未知 ID 恒 false）；装配门控与卡片显隐共用。
bool moduleEnabled(const SettingsConfig& cfg, const std::string& moduleId);

// 代理端口输入解析（纯函数）：只接受 [kMinProxyPort, kMaxProxyPort] 内纯数字，
// 空格/符号/超长/0/越界一律 false；out 仅在 true 时写入。
bool parseProxyPortInput(const std::string& text, int& out);

// 主题模式解析（纯函数）：themeMode + 系统深浅检测 → 实际生效的深色档。
// core 不可依赖 platform，故「系统是否深色」由 module 层探测后传入；未知 mode 按 auto。
bool resolveThemeDark(const std::string& themeMode, bool systemPrefersDark);

// 日志级别解析（纯函数）：只认四档大写字符串，未知值（含 Trace/Fatal）→ Info。
// core::log::setLevel 消费结果（onStart 恢复持久化档、切档即时应用）。
core::log::Level parseLogLevel(const std::string& mode);

// 关于卡文案（纯函数）：版本/构建信息来自 CMake 注入宏，宏缺失回退 "dev"/"unknown"。
std::string aboutVersionLine();
std::string aboutEuiNeoLine();

// ---- 窗口效果档位 → 参数映射（纯函数，供单测）----

// 档位 → clearColor alpha（内容区「主题色纱」）：off=1.0 实色；取值与标定证据见 .cpp。
float windowEffectAlpha(const std::string& mode);

// 档位 → 标题栏底 alpha（窗口背景色那层）。必须与 windowEffectAlpha 分开：两层
// 串联，同 α 喂两层必然劈叉（黑壁纸实测差 105/255）。Windows 侧忽略该字段。
float windowEffectTitleBarAlpha(const std::string& mode);

// 无材质档（off/translucent）主题色有效不透明度 = 1−(1−标题栏α)(1−内容区α)；
// 供单测锁「黑壁纸 ≥244 且 <250 目标带」（只锁常量值锁不住目标）。磨砂档不适用。
float windowEffectThemeOpacity(const std::string& mode);

// 档位 → EUI-NEO backdrop 请求：off/translucent 显式关，frosted 请求 Acrylic。
core::platform::WindowEffect windowEffectRequest(const std::string& mode);

// 降级提示文案（activeWindowEffect 回查 vs 所选档；一致返回空串）：translucent
// 拿不到透明 / frosted 拿不到 Acrylic 才提示；off 档视觉等价，不提示。
std::string windowEffectDegradedHint(const std::string& mode,
                                     core::platform::WindowEffect active);

// 按档位 + 当前主题应用窗口视觉，启动恢复 / 切档 / 主题切换三处共用。
void applyWindowVisuals(const std::string& mode);

// 应用配置读写（不抛异常）。
// 落点 = SQLite app_config 表，键命名空间前缀 "config.json:"，其下按顶层键一行一个
// （theme/themeMode/.../enabledModules），value 是该键的 JSON 值文本。
// 按顶层键拆行：多个写者各自只关心一两个键并分别写回，整份单行会互相覆盖；
// load 是一次前缀查询，save 是一事务内 UPSERT + 清掉不再写出的旧键。
// 无迁移、无兼容：不读旧 config.json，库里没有键 = 用默认值。
class SettingsStore {
public:
    // db：应用共享连接（生产传 ::core::appDatabase()，测试传临时库）。允许为
    // nullptr = 数据库没就绪：save() 返回 fail，load() 回退默认值 + LOG_ERROR。
    explicit SettingsStore(core::data::SqliteDb* db);

    // 读取配置；键缺失 → 默认值。库未就绪 / 表不存在 → 默认值 + LOG_ERROR
    //（不能返回 Result，故另配 loadChecked()）。
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
