// src/module_settings/settings_module.cpp
// 设置页模块：Module 实现、生命周期、设置变更动作、日志 tail 与外部状态快照、卡片注册表。
// GitHub Sync 的编排（push/pull/firstsync/节流驱动面之外的动作执行）在 github_sync_engine，
// 本文件只留 UI 接线 + 后台线程 + 每帧收割 + 弹窗渲染。
// 依赖边界：module_* 互不依赖 —— claude 经 core 窄接口、模块开关经 module_toggle_bridge.h。
// 渲染路径不做 fs 查询：外部状态统一在 refreshExternalState 装载，compose 只读快照。

#include "app_tools.h"
#include "settings_config.h"
#include "settings_ui_cards.h"
#include "github_sync_card.h"
#include "github_sync_config.h"
#include "github_sync_engine.h"
#include "github_sync_logic.h"
#include "git_error_dialog.h"
#include "github_sync_test_seam.h"
#include "local_git_probe.h"
#include "settings_ui_logic.h"

#include "core/claude_proxy_service.h"
#include "core/database.h"
#include "core/event_bus.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/service_locator.h"
#include "core/theme.h"
#include "core/tool.h"
#include "core/component_theme.h"
#include "platform/github_api.h"     // platform::github::ApiLogEntry（弹窗日志源；GithubApi 由同步引擎持有）
#include <core/platform/platform.h>   // core::platform::requestUiUpdate（唤醒主循环）
#include <core/window/window_backend.h>   // core::window::setClipboardText（复制详情）
#include "platform/autostart.h"      // platform::autostart（开机自启动注册态回读/写入，UAT6）
#include "platform/system_theme.h"
#include "platform/wallpaper_capability.h"  // platform::wallpaper::supported（壁纸卡可见性）

// 模块开关即时生效桥：函数指针缝 + 默认 no-op，真 handler 由 main.cpp 装配段注入。
#include "module_toggle_bridge.h"

#include <ui_kit/columns.h>
#include <ui_kit/metrics.h>  // kHeaderH（页头行高；kSettingsHeaderH 已并入共享源）
#include <ui_kit/toast.h>
#include <ui_kit/wtab.h>

#include <core/render/text.h>  // core::TextPrimitive::measureTextWidth

#include <eui/dsl.h>

#include <components/scrollview.h>
#include <components/tabs.h>
#include <components/theme.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

// 动态背景运行时镜像的单一符号桥（实现见 app_sidebar.cpp，compose 门控读它）：
// 模块层不 include app 层头文件（层级方向 app→module），此处前置声明、链接期
// 合并到同一 `app` 命名空间（与 EUI-NEO 的 app:: 前置声明并存，函数名不冲突）。
namespace app {
void setFluidBackgroundEnabled(bool enabled);
}

namespace module::settings {

namespace {

// 卡片注册表：每行 = 一张卡（tab / 门控 / 卡高种类 / 是否吃页面几何 / 半宽配对 / 绘制函数），
// 顺序 = 绘制顺序（半宽卡相邻者两两成行，见 drawCardRegion 双游标）。
enum class CardHeightKind {
    Fixed,
    ProxyWarnings,
    FillRemainder,
    GithubSync,   // 恒满高（UAT4 起不按启用态折叠；UAT5 方案 A 三段式，见 kGithubSyncCardHeight）
};

struct CardSpec {
    int tab;
    const char* id;
    const char* gate;       // nullptr = 恒显；否则 = 模块开关门控键
    CardHeightKind heightKind;
    float height;           // Fixed 档生效
    bool pageGeom;          // 下发卡顶页面绝对 y + 屏高（下拉 openUp 几何）
    bool half;              // 半宽卡：与相邻半宽卡两两成行（双列配对，UAT3 多列分栏）
    void (*draw)(const CardDrawArgs&);
    bool (*visible)();      // nullptr = 恒显；否则返回 false 时整卡跳过（不占高度）
};

// 新增/调整字段必须同步**全部行**（聚合初始化按位置匹配，漏补 = 静默错位）。
const CardSpec kCardSpecs[] = {
    {kTabGeneral, "settings.card.appearance", nullptr, CardHeightKind::Fixed,
     kAppearanceCardHeight, false, /*half=*/true, &drawAppearanceCard, nullptr},
    {kTabGeneral, "settings.card.editor", nullptr, CardHeightKind::Fixed,
     kEditorFontCardHeight, true, /*half=*/true, &drawEditorFontCard, nullptr},
    // UAT5：Claude 代理卡自「模块」tab 迁「通用」tab（appearance|editor 配对行
    // 下方、保持全宽）；gate 语义保留（claude 模块关 → 整卡隐藏）。
    {kTabGeneral, "settings.card.proxy", kModuleIdClaudeProxy,
     CardHeightKind::ProxyWarnings, 0.0f, false, /*half=*/false, &drawProxyCard,
     nullptr},
    {kTabModules, "settings.card.modules", nullptr, CardHeightKind::Fixed,
     kModulesCardHeight, false, /*half=*/false, &drawModulesCard, nullptr},
    // 壁纸卡：平台能力不支持时整卡不建（不是禁用态）——visible 在卡区入口判定，
    // 不占高度、不留空白；开关链路与模块插拔卡同走 applyModuleEnabled。
    {kTabModules, "settings.card.wallpaper", nullptr, CardHeightKind::Fixed,
     kWallpaperCardHeight, false, /*half=*/false, &drawWallpaperCard,
     &::platform::wallpaper::supported},
    {kTabLog, "settings.card.log", nullptr, CardHeightKind::Fixed,
     kLogSettingsCardHeight, true, /*half=*/false, &drawLogCard, nullptr},
    {kTabLog, "settings.tab.log", nullptr, CardHeightKind::FillRemainder,
     0.0f, false, /*half=*/false, &drawLogViewCard, nullptr},
    {kTabSync, "settings.card.github_sync", nullptr, CardHeightKind::GithubSync,
     0.0f, false, /*half=*/false, &drawGithubSyncCard, nullptr},
};

// ── GitHub Sync（纯 API 方案，无 git 依赖）──
// 远端路径常量在 github_sync_logic.h（kRemoteSqlPath / remoteNotePath）。
// skip 表清单单一来源 = core::sql_dump::defaultSkipTables()，不得本地另抄。

// epoch 秒（节流窗口与备份文件名共用）。
int64_t nowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

class SettingsModule;

namespace {
// 退出钩子目标：onStart 挂上、onStop/析构摘掉；main 的 atexit 经
// runGithubSyncExitPush 间接调用（见 github_sync_exit_hook.h 的注册序约定）。
SettingsModule* gExitHookTarget = nullptr;

// 测试缝目标（见 github_sync_test_seam.h）：与 gExitHookTarget 同款生命周期，
// 但刻意分开两个指针——退出钩子在真实运行时也活跃，测试缝只在测试门开时用，
// 混用一个指针会让「谁把目标清空」变得不可分辨。
SettingsModule* gSyncTestTarget = nullptr;
}  // namespace

class SettingsModule : public ::core::Module {
public:
    // 退出钩子目标兜底摘除：用例/异常路径下 onStop 未跑时，atexit 不得打到悬垂指针。
    ~SettingsModule() override {
        if (gExitHookTarget == this) gExitHookTarget = nullptr;
        if (gSyncTestTarget == this) gSyncTestTarget = nullptr;
    }

    ::core::PageId pageId() const override { return ::core::PageId::Settings; }
    std::string displayName() const override { return "设置"; }
    std::string shortName() const override { return "设置"; }
    std::string iconGlyph() const override { return eui::utf8(0xF013); }  // fa-gear

    // ── 测试缝（见 github_sync_test_seam.h；只有该头的自由函数经 gSyncTestTarget 调它）──
    // 公开是因为自由函数不是友元；不构成生产 API——门禁未开时自由函数根本不转发。
    GithubSyncTestResult runGithubSyncForTest(GithubSyncTestAction action,
                                              bool manual, int timeoutMs);
    int runLocalGitProbeForTest();

