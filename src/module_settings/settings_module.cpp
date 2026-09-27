// src/module_settings/settings_module.cpp
// P13 设置页模块（spec §8.5 / 任务 T11）
//
// 设置项：
//   1. 主题模式下拉（跟随系统/浅色/深色，spec §8.5「跟随系统主题」，spec
//      缩水 A）——themeMode 存意图、darkTheme 存解析档；auto 经
//      platform::prefersDarkTheme() 在 applyThemeRuntime 解析（启动/回本页/
//      切档三处），core::theme::setDark/setLight，compose 每帧读 current()，
//      切换后下一帧全页生效；窗口底色（clearColor）走 EUI-NEO 运行时 API
//      app::setClearColor（磨砂 Phase B），主题/窗口效果切换即时生效
//      （main.cpp theme 回调同款路径）
//   1b. 窗口效果三档（关闭/半透/磨砂，磨砂 Phase D）——applyWindowVisuals
//      （clearColor alpha 按档 + setWindowEffect backdrop 请求）即时生效；
//      activeWindowEffect 回查降级，行内提示（不支持磨砂 → 回退半透等）
//   2. 字号缩放——core::theme::setFontScale 全局生效：token 层缩放让所有
//      经 t.sizes.fontSize* 取字号的模块零改动受益；本页非 token 的硬编码
//      小字号经 fs() 辅助函数跟随同一缩放
//   3. Claude 代理：随应用启动开关（Phase B 硬性需求 1，默认关）+ 代理端口
//      数字输入（总设计 §8.5 第 5 条，审计 [14]：默认 8888，改后运行中自动
//      重启）+ Claude Code 接管状态（Phase D UI 接入，数据源 ClaudeService）。
//      代理仅监听 127.0.0.1、无鉴权，无 token 展示/生成（2026-09-27 移除）
//   4. Markdown 笔记目录只读展示（改目录属于范围外，仅展示）+ 编辑器字号
//      档位（spec §8.5「编辑器字体大小」，spec 缩水 B；Task B tab 化后收在
//      「通用」tab 的编辑器字号卡——原宿主日志卡行 2 的 z 逃逸理由已随
//      分 tab 消失：不同 tab 的卡不再同屏叠放）。值经
//      core::theme::editorFontSize 全局通道传 markdown（模块隔离）
//   5. 调用日志保留天数（0=永久）——切换档位立即执行一次滚动清理并
//      反馈「已清理 N 条」（core::pruneCallLogs，经 appDatabase() 共享
//      连接；启动时 main.cpp 仍会按持久化配置再清一次，幂等）
//   5b. 日志级别四档（spec §8.5「日志级别」，spec 缩水 D）——切档即时
//      core::log::setLevel（下一行日志起生效）；重启由 onStart 恢复
//   6. 模块管理（模块插拔方案 A，设计 2026-09-22 §5.4）：三个工具模块
//      （markdown/todo/claude_proxy）各一行开关；home/settings 恒启用不
//      展示开关。开关只写 config.json，**重启生效**（注册是启动期行为）；
//      对应模块关闭时其专属卡片整卡隐藏（代理卡/目录卡，不显示故障态）
//   7. 关于卡（spec §8.5「关于」，spec 缩水 C）：DevDesk 版本/构建类型/
//      构建时间戳 + EUI-NEO rev（CMake 注入宏）+ 配置/日志文件路径，恒显
//   8. 页面结构（Task B tab 化）：通用 / 模块 / 日志 / 关于 四 tab
//      （components::tabs）；「日志」tab 含日志查看卡（core::paths::logFile()
//      末尾 500 行 tail + 刷新按钮，读盘在 compose 顶部一次性完成）
//
// 入口说明：设置入口在 main.cpp 的常驻侧边栏（app.sidebar，spec §2/§6.5.2），
// 与其他四个模块同列。
//
// 依赖说明：module_* 之间原则上不互相依赖（防循环）。claude 侧（审计
// [5][36] 修复后）settings 不再 link devdesk_module_claude_proxy，仅经
// claude_service.h（CMake PRIVATE include 目录 ../module_claude_proxy）读取
// main.cpp 注册的 ClaudeService::global() 单例——头声明 + 最终可执行文件
// 解析符号，无 CMake 依赖边。pet 侧（同审计项）不 include、不 link：
// 桌宠卡全部调用（discoverSkins/applyPetSkin/resetPetPosition/
// currentSkinName）走下方 extern
// 自由函数桥，实现在 pet 模块内。两条边均单向（claude/pet 不反向依赖
// settings），无环；桌宠非 Router 页面模块，设计 2026-09-22 §4.7 接线表
// 明确设置页承载桌宠卡。
#include "settings_config.h"

#include "claude_service.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/theme.h"
#include "core/component_theme.h"
#include "platform/file_open.h"
#include "platform/system_theme.h"

// ---- 审计 [5][36] 解耦桥：module_pet 自由函数声明 ----
// 声明已收口到中立桥头 module_pet_bridge.h（src 根、应用层，审计 M1 短期
// 项）：settings 仍不 include module_pet 任何头、CMake 不 link
// devdesk_module_pet——符号在最终可执行文件（DevDesk / devdesk_tests 均直接
// 链接全部 module_*）链接期解析。只读查询 / 改对方状态两组边界与签名漂移
// static_assert 见该头。
#include "module_pet_bridge.h"

#include <eui/app.h>
#include <eui/dsl.h>

#include <components/button.h>
#include <components/dialog.h>
#include <components/dropdown.h>
#include <components/input.h>
#include <components/scrollview.h>
#include <components/switch.h>
#include <components/tabs.h>
#include <components/theme.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace module::settings {

namespace {

// 字号缩放档位（%）
constexpr int kFontPresets[] = {85, 100, 115, 130};
constexpr int kFontPresetCount = 4;
const char* kFontPresetLabels[] = {"小 85%", "标准 100%", "大 115%", "特大 130%"};

// 日志保留档位（天；0 = 永久）
constexpr int kRetentionPresets[] = {0, 7, 30, 90, 180, 365};
constexpr int kRetentionPresetCount = 6;

// 窗口效果档位（磨砂 Phase D；与 settings_config 的三档字符串一一对应）
const char* kWindowEffectPresets[] = {
    kWindowEffectOff, kWindowEffectTranslucent, kWindowEffectFrosted};
constexpr int kWindowEffectPresetCount = 3;
const char* kWindowEffectPresetLabels[] = {"关闭（默认）", "半透", "磨砂（Acrylic）"};

// 主题模式三档（spec §8.5「跟随系统主题」，spec 缩水 A；与 settings_config
// 的 kThemeMode* 一一对应）
const char* kThemeModePresets[] = {kThemeModeAuto, kThemeModeLight,
                                   kThemeModeDark};
constexpr int kThemeModePresetCount = 3;
const char* kThemeModePresetLabels[] = {"跟随系统", "浅色", "深色"};

// 编辑器字号档位（px；spec 缩水 B；15 = fontSizeBody token 基准 = 旧硬编值）
constexpr int kEditorFontPresets[] = {13, 15, 17, 20};
constexpr int kEditorFontPresetCount = 4;
const char* kEditorFontPresetLabels[] = {
    "小 13px", "标准 15px", "大 17px", "特大 20px"};

// 日志级别四档（spec 缩水 D；与 settings_config 的 kLogLevel* 一一对应，
// 核心档用裸大写词 = 与 core::log::levelToString 输出对齐）
const char* kLogLevelPresets[] = {kLogLevelDebug, kLogLevelInfo,
                                  kLogLevelWarn, kLogLevelError};
constexpr int kLogLevelPresetCount = 4;
const char* kLogLevelPresetLabels[] = {
    "DEBUG（详尽）", "INFO（默认）", "WARN", "ERROR"};

// —— 日志保留下拉 openUp 越界判定几何（tech-debt A，原为裸魔数）——
// 日志卡是页面最后一张卡，下拉向下展开可能被窗体底边裁剪；这里用同一套
// 几何预先判断"向下展开是否出界"，出界则置 openUp 让 dropdown 向上展开。
// 弹层三项必须与 EUI-NEO dropdown 实际渲染一致，否则判定失真：取值来源是
// EUI-NEO components/theme.h 的默认 metrics（control.menuItem=34 /
// spacing.compact=8 / spacing.small=6），若将来主题覆写 metrics 需同步。
constexpr float kLogDropdownOffsetY = 48.0f;    // ddwrap 在日志卡内 y 偏移
constexpr float kLogDropdownHeight = 40.0f;     // dropdown 控件高度
constexpr float kEditorFontDdOffsetY = 42.0f;   // 编辑器字号卡内 dropdown y（Task B 独立卡）
constexpr float kLogEditorFontHeight = 36.0f;   // 编辑器字号 dropdown 高
constexpr float kLogLevelOffsetY = 92.0f;       // 日志级别行 y 偏移（spec 缩水 D；编辑器字号行迁出通用 tab 后上移）
constexpr float kLogLevelHeight = 36.0f;        // 日志级别 dropdown 高
constexpr float kDropdownPopupGap = 8.0f;       // 字段底边与弹层间距（spacing.compact）
constexpr float kDropdownItemHeight = 34.0f;    // 单个菜单项高（control.menuItem）
constexpr float kDropdownPopupPadding = 12.0f;  // 弹层上下 padding（spacing.small × 2）
constexpr float kWindowBottomMargin = 16.0f;    // 窗口底边安全留白，越界 = 有裁剪风险

// ---- 设置页 4 tab（Task B：页面太长拆 tab + 日志查看）----
// 通用 = 外观 + 编辑器字号；模块 = 插拔开关 + 桌宠 + 代理 + 目录；
// 日志 = 保留天数 + 级别 + tail 查看；关于 = 版本/路径
const char* kSettingTabLabels[] = {"通用", "模块", "日志", "关于"};
constexpr int kSettingTabCount = 4;
constexpr int kTabGeneral = 0;
constexpr int kTabModules = 1;
constexpr int kTabLog = 2;
constexpr int kTabAbout = 3;
constexpr float kSettingsTabHeight = 40.0f;       // tab 栏高
constexpr float kLogSettingsCardHeight = 148.0f;  // 日志卡（编辑器字号迁出后收窄）
constexpr float kEditorFontCardHeight = 108.0f;   // 编辑器字号卡（通用 tab）
constexpr int kLogTailMaxLines = 500;             // 日志查看 tail 行数上限
constexpr long kLogTailReadWindow = 256 * 1024;   // tail 只读末尾 256KB，防大文件卡顿

int nearestIndex(int value, const int* presets, int count) {
    int best = 0;
    int bestDist = 1 << 30;
    for (int i = 0; i < count; ++i) {
        const int d = std::abs(presets[i] - value);
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

} // namespace

class SettingsModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::Settings; }
    std::string displayName() const override { return "设置"; }
    std::string iconGlyph() const override { return eui::utf8(0xF013); }  // fa-gear