    void onStart() override {
        store_ = std::make_unique<SettingsStore>(::core::appDatabase());
        cfg_ = store_->load();
        githubSyncStore_ = std::make_unique<GithubSyncStore>(::core::appDatabase());
        githubSyncCfg_ = githubSyncStore_->load();
        // 上次退出推送未完成 → 卡片黄字「有未同步改动」，同步成功即清。
        if (auto notice = githubSyncStore_->hasUnsyncedNotice();
            notice.isOk() && notice.value()) {
            githubSyncUnsyncedNotice_ = true;
        }
        // 本地 git 探测：后台线程 + 结果缓存，UI 线程不因
        // xcode-select 卡住启动；配置变更置脏重探。
        githubSyncLocalGitProbeDirty_ = true;
        gExitHookTarget = this;   // 进程退出钩子的目标，onStop 注销
        gSyncTestTarget = this;   // 测试缝目标（见 github_sync_test_seam.h），onStop 注销
        // dirty 触发源：笔记写入/新建/重命名/删除 + 待办增删改 + 待办列增删改重排改色——
        // 每个写路径事件都要订上，漏一个 = 那类改动永远不自动同步。token 必须存成员，
        // 丢了就退订。事件名直接作 source：日志里看得出是哪条写路径置的位。
        for (const char* ev :
             {::core::event::kMdSaved, ::core::event::kMdCreated,
              ::core::event::kMdRenamed, ::core::event::kMdDeleted,
              ::core::event::kMdTagsChanged,
              ::core::event::kTodoCreated, ::core::event::kTodoUpdated,
              ::core::event::kTodoMoved, ::core::event::kTodoCompleted,
              ::core::event::kTodoUncompleted, ::core::event::kTodoDeleted}) {
            githubSyncSubs_.push_back(::core::event::bus().subscribe(
                ev,
                [this, ev](const std::string&) { markGithubSyncDirty(ev); }));
        }
        // 注册 app.read_settings；name 冲突记日志不中断。
        appReadSettingsTool_ =
            makeAppReadSettingsTool(::core::appDatabase());
        auto toolReg =
            ::core::ToolRegistry::instance().registerTool(appReadSettingsTool_);
        if (toolReg.isFail()) {
            LOG_ERROR("Settings: app.read_settings 注册失败: " +
                      toolReg.errorMessage());
        }
        applyThemeRuntime();
        // 启动时恢复强调色（load 期写入 cfg_.accentTheme，sanitized 已保证合法值）
        {
            ::core::theme::Accent a;
            if (cfg_.accentTheme == "purple")      a = ::core::theme::Accent::Purple;
            else if (cfg_.accentTheme == "pink")   a = ::core::theme::Accent::Pink;
            else if (cfg_.accentTheme == "blue")   a = ::core::theme::Accent::Blue;
            else                                    a = ::core::theme::Accent::Clay;
            ::core::theme::setAccent(a);
        }
        ::core::theme::setFontScale(
            static_cast<float>(cfg_.fontScalePercent) / 100.0f);
        // 编辑器字号经 core::theme 通道传给 markdown（模块隔离）。
        ::core::theme::setEditorFontSize(
            static_cast<float>(cfg_.editorFontSize));
        ::core::log::setLevel(parseLogLevel(cfg_.logLevel));
        LOG_INFO("Settings 模块启动（主题=" + ::core::theme::currentThemeName() +
                 "（模式=" + cfg_.themeMode + "）" +
                 ", 字号=" + std::to_string(cfg_.fontScalePercent) + "%" +
                 ", 编辑器字号=" + std::to_string(cfg_.editorFontSize) + "px" +
                 ", 日志级别=" + cfg_.logLevel +
                 ", 日志保留=" + retentionLabel(cfg_.logRetentionDays) + "）");
        // 日志 tail 首次装载同步做一次：首帧必须有值，异步首刷会闪一帧「暂无日志内容」。
        applyLogTail(readLogTailLines());
        refreshExternalState();
    }

    void onStop() override {
        // 先收后台线程再放资源：GitHub Sync 线程体捕获 this，join 前释放任何成员都会悬垂。
        if (githubSyncThread_.joinable()) githubSyncThread_.join();
        // 本地 git 探测在途的 future 也捕获 this → 等它收束（探测本身有 3s 超时上界）。
        if (githubSyncLocalGitProbeFuture_.valid()) {
            githubSyncLocalGitProbeFuture_.wait();
            githubSyncLocalGitProbeFuture_ = {};
        }
        if (gExitHookTarget == this) gExitHookTarget = nullptr;
        if (gSyncTestTarget == this) gSyncTestTarget = nullptr;
        // 退订 dirty 事件（token 析构即退订；不清会留下悬垂 this 的 handler）。
        githubSyncSubs_.clear();
        // 先注销工具再释放（registry find() 返回 shared_ptr keep-alive，在跑 run 不会悬垂）。
        ::core::ToolRegistry::instance().unregisterTool("app.read_settings");
        appReadSettingsTool_.reset();
        store_.reset();
        LOG_INFO("Settings 模块停止");
    }

    void onPageEnter() override {
        // auto 模式的跟随点：EUI-NEO 无系统主题变更回调，只能回本页时重新解析。
        if (cfg_.themeMode == kThemeModeAuto) {
            applyThemeRuntime();
        }
        // 进页面 = 外部状态刷新点；变化频率低，不需要每帧重查。
        refreshExternalState();
        // H1: 进页面不自动弹错误弹窗——错误留在卡片里、用户点「查看详情」才弹
        githubSyncErrorDialog_.open = false;
    }