    // ============ 生命周期 ============

    void onStart() override {
        store_ = std::make_unique<SettingsStore>(::core::paths::configFile());
        cfg_ = store_->load();
        // 启动即应用持久化主题 + 字号缩放（main.cpp 的 dslAppConfig 也做过
        // 同样恢复——这里幂等重放，保证模块独立使用时行为一致）。主题走
        // applyThemeRuntime：themeMode=auto 时按 platform 检测结果解析深浅
        // （main.cpp 只认持久化的 theme=上次解析值，这里随即纠正到当前系统档）
        applyThemeRuntime();
        ::core::theme::setFontScale(
            static_cast<float>(cfg_.fontScalePercent) / 100.0f);
        // 编辑器字号通道（spec 缩水 B）：值存 SettingsConfig，运行时经
        // core::theme 传给 markdown（模块隔离：markdown 不 include settings）
        ::core::theme::setEditorFontSize(
            static_cast<float>(cfg_.editorFontSize));
        // 日志级别档（spec 缩水 D）：core::log::setLevel 已存在，这里只是
        // 把持久化档应用到运行时（main.cpp 启动日志先于本模块，恒定按
        // 默认 INFO 过滤——DEBUG 档从 settings 启动起生效）
        ::core::log::setLevel(parseLogLevel(cfg_.logLevel));
        LOG_INFO("Settings 模块启动（主题=" + ::core::theme::currentThemeName() +
                 "（模式=" + cfg_.themeMode + "）" +
                 ", 字号=" + std::to_string(cfg_.fontScalePercent) + "%" +
                 ", 编辑器字号=" + std::to_string(cfg_.editorFontSize) + "px" +
                 ", 日志级别=" + cfg_.logLevel +
                 ", 日志保留=" + retentionLabel() + "）");
    }

    void onStop() override {
        store_.reset();
        LOG_INFO("Settings 模块停止");
    }

    // 重新进入设置页（EUI-NEO 无系统主题变更回调，auto 模式回到本页重新解析）
    void onPageEnter() override {
        // auto 模式的跟随点：系统深浅在运行中变更后，回到本页即重新解析
        //（EUI-NEO 无系统主题变更回调，无法做到任意页面实时跟随——已登记
        // 为限制；切档/启动两处的解析见 applyThemeRuntime）
        if (cfg_.themeMode == kThemeModeAuto) {
            applyThemeRuntime();
        }
    }

    // 产品审计 [22]：Ctrl+S 显式保存。设置页除端口草稿外全部即时落盘
    //（开关/下拉一改即 persist()），故 Ctrl+S 语义 = 提交唯一待决变更：
    //   - 端口草稿非空且 ≠ 已应用端口 → 走 applyProxyPort()（=「应用」按钮，
    //     含代理运行中自动重启保 token 的完整语义）；
    //   - 否则重写当前配置落盘 + 状态行回显「已保存」（给用户明确的保存反馈，
    //     与 markdown 编辑器 Ctrl+S 的心智一致）。
    // 「重新生成 Token」确认弹窗打开时不响应（防打字/确认中途产生副作用）。
    // 通道 = app 级 onKeyEvent → Router::dispatchKeyEvent（同 Todo/Markdown
    // 先例；EUI-NEO 无全局 OS hotkey，窗口聚焦时生效，已登记限制）。
    bool onKeyEvent(const eui::KeyEvent& key) override {
        if (key.action != eui::KeyAction::Press) {
            return false;
        }
        if (!key.modifiers.shortcut() || key.modifiers.alt) {
            return false;
        }
        if (key.key != eui::InputKey::S) {
            return false;
        }
        if (!proxyPortDraft_.empty() &&
            proxyPortDraft_ != std::to_string(cfg_.proxyPort)) {
            applyProxyPort();
            return true;
        }
        persist();
        setStatus("设置已保存（本页变更本就即时持久化）");
        LOG_INFO("Settings: Ctrl+S 显式保存");
        return true;
    }

    // ============ 设置变更 ============

    // 按 themeMode 解析并应用深浅（spec 缩水 A）。系统检测经
    // platform::prefersDarkTheme()（module 层可依赖 platform；core 不可，
    // 故解析在此而不在 core::theme）。解析结果同步写回 cfg_.darkTheme，
    // persist 时以 "theme" 键落盘，供 main.cpp 下次启动直读
    void applyThemeRuntime() {
        const bool dark =
            resolveThemeDark(cfg_.themeMode, platform::prefersDarkTheme());
        if (dark) {
            ::core::theme::setDark();
        } else {
            ::core::theme::setLight();
        }
        cfg_.darkTheme = dark;
    }

    // cfg_.themeMode（三档字符串）→ 下拉索引；sanitized 保证只会是三档之一
    int themeModeIndex() const {
        if (cfg_.themeMode == kThemeModeLight) return 1;
        if (cfg_.themeMode == kThemeModeDark) return 2;
        return 0;  // auto
    }

    void applyThemeMode(int index) {
        if (index < 0 || index >= kThemeModePresetCount) return;
        cfg_.themeMode = kThemeModePresets[index];
        applyThemeRuntime();
        persist();
        setStatus(std::string("主题模式：") + kThemeModePresetLabels[index] +
                  "（当前=" + (::core::theme::current().isDark ? "深色" : "浅色") +
                  "）");
        LOG_INFO("Settings: 主题模式 → " + cfg_.themeMode + "（生效=" +
                 ::core::theme::currentThemeName() + "）");
    }

    void applyFontPreset(int index) {
        if (index < 0 || index >= kFontPresetCount) return;
        cfg_.fontScalePercent = kFontPresets[index];
        // 写入 core::theme 全局缩放：token 读取层生效，所有模块零改动受益
        ::core::theme::setFontScale(
            static_cast<float>(cfg_.fontScalePercent) / 100.0f);
        persist();
        setStatus("字号缩放：" + std::to_string(cfg_.fontScalePercent) +
                  "%（下一帧全局生效）");
        LOG_INFO("Settings: 字号缩放 → " + std::to_string(cfg_.fontScalePercent) +
                 "%（core::theme token 层全局缩放）");
    }

    // 编辑器字号档位切换（spec 缩水 B）：写 core::theme 全局通道即时生效
    //（markdown compose 每帧读 editorFontSize()），persist 供下次启动恢复
    void applyEditorFontPreset(int index) {
        if (index < 0 || index >= kEditorFontPresetCount) return;
        cfg_.editorFontSize = kEditorFontPresets[index];
        ::core::theme::setEditorFontSize(
            static_cast<float>(cfg_.editorFontSize));
        persist();
        setStatus("编辑器字号：" + std::to_string(cfg_.editorFontSize) +
                  "px（Markdown 编辑器下一帧生效）");
        LOG_INFO("Settings: 编辑器字号 → " + std::to_string(cfg_.editorFontSize) +
                 "px（core::theme 全局通道）");
    }

    // 窗口效果三档切换（磨砂 Phase D）：clearColor alpha + backdrop 请求即时
    // 生效（EUI-NEO 运行时 API，下一帧应用）；系统不支持时 compose 每帧回查
    // activeWindowEffect 显示降级提示行（不需要 toast——提示随档位常驻更稳）
    void applyWindowEffectPreset(int index) {
        if (index < 0 || index >= kWindowEffectPresetCount) return;
        cfg_.windowEffect = kWindowEffectPresets[index];
        persist();
        applyWindowVisuals(cfg_.windowEffect);
        setStatus(std::string("窗口效果：") + kWindowEffectPresetLabels[index] +
                  "（即时生效）");
        LOG_INFO("Settings: 窗口效果 → " + cfg_.windowEffect);
    }

    void applyRetentionPreset(int index) {
        if (index < 0 || index >= kRetentionPresetCount) return;
        cfg_.logRetentionDays = kRetentionPresets[index];
        persist();
        // 0 = 永久：跳过清理；否则切档立即执行一次滚动清理并反馈条数
        //（此前只在启动时读档清理一次，改档要重启才生效）
        if (cfg_.logRetentionDays == 0) {
            setStatus("日志保留：永久保留（不清理历史日志）");
            LOG_INFO("Settings: 日志保留 → 永久保留（跳过清理）");
            return;
        }
        auto* db = ::core::appDatabase();
        if (db == nullptr) {
            setStatus("日志保留：" + retentionLabel() +
                      "（数据库未就绪，重启后生效）");
            LOG_WARN("Settings: appDatabase 未注册，跳过立即清理");
            return;
        }
        auto pruned = ::core::pruneCallLogs(*db, cfg_.logRetentionDays);
        if (pruned.isOk()) {
            setStatus("日志保留：" + retentionLabel() + "，已清理 " +
                      std::to_string(pruned.unsafeValue()) + " 条历史日志");
            LOG_INFO("Settings: 日志保留 → " + retentionLabel() + "，已清理 " +
                     std::to_string(pruned.unsafeValue()) + " 条");
        } else {
            setStatus("日志保留：" + retentionLabel() +
                      "（清理失败：" + pruned.errorMessage() + "）", true);
            LOG_ERROR("Settings: 立即清理失败: " + pruned.errorMessage());
        }
    }

    // 请求重新生成 token：代理未运行 → 不可生成（token 随 startProxy 生成，
    // 没有「预生成」概念，也不允许顺带把代理拉起来——Phase B 修的怪行为）；
    // 运行中 → 先弹确认（一键作废在用 token）
    // 「随应用启动代理」开关（provider-config Phase B 硬性需求 1）
    void applyAutoStart(bool on) {
        cfg_.claudeProxyAutoStart = on;
        persist();
        setStatus(on ? "代理将随应用启动（下次启动生效）"
                     : "代理不再随应用启动（下次启动生效）");
        LOG_INFO("Settings: claudeProxyAutoStart → " +
                 std::string(on ? "true" : "false"));
    }

    // 应用代理端口（总设计 §8.5 第 5 条，审计 2026-09-23 [14]）：
    // 解析 draft → 写 config.json → 同步 ClaudeService::configuredProxyPort
    //（Claude 页手动启动 / main 自启读它）→ 运行中自动 stop→start 重启。
    // 重启失败：代理已停、settings.json 已被 stopProxy 还原（不会指向死端口），
    // 状态栏提示可换端口重试。
    void applyProxyPort() {
        int port = 0;
        if (!parseProxyPortInput(proxyPortDraft_, port)) {
            setStatus("端口无效：请输入 " + std::to_string(kMinProxyPort) +
                          " - " + std::to_string(kMaxProxyPort) + " 的纯数字",
                      true);
            return;
        }
        if (port == cfg_.proxyPort) {
            setStatus("代理端口未变化（" + std::to_string(port) + "）");
            return;
        }
        const int oldPort = cfg_.proxyPort;
        cfg_.proxyPort = port;
        persist();
        proxyPortDraft_ = std::to_string(port);   // 规范化显示（"08888" → "8888"）

        auto* svc = ::module::claude::ClaudeService::global();
        if (svc) {
            svc->setConfiguredProxyPort(port);
        }
        if (!svc || !svc->isProxyRunning()) {
            setStatus("代理端口已保存：" + std::to_string(oldPort) + " → " +
                      std::to_string(port) + "（下次启动代理生效）");
            LOG_INFO("Settings: 代理端口 " + std::to_string(oldPort) + " → " +
                     std::to_string(port) + "（代理未运行，下次启动生效）");
            return;
        }
        svc->stopProxy();
        auto r = svc->startProxy("127.0.0.1", port);
        if (r.isFail()) {
            setStatus("端口已保存为 " + std::to_string(port) +
                          "，但代理重启失败（" + r.errorMessage() +
                          "）；代理已停止，可换端口或释放占用后在 Claude 页重启",
                      true);
            LOG_ERROR("Settings: 代理端口改 " + std::to_string(port) +
                      " 重启失败: " + r.errorMessage());
            return;
        }
        setStatus("代理端口已改为 " + std::to_string(port) + " 并自动重启");
        LOG_INFO("Settings: 代理端口 " + std::to_string(oldPort) + " → " +
                 std::to_string(port) + "，代理已自动重启");
    }

    // 日志级别切档（spec 缩水 D）：core::log::setLevel 即时应用（下一行日志
    // 起生效，无缓冲），persist 供下次启动由 onStart 恢复
    void applyLogLevelPreset(int index) {
        if (index < 0 || index >= kLogLevelPresetCount) return;
        cfg_.logLevel = kLogLevelPresets[index];
        ::core::log::setLevel(parseLogLevel(cfg_.logLevel));
        persist();
        setStatus("日志级别：" + cfg_.logLevel + "（即时生效）");
        LOG_INFO("Settings: 日志级别 → " + cfg_.logLevel + "（core::log::setLevel）");
    }

    // cfg_.logLevel（四档字符串）→ 下拉索引；sanitized 保证只会是四档之一
    int logLevelIndex() const {
        if (cfg_.logLevel == kLogLevelDebug) return 0;
        if (cfg_.logLevel == kLogLevelWarn) return 2;
        if (cfg_.logLevel == kLogLevelError) return 3;
        return 1;  // INFO 默认档
    }

    // 模块开关（插拔方案 A §5.4）：只写 config.json，注册门控在下次启动的
    // main.cpp 装配处生效。home/settings 恒启用，UI 不提供开关
    void applyModuleEnabled(const std::string& moduleId, bool on) {
        auto& v = cfg_.enabledModules;
        auto it = std::find(v.begin(), v.end(), moduleId);
        if (on && it == v.end()) {
            v.push_back(moduleId);
        } else if (!on && it != v.end()) {
            v.erase(it);
        } else {
            return;  // 状态没变（重复点击），不写盘
        }
        persist();
        setStatus((on ? "模块已开启" : "模块已关闭") +
                  std::string("，重启 DevDesk 后生效"));
        LOG_INFO("Settings: 模块开关 " + moduleId + " → " +
                 (on ? "启用" : "关闭") + "（重启生效）");
    }

    void persist() {
        if (!store_) return;
        auto r = store_->save(cfg_);
        if (r.isFail()) {
            setStatus("配置保存失败：" + r.errorMessage(), true);
            LOG_ERROR("Settings: 配置保存失败: " + r.errorMessage());
        }
    }

    void setStatus(const std::string& msg, bool isError = false) {
        statusMessage_ = msg;
        statusIsError_ = isError;
    }

    // ============ UI ============

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        const auto& t = core::theme::current();
        const float pad = t.sizes.spacingXXLarge;  // 40
        const float contentW = screen.width - pad * 2.0f;

        // 页面底色 = 主窗口 clearColor（随主题/窗口效果档运行时跟随，
        // 磨砂 Phase B/D）——不自铺不透明底，半透/磨砂档主体才生效

        const float headerH = 64.0f;
        const float tabH = kSettingsTabHeight;
        // 日志 tail 只在「切到日志 tab / 点刷新」后读一次（读盘进 compose 顶部
        // 而非 scrollView content lambda：后者被 measure/paint 多次调用）
        if (settingsTab_ == kTabLog && logTailDirty_) {
            readLogTail();
            logTailDirty_ = false;
        }
        // contentKey 带 proxyH：警告行数变化时滚动高度缓存失效重测
        auto* svc = ::module::claude::ClaudeService::global();
        const bool managed = svc && svc->isClaudeCodeManaged();
        size_t warnLines = 0;
        if (managed && svc) {
            warnLines = std::min<size_t>(
                svc->lastSyncWarnings().size(), 2);
        }
        // 代理卡高 = 内容底沿 + 16px 卡内下边距（与本 tab 其它卡一致）。内容底沿：
        // 接管状态行 98+18=116；有警告时改为警告行区底（118 + 16×行数，首行与
        // 上一行留 2px）。旧公式 174+28+(managed?20:0) 的基数是「鼠标穿透」行等
        // 已移除内容的残留，卡底留 50~100px 空白
        const float proxyBottom = warnLines > 0
            ? 118.0f + static_cast<float>(warnLines) * 16.0f
            : 116.0f;
        const float proxyH = proxyBottom + 16.0f;
        // 模块插拔门控（§5.4）：claude 关 → 代理卡整卡隐藏；markdown
        // 关 → 目录卡整卡隐藏（不显示故障态）。读 cfg_（= 启动时注册
        // 状态的来源），拨动开关即时预览下次启动的卡片布局
        const bool claudeOn =
            moduleEnabled(cfg_, kModuleIdClaudeProxy);
        const bool mdOn = moduleEnabled(cfg_, kModuleIdMarkdown);
        const bool petOn = moduleEnabled(cfg_, kModuleIdPet);
        const auto compTheme = core::theme::componentTokens(t);