    // Ctrl+S：端口草稿非空且 ≠ 已应用端口 → applyProxyPort，否则重写配置 + 回显「已保存」。
    bool onKeyEvent(const eui::KeyEvent& key) override {
        if (key.action != eui::KeyAction::Press) {
            return false;
        }
        // Esc 关当前打开的下拉（下拉独占打开 → 关它 = 关最上层）；全关时不消费。
        if (key.key == eui::InputKey::Escape) {
            if (openDropdown_ < 0) return false;
            openDropdown_ = -1;
            return true;
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

    // 按 themeMode 解析并应用深浅（系统检测在模块层：module 可依赖 platform 而 core 不可）。
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

    // 字号无级调节（UAT6.2，80-130%）：钳制后沿用切档链路（setFontScale → 唤醒
    // compose → 落盘）。同值早退是拖拽链的量化契约：slider onChange 在拖拽中每次
    // 归一化变动都回调，只有量化到 1% 且与现值不同才走落库链。
    void applyFontScalePercent(int percent) {
        percent = std::clamp(percent, kMinFontScalePercent, kMaxFontScalePercent);
        if (percent == cfg_.fontScalePercent) return;
        cfg_.fontScalePercent = percent;
        ::core::theme::setFontScale(
            static_cast<float>(cfg_.fontScalePercent) / 100.0f);
        // 切档即生效不依赖点击输入顺带触发的 composeRequested：apply 路径显式
        // 唤醒下一帧 compose（幂等——同帧已请求时多一次重排无害，但把「立即
        // 反映」变成 apply 链自带属性；UAT6 字号设置实时生效）。
        core::platform::requestUiUpdate();
        persist();
        setStatus("字号缩放：" + std::to_string(cfg_.fontScalePercent) +
                  "%（下一帧全局生效）");
        LOG_INFO("Settings: 字号缩放 → " + std::to_string(cfg_.fontScalePercent) +
                 "%（core::theme token 层全局缩放）");
    }

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

    // 窗口效果三档切换：即时生效；系统不支持时自动降级（UAT3 #10 起降级提示行
    // 不再显示，行为静默但档位照常落）。
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
        // 0 = 永久：跳过清理；否则切档立即执行一次滚动清理并反馈条数。
        if (cfg_.logRetentionDays == 0) {
            setStatus("日志保留：永久保留（不清理历史日志）");
            LOG_INFO("Settings: 日志保留 → 永久保留（跳过清理）");
            return;
        }
        auto* db = ::core::appDatabase();
        if (db == nullptr) {
            setStatus("日志保留：" + retentionLabel(cfg_.logRetentionDays) +
                      "（数据库未就绪，重启后生效）");
            LOG_WARN("Settings: appDatabase 未注册，跳过立即清理");
            return;
        }
        auto pruned = ::core::pruneCallLogs(*db, cfg_.logRetentionDays);
        if (pruned.isOk()) {
            setStatus("日志保留：" + retentionLabel(cfg_.logRetentionDays) +
                      "，已清理 " +
                      std::to_string(pruned.value()) + " 条历史日志");
            LOG_INFO("Settings: 日志保留 → " +
                     retentionLabel(cfg_.logRetentionDays) + "，已清理 " +
                     std::to_string(pruned.value()) + " 条");
        } else {
            setStatus("日志保留：" + retentionLabel(cfg_.logRetentionDays) +
                      "（清理失败：" + pruned.errorMessage() + "）", true);
            LOG_ERROR("Settings: 立即清理失败: " + pruned.errorMessage());
        }
    }

    // 「随应用启动代理」开关（默认关，false 时 main 只注册单例、不拉起 8888）。
    void applyAutoStart(bool on) {
        cfg_.claudeProxyAutoStart = on;
        persist();
        setStatus(on ? "代理将随应用启动（下次启动生效）"
                     : "代理不再随应用启动（下次启动生效）");
        LOG_INFO("Settings: claudeProxyAutoStart → " +
                 std::string(on ? "true" : "false"));
    }

    // 「开机自启动」开关（UAT6）：先写平台注册项（Windows HKCU Run / macOS
    // LaunchAgents，注册内容带 --minimized），回读真值后再持久化——写失败时
    // cfg/开关回到真值，不停留在错态。设置页显示的恒是注册真值（防漂移）。
    void applyAutostartEnabled(bool on) {
        platform::autostart::AutostartController ctl(
            platform::autostart::systemOps());
        const auto r = ctl.setRegistered(on);
        autostartRegistered_ = ctl.isRegistered();  // 成败都回读真值
        cfg_.autostartEnabled = autostartRegistered_;  // 意图对齐真值再落库
        persist();
        if (r.isFail()) {
            setStatus((on ? "开启开机自启动失败: " : "关闭开机自启动失败: ") +
                          r.errorMessage(), true);
            LOG_ERROR("Settings: 开机自启动注册态写入失败（→ " +
                      std::string(on ? "on" : "off") +
                      "）: " + r.errorMessage());
            return;
        }
        setStatus(autostartRegistered_
                      ? "开机自启动已开启：下次开机自动运行并最小化到托盘"
                      : "开机自启动已关闭");
        LOG_INFO("Settings: 开机自启动 → " +
                 std::string(autostartRegistered_ ? "启用" : "停用") +
                 "（注册态已写入平台）");
    }

    // 「动态背景」开关（UAT6 主窗活体流质）：cfg 落库 + app 层运行时镜像同步
    // （compose 门控读镜像，app_sidebar 持有），下一帧即撤/挂帧泵——关闭后当帧
    // 不再挂泵（零成本），开启当帧恢复漂移。app::setFluidBackgroundEnabled 经
    // 前置声明接入（模块不 include app 层头，单一符号桥）。
    void applyFluidBackgroundEnabled(bool on) {
        cfg_.fluidBackgroundEnabled = on;
        ::app::setFluidBackgroundEnabled(on);
        core::platform::requestUiUpdate();  // 唤醒下一帧 compose（开→挂泵/关→撤泵）
        persist();
        setStatus(on ? "动态背景已开启（主窗口背景液态流质）"
                     : "动态背景已关闭（恢复静态背景）");
        LOG_INFO("Settings: fluidBackgroundEnabled → " +
                 std::string(on ? "true" : "false"));
    }

    // 应用代理端口：重启失败时代理已停、sidecar 已还原，状态栏提示可换端口重试。
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

        auto* svc = claudeProxy();
        if (svc) {
            svc->setConfiguredProxyPort(port);
        }
        if (!svc || !svc->isProxyRunning()) {
            // 端口生效时机两处必须同步：① main.cpp assembleClaudeServiceNow 装配注入；② 会话内启用 claude 模块时的惰性装配。
            setStatus("代理端口已保存：" + std::to_string(oldPort) + " → " +
                      std::to_string(port) +
                      "（代理未运行：手动启动代理，或启用 claude 模块时生效）");
            LOG_INFO("Settings: 代理端口 " + std::to_string(oldPort) + " → " +
                     std::to_string(port) +
                     "（代理未运行；手动启动代理 / 本会话内启用 claude 模块时生效）");
            return;
        }
        svc->stopProxy();
        auto r = svc->startProxy("127.0.0.1", port);
        // stop→start 会改写/删除 sidecar；就地刷新快照（成败都刷）。
        refreshExternalState();
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

    // 日志级别切档同时置 logTailDirty_：否则同屏 tail 快照不动，用户看不到切档效果。
    void applyLogLevelPreset(int index) {
        if (index < 0 || index >= kLogLevelPresetCount) return;
        cfg_.logLevel = kLogLevelPresets[index];
        ::core::log::setLevel(parseLogLevel(cfg_.logLevel));
        persist();
        logTailDirty_ = true;   // 切档后可见面即时跟随
        setStatus("日志级别：" + cfg_.logLevel + "（即时生效）");
        LOG_INFO("Settings: 日志级别 → " + cfg_.logLevel + "（core::log::setLevel）");
    }

    void setProxyPortDraft(const std::string& v) {
        proxyPortDraft_ = v;
        // 校验错误态随下一次输入清除（UAT5 页头小字删除后 status* 仅内部状态，
        // 无渲染面；清错逻辑保留给后续可能的新反馈通道）。
        if (statusIsError_) {
            statusMessage_.clear();
            statusIsError_ = false;
        }
    }

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
        app::toggle::onModuleToggled(moduleId, on);   // 即时生效（桥内处理反馈）
        setStatus((on ? "模块已开启：" : "模块已关闭：") + moduleId);
        LOG_INFO("Settings: 模块开关 " + moduleId + " → " +
                 (on ? "启用" : "关闭"));
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

    // 桥失败分支走 toast（页头副标题反馈太弱）；节点 id = settings.toast。
    void showToast(const std::string& title, const std::string& msg = "",
                   bool isError = false) {
        toast_.show(title, msg, isError);
    }

    void drawToast(eui::Ui& ui, const eui::Screen& screen,
                   const core::theme::Tokens& t) {
        ui_kit::drawToast(ui, screen, "settings.toast", toast_,
                          core::theme::componentTokensWeChat(t),
                          ui_kit::ToastOptions{.okIcon = 0xF058});
    }

    // ── GitHub Sync 操作方法 ──

    void applyGithubSyncEnabled(bool on) {
        githubSyncCfg_.enabled = on;
        if (githubSyncStore_) githubSyncStore_->save(githubSyncCfg_);
        setStatus(on ? "GitHub 同步已启用" : "GitHub 同步已禁用");
        LOG_INFO("Settings: GitHub Sync " + std::string(on ? "启用" : "关闭"));
        if (on) maybeStartFirstGithubSync();
    }

    // 首次方向：启用开关打开且配置齐全时跑一次首次同步
    //（先校验连通性，过了才判方向：远端有数据 → 拉取覆盖；没有 → 首次上传）。
    // 只跑一次：成功后落 first_sync_done，repoUrl/branch 变更即清除（换仓 = 重新判定方向）。
    // 配置不齐（URL/Token 缺）静默跳过——用户手动操作时自会得到明确报错，
    // 不能在拨开关的瞬间弹错误弹窗。
    void maybeStartFirstGithubSync() {
        if (githubSyncThread_.joinable()) return;
        if (!githubSyncStore_) return;
        if (githubSyncCfg_.repoUrl.empty()) return;
        auto done = githubSyncStore_->isFirstSyncDone();
        if (done.isOk() && done.value()) return;
        const bool hasToken = !githubSyncTokenDraft_.empty() ||
                              githubSyncStore_->loadToken().isOk();
        if (!hasToken) return;
        startGithubSyncAction(SyncAction::ValidateFirstSync, /*manual=*/true);
    }

    // 目标仓库/分支变了 → 首次方向重新判定（下次启用或手动同步生效）。
    void clearGithubSyncFirstSyncFlag(const char* what) {
        if (!githubSyncStore_) return;
        if (auto r = githubSyncStore_->clearFirstSyncDone(); r.isFail()) {
            LOG_WARN(std::string("Settings: 清除首次同步标记失败（") + what +
                     "）: " + r.errorMessage());
        }
    }

    void githubSyncPush() {
        // 换仓/换分支会清 first_sync_done → 手动同步先重跑首次方向判定
        //（否则用户必须重拨启用开关才有方向判定）。FirstSync 内含测试连接三步校验，
        // 与启用流程同一道闸：首次方向判定会覆盖本地库或上传全量，不能跳过校验。
        if (githubSyncStore_ && !githubSyncCfg_.repoUrl.empty()) {
            auto done = githubSyncStore_->isFirstSyncDone();
            if (done.isOk() && !done.value()) {
                LOG_INFO("Settings: 首次方向标记缺失（换仓/换分支或首次启用），"
                         "手动同步改跑 FirstSync");
                startGithubSyncAction(SyncAction::FirstSync, /*manual=*/true);
                return;
            }
        }
        startGithubSyncAction(SyncAction::Push, true);
    }

    void githubSyncPull() { startGithubSyncAction(SyncAction::Pull, true); }

    void applyGithubSyncTestConnection() {
        startGithubSyncAction(SyncAction::Test, true);
    }

    // 409 冲突弹窗的主动作：拉取远端后再推（二次确认已在弹窗内完成）。
    void githubSyncPullThenPush() {
        startGithubSyncAction(SyncAction::PullThenPush, true);
    }

    // 失败弹窗「重试」：按上次动作重跑（无上次 → 测试连接）。
    void retryGithubSync() {
        closeGithubSyncErrorDialog();
        startGithubSyncAction(githubSyncLastAction_ == SyncAction::None
                                  ? SyncAction::Test
                                  : githubSyncLastAction_,
                              true);
    }

    void showGithubSyncErrorDetail() { githubSyncErrorDialog_.open = true; }

    void closeGithubSyncErrorDialog() {
        githubSyncErrorDialog_.open = false;
        githubSyncErrorDialog_.conflictConfirmArmed = false;
    }

    // 复制详情：文本由弹窗拼好，这里只落 EUI-NEO 剪贴板 + 回执。
    void copyGithubSyncDetail(const std::string& text) {
        ::core::window::setClipboardText(text);
        showToast("已复制详情");
    }

    // 自动同步判定：过 gsync::autoPushDue 才推（启用 + 自动推送开关 + dirty +
    // 节流窗口）；手动「立即同步」不走本函数，故不受开关与窗口限制。
    void maybeAutoGithubSync() {
        if (!gsync::autoPushDue(githubSyncCfg_.enabled, githubSyncCfg_.autoPush,
                                githubSyncDirty_, githubSyncLastCommitSec_,
                                nowSec())) {
            return;
        }
        if (githubSyncThread_.joinable()) return;
        startGithubSyncAction(SyncAction::Push, /*manual=*/false);
    }

    // dirty 触发源：笔记写入 + 待办增删改。其他配置变更不主动触发。
    void markGithubSyncDirty(const char* source) {
        if (githubSyncDirty_) return;
        githubSyncDirty_ = true;
        LOG_INFO(std::string("Settings: GitHub Sync 置 dirty（来源=") + source + "）");
    }

    // 无 git 警告：用户改配置时提示一次，不阻塞同步、不在启动/失败时弹。
    void maybeWarnNoLocalGit() {
        if (githubSyncLocalGitProbe_.load(std::memory_order_acquire) != 0 ||
            githubSyncNoGitWarned_) {
            return;   // 探测未回（-1）/ 可用（1）/ 已警告过 → 不弹
        }
        githubSyncNoGitWarned_ = true;
        showToast("本地未检测到 git",
                  "同步仍可用，但没有本地版本历史（远端 + 本地备份兜底）", false);
    }

    // 本地 git 探测：后台线程跑 + 结果缓存，UI 线程零阻塞；配置变更
    // （仓库/分支/Token）置脏 → 下一帧重探，会话内装好 git 灰字会消失。
    // 探测自身带超时上界（平台实现层），在途 future 由 onStop 收束。
    void maybeRefreshLocalGitProbe() {
        if (githubSyncLocalGitProbeFuture_.valid()) {
            if (githubSyncLocalGitProbeFuture_.wait_for(
                    std::chrono::seconds(0)) != std::future_status::ready) {
                return;   // 在途
            }
            githubSyncLocalGitProbeFuture_ = {};   // 就绪即回收，避免 valid() 恒真
            return;
        }
        if (!githubSyncLocalGitProbeDirty_) return;
        githubSyncLocalGitProbeDirty_ = false;
        githubSyncLocalGitProbeFuture_ = std::async(std::launch::async, [this] {
            const bool available = localGitAvailable();
            githubSyncLocalGitProbe_.store(available ? 1 : 0,
                                           std::memory_order_release);
            core::platform::requestUiUpdate();
        });
    }

    // 退出补推：dirty 且距上次 commit ≥60s 且自动推送开关开着才推；最多等 5s，
    // 到点未完即放弃并置未同步提示键 → 下次启动卡片黄字。manual=true 绕开 5 分钟
    // 节流（退出补推是一次性的）。
    // 等待/放行逻辑见 waitOrDetachSyncThread；已有任务在飞时也走同一条等待，
    // 否则 onStop 的无界 join 会把退出拖到整轮推送结束（断网时数十秒~分钟）。
    void runExitSyncPush() {
        if (githubSyncStore_ == nullptr) return;
        if (githubSyncThread_.joinable()) {
            // 自动/手动同步在飞：退出不等它跑完（onStop 的 join 也不得等）
            waitOrDetachSyncThread();
            return;
        }
        if (!githubSyncCfg_.enabled || !githubSyncCfg_.autoPush ||
            !githubSyncDirty_) {
            // 关掉自动推送 = 用户明确选择「只手动推」：不推，但「有改动没上传」这个
            // 事实必须留信号——否则退出后既不推也没黄字，用户会以为已经同步过了。
            if (githubSyncCfg_.enabled && !githubSyncCfg_.autoPush &&
                githubSyncDirty_) {
                if (auto r = githubSyncStore_->setUnsyncedNotice(); r.isFail()) {
                    LOG_WARN("Settings: 未同步提示落库失败: " + r.errorMessage());
                }
            }
            return;
        }
        if (!gsync::exitPushDue(githubSyncLastCommitSec_, nowSec())) return;
        LOG_INFO("Settings: 退出补推启动");
        startGithubSyncAction(SyncAction::Push, /*manual=*/true);
        waitOrDetachSyncThread();
    }

    // 线程侧取消判据（退出闸置位后，各阶段边界尽快收束）。
    bool syncAborted() const {
        return githubSyncAbortRequested_.load(std::memory_order_acquire);
    }

    // 退出路径的等待闸：最多等 5s（50 × 100ms），到点 detach 放行。
    // detach 后仍有窗口：SettingsModule 与 appDatabase 都是函数局部 static，析构
    // 晚于本 atexit 钩子但仍在退出过程中 → 残留下限 = 取消位把窗口压到「一次网络
    // 调用 + 一个阶段边界」；残余风险 = 退出瞬间线程撞 static 析构（表现为退出期
    // 崩溃，SQLite 事务性保证不脏库）。彻底方案是同步线程挂进程生命期持有（未做）。
    // detach 后 githubSyncThread_.joinable()==false，onStop 的 join 直接跳过，退出
    // 不被整轮推送阻塞。
    void waitOrDetachSyncThread() {
        // 先置取消位再等：线程在下一个阶段边界收束，多数情况下 5s 内自然完成，
        // 无需走到 detach。
        githubSyncAbortRequested_.store(true, std::memory_order_release);
        for (int i = 0; i < 50; ++i) {
            if (githubSyncDone_.load(std::memory_order_acquire)) {
                LOG_INFO("Settings: 退出补推 5 秒内完成");
                return;   // 已完成：onStop 的 join 瞬间返回
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (githubSyncDirty_) {
            if (auto r = githubSyncStore_->setUnsyncedNotice(); r.isFail()) {
                LOG_WARN("Settings: 未同步提示落库失败: " + r.errorMessage());
            }
        }
        if (githubSyncThread_.joinable()) githubSyncThread_.detach();
        LOG_WARN("Settings: 退出补推 5 秒内未完成，已放弃等待并放行线程"
                 "（下次启动卡片提示）");
    }

    // 前置校验失败等 UI 线程内的即时错误：进错误行，不自动弹弹窗。
    // H1: 错误留在卡片内（警告色+图标），用户点「查看详情」才弹。
    void setGithubSyncError(const std::string& msg) {
        githubSyncStatus_ = msg;
        githubSyncStatusError_ = true;
        githubSyncError_ = msg;
        githubSyncErrorDialog_.variant = GitErrorDialogVariant::Error;
        githubSyncErrorDialog_.conflictConfirmArmed = false;
        githubSyncErrorDialog_.selectedLogIndex = 0;
        LOG_WARN("Settings: GitHub Sync " + msg);
    }

    // 发起一个后台同步动作。三按钮在运行中已禁用，这里的 joinable 判定是兜底；
    // 但失败弹窗「重试」不在按钮禁用面内（弹窗按钮恒可点）——已有同步在飞时必须
    // 给反馈（G 批遗留：静默返回被用户当「重试点了没反应」），自动路径保持静默。
    // runExitSyncPush 不受影响：joinable 时它先走 waitOrDetachSyncThread 早退，
    // 不会进入本分支（atexit 阶段弹 toast 也无人可见）。
    void startGithubSyncAction(SyncAction action, bool manual) {
        harvestGithubSync();
        if (githubSyncThread_.joinable()) {
            if (manual) {
                showToast("已有同步在进行中", "请等待本轮同步结束后再操作");
                LOG_INFO("Settings: GitHub Sync 已有任务在飞，忽略手动请求（action=" +
                         std::to_string(static_cast<int>(action)) + "）");
            }
            return;
        }
        if (githubSyncCfg_.repoUrl.empty()) {
            setGithubSyncError("未配置仓库 URL");
            return;
        }
        // 节流：自动同步 5 分钟内最多一次；手动「立即同步」绕开。
        if (!manual && !gsync::throttleDue(githubSyncLastCommitSec_, nowSec())) {
            return;
        }
        std::string token = githubSyncTokenDraft_;
        if (token.empty() && githubSyncStore_) {
            auto t = githubSyncStore_->loadToken();
            if (t.isOk()) token = t.value();
        }
        if (token.empty()) {
            setGithubSyncError("未配置 Token（请在上方填写后再操作）");
            return;
        }
        // 用户在本次会话里填过 Token → 落库（下次启动仍可用）。
        if (!githubSyncTokenDraft_.empty() && githubSyncStore_) {
            auto saved = githubSyncStore_->saveToken(githubSyncTokenDraft_);
            if (saved.isFail()) {
                LOG_WARN("Settings: GitHub Sync Token 落库失败: " +
                         saved.errorMessage());
            }
        }

        githubSyncLastAction_ = action;
        githubSyncLastManual_ = manual;
        githubSyncRunning_ = true;
        githubSyncAbortRequested_.store(false, std::memory_order_release);
        githubSyncError_.clear();
        githubSyncStatus_.clear();
        githubSyncStatusError_ = false;
        githubSyncLogs_.clear();
        githubSyncBranchHint_.clear();
        {
            std::lock_guard<std::mutex> lk(githubSyncMutex_);
            githubSyncStagedOverwriteFiles_.clear();
            githubSyncStagedDeleteSkipHint_.clear();
        }
        githubSyncOverwriteFiles_.clear();
        githubSyncDeleteSkipHint_.clear();
        publishGithubSyncProgress("① 准备中…");
        githubSyncProgress_ = "① 准备中…";

        const std::string repoUrl = githubSyncCfg_.repoUrl;
        const std::string branch = githubSyncCfg_.branch;
        githubSyncThread_ = std::thread([this, action, repoUrl, token, branch] {
            // 编排已抽进 GithubSyncEngine（依赖注入面见 github_sync_engine.h）：
            // 引擎不持线程，run() 同步阻塞跑完 → 结果一次性搬进 staged_*，
            // harvest 的搬运点不变（join 即 happens-before，Result 无需原子）。
            SyncEngineDeps deps;
            deps.db = ::core::appDatabase();
            deps.store = githubSyncStore_.get();
            deps.nowSec = [] { return nowSec(); };
            deps.progress = [this](const std::string& text) {
                publishGithubSyncProgress(text);
            };
            deps.aborted = [this] { return syncAborted(); };
            GithubSyncEngine engine(std::move(deps));
            const SyncRunResult result =
                engine.run(action, repoUrl, token, branch);
            {
                std::lock_guard<std::mutex> lk(githubSyncMutex_);
                githubSyncStagedLogs_ = result.logs;
                githubSyncStagedOk_ = result.ok;
                githubSyncStagedSummary_ = result.summary;
                githubSyncStagedConflict_ = result.conflict;
                githubSyncStagedPublicRepo_ = result.publicRepo;
                githubSyncStagedBranchHint_ = result.branchHint;
                githubSyncStagedOverwriteFiles_ = result.overwriteFiles;
                if (!result.deleteSkipHint.empty()) {
                    githubSyncStagedDeleteSkipHint_ = result.deleteSkipHint;
                }
                if (result.dbImported) {
                    githubSyncStagedDbImported_.store(
                        true, std::memory_order_release);
                }
            }
            githubSyncDone_.store(true, std::memory_order_release);
            if (!syncAborted()) {
                // 就绪之后才唤醒（次序反了 UI 抢跑读不到结果）；退出中不唤醒
                core::platform::requestUiUpdate();
            }
        });
        LOG_INFO("Settings: GitHub Sync 后台任务启动（action=" +
                 std::to_string(static_cast<int>(action)) +
                 (manual ? "，手动" : "，自动") + "）");
    }

    // 本地 git 探测：平台差异在 local_git_probe_*.cpp
    //（macOS 直接调 /usr/bin/git 会弹 CLT 安装 GUI，故按平台拆、只查不执行）。

    // 后台线程写阶段文案（UI 每帧消费）。
    void publishGithubSyncProgress(const std::string& text) {
        std::lock_guard<std::mutex> lk(githubSyncMutex_);
        githubSyncProgressPending_ = text;
    }

    // compose 每帧：① 消费进度；② 任务完成则 join + 落结果（失败弹详情 / 409 弹冲突）。
    void harvestGithubSync() {
        {
            std::lock_guard<std::mutex> lk(githubSyncMutex_);
            if (!githubSyncProgressPending_.empty()) {
                githubSyncProgress_ = githubSyncProgressPending_;
                githubSyncProgressPending_.clear();
            }
        }
        if (!githubSyncThread_.joinable()) return;
        if (!githubSyncDone_.load(std::memory_order_acquire)) return;
        githubSyncThread_.join();   // join 即 happens-before，之后读 staged_* 安全
        githubSyncDone_.store(false, std::memory_order_release);
        githubSyncRunning_ = false;
        githubSyncProgress_.clear();
        bool conflict = false;
        {
            std::lock_guard<std::mutex> lk(githubSyncMutex_);
            githubSyncLogs_ = githubSyncStagedLogs_;
            githubSyncResultOk_ = githubSyncStagedOk_;
            githubSyncResultSummary_ = githubSyncStagedSummary_;
            conflict = githubSyncStagedConflict_;
            if (githubSyncStagedPublicRepo_) githubSyncRepoPublic_ = true;
            if (!githubSyncStagedBranchHint_.empty()) {
                githubSyncBranchHint_ = githubSyncStagedBranchHint_;
            }
            githubSyncStagedLogs_.clear();
            githubSyncProgressPending_.clear();
            githubSyncOverwriteFiles_ = githubSyncStagedOverwriteFiles_;
            githubSyncStagedOverwriteFiles_.clear();
            if (!githubSyncStagedDeleteSkipHint_.empty()) {
                githubSyncDeleteSkipHint_ = githubSyncStagedDeleteSkipHint_;
                githubSyncStagedDeleteSkipHint_.clear();
            }
        }
        // 导入覆盖过库 → 本模块缓存重载（store 指针仍有效，只重读配置）。
        const bool dbImportedThisRun =
            githubSyncStagedDbImported_.exchange(false, std::memory_order_acq_rel);
        githubSyncLastConflict_ = conflict;
        githubSyncLastDbImported_ = dbImportedThisRun;
        if (dbImportedThisRun) {
            if (githubSyncStore_) {
                githubSyncCfg_ = githubSyncStore_->load();
                LOG_INFO("Settings: 库导入后已重载 GitHub Sync 配置缓存");
            }
        }
        githubSyncStatus_ = githubSyncResultSummary_;
        githubSyncStatusError_ = !githubSyncResultOk_;
        if (githubSyncResultOk_) {
            // 清 dirty / 记节流时点 / 清退出未同步提示只认「真推送过」的动作——
            // 测试连接与纯拉取不推本地改动，点了它们不得抹掉未同步语义。
            const bool pushedToRemote =
                githubSyncLastAction_ == SyncAction::Push ||
                githubSyncLastAction_ == SyncAction::PullThenPush ||
                githubSyncLastAction_ == SyncAction::FirstSync ||
                githubSyncLastAction_ == SyncAction::ValidateFirstSync;
            if (pushedToRemote) {
                // commit 成功 → 清 dirty + 记时刻（节流窗口起点）。
                githubSyncDirty_ = false;
                githubSyncLastCommitSec_ = nowSec();
                // 同步成功即清「退出时未同步」提示（黄字随下一帧消失）。
                if (githubSyncUnsyncedNotice_) {
                    githubSyncUnsyncedNotice_ = false;
                    if (githubSyncStore_) {
                        if (auto r = githubSyncStore_->clearUnsyncedNotice();
                            r.isFail()) {
                            LOG_WARN("Settings: 未同步提示清除失败: " +
                                     r.errorMessage());
                        }
                    }
                }
            }
            githubSyncError_.clear();
            // 首次方向跑成功 → 落「首次已完成」（repoUrl/branch 变更时清除）。
            if ((githubSyncLastAction_ == SyncAction::FirstSync ||
                 githubSyncLastAction_ == SyncAction::ValidateFirstSync) &&
                githubSyncStore_) {
                if (auto r = githubSyncStore_->setFirstSyncDone(); r.isFail()) {
                    LOG_WARN("Settings: 首次同步标记落库失败: " +
                             r.errorMessage());
                }
            }
            LOG_INFO("Settings: GitHub Sync 完成: " + githubSyncResultSummary_);
            return;
        }
        githubSyncError_ = githubSyncResultSummary_;
        // 失败也记节流时点（与成功路径同一变量）：dirty 不清（改动仍待推），但不
        // 记的话线程完成唤醒下一帧 maybeAutoGithubSync 立即重启 → 每轮全库 dump +
        // 多次 API 的自持重试风暴。手动重试不受影响（throttle 只拦自动路径）。
        githubSyncLastCommitSec_ = nowSec();
        githubSyncErrorDialog_.selectedLogIndex = 0;
        githubSyncErrorDialog_.conflictConfirmArmed = false;
        // 变体判定抽在 git_error_dialog 的纯函数里（可离线单测）：409 走冲突变体、
        // 401 走定向引导、其余通用。判据 = 请求日志状态码（SyncRunResult 不带错误码）。
        githubSyncErrorDialog_.variant =
            variantForSyncFailure(githubSyncLogs_, conflict);
        // H1: 手动失败不再自动弹弹窗——错误留在卡片内，用户点「查看详情」才弹。
        // 自动同步失败只留卡片红字 + 详情入口（无变更：本来就不弹）。
        // 启用流程的校验失败同自动路径（无变更：本来就不弹）。
        LOG_ERROR("Settings: GitHub Sync 失败: " + githubSyncResultSummary_);
    }

    // 错误详情弹窗（compose 每帧绘制；open=false 时整树仍建、面板透明）。
    void drawGithubSyncErrorDialog(eui::Ui& ui, const eui::Screen& screen,
                                   const core::theme::Tokens& t) {
        GitErrorDialogIn in;
        in.summary = githubSyncError_;
        in.logs = [this] { return githubSyncLogs_; };
        in.overwriteFiles = githubSyncOverwriteFiles_;

        GitErrorDialogActions a;
        a.retry = [this] { retryGithubSync(); };
        a.pullThenPush = [this] { githubSyncPullThenPush(); };
        a.updateToken = [this] {
            // 401 定向引导：关弹窗把用户送回卡片（Token 输入框就在卡片顶部），
            // 并给一句可执行的下一步。不动 githubSyncError_ —— 401 红字要留着，
            // 用户更新 Token 后点同步会自然覆盖它。
            closeGithubSyncErrorDialog();
            showToast("请在上方 Token 输入框更新后重新同步");
        };
        a.close = [this] { closeGithubSyncErrorDialog(); };
        a.copyDetail = [this](const std::string& text) {
            copyGithubSyncDetail(text);
        };
        a.selectLog = [this](int idx) {
            githubSyncErrorDialog_.selectedLogIndex = idx;
        };
        a.logScrolled = [this](float v) {
            githubSyncErrorDialog_.logScrollOffset = v;
        };
        drawGitErrorDialog(ui, screen, t, githubSyncErrorDialog_, in, a);
    }

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        const auto& t = core::theme::current();
        // GitHub Sync 后台任务的每帧收割（进度 + 完成落结果）；与 tab 无关，恒跑。
        harvestGithubSync();
        // 自动同步判定（dirty + 节流窗口）；手动按钮不走这里。
        maybeAutoGithubSync();
        // 本地 git 探测收割/重探：非阻塞，结果驱动卡片灰字。
        maybeRefreshLocalGitProbe();
        const float pad = t.sizes.spacingLarge * 2.0f;  // 32 —— S4.2 弹性内边距（spacingLarge=16×2=32）
        const float contentW = screen.width - pad * 2.0f;   // 宽 = 窗口宽 − 双侧内边距

        // 页面底色 = 主窗口 clearColor，不自铺不透明底。

        // UAT5 全局顶部收紧：共享 kHeaderH 已 64→40（本页先行档并入共享源，
        // kSettingsHeaderH 删除）。cardsTopY 公式不变（100% 档 40+40+16+40+16=152）。
        const float headerH = ui_kit::kHeaderH;
        const float tabH = kSettingsTabHeight;
        // 日志 tail 读盘在后台线程，渲染恒读快照 → 切 tab / 首帧不闪空。
        if (settingsTab_ == kTabLog) {
            refreshLogTailCache();
        }
        auto* svc = claudeProxy();
        if (!externalStateLoaded_) {
            refreshExternalState();
        }
        size_t warnLines = 0;
        if (svc) {
            // 警告行不以「已接管」为门：takeOver 写 sidecar 前失败时 sidecar 不存在，但失败原因恰是最该看见的一条。
            warnLines = std::min<size_t>(
                svc->lastSyncWarnings().size(), 2);
        }
        const float proxyH = proxyCardHeightFor(warnLines);
        // 模块插拔门控：读 cfg_（启动注册状态的来源），拨开关即时预览下次启动的布局。
        // UAT5 删目录卡后 markdown 开关不再影响设置页卡集，contentKey 只签 claude。
        const bool claudeOn =
            moduleEnabled(cfg_, kModuleIdClaudeProxy);
        const auto compTheme = core::theme::componentTokensWeChat(t);

        ui.column("settings.root")
            .size(screen.width, screen.height)
            .padding(pad, 0.0f, pad, pad)  // 上=0 标题顶格（2026-10-25 拍板）——EUI padding 序=(left,top,right,bottom)
            .gap(t.sizes.spacingLarge)
            .content([&] {
                drawHeader(ui, contentW, headerH, t);

                // tab 栏：S2.2 页面级大 Tab——内容宽（文字实宽+水平内边距 16×2）、
                // 枚间 4px、左对齐；等宽铺满会使单枚内边距远超 16 口径。文字实宽
                // 与 wtab 渲染同参（14px/默认字重 400）。4 Tab 顺序 + 标签不动
                // （kSettingTabLabels）；hit area id = settings.tabs.N.hit（测试按此点击）。
                ui.stack("settings.tabs.wrap")
                    .size(contentW, tabH)
                    .content([&] {
                        constexpr float kTabPadH = 16.0f;  // S2.2 水平内边距
                        constexpr float kTabGap = 4.0f;    // S2.2 Tab 间距
                        float tabX = 0.0f;
                        for (int i = 0; i < kSettingTabCount; ++i) {
                            const float tabW =
                                core::TextPrimitive::measureTextWidth(
                                    kSettingTabLabels[i], {},
                                    ui_kit::kWtabFontSize, 400) +
                                2.0f * kTabPadH;
                            ui_kit::wtab(
                                ui,
                                std::string("settings.tabs.") +
                                    std::to_string(i),
                                kSettingTabLabels[i], settingsTab_ == i,
                                tabX, 0.0f, tabW, tabH, t,
                                [this, i]() {
                                    settingsTab_ = i;
                                    if (i == kTabLog) logTailDirty_ = true;
                                    if (i == kTabModules)
                                        refreshExternalState();
                                });
                            tabX += tabW + kTabGap;
                        }
                    })
                    .build();

                // 当前 tab 的卡区仍套 scrollView（4+ 卡总高可超小窗口可视高）。
                const float cardsTopY = pad + headerH +
                    t.sizes.spacingLarge + tabH + t.sizes.spacingLarge;
                // 底部提示行（§6 item 11）：12px 三级灰居中「所有设置即时生效…」
                const float bottomHintH = roundFs(20.0f);
                // cardsH S4.2 弹性：底 padding 已为 0（列 .padding 底=0），cardsH 底缘贴 bottomHint 上方 gap 处
                const float cardsH =
                    screen.height - cardsTopY - bottomHintH -
                    t.sizes.spacingLarge;
                // 半宽配对列宽桶（.c348 / .c648 …）：换窗宽后列宽变 → 配对行高随
                // 对内卡高变，内容高缓存必须失效重测（UAT3 多列分栏）。
                float pairColW = contentW;
                ui_kit::columnX(contentW, t.sizes.spacingLarge, &pairColW,
                                nullptr);
                components::scrollView(ui, "settings.cards.scroll")
                    .size(contentW, cardsH)
                    .gap(t.sizes.spacingLarge)
                    .step(60.0f)
                    .theme(compTheme)
                    // contentKey 带 tab 索引 + 代理卡高 + 卡片显隐签名 + 列宽桶：
                    // 切 tab / 卡片增减 / 换窗宽（列宽桶变）时缓存失效重测。
                    .contentKey("settings.cards.t" +
                                std::to_string(settingsTab_) + "." +
                                std::to_string(static_cast<int>(proxyH)) +
                                (claudeOn ? "C" : "c") +
                                ".c" +
                                std::to_string(static_cast<int>(pairColW +
                                                               0.5f)))
                    .offset(cardsScrollOffset_)
                    .onChange([this](float v) { cardsScrollOffset_ = v; })
                    .content([&](eui::Ui& inner, float contentWidth,
                                 float viewportHeight) {
                        (void)viewportHeight;
                        drawCardRegion(inner, contentWidth, cardsTopY, cardsH,
                                       screen.height, t, proxyH, svc);
                    })
                    .build();

                // 底部生效提示（§6 item 11）：12px wechat.textTertiary 居中
                ui.text("settings.bottom.hint")
                    .size(contentW, bottomHintH)
                    .text("所有设置即时生效，自动保存到本地数据库")
                    .fontSize(fs(12.0f))
                    .lineHeight(bottomHintH)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .color(t.colors.textTertiary)
                    .build();
            })
            .build();

        drawToast(ui, screen, t);
        drawGithubSyncErrorDialog(ui, screen, t);

        LOG_DEBUG("Settings 模块 compose 完成");
    }

private:
    // 跨模块服务取用：经 core 只读服务定位器；未注册 → nullptr，调用点按「服务不可用」降级。
    static ::core::IClaudeProxyService* claudeProxy() {
        auto r =
            ::core::ServiceLocator::requireService<::core::IClaudeProxyService>();
        return r.isOk() ? r.value() : nullptr;
    }

    // 7 个下拉共用一个「谁打开着」索引（取值语义见 settings_ui_logic.h）。
    void setDropdownOpen(Dropdown d, bool v) {
        openDropdown_ = applyDropdownOpen(openDropdown_, d, v);
    }

    // 卡片回调装配（compose 每帧）：lambda 只捕获 this；卡片按值拷进元素回调 → 不悬垂。
    SettingsCardActions cardActions() {
        SettingsCardActions a;
        a.applyThemeMode = [this](int idx) { applyThemeMode(idx); };
        a.applyFontScalePercent = [this](int percent) { applyFontScalePercent(percent); };
        a.applyWindowEffect = [this](int idx) { applyWindowEffectPreset(idx); };
        a.applyModuleEnabled = [this](const std::string& moduleId, bool on) {
            applyModuleEnabled(moduleId, on);
        };
        a.setDropdownOpen = [this](Dropdown d, bool v) { setDropdownOpen(d, v); };
        a.applyProxyAutoStart = [this](bool on) { applyAutoStart(on); };
        a.applyProxyPort = [this] { applyProxyPort(); };
        a.setProxyPortDraft = [this](const std::string& v) {
            setProxyPortDraft(v);
        };
        // Esc=丢弃端口草稿（不触发 applyProxyPort；不清红字状态）。
        a.discardProxyPortDraft = [this] { proxyPortDraft_.clear(); };
        a.applyRetentionPreset = [this](int idx) { applyRetentionPreset(idx); };
        a.applyLogLevelPreset = [this](int idx) { applyLogLevelPreset(idx); };
        a.applyEditorFontPreset = [this](int idx) { applyEditorFontPreset(idx); };
        a.applyAutostartEnabled = [this](bool on) { applyAutostartEnabled(on); };
        a.applyFluidBackgroundEnabled = [this](bool on) { applyFluidBackgroundEnabled(on); };
        a.requestLogTailReload = [this] { logTailDirty_ = true; };
        a.logViewScrolled = [this](float v) { logViewScrollOffset_ = v; };

        // GitHub Sync 回调
        a.applyGithubSyncEnabled = [this](bool on) { applyGithubSyncEnabled(on); };
        a.applyGithubSyncRepoUrl = [this](const std::string& url) {
            githubSyncCfg_.repoUrl = url;
            clearGithubSyncFirstSyncFlag("repoUrl");
            githubSyncLocalGitProbeDirty_ = true;   // 配置变更 → 探测失效
            maybeWarnNoLocalGit();
        };
        a.applyGithubSyncBranch = [this](const std::string& branch) {
            githubSyncCfg_.branch = branch;
            clearGithubSyncFirstSyncFlag("branch");
            githubSyncLocalGitProbeDirty_ = true;
            maybeWarnNoLocalGit();
        };
        a.setGithubSyncTokenDraft = [this](const std::string& token) {
            githubSyncTokenDraft_ = token;
            githubSyncLocalGitProbeDirty_ = true;
            maybeWarnNoLocalGit();
        };
        a.applyGithubSyncAutoPush = [this](bool on) {
            githubSyncCfg_.autoPush = on;
        };
        a.githubSyncPush = [this] { githubSyncPush(); };
        a.githubSyncPull = [this] { githubSyncPull(); };
        a.githubSyncTestConnection = [this] { applyGithubSyncTestConnection(); };
        a.githubSyncShowErrorDetail = [this] { showGithubSyncErrorDetail(); };
        a.toggleGithubSyncTokenVisibility = [this] { githubSyncTokenVisible_ = !githubSyncTokenVisible_; };
        a.toggleGithubSyncDetail = [this] { githubSyncDetailExpanded_ = !githubSyncDetailExpanded_; };

        return a;
    }

    // 注册表卡高解析：Fixed = 表内常量；ProxyWarnings = 警告行数推导；FillRemainder = 剩余视口高。
    static float cardHeight(const CardSpec& spec, float proxyH, float remain) {
        switch (spec.heightKind) {
        case CardHeightKind::Fixed:
            // spec.height 是 100% 档基准，运行期随生效字号放大：卡高不同步放大就会把尾行挤出卡外。
            return roundFs(spec.height);
        case CardHeightKind::ProxyWarnings:
            return proxyH;
        case CardHeightKind::FillRemainder:
            // UAT6 零滚动收口：日志 tab 在最小窗（1000×640 → 卡区可用高 372）下
            // remain = 372 − roundFs(144) − gap 16 = 212——旧下限 260 > 212 把日志
            // 查看卡强行抬到 260，内容高 420 > 372 溢出 48px 出滚动条（85% 档
            // 398 > 375 溢出 23px；用户判「完全没必要」）。下限降到 160：只在比
            // 真实最小页高 600 更矮的退化视口兜底（100% 档 min 窗下 remain 212 恒
            // 不触发），正常路径卡高 = remain 恰好填满 → 内容高 = 可用高，日志
            // tab 100%~130% 档全部零滚动。
            return std::max(160.0f, remain);
        case CardHeightKind::GithubSync:
            // 恒满高（UAT4）：未启用态也全字段渲染（去门控），启用/未启用只差
            // 表单区是否可交互，不参与卡高决策。UAT5 方案 A 三段式后的最坏行序
            // 复算见 kGithubSyncCardHeight 注释（settings_ui_logic.h）。
            return roundFs(kGithubSyncCardHeight);
        }
        return spec.height;  // 兜底：枚举穷举，仅为满足「所有路径有返回」
    }

    // 遍历注册表画本 tab 的卡。UAT3 多列分栏：单游标改双游标——半宽卡两两成行
    //（行高 = max(对内卡高)，左卡 x=0、右卡 x=colW+gap，包一层全宽 row 作内容列子元素）；
    // 全宽卡独占一行并清掉挂起的半宽游标（落单半宽卡直接作内容列子元素，宽=colW）。
    // 分组标题/组间分隔线机制随 UAT5 删目录卡一并拆除（代理卡迁通用 tab 后
    // 各卡自带标题、无全宽卡再需要组题锚）。UAT4：模块插拔卡按用户拍板改全宽
    //（原 cols1 批落单半宽），卡内 2×2 开关格随全宽拉开；落单半宽兜底逻辑保留不动。
    // 纵向游标只累计参与绘制的卡，带下拉的卡需页面绝对 Y（openUp）。
    void drawCardRegion(eui::Ui& ui, float contentWidth, float cardsTopY,
                        float cardsH, float screenH,
                        const core::theme::Tokens& t, float proxyH,
                        ::core::IClaudeProxyService* svc) {
        // logTail_ 保持原始不预过滤——过滤在装配期做，切档零额外磁盘 IO、下一帧立即生效；
        // 若在 readLogTailLines/applyLogTail 里过滤，切到低级别要重读磁盘才能看到更多行。
        auto filteredLogTail = filterLogTailByLevel(logTail_, parseLogLevel(cfg_.logLevel));
        SettingsCardsIn in{cfg_, proxyPortDraft_,
                           filteredLogTail};
        in.logTailVersion = logTailVersion_;
        in.logViewScrollOffset = logViewScrollOffset_;
        in.takeoverManaged = takeoverManaged_;
        in.openDropdown = openDropdown_;
        in.proxy = svc;
        // 开机自启动（UAT6）：注册真值快照（POSIX 兜底 false → 行整行不建）
        in.autostartSupported = autostartSupported_;
        in.autostartRegistered = autostartRegistered_;
        // GitHub Sync 快照
        in.githubSyncEnabled = githubSyncCfg_.enabled;
        in.githubSyncRepoUrl = githubSyncCfg_.repoUrl;
        in.githubSyncBranch = githubSyncCfg_.branch;
        in.githubSyncTokenDraft = githubSyncTokenDraft_;
        in.githubSyncAutoPush = githubSyncCfg_.autoPush;
        in.githubSyncStatus = githubSyncStatus_;
        in.githubSyncStatusError = githubSyncStatusError_;
        in.githubSyncRunning = githubSyncRunning_;
        in.githubSyncProgress = githubSyncProgress_;
        in.githubSyncError = githubSyncError_;
        in.githubSyncRepoPublic = githubSyncRepoPublic_;
        in.githubSyncBranchHint = githubSyncBranchHint_;
        in.githubSyncLocalGitUnavailable =
            githubSyncLocalGitProbe_.load(std::memory_order_acquire) == 0;
        in.githubSyncUnsyncedNotice = githubSyncUnsyncedNotice_;
        in.githubSyncDeleteSkipHint = githubSyncDeleteSkipHint_;
        in.githubSyncTokenVisible = githubSyncTokenVisible_;
        in.githubSyncDetailExpanded = githubSyncDetailExpanded_;
        const SettingsCardActions a = cardActions();
        const float gapY = t.sizes.spacingLarge;
        float cardY = 0.0f;
        // 越界 tab（不可达）回落通用页
        const int tab = (settingsTab_ >= 0 && settingsTab_ < kSettingTabCount)
                            ? settingsTab_
                            : kTabGeneral;
        // 半宽配对列几何：列宽/右列 x 单一来源 ui_kit::columnX（gap 与卡间距一致）。
        float colW = contentWidth;
        ui_kit::columnX(contentWidth, gapY, &colW, nullptr);
        int pairSeq = 0;  // 配对行 id 序号（每次绘制从 0 起，帧内稳定可测）
        // 挂起的半宽卡：等相邻半宽卡成对；全宽卡/收尾时落单冲刷。
        const CardSpec* pendingHalf = nullptr;

        // 落单半宽卡：直接作内容列子元素（列起始对齐 START → x=0，宽 = colW）。
        auto flushLoneHalf = [&]() {
            if (pendingHalf == nullptr) return;
            const float h = cardHeight(*pendingHalf, proxyH,
                                      cardsH - cardY);
            CardDrawArgs args{ui, pendingHalf->id, colW, h, t,
                              cardsTopY + cardY - cardsScrollOffset_, screenH,
                              in, a};
            pendingHalf->draw(args);
            cardY += h + gapY;
            pendingHalf = nullptr;
        };
        // 成对半宽卡：包一层全宽 row（行高 = max(对内卡高)，卡各持自身卡高——
        // 右卡矮于行高时下方自然留白，不拉伸）。对内两卡 pageY 同为行顶（openUp 几何一致）。
        auto flushHalfPair = [&](const CardSpec& rightSpec) {
            const CardSpec& leftSpec = *pendingHalf;
            const float hL = cardHeight(leftSpec, proxyH,
                                        cardsH - cardY);
            const float hR = cardHeight(rightSpec, proxyH,
                                        cardsH - cardY);
            const float rowH = std::max(hL, hR);
            const float rowY = cardY;
            ui.row(std::string("settings.cards.pair.") +
                   std::to_string(pairSeq++))
                .size(contentWidth, rowH)
                .gap(gapY)
                .content([&] {
                    CardDrawArgs left{ui, leftSpec.id, colW, hL, t,
                                      cardsTopY + rowY - cardsScrollOffset_,
                                      screenH, in, a};
                    leftSpec.draw(left);
                    CardDrawArgs right{ui, rightSpec.id, colW, hR, t,
                                       cardsTopY + rowY - cardsScrollOffset_,
                                       screenH, in, a};
                    rightSpec.draw(right);
                })
                .build();
            cardY = rowY + rowH + gapY;
            pendingHalf = nullptr;
        };

        for (const CardSpec& spec : kCardSpecs) {
            if (spec.tab != tab) continue;
            if (spec.visible != nullptr && !spec.visible()) {
                continue;  // 平台能力不支持 → 整卡不建（不占高度、不留空白）
            }
            if (spec.gate != nullptr && !moduleEnabled(cfg_, spec.gate)) {
                continue;  // 模块关 → 整卡隐藏（不显示故障态）
            }
            if (spec.half) {
                if (pendingHalf == nullptr) {
                    pendingHalf = &spec;   // 等相邻半宽卡成对
                    continue;
                }
                flushHalfPair(spec);
                continue;
            }
            flushLoneHalf();   // 全宽卡先清掉挂起的半宽游标
            const float h =
                cardHeight(spec, proxyH, cardsH - cardY);
            CardDrawArgs args{ui, spec.id, contentWidth, h, t,
                              cardsTopY + cardY - cardsScrollOffset_, screenH,
                              in, a};
            spec.draw(args);
            cardY += h + gapY;
        }
        flushLoneHalf();   // 收尾：tab 末尾落单的半宽卡
    }

    void drawHeader(eui::Ui& ui, float contentW, float headerH,
                    const core::theme::Tokens& t) {
        // UAT5：标题下的说明/状态小字行（settings.subtitle）整行删除——页头只留
        // 「设置」大标题；headerH 由共享 64 收紧为本页专用 40（不留空洞）。
        // statusMessage_/statusIsError_ 仍是模块内部状态（端口草稿输入清错逻辑
        // 读 statusIsError_），但不再有页头渲染面。
        ui.row("settings.header")
            .size(contentW, headerH)
            .alignItems(eui::Align::CENTER)
            .content([&] {
                ui.column("settings.header.text")
                    .size(contentW, headerH)
                    .content([&] {
                        ui.text("settings.h1")
                            .size(contentW, 34.0f)
                            .text("设置")
                            .fontSize(t.sizes.fontSizePage)
                            .lineHeight(34.0f)
                            .fontWeight(700)
                            .color(t.colors.textPrimary)
                            .build();
                    })
                    .build();
            })
            .build();
    }

    // 纯读盘（static → 可后台跑）：只读文件末尾窗口防大日志进内存，滑动裁剪避免 O(n²)。
    static std::vector<std::string> readLogTailLines() {
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
        return lines;
    }

    // 把读盘结果落成快照（只在渲染线程调：onStart 首次装载 + compose 收割/兜底）
    void applyLogTail(std::vector<std::string> lines) {
        const size_t n = lines.size();
        logTail_ = std::move(lines);
        ++logTailVersion_;
        logTailLoaded_ = true;   // 快照已装载：此后重读全走后台
        LOG_INFO("Settings: 日志 tail 刷新，读取 " + std::to_string(n) + " 行");
    }

    // compose 只收割快照 + 只发意图，不阻塞；可见时机最多晚一个 tick。
    void refreshLogTailCache() {
        if (logTailFuture_.valid() &&
            logTailFuture_.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready) {
            auto lines = logTailFuture_.get();   // shared_future::get 不失效
            logTailFuture_ = {};                 // 显式清空 = 让出「在途」判据
            applyLogTail(std::move(lines));
        }
        if (!logTailLoaded_) {
            // 冷启动兜底：快照从未装载过时本帧同步读一次（首帧必须有值，只发生一次）。
            applyLogTail(readLogTailLines());
            logTailDirty_ = false;
            return;
        }
        if (!logTailDirty_ || logTailFuture_.valid()) {
            return;
        }
        logTailDirty_ = false;
        // 先 set_value 后唤醒：std::async 在 callable 返回后才发布值，任务内先唤醒会让 UI 抢跑收割到未就绪。
        auto promise =
            std::make_shared<std::promise<std::vector<std::string>>>();
        logTailFuture_ = promise->get_future().share();
        logTailWorker_ = std::async(std::launch::async, [promise] {
            try {
                promise->set_value(readLogTailLines());
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
            core::platform::requestUiUpdate();            // 就绪之后才唤醒
        });
    }

    // 外部状态只读快照刷新点（读一次存成员，compose 只读）；调用点全在非渲染路径 + compose 冷启动兜底。
    void refreshExternalState() {
        auto* svc = claudeProxy();
        takeoverManaged_ = svc && svc->isClaudeCodeManaged();
        // 开机自启动（UAT6）：真值回读（HKCU Run / LaunchAgents 有项 = 开，只读不写）
        // 防漂移——手改注册表 / 上次注销失败在开页时自愈；cfg 意图对齐真值
        //（下次 persist 落库，开关显示恒走 autostartRegistered_ 真值）
        autostartSupported_ = platform::autostart::supported();
        autostartRegistered_ = platform::autostart::AutostartController(
            platform::autostart::systemOps()).isRegistered();
        cfg_.autostartEnabled = autostartRegistered_;
        externalStateLoaded_ = true;
    }

    std::unique_ptr<SettingsStore> store_;
    SettingsConfig cfg_;
    // GitHub Sync
    std::unique_ptr<GithubSyncStore> githubSyncStore_;
    GithubSyncConfig githubSyncCfg_;
    std::string githubSyncTokenDraft_;        // Token 输入草稿（不存明文）
    std::string githubSyncStatus_;
    bool githubSyncStatusError_ = false;
    // GitHub Sync 后台任务：原子完成位 + 每帧 compose 收割（与详情页测试连接线程同款模式）。
    std::thread githubSyncThread_;
    std::atomic<bool> githubSyncDone_{false};
    // 后台线程写、UI 线程读的共享面一律经此锁（进度文案 + 结果暂存）。
    std::mutex githubSyncMutex_;
    std::string githubSyncProgressPending_;   // 后台写的阶段文案（UI 每帧消费）
    std::vector<platform::github::ApiLogEntry> githubSyncStagedLogs_;
    std::string githubSyncStagedSummary_;
    bool githubSyncStagedOk_ = false;
    bool githubSyncStagedConflict_ = false;
    bool githubSyncStagedPublicRepo_ = false;
    std::string githubSyncStagedBranchHint_;
    // 409 时「拉取将覆盖的本地未推文件」清单（后台线程产出 → UI 搬运）。
    std::vector<std::string> githubSyncStagedOverwriteFiles_;
    // 本轮导入成功 → harvest 在 UI 线程重载本模块配置缓存（app_config 被
    // DROP 重建过）。
    std::atomic<bool> githubSyncStagedDbImported_{false};
    // 退出取消位。退出闸超时前先置位——同步线程在每个阶段边界与
    // 笔记循环体内查它，尽快收束（碰 store_/db/stateSet/requestUiUpdate 前）。
    std::atomic<bool> githubSyncAbortRequested_{false};
    // 删除传播安全栏跳过（本地仍在但读失败）的 UI 提示。
    std::string githubSyncStagedDeleteSkipHint_;
    // 以下成员仅 UI 线程读写（harvest 在 join 后搬运）。
    bool githubSyncRunning_ = false;
    std::string githubSyncProgress_;          // 卡片进度行快照
    std::string githubSyncError_;             // 卡片错误行快照（非空 = 显示 + 详情按钮）
    std::vector<platform::github::ApiLogEntry> githubSyncLogs_;  // 弹窗日志源
    std::string githubSyncResultSummary_;
    bool githubSyncResultOk_ = false;
    // 上一轮完成的动作是否走了 409 冲突 / 是否导入过远端库：harvest 从 staged 搬来
    //（conflict 原本只进弹窗 variant、dbImported 原本被 exchange 消费掉，测试缝读不到）。
    bool githubSyncLastConflict_ = false;
    bool githubSyncLastDbImported_ = false;
    SyncAction githubSyncLastAction_ = SyncAction::None;  // 弹窗「重试」按原动作重跑
    bool githubSyncLastManual_ = false;   // 手动失败才弹窗；自动失败只留卡片红字
    GitErrorDialogState githubSyncErrorDialog_;
    // 编排状态
    bool githubSyncDirty_ = false;          // 笔记写入 / 待办增删改
    int64_t githubSyncLastCommitSec_ = 0;   // 上次 commit 时刻（节流窗口起点）
    bool githubSyncRepoPublic_ = false;     // 测试连接发现 public 仓 → 黄字提醒
    std::string githubSyncBranchHint_;      // 分支被自动创建提示
    std::vector<std::string> githubSyncOverwriteFiles_;  // 确认框覆盖清单（UI 侧）
    // 本地 git 探测：-1 未知/在途、0 不可用、1 可用（后台线程原子写）。
    std::atomic<int> githubSyncLocalGitProbe_{-1};
    std::future<void> githubSyncLocalGitProbeFuture_;  // 在途探测（仅 UI 线程碰）
    bool githubSyncLocalGitProbeDirty_ = false;        // 首探 + 配置变更置位
    bool githubSyncNoGitWarned_ = false;    // 无 git 警告只弹一次（改配置时触发）
    bool githubSyncUnsyncedNotice_ = false;  // 退出时未推完 → 卡片黄字
    bool githubSyncTokenVisible_ = false;      // S3.7#6 Token 显隐切换
    bool githubSyncDetailExpanded_ = false;    // S3.7#4 同步说明折叠展开
    std::string githubSyncDeleteSkipHint_;   // 删远端跳过提示（UI 侧）
    // EventBus 订阅 token 必须持有：token 析构即退订（丢弃 = 订阅立刻失效）。
    std::vector<::core::event::EventBus::Subscription> githubSyncSubs_;
    // app.read_settings 工具：onStart 注册 / onStop 注销；与注册表同寿（注销对未注册名是 no-op）。
    std::shared_ptr<core::ITool> appReadSettingsTool_;
    // 当前打开的下拉（7 个 bool 收成一个索引，-1 = 全关）；取值 = static_cast<int>(Dropdown)。
    int openDropdown_ = -1;
    // 代理端口输入草稿：空 = 未编辑（compose 显示 cfg_.proxyPort），应用成功后刷新。
    std::string proxyPortDraft_;
    float cardsScrollOffset_ = 0.0f;      // 卡片区 scrollView 滚动位置
    int settingsTab_ = 0;                 // 当前 tab（通用/模块/日志/同步）
    std::vector<std::string> logTail_;    // 日志文件末尾行内存快照（compose 只渲染）
    unsigned logTailVersion_ = 0;         // tail 读取版本号（进日志查看卡 contentKey）
    bool logTailDirty_ = true;            // 待重读标记（切日志 tab / 点刷新置位）
    bool logTailLoaded_ = false;          // 快照是否装载过（冷启动兜底，见 refreshLogTailCache）
    // 后台读盘：结果槽 = shared_future（set_value 后再 requestUiUpdate 唤醒），在途判据 = logTailFuture_.valid()。
    std::shared_future<std::vector<std::string>> logTailFuture_;
    std::future<void> logTailWorker_;
    bool externalStateLoaded_ = false;    // 首次装载标记（compose 冷启动兜底）
    bool takeoverManaged_ = false;        // Claude Code 接管（sidecar 存在性）
    // 开机自启动（UAT6）：注册真值快照（开关显示源，防漂移）；supported=false → 卡内行整行不建
    bool autostartSupported_ = true;
    bool autostartRegistered_ = false;
    float logViewScrollOffset_ = 0.0f;    // 日志查看卡内滚动位置
    std::string statusMessage_;
    bool statusIsError_ = false;
    // toast 单实例状态位（EUI-NEO toast 无队列，见 ui_kit/toast.h）
    ui_kit::ToastState toast_;
};

} // namespace module::settings

namespace module::settings {

// ── 测试缝实现（见 github_sync_test_seam.h）──
GithubSyncTestResult SettingsModule::runGithubSyncForTest(
    GithubSyncTestAction action, bool manual, int timeoutMs) {
    GithubSyncTestResult out;
    // 先收掉上一轮残留：结果成员是「最近一次 harvest 的产物」，不清会把旧值当本轮。
    harvestGithubSync();
    githubSyncLastConflict_ = false;
    githubSyncLastDbImported_ = false;
    SyncAction a = SyncAction::None;
    switch (action) {
        case GithubSyncTestAction::Push: a = SyncAction::Push; break;
        case GithubSyncTestAction::Pull: a = SyncAction::Pull; break;
        case GithubSyncTestAction::PullThenPush: a = SyncAction::PullThenPush; break;
        case GithubSyncTestAction::FirstSync: a = SyncAction::FirstSync; break;
        case GithubSyncTestAction::ValidateFirstSync:
            a = SyncAction::ValidateFirstSync;
            break;
        case GithubSyncTestAction::Test: a = SyncAction::Test; break;
    }
    startGithubSyncAction(a, manual);
    // 启动被拒（无 URL / 无 token / 线程在飞）：running 与 done 都假 → 不空等。
    if (!githubSyncRunning_ && !githubSyncDone_.load(std::memory_order_acquire)) {
        out.summary = githubSyncError_;
        return out;
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (!githubSyncDone_.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return out;  // ran=false
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    harvestGithubSync();   // join + 搬 staged → UI 侧成员，之后读结果安全
    out.ran = true;
    out.ok = githubSyncResultOk_;
    out.summary = githubSyncResultSummary_;
    out.error = githubSyncError_;
    out.conflict = githubSyncLastConflict_;
    out.dbImported = githubSyncLastDbImported_;
    out.overwriteFiles = githubSyncOverwriteFiles_;
    out.branchHint = githubSyncBranchHint_;
    return out;
}

int SettingsModule::runLocalGitProbeForTest() {
    // 探测平时由 compose 每帧调起，无头驱动没有帧 → 显式触发并等 future 收束
    //（平台实现自带 3s 上界，wait() 不会无限挂）。
    githubSyncLocalGitProbeDirty_ = true;
    maybeRefreshLocalGitProbe();
    if (githubSyncLocalGitProbeFuture_.valid()) {
        githubSyncLocalGitProbeFuture_.wait();
        githubSyncLocalGitProbeFuture_ = {};
    }
    return githubSyncLocalGitProbe_.load(std::memory_order_acquire);
}

bool githubSyncTestSeamAvailable() { return githubTestModeEnabled(); }

GithubSyncTestResult githubSyncRunForTest(GithubSyncTestAction action,
                                          bool manual, int timeoutMs) {
    if (!githubSyncTestSeamAvailable() || gSyncTestTarget == nullptr) return {};
    return gSyncTestTarget->runGithubSyncForTest(action, manual, timeoutMs);
}

int githubSyncLocalGitProbeForTest() {
    if (!githubSyncTestSeamAvailable() || gSyncTestTarget == nullptr) return -1;
    return gSyncTestTarget->runLocalGitProbeForTest();
}

void runGithubSyncExitPush() {
    if (gExitHookTarget != nullptr) gExitHookTarget->runExitSyncPush();
}

}  // namespace module::settings

extern "C" ::core::Module* create_settings_module() {
    return new module::settings::SettingsModule();
}