        ui.column("settings.root")
            .size(screen.width, screen.height)
            .padding(pad)
            .gap(t.sizes.spacingLarge)
            .content([&] {
                drawHeader(ui, contentW, headerH, t);

                // tab 栏（Task B：长页拆 4 tab）。components::tabs 无
                // .position → 套定位 stack（仿 claude detail tabs.wrap 写法；
                // 本列是 column 流式布局，stack 自然排在 header 下方）
                ui.stack("settings.tabs.wrap")
                    .size(contentW, tabH)
                    .content([&] {
                        components::tabs(ui, "settings.tabs")
                            .size(contentW, tabH)
                            .items(std::vector<std::string>(
                                kSettingTabLabels,
                                kSettingTabLabels + kSettingTabCount))
                            .selected(settingsTab_)
                            .fontSize(t.sizes.fontSizeSmall)
                            .theme(compTheme)
                            .onChange([this](int idx) {
                                settingsTab_ = idx;
                                // 切到日志 tab：下一帧 compose 顶部重读 tail
                                if (idx == kTabLog) logTailDirty_ = true;
                            })
                            .build();
                    })
                    .build();

                // 当前 tab 的卡区仍套 scrollView（W10 复审项结论在模块 tab
                // 依旧成立：4+ 卡总高可超小窗口可视高）
                const float cardsTopY = pad + headerH +
                    t.sizes.spacingLarge + tabH + t.sizes.spacingLarge;
                const float cardsH = screen.height - cardsTopY - pad;
                components::scrollView(ui, "settings.cards.scroll")
                    .size(contentW, cardsH)
                    .gap(t.sizes.spacingLarge)
                    .step(60.0f)
                    .theme(compTheme)
                    // contentKey 带 tab 索引 + 代理卡高 + 卡片显隐签名：切
                    // tab / 卡片增减时滚动高度缓存失效重测（不带 tab 索引会
                    // 沿用上 tab 的内容高 → 滚动范围错）
                    .contentKey("settings.cards.t" +
                                std::to_string(settingsTab_) + "." +
                                std::to_string(static_cast<int>(proxyH)) +
                                (claudeOn ? "C" : "c") + (mdOn ? "M" : "m") +
                                (petOn ? "P" : "p"))
                    .offset(cardsScrollOffset_)
                    .onChange([this](float v) { cardsScrollOffset_ = v; })
                    .content([&](eui::Ui& inner, float contentWidth,
                                 float viewportHeight) {
                        (void)viewportHeight;
                        // 卡片纵向游标（scrollView 内容空间，顶 = 0）。带下拉
                        // 的卡算 openUp 需页面绝对 Y：字段页内位置
                        // = cardsTopY + 内容Y - 滚动偏移。游标只累计**本 tab
                        // 参与绘制**的卡（tab 过滤后卡序列变了，全量累加会让
                        // 下拉方向算错）
                        const float gapY = t.sizes.spacingLarge;
                        float cardY = 0.0f;
                        switch (settingsTab_) {
                        case kTabLog: {
                            drawLogCard(inner, "settings.card.log",
                                        contentWidth, kLogSettingsCardHeight,
                                        t,
                                        cardsTopY + cardY -
                                            cardsScrollOffset_,
                                        screen.height);
                            cardY += kLogSettingsCardHeight + gapY;
                            // 日志查看卡吃掉剩余视口高（内容恰好一屏，避免
                            // 内外双层滚动条并存）；极小窗口兜底 260 由外层
                            // scrollView 负责滚动
                            const float viewH =
                                std::max(260.0f, cardsH - cardY);
                            drawLogViewCard(inner, "settings.tab.log",
                                            contentWidth, viewH, t);
                            break;
                        }
                        case kTabAbout:
                            // 关于卡恒显（不受模块开关门控，自身就是排障信息源）
                            drawAboutCard(inner, "settings.card.about",
                                          contentWidth, 150.0f, t);
                            break;
                        case kTabModules:
                            drawModulesCard(inner, "settings.card.modules",
                                            contentWidth, 288.0f, t);
                            cardY += 288.0f + gapY;
                            if (petOn) {
                                // 卡高 = 内容底沿 104（皮肤下拉 64+40）+ 16px 下边距。
                                // （「鼠标穿透」toggle 行 2026-09-24 已移除，120→168
                                // 的加高是残留 → 回 120，卡底不再留 64px 空白）
                                drawPetCard(inner, "settings.card.pet",
                                            contentWidth, 120.0f, t);
                                cardY += 120.0f + gapY;
                            }
                            if (mdOn) {
                                drawDirectoryCard(
                                    inner, "settings.card.directory",
                                    contentWidth, 108.0f, t);
                                cardY += 108.0f + gapY;
                            }
                            if (claudeOn) {
                                drawProxyCard(inner, "settings.card.proxy",
                                              contentWidth, proxyH, t);
                            }
                            break;
                        case kTabGeneral:
                        default:
                            drawAppearanceCard(
                                inner, "settings.card.appearance",
                                contentWidth, 160.0f, t);
                            cardY += 160.0f + gapY;
                            drawEditorFontCard(
                                inner, "settings.card.editor",
                                contentWidth, kEditorFontCardHeight, t,
                                cardsTopY + cardY - cardsScrollOffset_,
                                screen.height);
                            break;
                        }
                    })
                    .build();
            })
            .build();

        LOG_DEBUG("Settings 模块 compose 完成");
    }

private:
    // 非 token 的硬编码字号跟随全局缩放（单一来源 core::theme::fontScale；
    // token 字号已在 theme 层缩放，不要再经 fs() 包一层，避免双重缩放）
    float fs(float base) const {
        return base * ::core::theme::fontScale();
    }

    std::string retentionLabel() const {
        return cfg_.logRetentionDays == 0
                   ? "永久保留"
                   : std::to_string(cfg_.logRetentionDays) + " 天";
    }

    // cfg_.windowEffect（三档字符串）→ 下拉索引；sanitized 保证只会是三档之一
    int windowEffectIndex() const {
        if (cfg_.windowEffect == kWindowEffectTranslucent) return 1;
        if (cfg_.windowEffect == kWindowEffectFrosted) return 2;
        return 0;
    }

    // ---------- 顶部 Header ----------
    void drawHeader(eui::Ui& ui, float contentW, float headerH,
                    const core::theme::Tokens& t) {
        ui.row("settings.header")
            .size(contentW, headerH)
            .alignItems(eui::Align::CENTER)
            .content([&] {
                ui.column("settings.header.text")
                    .size(contentW, headerH)
                    .gap(2.0f)
                    .content([&] {
                        ui.text("settings.h1")
                            .size(contentW, 34.0f)
                            .text("设置")
                            .fontSize(t.sizes.fontSizePage)
                            .lineHeight(34.0f)
                            .fontWeight(700)
                            .color(t.colors.textPrimary)
                            .build();
                        const std::string sub = statusMessage_.empty()
                            ? "所有变更即时保存到 config.json · 左侧侧边栏切换页面"
                            : statusMessage_;
                        ui.text("settings.subtitle")
                            .size(contentW, 20.0f)
                            .text(sub)
                            .fontSize(t.sizes.fontSizeSmall)
                            .lineHeight(20.0f)
                            .color(statusIsError_ ? t.colors.error
                                                  : t.colors.textSecondary)
                            .build();
                    })
                    .build();
            })
            .build();
    }

    // ---------- 卡片公共背景 ----------
    void cardFrame(eui::Ui& ui, const std::string& id, float w, float h,
                   const core::theme::Tokens& t) {
        ui.rect(id + ".bg")
            .position(0.0f, 0.0f)
            .size(w, h)
            .color(t.colors.surface)
            .radius(t.sizes.radiusLarge)
            .border(1.0f, t.colors.borderDefault)
            .build();
    }

    // ---------- 卡片 1：外观（主题 + 字号） ----------
    // zIndex(30)：字号下拉的弹出层要盖过下方卡片（zIndex 只在兄弟间排序，
    // 弹层在卡片子树内，需整卡抬层）
    void drawAppearanceCard(eui::Ui& ui, const std::string& id, float w,
                            float h, const core::theme::Tokens& t) {
        const auto compTheme = core::theme::componentTokens(t);
        ui.stack(id)
            .size(w, h)
            .zIndex(30)
            .content([&] {
                cardFrame(ui, id, w, h, t);
                ui.text(id + ".title")
                    .position(28.0f, 18.0f)
                    .size(w - 56.0f, 24.0f)
                    .text("外观")
                    .fontSize(t.sizes.fontSizeLarge)
                    .lineHeight(24.0f)
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();
                ui.text(id + ".desc")
                    .position(28.0f, 44.0f)
                    .size(w - 56.0f, 18.0f)
                    .text("主题与窗口底色即时生效；字号缩放全局生效")
                    .fontSize(fs(12.0f))
                    .lineHeight(18.0f)
                    .color(t.colors.textTertiary)
                    .build();

                // 左：主题切换
                ui.text(id + ".theme.label")
                    .position(28.0f, 72.0f)
                    .size(200.0f, 16.0f)
                    .text("主题")
                    .fontSize(fs(12.0f))
                    .lineHeight(16.0f)
                    .color(t.colors.textSecondary)
                    .build();
                // 主题模式下拉（spec 缩水 A）：跟随系统/浅色/深色三档，替代
                // 原「深色/浅色」双按钮（原按钮只能表达显式档，无法表达 auto）。
                // 右缘 28+240=268，窄窗自适应后 fontX≥324，不碰撞（见
                // settings_effect_dropdown_fits_min_window）
                ui.stack(id + ".theme.ddwrap")
                    .position(28.0f, 94.0f)
                    .size(240.0f, 40.0f)
                    .content([&] {
                        components::dropdown(ui, id + ".theme.dd")
                            .size(240.0f, 40.0f)
                            .theme(compTheme)
                            .items(std::vector<std::string>(
                                kThemeModePresetLabels,
                                kThemeModePresetLabels +
                                    kThemeModePresetCount))
                            .selected(themeModeIndex())
                            .open(themeDropdownOpen_)
                            .onOpenChange(
                                [this](bool v) { themeDropdownOpen_ = v; })
                            .onChange(
                                [this](int idx) { applyThemeMode(idx); })
                            .build();
                    })
                    .build();

                // 右：字号缩放 + 右二：窗口效果三档（磨砂 Phase D），即时生效。
                // 终验复审：固定 x=380/680 在 1200 最小窗（页面宽 940、卡内宽
                // 860）下效果下拉右缘到 920，超出卡片 60px——右缘自适应：
                // 宽窗保持原位，窄窗右对齐收进卡内（列宽 240、列距 28、右缘留 28）
                const float effectX = std::min(680.0f, w - 28.0f - 240.0f);
                const float fontX = std::min(380.0f, effectX - 240.0f - 28.0f);
                ui.text(id + ".font.label")
                    .position(fontX, 72.0f)
                    .size(240.0f, 16.0f)
                    .text("字号缩放（正文基准 15px）")
                    .fontSize(fs(12.0f))
                    .lineHeight(16.0f)
                    .color(t.colors.textSecondary)
                    .build();
                ui.stack(id + ".font.ddwrap")
                    .position(fontX, 94.0f)
                    .size(240.0f, 40.0f)
                    .content([&] {
                        components::dropdown(ui, id + ".font.dd")
                            .size(240.0f, 40.0f)
                            .theme(compTheme)
                            .items(std::vector<std::string>(
                                kFontPresetLabels,
                                kFontPresetLabels + kFontPresetCount))
                            .selected(nearestIndex(cfg_.fontScalePercent,
                                                   kFontPresets, kFontPresetCount))
                            .open(fontDropdownOpen_)
                            .onOpenChange([this](bool v) { fontDropdownOpen_ = v; })
                            .onChange([this](int idx) { applyFontPreset(idx); })
                            .build();
                    })
                    .build();

                // 右二：窗口效果三档（磨砂 Phase D），即时生效（effectX 见上）
                ui.text(id + ".effect.label")
                    .position(effectX, 72.0f)
                    .size(240.0f, 16.0f)
                    .text("窗口效果")
                    .fontSize(fs(12.0f))
                    .lineHeight(16.0f)
                    .color(t.colors.textSecondary)
                    .build();
                ui.stack(id + ".effect.ddwrap")
                    .position(effectX, 94.0f)
                    .size(240.0f, 40.0f)
                    .content([&] {
                        components::dropdown(ui, id + ".effect.dd")
                            .size(240.0f, 40.0f)
                            .theme(compTheme)
                            .items(std::vector<std::string>(
                                kWindowEffectPresetLabels,
                                kWindowEffectPresetLabels +
                                    kWindowEffectPresetCount))
                            .selected(windowEffectIndex())
                            .open(effectDropdownOpen_)
                            .onOpenChange(
                                [this](bool v) { effectDropdownOpen_ = v; })
                            .onChange(
                                [this](int idx) { applyWindowEffectPreset(idx); })
                            .build();
                    })
                    .build();
                // 降级提示行（activeWindowEffect 回查实际生效值；一致时为空不显示）
                const std::string degraded = windowEffectDegradedHint(
                    cfg_.windowEffect, app::activeWindowEffect());
                if (!degraded.empty()) {
                    // ⚠ 字符换 FA：图标/文字拆两个元素（icon 字体通道）
                    ui.text(id + ".effect.hint.icon")
                        .position(effectX, 140.0f)
                        .size(14.0f, 16.0f)
                        .icon(0xF071)  // fa-triangle-exclamation
                        .fontSize(fs(11.0f))
                        .lineHeight(16.0f)
                        .color(t.colors.warning)
                        .build();
                    ui.text(id + ".effect.hint")
                        .position(effectX + 16.0f, 140.0f)
                        .size(w - effectX - 16.0f - 28.0f, 16.0f)
                        .text(degraded)
                        .fontSize(fs(11.0f))
                        .lineHeight(16.0f)
                        .color(t.colors.warning)
                        .build();
                }
            })
            .build();
    }

    // ---------- 卡片 2：模块管理（插拔方案 A §5.4） ----------
    // 三个工具模块各一行开关；关闭 = 下次启动不注册进 Router（不执行代码、
    // 不显示侧边栏入口）。home/settings 恒启用，不提供开关
    void drawModulesCard(eui::Ui& ui, const std::string& id, float w, float h,
                         const core::theme::Tokens& t) {
        struct ModuleMeta {
            const char* moduleId;
            const char* name;
            const char* desc;
        };
        static const ModuleMeta kToggleable[] = {
            {kModuleIdMarkdown, "Markdown 笔记本",
             "本地文件存储 + SQLite FTS5 全文搜索"},
            {kModuleIdTodo, "Todo 看板", "待办任务管理，多列卡片看板"},
            {kModuleIdClaudeProxy, "Claude 配置切换器",
             "Profile 管理 + 本地代理（关闭后 8888 不再监听）"},
            {kModuleIdPet, "桌宠",
             "桌面小宠物：透明置顶小窗，可拖拽/点击互动（默认关闭）"},
        };
        constexpr int kToggleCount = static_cast<int>(sizeof(kToggleable) / sizeof(kToggleable[0]));
        const auto compTheme = core::theme::componentTokens(t);
        ui.stack(id)
            .size(w, h)
            .content([&] {
                cardFrame(ui, id, w, h, t);
                ui.text(id + ".title")
                    .position(28.0f, 16.0f)
                    .size(w - 56.0f, 22.0f)
                    .text("模块")
                    .fontSize(t.sizes.fontSizeLarge)
                    .lineHeight(22.0f)
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();
                ui.text(id + ".desc")
                    .position(28.0f, 40.0f)
                    .size(w - 56.0f, 16.0f)
                    .text("关闭模块后需重启应用生效（关闭 = 该模块代码不再执行、"
                          "侧边栏不显示入口）")
                    .fontSize(fs(12.0f))
                    .lineHeight(16.0f)
                    .color(t.colors.textTertiary)
                    .build();

                for (int i = 0; i < kToggleCount; ++i) {
                    const ModuleMeta& meta = kToggleable[i];
                    const float rowY = 68.0f + static_cast<float>(i) * 56.0f;
                    const std::string suffix = std::to_string(i);
                    ui.text(id + ".name." + suffix)
                        .position(28.0f, rowY)
                        .size(w - 160.0f, 18.0f)
                        .text(meta.name)
                        .fontSize(t.sizes.fontSizeBody - 1.0f)
                        .lineHeight(18.0f)
                        .fontWeight(500)
                        .color(t.colors.textPrimary)
                        .build();
                    ui.text(id + ".rowdesc." + suffix)
                        .position(28.0f, rowY + 20.0f)
                        .size(w - 160.0f, 16.0f)
                        .text(meta.desc)
                        .fontSize(fs(12.0f))
                        .lineHeight(16.0f)
                        .color(t.colors.textTertiary)
                        .build();
                    // SwitchBuilder 无 position，套 stack 定位（与代理卡同款）
                    ui.stack(id + ".sw.wrap." + suffix)
                        .position(w - 28.0f - 80.0f, rowY - 2.0f)
                        .size(80.0f, 32.0f)
                        .content([&] {
                            components::toggleSwitch(ui, id + ".sw." + suffix)
                                .size(80.0f, 32.0f)
                                .checked(moduleEnabled(cfg_, meta.moduleId))
                                .theme(compTheme)
                                .onChange([this, mid = meta.moduleId](
                                              bool v) {
                                    applyModuleEnabled(mid, v);
                                })
                                .build();
                        })
                        .build();
                }
            })
            .build();
    }

    // ---------- 卡片 3：桌宠（M2 多皮肤 UI，设计 §4.5/§4.6） ----------
    // 皮肤下拉（discoverSkins 发现列表）+ 位置重置按钮；两者即时生效
    //（换肤热切换/位置直接挪窗），与开关不同（开关重启生效）
    void drawPetCard(eui::Ui& ui, const std::string& id, float w, float h,
                     const core::theme::Tokens& t) {
        // 皮肤清单/当前皮肤首帧惰性加载（目录扫描不进每帧路径）
        if (!petSkinDataLoaded_) {
            petSkinDataLoaded_ = true;
            petSkins_ = ::module::pet::discoverSkins();
            // 审计 [5][36]：原「构造盘 store 读一次快照」换成 pet 侧只读桥
            //（两次独立读盘代替一次快照——初值展示语义，
            // 与 apply 双写路径无竞态顾虑）
            petSkinName_ = ::module::pet::currentSkinName();
        }
        const auto compTheme = core::theme::componentTokens(t);
        ui.stack(id)
            .size(w, h)
            .content([&] {
                cardFrame(ui, id, w, h, t);
                ui.text(id + ".title")
                    .position(28.0f, 16.0f)
                    .size(w - 56.0f, 22.0f)
                    .text("桌宠")
                    .fontSize(t.sizes.fontSizeLarge)
                    .lineHeight(22.0f)
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();
                ui.text(id + ".desc")
                    .position(28.0f, 40.0f)
                    .size(w - 56.0f, 16.0f)
                    .text("换肤与位置重置即时生效；开关桌宠需重启应用")
                    .fontSize(fs(12.0f))
                    .lineHeight(16.0f)
                    .color(t.colors.textTertiary)
                    .build();
                // 皮肤下拉（列表为空时占位一项，选择被忽略）
                std::vector<std::string> items = petSkins_;
                if (items.empty()) {
                    items.emplace_back("（未发现皮肤）");
                }
                int selected = 0;
                for (size_t i = 0; i < petSkins_.size(); ++i) {
                    if (petSkins_[i] == petSkinName_) {
                        selected = static_cast<int>(i);
                        break;
                    }
                }
                ui.stack(id + ".skin.ddwrap")
                    .position(28.0f, 64.0f)
                    .size(240.0f, 40.0f)
                    .content([&] {
                        components::dropdown(ui, id + ".skin.dd")
                            .size(240.0f, 40.0f)
                            .theme(compTheme)
                            .items(items)
                            .selected(selected)
                            .open(petSkinDropdownOpen_)
                            .onOpenChange(
                                [this](bool v) { petSkinDropdownOpen_ = v; })
                            .onChange([this](int idx) {
                                if (petSkins_.empty() || idx < 0 ||
                                    idx >= static_cast<int>(petSkins_.size())) {
                                    return;
                                }
                                petSkinName_ = petSkins_[static_cast<size_t>(idx)];
                                ::module::pet::applyPetSkin(petSkinName_);
                                setStatus("桌宠皮肤已切换：" + petSkinName_);
                            })
                            .build();
                    })
                    .build();
                // 位置重置按钮（回主屏右下角，持久化坐标清回未设置）
                components::button(ui, id + ".reset")
                    .position(w - 28.0f - 160.0f, 64.0f)
                    .size(160.0f, 40.0f)
                    .text("位置重置")
                    .fontSize(14.0f)
                    .theme(compTheme, /*primary=*/false)
                    .textColor(t.colors.textPrimary)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] {
                        ::module::pet::resetPetPosition();
                        setStatus("桌宠位置已重置（主屏右下角）");
                    })
                    .build();
            })
            .build();
    }

    // ---------- 卡片 4：Markdown 笔记目录（只读 + 打开目录） ----------
    // 打开笔记目录（总设计 §8.5 第 2 条「打开目录」按钮，审计 [15] 短期项）：
    // 目录不存在先创建（点「打开」的预期是能看到能用的目录），再交平台
    // 文件管理器打开（Win ShellExecuteW / macOS open / xdg-open）
    void openNotesDirectory() {
        const std::filesystem::path mdDir = ::core::paths::markdownDir();
        std::error_code ec;
        std::filesystem::create_directories(mdDir, ec);
        if (ec) {
            setStatus("笔记目录创建失败：" + ec.message(), true);
            LOG_ERROR("Settings: create_directories 失败 " + mdDir.string() +
                      ": " + ec.message());
            return;
        }
        auto r = ::platform::file_open::openDirectory(mdDir.string());
        if (r.isFail()) {
            setStatus("打开目录失败：" + r.errorMessage(), true);
            LOG_ERROR("Settings: openDirectory 失败 " + mdDir.string() + ": " +
                      r.errorMessage());
            return;
        }
        setStatus("已在文件管理器中打开笔记目录");
        LOG_INFO("Settings: 已打开笔记目录 " + mdDir.string());
    }

    void drawDirectoryCard(eui::Ui& ui, const std::string& id, float w,
                           float h, const core::theme::Tokens& t) {
        ui.stack(id)
            .size(w, h)
            .content([&] {
                cardFrame(ui, id, w, h, t);
                ui.text(id + ".title")
                    .position(28.0f, 16.0f)
                    .size(w - 56.0f, 22.0f)
                    .text("Markdown 笔记目录（只读）")
                    .fontSize(t.sizes.fontSizeLarge)
                    .lineHeight(22.0f)
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();

                const std::filesystem::path mdDir =
                    ::core::paths::markdownDir();
                std::error_code ec;
                const bool exists =
                    std::filesystem::exists(mdDir, ec) && !ec;
                const float dirBtnW = 110.0f;
                ui.rect(id + ".path.bg")
                    .position(28.0f, 46.0f)
                    .size(w - 56.0f - dirBtnW - 12.0f, 34.0f)
                    .color(t.colors.surfaceElevated)
                    .radius(t.sizes.radiusSmall)
                    .border(1.0f, t.colors.borderMuted)
                    .build();
                ui.text(id + ".path.value")
                    .position(40.0f, 46.0f)
                    .size(w - 80.0f - dirBtnW - 12.0f, 34.0f)
                    .text(mdDir.string() + (exists ? "" : "（尚未创建）"))
                    .fontSize(fs(13.0f))
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textSecondary)
                    .build();
                // 「打开目录」按钮（审计 [15]：路径在 APPDATA 深处，此前用户
                // 只能手拼路径；改目录本身仍属后续功能，登记 KNOWN_LIMITATIONS）
                components::button(ui, id + ".open")
                    .position(w - 28.0f - dirBtnW, 46.0f)
                    .size(dirBtnW, 34.0f)
                    .text("打开目录")
                    .icon(0xF07C)  // fa-folder-open
                    .fontSize(fs(13.0f))
                    .colors(t.colors.surfaceElevated, t.colors.hover,
                            t.colors.active)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radiusSmall)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] { openNotesDirectory(); })
                    .build();
                ui.text(id + ".hint")
                    .position(28.0f, 86.0f)
                    .size(w - 56.0f, 14.0f)
                    .text("修改笔记目录属于后续功能，当前仅展示（可用「打开目录」"
                          "定位后迁移笔记）")
                    .fontSize(fs(11.0f))
                    .lineHeight(14.0f)
                    .color(t.colors.textTertiary)
                    .build();
            })
            .build();
    }

    // ---------- 卡片 4：Claude 代理 token（claude 模块关闭时整卡隐藏） ----------
    void drawProxyCard(eui::Ui& ui, const std::string& id, float w, float h,
                       const core::theme::Tokens& t) {
        ui.stack(id)
            .size(w, h)
            .content([&] {
                cardFrame(ui, id, w, h, t);
                auto* svc = ::module::claude::ClaudeService::global();
                const bool running = svc && svc->isProxyRunning();
                const int port = running ? svc->proxyPort() : 0;

                ui.text(id + ".title")
                    .position(28.0f, 16.0f)
                    .size(240.0f, 22.0f)
                    .text("Claude 代理")
                    .fontSize(t.sizes.fontSizeLarge)
                    .lineHeight(22.0f)
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();

                // 随应用启动代理（Phase B 硬性需求 1，默认关）——放卡片右上，
                // 不新增卡片高度（SwitchBuilder 无 position，套 stack 定位，
                // 与字号下拉同款包装）
                const auto compTheme = core::theme::componentTokens(t);
                ui.stack(id + ".autostart.wrap")
                    .position(w - 28.0f - 240.0f, 12.0f)
                    .size(240.0f, 28.0f)
                    .content([&] {
                        components::toggleSwitch(ui, id + ".autostart")
                            .size(240.0f, 28.0f)
                            .text("随应用启动代理")
                            .fontSize(fs(13.0f))
                            .checked(cfg_.claudeProxyAutoStart)
                            .theme(compTheme)
                            .onChange([this](bool v) { applyAutoStart(v); })
                            .build();
                    })
                    .build();

                ui.text(id + ".desc")
                    .position(28.0f, 40.0f)
                    .size(w - 56.0f, 16.0f)
                    .text(running
                          ? "本地代理 http://127.0.0.1:" + std::to_string(port) +
                                "（仅本机可访问，无需凭据）"
                          : "本地代理未运行（默认不随应用启动，可在 Claude 页手动启动）")
                    .fontSize(fs(12.0f))
                    .lineHeight(16.0f)
                    .color(t.colors.textTertiary)
                    .build();

                // ---- 代理端口行（总设计 §8.5 第 5 条，审计 2026-09-23 [14]）----
                // 数字输入 + Enter/应用按钮提交；代理运行中改端口自动 stop→start
                // 重启（保持 token，只换端口），未运行则下次启动生效
                const std::string portText = proxyPortDraft_.empty()
                    ? std::to_string(cfg_.proxyPort)
                    : proxyPortDraft_;
                ui.text(id + ".port.label")
                    .position(28.0f, 62.0f)
                    .size(72.0f, 18.0f)
                    .text("代理端口")
                    .fontSize(fs(12.0f))
                    .lineHeight(28.0f)
                    .color(t.colors.textSecondary)
                    .build();
                components::input(ui, id + ".port.input")
                    .position(104.0f, 60.0f)
                    .size(96.0f, 28.0f)
                    .value(portText)
                    .placeholder("8888")
                    .fontSize(fs(13.0f))
                    .theme(compTheme)
                    .onChange([this](const std::string& v) {
                        proxyPortDraft_ = v;
                        // 审计 [19]：校验错误（红字状态行）随下一次输入清除，
                        // 对齐 claude formError_ 的「就地红字 + 输入即清」语义；
                        // 成功提示（非红字）不打断，保留到下一次操作
                        if (statusIsError_) {
                            statusMessage_.clear();
                            statusIsError_ = false;
                        }
                    })
                    .onEnter([this] { applyProxyPort(); })
                    // 审计 [21]：Esc=丢弃端口草稿（回落已应用端口，不再误触发
                    // applyProxyPort——旧映射下 Esc 会提交端口并重启代理）
                    .onCancel([this] { proxyPortDraft_.clear(); })
                    .build();
                components::button(ui, id + ".port.apply")
                    .position(208.0f, 60.0f)
                    .size(64.0f, 28.0f)
                    .text("应用")
                    .fontSize(fs(13.0f))
                    .colors(t.colors.primary, t.colors.primaryHover,
                            t.colors.primaryPressed)
                    .textColor(t.colors.onPrimary)
                    .radius(t.sizes.radiusSmall)
                    .onClick([this] { applyProxyPort(); })
                    .build();
                ui.text(id + ".port.hint")
                    .position(282.0f, 62.0f)
                    .size(w - 282.0f - 28.0f, 18.0f)
                    .text("改后回车或点「应用」；代理运行中自动重启")
                    .fontSize(fs(12.0f))
                    .lineHeight(18.0f)
                    .color(t.colors.textTertiary)
                    .build();

                // Claude Code 接管状态（Phase D，§6.3）：sidecar 是否存在推导，
                // 让还原机制对用户可见；上层覆盖警告（§5.3 检测命中时）逐行展示
                const bool managed = svc && svc->isClaudeCodeManaged();
                ui.text(id + ".takeover.label")
                    .position(28.0f, 98.0f)
                    .size(150.0f, 18.0f)
                    .text("Claude Code 接管")
                    .fontSize(fs(12.0f))
                    .lineHeight(18.0f)
                    .color(t.colors.textSecondary)
                    .build();
                const std::string ccPath =
                    ::module::claude::settings_sync::defaultPaths()
                        .settingsFile.string();
                ui.text(id + ".takeover.value")
                    .position(190.0f, 98.0f)
                    .size(w - 190.0f - 28.0f, 18.0f)
                    .text(managed
                          ? ("● 已接管 " + ccPath)
                          : std::string("○ 未接管（启动代理后自动接管，停止时还原）"))
                    .fontSize(fs(12.0f))
                    .lineHeight(18.0f)
                    .color(managed ? t.colors.success : t.colors.textTertiary)
                    .build();
                if (managed && svc) {
                    const auto& warns = svc->lastSyncWarnings();
                    // ponytail: 最多展示 2 条（卡片高度约束），更多警告在
                    // 日志（LOG_WARN 全量）可查；⚠ 换 FA（图标/文字拆两个元素）
                    const size_t shown =
                        std::min<size_t>(warns.size(), 2);
                    for (size_t i = 0; i < shown; ++i) {
                        ui.text(id + ".takeover.warn.icon." + std::to_string(i))
                            .position(28.0f, 118.0f + static_cast<float>(i) * 16.0f)
                            .size(12.0f, 16.0f)
                            .icon(0xF071)  // fa-triangle-exclamation
                            .fontSize(fs(11.0f))
                            .lineHeight(16.0f)
                            .color(t.colors.warning)
                            .build();
                        ui.text(id + ".takeover.warn." + std::to_string(i))
                            .position(42.0f, 118.0f + static_cast<float>(i) * 16.0f)
                            .size(w - 42.0f - 28.0f, 16.0f)
                            .text(warns[i])
                            .fontSize(fs(11.0f))
                            .lineHeight(16.0f)
                            .color(t.colors.warning)
                            .build();
                    }
                }
            })
            .build();
    }

    // ---------- 卡片 5：日志（保留天数 + 级别，日志 tab） ----------
    // 行内下拉的弹层用 cardTopY/screenH 几何估算判断向下展开是否出界，出界则
    // openUp 向上（修复菜单被窗体裁剪）。spec 缩水 B 的编辑器字号 Task B 起
    // 迁到「通用」tab 独立卡（不同 tab 的卡不同屏，原「末卡 z 逃逸宿主」
    // 理由失效）。cardTopY/screenH：弹层几何用文件顶部 kDropdown* /
    // kLogDropdown* 常量估算（与 dropdown 内部 metrics 一致）
    void drawLogCard(eui::Ui& ui, const std::string& id, float w, float h,
                     const core::theme::Tokens& t, float cardTopY,
                     float screenH) {
        const auto compTheme = core::theme::componentTokens(t);
        ui.stack(id)
            .size(w, h)
            .zIndex(30)
            .content([&] {
                cardFrame(ui, id, w, h, t);
                ui.text(id + ".title")
                    .position(28.0f, 16.0f)
                    .size(w - 56.0f, 22.0f)
                    .text("日志")
                    .fontSize(t.sizes.fontSizeLarge)
                    .lineHeight(22.0f)
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();

                // 档位说明（0 = 永久）；存的是原始天数 int，T10 清理任务按值读取
                static const char* kRetentionLabels[] = {
                    "永久保留", "7 天", "30 天", "90 天", "180 天", "365 天"};
                ui.stack(id + ".ddwrap")
                    .position(28.0f, kLogDropdownOffsetY)
                    .size(240.0f, kLogDropdownHeight)
                    .content([&] {
                        // 字段绝对底边 = 卡顶 + 卡内 y 偏移 + 字段高
                        const float fieldBottomY =
                            cardTopY + kLogDropdownOffsetY + kLogDropdownHeight;
                        const float popupH = static_cast<float>(kRetentionPresetCount) *
                                                 kDropdownItemHeight +
                                             kDropdownPopupPadding;
                        const bool openUp = fieldBottomY + kDropdownPopupGap +
                                                popupH >
                                            screenH - kWindowBottomMargin;
                        components::dropdown(ui, id + ".dd")
                            .size(240.0f, kLogDropdownHeight)
                            .theme(compTheme)
                            .items(std::vector<std::string>(
                                kRetentionLabels,
                                kRetentionLabels + kRetentionPresetCount))
                            .selected(nearestIndex(cfg_.logRetentionDays,
                                                   kRetentionPresets,
                                                   kRetentionPresetCount))
                            .open(retentionDropdownOpen_)
                            .openUp(openUp)
                            .onOpenChange(
                                [this](bool v) { retentionDropdownOpen_ = v; })
                            .onChange(
                                [this](int idx) { applyRetentionPreset(idx); })
                            .build();
                    })
                    .build();
                ui.text(id + ".hint")
                    .position(288.0f, 60.0f)
                    .size(w - 316.0f, 18.0f)
                    .text("0 = 永久保留；30 天为默认。切换档位立即清理一次。")
                    .fontSize(fs(12.0f))
                    .lineHeight(18.0f)
                    .color(t.colors.textTertiary)
                    .build();

                // ---- 行 2：日志级别（spec §8.5「日志级别」，spec 缩水 D；
                // Task B：原行 2 编辑器字号迁至「通用」tab 独立卡后上移）：
                // 切档即时 core::log::setLevel（DEBUG 起打 LOG_DEBUG 详录），
                // 重启由 onStart 恢复持久化档 ----
                ui.text(id + ".level.label")
                    .position(28.0f, 100.0f)
                    .size(84.0f, 20.0f)
                    .text("日志级别")
                    .fontSize(fs(12.0f))
                    .lineHeight(20.0f)
                    .color(t.colors.textSecondary)
                    .build();
                ui.stack(id + ".level.ddwrap")
                    .position(124.0f, kLogLevelOffsetY)
                    .size(150.0f, kLogLevelHeight)
                    .content([&] {
                        const float levelFieldBottomY =
                            cardTopY + kLogLevelOffsetY + kLogLevelHeight;
                        const float levelPopupH =
                            static_cast<float>(kLogLevelPresetCount) *
                                    kDropdownItemHeight +
                                kDropdownPopupPadding;
                        const bool levelOpenUp = levelFieldBottomY +
                                    kDropdownPopupGap + levelPopupH >
                                screenH - kWindowBottomMargin;
                        components::dropdown(ui, id + ".level.dd")
                            .size(150.0f, kLogLevelHeight)
                            .theme(compTheme)
                            .items(std::vector<std::string>(
                                kLogLevelPresetLabels,
                                kLogLevelPresetLabels + kLogLevelPresetCount))
                            .selected(logLevelIndex())
                            .openUp(levelOpenUp)
                            .open(logLevelDropdownOpen_)
                            .onOpenChange(
                                [this](bool v) { logLevelDropdownOpen_ = v; })
                            .onChange([this](int idx) {
                                applyLogLevelPreset(idx);
                            })
                            .build();
                    })
                    .build();
                ui.text(id + ".level.hint")
                    .position(288.0f, 100.0f)
                    .size(w - 316.0f, 18.0f)
                    .text("DEBUG 最详尽（排障）；默认 INFO。切档即时生效。")
                    .fontSize(fs(12.0f))
                    .lineHeight(18.0f)
                    .color(t.colors.textTertiary)
                    .build();
            })
            .build();
    }

    // ---------- 卡片 6：关于（spec §8.5「关于」卡，spec 缩水 C） ----------
    // 版本/构建信息（CMake 注入宏，见 settings_config.cpp aboutVersionLine/
    // aboutEuiNeoLine）+ 配置/日志文件路径（排障入口：路径都在 APPDATA 深处，
    // 审计 [15] 同款痛点）。纯展示无交互控件 → 无弹层，维持 z0 不影响任何
    // 兄弟卡的 z 逃逸。GitHub 链接暂未展示（DevDesk 无公开仓库，登记决策项）
    void drawAboutCard(eui::Ui& ui, const std::string& id, float w, float h,
                       const core::theme::Tokens& t) {
        ui.stack(id)
            .size(w, h)
            .content([&] {
                cardFrame(ui, id, w, h, t);
                ui.text(id + ".title")
                    .position(28.0f, 16.0f)
                    .size(w - 56.0f, 22.0f)
                    .text("关于")
                    .fontSize(t.sizes.fontSizeLarge)
                    .lineHeight(22.0f)
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();
                ui.text(id + ".version")
                    .position(28.0f, 50.0f)
                    .size(w - 56.0f, 18.0f)
                    .text(aboutVersionLine())
                    .fontSize(fs(13.0f))
                    .lineHeight(18.0f)
                    .color(t.colors.textSecondary)
                    .build();
                ui.text(id + ".eui")
                    .position(28.0f, 72.0f)
                    .size(w - 56.0f, 18.0f)
                    .text(aboutEuiNeoLine())
                    .fontSize(fs(13.0f))
                    .lineHeight(18.0f)
                    .color(t.colors.textSecondary)
                    .build();
                ui.text(id + ".config")
                    .position(28.0f, 98.0f)
                    .size(w - 56.0f, 16.0f)
                    .text("配置文件: " + ::core::paths::configFile().string())
                    .fontSize(fs(12.0f))
                    .lineHeight(16.0f)
                    .color(t.colors.textTertiary)
                    .build();
                ui.text(id + ".logfile")
                    .position(28.0f, 118.0f)
                    .size(w - 56.0f, 16.0f)
                    .text("日志文件: " + ::core::paths::logFile().string())
                    .fontSize(fs(12.0f))
                    .lineHeight(16.0f)
                    .color(t.colors.textTertiary)
                    .build();
            })
            .build();
    }

    // ---------- 日志 tail 读取（Task B，日志 tab） ----------
    // 仅在「切到日志 tab / 点刷新」后的 compose 顶部调用一次：scrollView
    // content lambda 被 measure/paint 多次调用，读盘绝不能进那里。只读文件
    // 末尾窗口（kLogTailReadWindow）防大日志整档进内存；保留末尾
    // kLogTailMaxLines 行（滑动裁剪，避免逐行 erase 的 O(n²)）
    void readLogTail() {
        std::vector<std::string> lines;
        std::ifstream f(::core::paths::logFile(), std::ios::binary);
        if (f) {
            f.seekg(0, std::ios::end);
            const std::streamoff size = f.tellg();
            if (size > static_cast<std::streamoff>(kLogTailReadWindow)) {
                f.seekg(size - static_cast<std::streamoff>(
                              kLogTailReadWindow));
                // 落点在半行中：丢掉开头半行
                std::string partial;
                std::getline(f, partial);
            } else {
                f.seekg(0);
            }
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();  // CRLF 行尾去 \r
                }
                lines.push_back(std::move(line));
                if (lines.size() >
                    static_cast<size_t>(kLogTailMaxLines) * 2) {
                    lines.erase(lines.begin(), lines.begin() +
                        static_cast<long>(lines.size() -
                                          kLogTailMaxLines));
                }
            }
            if (lines.size() > static_cast<size_t>(kLogTailMaxLines)) {
                lines.erase(lines.begin(), lines.begin() +
                    static_cast<long>(lines.size() - kLogTailMaxLines));
            }
        }
        logTail_ = std::move(lines);
        ++logTailVersion_;
        LOG_INFO("Settings: 日志 tail 刷新，读取 " +
                 std::to_string(logTail_.size()) + " 行");
    }

    // ---------- 编辑器字号卡（Task B：自日志卡行 2 迁入「通用」tab） ----------
    // spec §8.5「编辑器字体大小」（spec 缩水 B）：档位写 core::theme::
    // editorFontSize 通道，markdown 下一帧生效；默认 15px = fontSizeBody
    // token 基准。cardTopY/screenH：下拉弹层 openUp 几何估算（同款日志卡）
    void drawEditorFontCard(eui::Ui& ui, const std::string& id, float w,
                            float h, const core::theme::Tokens& t,
                            float cardTopY, float screenH) {
        const auto compTheme = core::theme::componentTokens(t);
        ui.stack(id)
            .size(w, h)
            .zIndex(30)
            .content([&] {
                cardFrame(ui, id, w, h, t);
                ui.text(id + ".title")
                    .position(28.0f, 16.0f)
                    .size(w - 56.0f, 22.0f)
                    .text("编辑器字号")
                    .fontSize(t.sizes.fontSizeLarge)
                    .lineHeight(22.0f)
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();
                ui.stack(id + ".ddwrap")
                    .position(28.0f, kEditorFontDdOffsetY)
                    .size(150.0f, kLogEditorFontHeight)
                    .content([&] {
                        const float efontFieldBottomY = cardTopY +
                            kEditorFontDdOffsetY + kLogEditorFontHeight;
                        const float efontPopupH =
                            static_cast<float>(kEditorFontPresetCount) *
                                    kDropdownItemHeight +
                                kDropdownPopupPadding;
                        const bool efontOpenUp = efontFieldBottomY +
                                    kDropdownPopupGap + efontPopupH >
                                screenH - kWindowBottomMargin;
                        components::dropdown(ui, id + ".dd")
                            .size(150.0f, kLogEditorFontHeight)
                            .theme(compTheme)
                            .items(std::vector<std::string>(
                                kEditorFontPresetLabels,
                                kEditorFontPresetLabels +
                                    kEditorFontPresetCount))
                            .selected(nearestIndex(cfg_.editorFontSize,
                                                   kEditorFontPresets,
                                                   kEditorFontPresetCount))
                            .openUp(efontOpenUp)
                            .open(editorFontDropdownOpen_)
                            .onOpenChange(
                                [this](bool v) { editorFontDropdownOpen_ = v; })
                            .onChange([this](int idx) {
                                applyEditorFontPreset(idx);
                            })
                            .build();
                    })
                    .build();
                ui.text(id + ".hint")
                    .position(190.0f, 48.0f)
                    .size(w - 218.0f, 18.0f)
                    .text("Markdown 编辑器正文字号；默认 15px（标准）。切换即时生效。")
                    .fontSize(fs(12.0f))
                    .lineHeight(18.0f)
                    .color(t.colors.textTertiary)
                    .build();
            })
            .build();
    }

    // ---------- 日志查看卡（Task B，日志 tab）：日志文件 tail + 刷新 ----------
    // logTail_ 是内存快照（读盘见 readLogTail，只发生在切 tab / 点刷新后），
    // 本方法只渲染字符串。逐行一个 text（固定行高，超宽行由 scrollView clip
    // 右侧裁切）；刷新按钮只置 dirty 标记，实际重读在下一帧 compose 顶部
    void drawLogViewCard(eui::Ui& ui, const std::string& id, float w, float h,
                         const core::theme::Tokens& t) {
        const auto compTheme = core::theme::componentTokens(t);
        const float lineH = 16.0f;
        ui.stack(id)
            .size(w, h)
            .content([&] {
                cardFrame(ui, id, w, h, t);
                const float btnW = 96.0f;
                ui.text(id + ".title")
                    .position(28.0f, 16.0f)
                    .size(w - 56.0f - btnW - 12.0f, 22.0f)
                    .text("运行日志（末尾 500 行）")
                    .fontSize(t.sizes.fontSizeLarge)
                    .lineHeight(22.0f)
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();
                components::button(ui, id + ".refresh")
                    .position(w - 28.0f - btnW, 14.0f)
                    .size(btnW, 28.0f)
                    .text("刷新")
                    .icon(0xF021)  // fa-arrows-rotate
                    .fontSize(fs(12.0f))
                    .colors(t.colors.surfaceElevated, t.colors.hover,
                            t.colors.active)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radiusSmall)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] { logTailDirty_ = true; })
                    .build();
                const float areaTop = 48.0f;
                components::scrollView(ui, id + ".scroll")
                    .position(28.0f, areaTop)
                    .size(w - 56.0f, h - areaTop - 16.0f)
                    .gap(0.0f)
                    .step(lineH)
                    .theme(compTheme)
                    // contentKey 带 tail 版本号：刷新重读后内容高缓存失效重测
                    .contentKey(id + ".scroll.v" +
                                std::to_string(logTailVersion_))
                    .offset(logViewScrollOffset_)
                    .onChange([this](float v) {
                        logViewScrollOffset_ = v;
                    })
                    .content([&](eui::Ui& inner, float cw, float vh) {
                        (void)vh;
                        if (logTail_.empty()) {
                            inner.text(id + ".empty")
                                .position(8.0f, 8.0f)
                                .size(cw - 16.0f, 20.0f)
                                .text("（暂无日志内容，或日志文件尚未生成）")
                                .fontSize(fs(12.0f))
                                .lineHeight(20.0f)
                                .color(t.colors.textTertiary)
                                .build();
                            return;
                        }
                        for (size_t i = 0; i < logTail_.size(); ++i) {
                            inner.text(id + ".line." + std::to_string(i))
                                .position(
                                    8.0f, static_cast<float>(i) * lineH)
                                .size(cw - 16.0f, lineH)
                                .text(logTail_[i])
                                .fontSize(fs(11.0f))
                                .lineHeight(lineH)
                                .color(t.colors.textSecondary)
                                .build();
                        }
                    })
                    .build();
            })
            .build();
    }

    // ---------- 状态 ----------
    std::unique_ptr<SettingsStore> store_;
    SettingsConfig cfg_;
    bool fontDropdownOpen_ = false;
    bool retentionDropdownOpen_ = false;
    bool effectDropdownOpen_ = false;      // 窗口效果下拉（磨砂 Phase D）
    bool themeDropdownOpen_ = false;       // 主题模式下拉（spec 缩水 A）
    bool editorFontDropdownOpen_ = false;  // 编辑器字号下拉（spec 缩水 B）
    bool logLevelDropdownOpen_ = false;    // 日志级别下拉（spec 缩水 D）
    bool petSkinDropdownOpen_ = false;     // 桌宠皮肤下拉（M2）
    bool petSkinDataLoaded_ = false;       // 皮肤清单惰性加载标记（M2）
    std::vector<std::string> petSkins_;    // 发现到的皮肤（M2）
    std::string petSkinName_;              // 当前皮肤名（M2）
    // 桌宠挂机穿透功能已于 2026-09-24 移除（本成员删除）
    // 代理端口输入草稿（审计 [14]）：空 = 未编辑，compose 显示 cfg_.proxyPort；
    // applyProxyPort 成功后由 persist 侧刷新（下次进页恢复显示配置值）
    std::string proxyPortDraft_;
    float cardsScrollOffset_ = 0.0f;      // 卡片区 scrollView 滚动位置
    // ---- Task B：tab 化 + 日志查看 ----
    int settingsTab_ = 0;                 // 当前 tab（通用/模块/日志/关于）
    std::vector<std::string> logTail_;    // 日志文件末尾行内存快照（compose 只渲染）
    unsigned logTailVersion_ = 0;         // tail 读取版本号（进日志查看卡 contentKey）
    bool logTailDirty_ = true;            // 待重读标记（切日志 tab / 点刷新置位）
    float logViewScrollOffset_ = 0.0f;    // 日志查看卡内滚动位置
    std::string statusMessage_;
    bool statusIsError_ = false;
};

} // namespace module::settings

extern "C" ::core::Module* create_settings_module() {
    return new module::settings::SettingsModule();
}
