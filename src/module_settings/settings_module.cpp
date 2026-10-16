// src/module_settings/settings_module.cpp
// 设置页模块：Module 实现、生命周期、设置变更动作、日志 tail 与外部状态快照、卡片注册表。
// 依赖边界：module_* 互不依赖 —— claude 经 core 窄接口、pet 经 module_pet_bridge.h、模块开关经 module_toggle_bridge.h。
// 渲染路径不做 fs 查询：外部状态统一在 refreshExternalState 装载，compose 只读快照。

#include "app_tools.h"
#include "settings_config.h"
#include "settings_ui_cards.h"
#include "github_sync_card.h"
#include "github_sync_config.h"
#include "github_sync_logic.h"
#include "git_error_dialog.h"
#include "local_git_probe.h"
#include "settings_ui_logic.h"

#include "core/claude_proxy_service.h"
#include "core/database.h"
#include "core/event_bus.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/service_locator.h"
#include "core/sql_dump.h"
#include "core/theme.h"
#include "core/tool.h"
#include "core/component_theme.h"
#include "platform/file_open.h"
#include "platform/github_api.h"     // platform::github::GithubApi / TestStep / ApiLogEntry
#include <core/platform/platform.h>   // core::platform::requestUiUpdate（唤醒主循环）
#include <core/window/window_backend.h>   // core::window::setClipboardText（复制详情）
#include "platform/system_theme.h"

// module_pet 解耦桥：settings 不 include module_pet 头，符号在可执行文件链接期解析。
#include "module_pet_bridge.h"

// 模块开关即时生效桥：函数指针缝 + 默认 no-op，真 handler 由 main.cpp 装配段注入。
#include "module_toggle_bridge.h"

#include <ui_kit/metrics.h>
#include <ui_kit/toast.h>

#include <eui/dsl.h>

#include <components/scrollview.h>
#include <components/tabs.h>
#include <components/theme.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace module::settings {

namespace {

// 卡片注册表：每行 = 一张卡（tab / 门控 / 卡高种类 / 是否吃页面几何 / 绘制函数），顺序 = 绘制顺序。
enum class CardHeightKind {
    Fixed,
    ProxyWarnings,
    FillRemainder,
    GithubSync,   // 随启用态切换满高/折叠高（未启用时卡片只画标题+描述+开关）
};

struct CardSpec {
    int tab;
    const char* id;
    const char* gate;       // nullptr = 恒显；否则 = 模块开关门控键
    CardHeightKind heightKind;
    float height;           // Fixed 档生效
    bool pageGeom;          // 下发卡顶页面绝对 y + 屏高（下拉 openUp 几何）
    void (*draw)(const CardDrawArgs&);
};

const CardSpec kCardSpecs[] = {
    {kTabGeneral, "settings.card.appearance", nullptr, CardHeightKind::Fixed,
     kAppearanceCardHeight, false, &drawAppearanceCard},
    {kTabGeneral, "settings.card.editor", nullptr, CardHeightKind::Fixed,
     kEditorFontCardHeight, true, &drawEditorFontCard},
    {kTabModules, "settings.card.modules", nullptr, CardHeightKind::Fixed,
     kModulesCardHeight, false, &drawModulesCard},
    {kTabModules, "settings.card.pet", kModuleIdPet, CardHeightKind::Fixed,
     kPetCardHeight, false, &drawPetCard},
    {kTabModules, "settings.card.directory", kModuleIdMarkdown,
     CardHeightKind::Fixed, kDirectoryCardHeight, false, &drawDirectoryCard},
    {kTabModules, "settings.card.proxy", kModuleIdClaudeProxy,
     CardHeightKind::ProxyWarnings, 0.0f, false, &drawProxyCard},
    {kTabLog, "settings.card.log", nullptr, CardHeightKind::Fixed,
     kLogSettingsCardHeight, true, &drawLogCard},
    {kTabLog, "settings.tab.log", nullptr, CardHeightKind::FillRemainder,
     0.0f, false, &drawLogViewCard},
    {kTabSync, "settings.card.github_sync", nullptr, CardHeightKind::GithubSync,
     0.0f, false, &drawGithubSyncCard},
};

// ── GitHub Sync（纯 API 方案，无 git 依赖）──
// 远端路径常量在 github_sync_logic.h（kRemoteSqlPath / remoteNotePath）。
// skip 表清单单一来源 = core::sql_dump::defaultSkipTables()，不得本地另抄。

// 同步动作（弹窗「重试」按原动作重跑；PullThenPush = 409 冲突弹窗的主动作；
// FirstSync / ValidateFirstSync = 首次同步：先跑测试连接三步校验，过了才做首次方向
// 判定，见 runGithubSyncCheckedFirstSync。两者跑同一条路，差别只在失败呈现——
// ValidateFirstSync 由启用开关触发、失败不弹窗（用户刚拨开关，弹窗等于打断）；
// FirstSync 由手动「立即同步」触发、失败弹窗。校验强度必须一致：首次同步要么拉
// 远端覆盖本地库、要么上传全量，配置未经验证就动数据是不可接受的）。
enum class SyncAction { None, Push, Pull, Test, PullThenPush, FirstSync, ValidateFirstSync };

// epoch 秒（节流窗口与备份文件名共用）。
int64_t nowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 整文件读回（小文件：database.sql 量级）。失败 → false。
bool readFileToString(const std::filesystem::path& f, std::string& out) {
    std::ifstream in(f, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// 整文件写入（截断）。失败 → false。
bool writeFileFromString(const std::filesystem::path& f,
                         const std::string& content) {
    std::ofstream out(f, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << content;
    return out.good();
}

} // namespace

class SettingsModule;

namespace {
// 退出钩子目标：onStart 挂上、onStop/析构摘掉；main 的 atexit 经
// runGithubSyncExitPush 间接调用（见 github_sync_exit_hook.h 的注册序约定）。
SettingsModule* gExitHookTarget = nullptr;
}  // namespace

class SettingsModule : public ::core::Module {
public:
    // 退出钩子目标兜底摘除：用例/异常路径下 onStop 未跑时，atexit 不得打到悬垂指针。
    ~SettingsModule() override {
        if (gExitHookTarget == this) gExitHookTarget = nullptr;
    }

    ::core::PageId pageId() const override { return ::core::PageId::Settings; }
    std::string displayName() const override { return "设置"; }
    std::string shortName() const override { return "设置"; }
    std::string iconGlyph() const override { return eui::utf8(0xF013); }  // fa-gear

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
        // dirty 触发源：笔记写入/新建/重命名/删除 + 待办增删改 + 待办列增删改重排改色——
        // 每个写路径事件都要订上，漏一个 = 那类改动永远不自动同步。token 必须存成员，
        // 丢了就退订。事件名直接作 source：日志里看得出是哪条写路径置的位。
        for (const char* ev :
             {::core::event::kMdSaved, ::core::event::kMdCreated,
              ::core::event::kMdRenamed, ::core::event::kMdDeleted,
              ::core::event::kTodoCreated, ::core::event::kTodoUpdated,
              ::core::event::kTodoMoved, ::core::event::kTodoCompleted,
              ::core::event::kTodoUncompleted, ::core::event::kTodoDeleted,
              ::core::event::kTodoColumnChanged}) {
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

    void applyFontPreset(int index) {
        if (index < 0 || index >= kFontPresetCount) return;
        cfg_.fontScalePercent = kFontPresets[index];
        ::core::theme::setFontScale(
            static_cast<float>(cfg_.fontScalePercent) / 100.0f);
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

    // 窗口效果三档切换：即时生效；系统不支持时 compose 每帧回查显示降级提示行。
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
        // 校验错误（红字）随下一次输入清除；成功提示保留到下一次操作。
        if (statusIsError_) {
            statusMessage_.clear();
            statusIsError_ = false;
        }
    }

    void changePetSkinAt(int idx) {
        if (idx < 0 || idx >= static_cast<int>(petSkins_.size())) {
            return;
        }
        petSkinName_ = petSkins_[static_cast<size_t>(idx)];
        ::module::pet::applyPetSkin(petSkinName_);
    }

    void changePetScaleAt(int idx) {
        if (idx < 0 || idx >= static_cast<int>(petScaleLabels_.size())) {
            return;
        }
        ::module::pet::applyPetScaleIndex(idx);
        petScaleIdx_ = idx;
        LOG_INFO("Settings: 桌宠显示大小 → " +
                 petScaleLabels_[static_cast<size_t>(idx)]);
    }

    void resetPetPosition() {
        ::module::pet::resetPetPosition();
        setStatus("桌宠位置已重置（主屏右下角）");
    }

    void showPetWindow() {
        using Outcome = ::module::pet::PetShowOutcome;
        switch (::module::pet::showPetWindow()) {
            case Outcome::Shown:
                setStatus("已请求显示桌宠");
                break;
            case Outcome::AlreadyOpen:
                showToast("桌宠已在显示中");
                break;
            case Outcome::NotAssembled:
                showToast("已启用，重启 DevDesk 后生效");
                break;
            case Outcome::StartFailed:
                showToast("桌宠启动失败", "皮肤资产不可用，详情见日志",
                          /*isError=*/true);
                break;
        }
    }

    // 桌宠开关即时可见性：关 → 立即关窗，开 → 立即原位重开（未装配则提示重启生效）。
    void applyPetModuleVisibility(bool on) {
        if (!on) {
            const bool closed = ::module::pet::closePetWindow();
            setStatus(closed
                          ? "桌宠已关闭并立即隐藏（重启 DevDesk 后不再装配）"
                          : "桌宠已关闭（本次会话未开窗；重启 DevDesk 后不再装配）");
            LOG_INFO(std::string("Settings: 桌宠模块关闭 → ") +
                     (closed ? "窗口已请求关闭" : "无窗口可关") + "（重启后不再装配）");
            return;
        }
        using Outcome = ::module::pet::PetShowOutcome;
        switch (::module::pet::showPetWindow()) {
            case Outcome::Shown:
            case Outcome::AlreadyOpen:
                setStatus("桌宠已开启并立即显示");
                LOG_INFO("Settings: 桌宠模块开启 → 窗口已重新入队开窗");
                break;
            case Outcome::NotAssembled:
                showToast("已启用，重启 DevDesk 后生效");
                LOG_INFO("Settings: 桌宠模块开启 → 本会话未装配，重启后生效");
                break;
            case Outcome::StartFailed:
                showToast("桌宠启动失败", "皮肤资产不可用，详情见日志",
                          /*isError=*/true);
                LOG_ERROR("Settings: 桌宠模块开启 → 开窗失败（皮肤不可用）");
                break;
        }
    }

    // 模块开关：只写 app_config + 经 app::toggle 桥发「开关意图」；桌宠额外做即时窗口同步。
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
        if (moduleId == kModuleIdPet) {
            applyPetModuleVisibility(on);
            return;
        }
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
                          core::theme::componentTokens(t),
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
                              GithubSyncStore::loadToken().isOk();
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

    // 前置校验失败等 UI 线程内的即时错误：进错误行 + 直接开详情弹窗。
    void setGithubSyncError(const std::string& msg) {
        githubSyncStatus_ = msg;
        githubSyncStatusError_ = true;
        githubSyncError_ = msg;
        githubSyncErrorDialog_.variant = GitErrorDialogVariant::Error;
        githubSyncErrorDialog_.conflictConfirmArmed = false;
        githubSyncErrorDialog_.selectedLogIndex = 0;
        githubSyncErrorDialog_.open = true;
        LOG_WARN("Settings: GitHub Sync " + msg);
    }

    // 发起一个后台同步动作。三按钮在运行中已禁用，这里的 joinable 判定是兜底。
    void startGithubSyncAction(SyncAction action, bool manual) {
        harvestGithubSync();
        if (githubSyncThread_.joinable()) return;
        if (githubSyncCfg_.repoUrl.empty()) {
            setGithubSyncError("未配置仓库 URL");
            return;
        }
        // 节流：自动同步 5 分钟内最多一次；手动「立即同步」绕开。
        if (!manual && !gsync::throttleDue(githubSyncLastCommitSec_, nowSec())) {
            return;
        }
        std::string token = githubSyncTokenDraft_;
        if (token.empty()) {
            auto t = GithubSyncStore::loadToken();
            if (t.isOk()) token = t.value();
        }
        if (token.empty()) {
            setGithubSyncError("未配置 Token（请在上方填写后再操作）");
            return;
        }
        // 用户在本次会话里填过 Token → 落 keychain（下次启动仍可用）。
        if (!githubSyncTokenDraft_.empty()) {
            auto saved = GithubSyncStore::saveToken(githubSyncTokenDraft_);
            if (saved.isFail()) {
                LOG_WARN("Settings: GitHub Sync Token 落 keychain 失败: " +
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
            runGithubSyncAction(action, repoUrl, token, branch);
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

    // 后台线程体：platform::github 的唯一调用点。结果暂存 staged_*（锁内），
    // 由 harvestGithubSync 在 join 后搬到 UI 侧成员。
    void runGithubSyncAction(SyncAction action, const std::string& repoUrl,
                             const std::string& token,
                             const std::string& branch) {
        if (syncAborted()) return;   // 退出中：不再碰 api/store/db
        const auto parts = platform::github::GithubApi::parseRepoUrl(repoUrl);
        std::string summary;
        bool ok = false;
        bool conflict = false;
        bool publicRepo = false;
        std::string branchHint;
        if (parts.first.empty() || parts.second.empty()) {
            summary = "仓库 URL 无法解析（需要 https://github.com/<owner>/<repo>）";
        } else {
            platform::github::GithubApi api(token, parts.first, parts.second);
            switch (action) {
                case SyncAction::Test: {
                    // 仓库可见性（public 黄字提醒，不硬拦）
                    auto repo = api.getRepo();
                    if (repo.isOk()) publicRepo = repo.value().isPrivate == false;
                    auto report = api.testConnection(
                        [this](const platform::github::TestStep& step) {
                            publishGithubSyncProgress(step.name +
                                                      (step.ok ? " ✓" : " ✗"));
                        });
                    ok = report.ok;
                    summary = report.summary;
                    break;
                }
                case SyncAction::Push:
                    ok = runGithubSyncPush(api, branch, summary, conflict,
                                           branchHint);
                    break;
                case SyncAction::Pull:
                    ok = runGithubSyncPull(api, branch, summary);
                    break;
                case SyncAction::PullThenPush: {
                    std::string pullSummary;
                    if (!runGithubSyncPull(api, branch, pullSummary)) {
                        summary = "先拉取失败：" + pullSummary;
                        break;
                    }
                    ok = runGithubSyncPush(api, branch, summary, conflict,
                                           branchHint);
                    if (ok) summary = "已拉取远端后再推送：" + summary;
                    break;
                }
                case SyncAction::ValidateFirstSync:
                case SyncAction::FirstSync:
                    ok = runGithubSyncCheckedFirstSync(api, branch, summary,
                                                       conflict, publicRepo,
                                                       branchHint);
                    break;
                case SyncAction::None:
                    break;
            }
            if (summary.empty()) summary = ok ? "完成" : "失败";
            std::lock_guard<std::mutex> lk(githubSyncMutex_);
            githubSyncStagedLogs_ = api.recentLog();
        }
        if (summary.empty()) summary = ok ? "完成" : "失败";
        std::lock_guard<std::mutex> lk(githubSyncMutex_);
        githubSyncStagedOk_ = ok;
        githubSyncStagedSummary_ = summary;
        githubSyncStagedConflict_ = conflict;
        githubSyncStagedPublicRepo_ = publicRepo;
        githubSyncStagedBranchHint_ = branchHint;
    }

    // 首次同步的唯一实现（启用开关与手动「立即同步」共用）：先跑测试连接三步
    // （令牌 / 仓库可推 / 读写探针），过了才做首次方向判定。校验失败时 summary 即
    // 失败摘要、返回 false，调用方按各自的失败呈现处理（不在这里决定弹窗与否）。
    bool runGithubSyncCheckedFirstSync(platform::github::GithubApi& api,
                                       const std::string& branch,
                                       std::string& summary, bool& conflict,
                                       bool& publicRepo,
                                       std::string& branchHint) {
        auto repo = api.getRepo();
        if (repo.isOk()) publicRepo = repo.value().isPrivate == false;
        auto report = api.testConnection(
            [this](const platform::github::TestStep& step) {
                publishGithubSyncProgress(step.name + (step.ok ? " ✓" : " ✗"));
            });
        if (!report.ok) {
            summary = report.summary;
            return false;
        }
        std::string firstSummary;
        if (!runGithubSyncFirstSync(api, branch, firstSummary, conflict,
                                    branchHint)) {
            summary = firstSummary;
            return false;
        }
        summary = "校验通过；" + firstSummary;
        return true;
    }

    // 首次方向：远端已有 database.sql → 先拉取覆盖（含备份）再推送；
    // 远端没有 → 首次上传（runGithubSyncPush 内含分支自动创建）。
    bool runGithubSyncFirstSync(platform::github::GithubApi& api,
                                const std::string& branch,
                                std::string& summary, bool& conflict,
                                std::string& branchHint) {
        if (syncAborted()) { summary = "同步已中止（退出中）"; return false; }
        publishGithubSyncProgress("① 探测远端数据…");
        auto probe = api.getFile(gsync::kRemoteSqlPath, branch);
        const bool exists = probe.isOk() && probe.value().exists;
        const bool missing =
            probe.isFail() && probe.errorCode() == core::ErrorCode::NotFound;
        switch (gsync::firstSyncChoice(exists, missing)) {
            case gsync::FirstSyncChoice::PullThenPush: {
                std::string pullSummary;
                if (!runGithubSyncPull(api, branch, pullSummary)) {
                    summary = "首次拉取失败：" + pullSummary;
                    return false;
                }
                if (!runGithubSyncPush(api, branch, summary, conflict,
                                       branchHint)) {
                    return false;
                }
                summary = "已用远端数据覆盖本地并推送：" + summary;
                return true;
            }
            case gsync::FirstSyncChoice::Push: {
                if (!runGithubSyncPush(api, branch, summary, conflict,
                                       branchHint)) {
                    return false;
                }
                summary = "首次上传完成：" + summary;
                return true;
            }
            case gsync::FirstSyncChoice::Fail:
                summary = "探测远端失败：" + (probe.isFail()
                                                  ? probe.errorMessage()
                                                  : std::string("远端状态未知"));
                return false;
        }
        summary = "首次同步方向未知";
        return false;
    }

    // 409 确认框的覆盖清单 = database.sql + 本轮本地有改动的笔记（Put 集）。
    // 任何冲突分支（sql PUT / 笔记 PUT / 删除传播）都经它暂存——清单空着弹
    // 确认框等于让用户在不知道会覆盖什么的情况下二次确认。
    void stashOverwriteListForConflict(
        const std::vector<std::string>& putNoteNames) {
        std::vector<std::string> overwrite{
            std::string(gsync::kRemoteSqlPath) + "（数据库）"};
        for (const auto& name : putNoteNames) {
            overwrite.push_back(gsync::remoteNotePath(name));
        }
        std::lock_guard<std::mutex> lk(githubSyncMutex_);
        githubSyncStagedOverwriteFiles_ = std::move(overwrite);
    }

    // 推送：dump 全库 + 笔记 → PUT / DELETE（单文件粒度 = 单次 commit）。
    bool runGithubSyncPush(platform::github::GithubApi& api,
                           const std::string& branch, std::string& summary,
                           bool& conflict, std::string& branchHint) {
        auto* db = ::core::appDatabase();
        if (db == nullptr) {
            summary = "数据库未就绪";
            return false;
        }
        // ① 全库 dump
        if (syncAborted()) { summary = "同步已中止（退出中）"; return false; }
        publishGithubSyncProgress("① 导出数据库…");
        const auto dumpFile = ::core::paths::sqlDumpFile();
        auto exported = ::core::sql_dump::exportToFile(
            *db, dumpFile, ::core::sql_dump::defaultSkipTables());
        if (exported.isFail()) {
            summary = "导出失败：" + exported.errorMessage();
            return false;
        }
        std::string dump;
        if (!readFileToString(dumpFile, dump)) {
            summary = "读取导出文件失败";
            return false;
        }
        // 体积上限：PUT 单请求 100MB（base64 ×1.37）→ 原始 ≈73MB，
        // 超限明确报错而不是让远端拒收 413 后报「HTTP 413」。
        if (gsync::dumpTooLargeForRemote(dump.size())) {
            summary = "数据库导出过大，暂不支持同步（原始 " +
                      std::to_string(dump.size() / (1000 * 1000)) +
                      " MB，上限约 73 MB；请清理调用日志或待办数据后重试）";
            LOG_ERROR("Settings: GitHub Sync " + summary);
            return false;
        }

        // ② 分支：不存在则以仓库默认分支为基点自动创建。
        publishGithubSyncProgress("② 校验分支…");
        if (syncAborted()) { summary = "同步已中止（退出中）"; return false; }
        auto repo = api.getRepo();
        if (repo.isFail()) {
            summary = "读取仓库信息失败：" + repo.errorMessage();
            return false;
        }
        auto probe = api.getFile(gsync::kRemoteSqlPath, branch);
        const bool branchMissing =
            probe.isFail() && probe.errorCode() == core::ErrorCode::NotFound;
        if (branchMissing) {
            if (syncAborted()) { summary = "同步已中止（退出中）"; return false; }
            auto created =
                api.createRef(branch, repo.value().defaultBranch);
            if (created.isFail()) {
                summary = "创建分支 " + branch + " 失败：" +
                          created.errorMessage();
                return false;
            }
            branchHint = gsync::branchCreatedHint(branch);
        }

        // ③ 远端笔记树（与 pull 同源 listTree）：状态机判 RemoteModified /
        // Diverged / LocalDeleted-远端也变 必须知道远端 sha——只靠 PUT 撞 409
        // 事后发现不够：本地删除会先静默删掉别人新写的内容。
        publishGithubSyncProgress("③ 读取远端笔记树…");
        if (syncAborted()) { summary = "同步已中止（退出中）"; return false; }
        auto tree = api.listTree(branch);
        if (tree.isFail()) {
            summary = "读取远端笔记树失败：" + tree.errorMessage();
            return false;
        }
        const std::string notesDirPrefix = gsync::kRemoteNotesDir;
        std::map<std::string, std::string> remoteNoteShas;   // 文件名 → 远端 sha
        for (const auto& entry : tree.value()) {
            if (entry.path.rfind(notesDirPrefix, 0) != 0 ||
                entry.path.size() <= notesDirPrefix.size() ||
                entry.path.compare(entry.path.size() - 3, 3, ".md") != 0) {
                continue;
            }
            remoteNoteShas.emplace(entry.path.substr(notesDirPrefix.size()),
                                   entry.sha);
        }

        // ④ 全量判态（纯本地 + 已取到的树；零写入）：每个名字过 classifyNote →
        // pushActionForState。有任一 Conflict 就先把清单列进确认框并中止——
        // 不推任何笔记、不动远端（database.sql 也留到冲突判定之后才 PUT）。
        std::map<std::string, gsync::LocalNote> local;
        for (auto& [name, content] : collectLocalNotes()) {
            local.emplace(name,
                          gsync::LocalNote{std::move(content),
                                           gsync::contentHash(content)});
        }
        auto synced = githubSyncStore_->syncedNoteRecords();
        if (synced.isFail()) {
            summary = "读取同步登记失败：" + synced.errorMessage();
            return false;
        }
        struct NotePlan {
            std::string name;
            gsync::NoteSyncState state;
            gsync::PushAction action;
        };
        std::vector<NotePlan> plans;
        std::vector<std::string> putNames;   // 覆盖清单用：本地有改动的笔记
        {
            std::set<std::string> names;
            for (const auto& [name, note] : local) {
                (void)note;
                names.insert(name);
            }
            for (const auto& [name, rec] : synced.value()) {
                (void)rec;
                names.insert(name);
            }
            for (const auto& name : names) {
                const auto rec = synced.value().find(name);
                const auto loc = local.find(name);
                const bool hasRecord = rec != synced.value().end();
                const bool localExists = loc != local.end();
                const bool localHashMatches =
                    hasRecord && localExists &&
                    !rec->second.contentHash.empty() &&
                    rec->second.contentHash == loc->second.hash;
                const bool remoteExists = remoteNoteShas.count(name) != 0;
                const bool remoteShaMatches =
                    hasRecord && remoteExists &&
                    remoteNoteShas[name] == rec->second.sha;
                const auto state = gsync::classifyNote(
                    hasRecord, localExists, localHashMatches, remoteShaMatches);
                const auto action = gsync::pushActionForState(
                    state, remoteExists, remoteShaMatches);
                if (action == gsync::PushAction::Put) putNames.push_back(name);
                plans.push_back(NotePlan{name, state, action});
            }
        }
        for (const auto& plan : plans) {
            if (plan.action != gsync::PushAction::Conflict) continue;
            conflict = true;
            summary = plan.state == gsync::NoteSyncState::LocalDeleted
                          ? ("远端笔记 notes/" + plan.name +
                             " 已被其他设备改动，本地删除未传播（409 冲突）")
                          : ("笔记 notes/" + plan.name +
                             " 本地与远端都有改动（409 冲突）");
            // 弹窗「拉取远端后再推」的拉取会把远端内容下载回本地并更新登记
            // （Diverged 与 LocalDeleted+远端变 都在 pull 的 Download 集里），
            // 重判 InSync 后本轮推送即可通过——不会原地打转。
            stashOverwriteListForConflict(putNames);
            return false;
        }

        // ⑤ database.sql（sha 作冲突检测：远端已变 → 409）
        publishGithubSyncProgress("④ 上传 database.sql…");
        if (syncAborted()) { summary = "同步已中止（退出中）"; return false; }
        std::string sha;
        if (probe.isOk() && probe.value().exists) sha = probe.value().sha;
        auto put = api.putFile(gsync::kRemoteSqlPath, dump,
                               "DevDesk sync: database.sql", sha);
        if (put.isFail()) {
            if (put.errorCode() == core::ErrorCode::Conflict) {
                conflict = true;
                summary = "远端 database.sql 已被改动（409 冲突）";
                stashOverwriteListForConflict(putNames);
            } else {
                summary = "上传 database.sql 失败：" + put.errorMessage();
            }
            return false;
        }
        // 登记 PUT 响应的新 sha：下次拉取据此识别「远端未变」。
        if (!put.value().empty()) {
            if (auto rec = githubSyncStore_->recordSyncedSqlSha(put.value());
                rec.isFail()) {
                LOG_WARN("Settings: database.sql 登记失败: " +
                         rec.errorMessage());
            }
        }

        // ⑥ 执行（PUT / DELETE），单文件粒度 = 单次远端 commit
        publishGithubSyncProgress("⑤ 同步笔记…");
        int putCount = 0;
        int delCount = 0;
        int deleteSkipCount = 0;   // 安全栏跳过数（进 UI 提示）
        for (const auto& plan : plans) {
            // 退出取消位——每个 PUT/DELETE（含 store 登记写）前查一次
            if (syncAborted()) { summary = "同步已中止（退出中）"; return false; }
            const std::string path = gsync::remoteNotePath(plan.name);
            if (plan.action == gsync::PushAction::Put) {
                // shaIfMatch：LocalModified 用登记 sha（远端动过 → 409，真并发
                // 检测）；New 或登记缺 sha 用树里的现值（无 → 无 sha 创建）。
                std::string shaIfMatch;
                const auto rec = synced.value().find(plan.name);
                if (rec != synced.value().end() && !rec->second.sha.empty()) {
                    shaIfMatch = rec->second.sha;
                } else if (remoteNoteShas.count(plan.name) != 0) {
                    shaIfMatch = remoteNoteShas[plan.name];
                }
                auto r = api.putFile(path, local.at(plan.name).content,
                                     "DevDesk sync: " + path, shaIfMatch);
                if (r.isFail()) {
                    if (r.errorCode() == core::ErrorCode::Conflict) {
                        // 判态与 PUT 之间远端又变了（竞态）→ 仍走冲突路径
                        conflict = true;
                        summary = "远端笔记 " + path + " 已被改动（409 冲突）";
                        stashOverwriteListForConflict(putNames);
                    } else {
                        summary = "上传笔记 " + path + " 失败：" +
                                  r.errorMessage();
                    }
                    return false;
                }
                // 登记 PUT 响应的新 sha + 本地内容 hash（hash 是下轮 InSync
                // 短路的依据）；新 sha 拿不到（响应解析失败）→ 登记空 sha。
                if (auto rec = githubSyncStore_->recordSyncedNote(
                        plan.name, r.value(), local.at(plan.name).hash);
                    rec.isFail()) {
                    LOG_WARN("Settings: 笔记推送登记失败 " + path + ": " +
                             rec.errorMessage());
                }
                ++putCount;
                continue;
            }
            if (plan.action == gsync::PushAction::Delete) {
                // 安全栏：登记有而本地读不到 ≠ 本地已删。文件还在磁盘上
                //（本轮读内容失败等瞬态原因）→ 不删远端、不清登记，下轮再判。
                std::error_code existsEc;
                const bool stillThere =
                    std::filesystem::exists(
                        ::core::paths::markdownDir() / plan.name, existsEc) &&
                    !existsEc;
                if (stillThere) {
                    LOG_WARN("Settings: 本地文件仍在但未能读取，跳过删除远端 " +
                             path);
                    ++deleteSkipCount;   // 跳过不能只进日志
                    continue;
                }
                // 远端已无此文件（LocalDeleted 判态里 remoteExists=false）→
                // 无需 DELETE，只清登记。DELETETE 的 sha 用树里的现值（判态时
                // 已确认 == 登记 sha，即未被别的设备改过——C6）。
                if (remoteNoteShas.count(plan.name) != 0) {
                    auto r = api.deleteFile(path,
                                            "DevDesk sync: delete " + path,
                                            remoteNoteShas[plan.name]);
                    if (r.isFail()) {
                        summary = "删除远端笔记 " + path + " 失败：" +
                                  r.errorMessage();
                        return false;
                    }
                }
                if (auto f = githubSyncStore_->forgetSyncedNote(plan.name);
                    f.isFail()) {
                    LOG_WARN("Settings: 笔记删除登记清理失败 " + plan.name +
                             ": " + f.errorMessage());
                }
                ++delCount;
            }
            // Skip：InSync（未变）/ RemoteModified（远端领先，交由 pull）
        }
        summary = "已推送 database.sql 与 " + std::to_string(putCount) +
                  " 篇笔记" + (delCount > 0 ? "（删除 " + std::to_string(delCount) +
                                              " 篇）" : "");
        if (deleteSkipCount > 0) {
            // 安全栏跳过不能只进日志（静默失败面）→ 卡片黄字
            std::lock_guard<std::mutex> lk(githubSyncMutex_);
            githubSyncStagedDeleteSkipHint_ =
                "有 " + std::to_string(deleteSkipCount) +
                " 篇本地仍在但读取失败的笔记，已跳过远端删除（下轮重试）";
        }
        return true;
    }

    // 拉取（全树列举 + 登记 sha 状态比对）：树列举 → database.sql 按登记 sha 判变 →
    // 笔记按登记比对下载。远端已删的本地笔记保留（保守语义）；
    // 远端库未变（树 sha == 登记）→ 跳过导入，本地未推改动不被旧数据覆盖。
    bool runGithubSyncPull(platform::github::GithubApi& api,
                           const std::string& branch, std::string& summary) {
        publishGithubSyncProgress("① 列举远端文件树…");
        auto tree = api.listTree(branch);
        if (tree.isFail()) {
            summary = "列举远端文件树失败：" + tree.errorMessage();
            return false;
        }
        const std::string notesDirPrefix = gsync::kRemoteNotesDir;
        std::string remoteSqlSha;
        std::map<std::string, std::string> remoteNotes;   // 文件名 → 远端 sha
        for (const auto& entry : tree.value()) {
            if (entry.path == gsync::kRemoteSqlPath) {
                remoteSqlSha = entry.sha;
                continue;
            }
            if (entry.path.rfind(notesDirPrefix, 0) != 0 ||
                entry.path.size() <= notesDirPrefix.size() ||
                entry.path.compare(entry.path.size() - 3, 3, ".md") != 0) {
                continue;
            }
            remoteNotes.emplace(entry.path.substr(notesDirPrefix.size()),
                                entry.sha);
        }

        auto* db = ::core::appDatabase();
        if (db == nullptr) {
            summary = "数据库未就绪";
            return false;
        }
        if (githubSyncStore_ == nullptr) {
            summary = "同步存储未就绪";
            return false;
        }
        bool sqlImported = false;
        std::string notesBackupNameOut;   // 摘要用：库导入发生时的笔记备份目录名

        // ② 笔记动作先算（纯本地，零网络）：拉取覆盖面 = 导入 database.sql 或
        // 下载笔记任一发生 → 先整目录备份笔记，再动库。
        auto synced = githubSyncStore_->syncedNoteRecords();
        if (synced.isFail()) {
            summary = "读取笔记登记失败：" + synced.errorMessage();
            return false;
        }
        std::map<std::string, gsync::LocalNote> localNotes;
        for (auto& [name, content] : collectLocalNotes()) {
            localNotes.emplace(name,
                               gsync::LocalNote{std::move(content),
                                                gsync::contentHash(content)});
        }
        const auto actions = gsync::pullNoteActions(
            remoteNotes, localNotes, synced.value());
        int downloadCount = 0;
        for (const auto& action : actions) {
            if (action.kind == gsync::PullAction::Kind::Download) {
                ++downloadCount;
            }
        }
        auto sqlRec = githubSyncStore_->syncedSqlSha();
        if (sqlRec.isFail()) {
            summary = "读取 database.sql 登记失败：" + sqlRec.errorMessage();
            return false;
        }
        const bool sqlImportPlanned =
            !remoteSqlSha.empty() && sqlRec.value() != remoteSqlSha;

        if (sqlImportPlanned || downloadCount > 0) {
            if (syncAborted()) { summary = "同步已中止（退出中）"; return false; }
            // 拉取覆盖前整目录备份笔记（与库备份同目录、独立计数、各保 5 份）。
            publishGithubSyncProgress("② 备份本地数据…");
            std::string notesBackupName;
            auto nb = backupNotesDir(notesBackupName);
            if (nb.isFail()) {
                summary = "笔记备份失败，已中止拉取：" + nb.errorMessage();
                return false;
            }
            notesBackupNameOut = notesBackupName;
            if (sqlImportPlanned) {
                // 覆盖本地 DB 前必须先备份（备份失败即中止，绝不在无退路时覆盖）。
                std::string backupName;
                auto backup = backupLocalDb(*db, backupName);
                if (backup.isFail()) {
                    summary = "备份失败，已中止拉取：" + backup.errorMessage();
                    return false;
                }
                publishGithubSyncProgress("③ 下载并导入数据库…");
                auto remote = api.getFile(gsync::kRemoteSqlPath, branch);
                if (remote.isFail()) {
                    summary = "下载 database.sql 失败：" + remote.errorMessage();
                    return false;
                }
                if (!remote.value().exists || remote.value().content.empty()) {
                    summary = "远端 database.sql 不存在或为空";
                    return false;
                }
                // 经 store 导入——begin/end 事件包住（订阅方暂停/恢复后台
                // 写者），成功发 imported（全局重载广播）+ PRAGMA 复位。
                auto imported =
                    githubSyncStore_->importDatabase(remote.value().content);
                if (imported.isFail()) {
                    summary = "导入失败：" + imported.errorMessage();
                    return false;
                }
                // 登记树 sha：下次拉取据此识别「远端未变」。
                if (auto rec = githubSyncStore_->recordSyncedSqlSha(remoteSqlSha);
                    rec.isFail()) {
                    LOG_WARN("Settings: database.sql 登记失败: " +
                             rec.errorMessage());   // 只影响下轮跳过判定，不算失败
                }
                sqlImported = true;
                // app_config 表被 DROP 重建，本模块缓存指向旧行 → 通知 harvest 重载。
                githubSyncStagedDbImported_.store(true,
                                                  std::memory_order_release);
            }
        }

        // ③ 笔记：按登记 sha 比对，只下载远端变过的
        publishGithubSyncProgress("④ 同步笔记…");
        int dlCount = 0;
        int keepCount = 0;
        for (const auto& action : actions) {
            // 退出取消位——每次下载写盘 / 登记写库前查一次
            if (syncAborted()) { summary = "同步已中止（退出中）"; return false; }
            if (action.kind == gsync::PullAction::Kind::KeepLocal) {
                LOG_INFO("Settings: 远端已删除笔记 " + action.fileName +
                         "，本地保留（pull 不替远端传播删除）");
                // 清登记——远端被别设备删掉、本地未改的笔记若留着旧登记，
                // 下轮 push 按 InSync 短路永不重推（远端永远缺这篇），本地一改
                // 又带失效 sha PUT → 409/422 死循环。清掉后下轮 push 判 New，
                // shaIfMatch 取远端树现值（不逐篇 GET；远端无此名 →
                // 无 sha PUT 重建，local wins）。
                if (auto f = githubSyncStore_->forgetSyncedNote(action.fileName);
                    f.isFail()) {
                    LOG_WARN("Settings: 笔记基线清理失败 " + action.fileName +
                             ": " + f.errorMessage());
                }
                ++keepCount;
                continue;
            }
            auto f = api.getFile(gsync::remoteNotePath(action.fileName), branch);
            if (f.isFail() || !f.value().exists) {
                summary = "下载笔记 " + action.fileName + " 失败：" +
                          (f.isFail() ? f.errorMessage()
                                      : std::string("远端已不存在"));
                return false;
            }
            if (!writeFileFromString(
                    ::core::paths::markdownDir() / action.fileName,
                    f.value().content)) {
                summary = "写入笔记文件失败：" + action.fileName;
                return false;
            }
            // 登记树 sha + 下载内容的 hash：下次 push 据此 InSync 短路、
            // 下次 pull 据此跳过未变文件。
            if (auto rec = githubSyncStore_->recordSyncedNote(
                    action.fileName, remoteNotes[action.fileName],
                    gsync::contentHash(f.value().content));
                rec.isFail()) {
                LOG_WARN("Settings: 笔记拉取登记失败 " + action.fileName + ": " +
                         rec.errorMessage());
            }
            ++dlCount;
        }
        summary = std::string(sqlImported ? "已导入 database.sql"
                                          : "database.sql 未变，未导入") +
                  "；下载 " + std::to_string(dlCount) + " 篇笔记" +
                  (keepCount > 0
                       ? "（远端已删 " + std::to_string(keepCount) +
                             " 篇，本地保留）"
                       : "") +
                  (!notesBackupNameOut.empty()
                       ? "（本地备份：" + notesBackupNameOut + "）"
                       : "");
        return true;
    }

    // 备份本地库到 backups/devdesk-<ts>.db（保留最近 5 份）。
    // 先 wal_checkpoint 再整文件拷贝：WAL 里未回写的页不 checkpoint 会丢。
    core::Result<void> backupLocalDb(core::data::SqliteDb& db,
                                     std::string& outName) {
        const auto dbFile = ::core::paths::sqliteDbFile();
        const auto dir = ::core::paths::appDataDir() / gsync::kBackupDirName;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
                                            "创建备份目录失败：" + ec.message());
        }
        auto cp = db.exec("PRAGMA wal_checkpoint(TRUNCATE)");
        if (cp.isFail()) {
            LOG_WARN("Settings: wal_checkpoint 失败（继续备份）: " +
                     cp.errorMessage());
        }
        const std::string name = gsync::backupFileName(nowSec());
        const auto target = dir / name;
        std::filesystem::copy_file(dbFile, target,
                                   std::filesystem::copy_options::overwrite_existing,
                                   ec);
        if (ec) {
            return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
                                            "拷贝数据库失败：" + ec.message());
        }
        gsync::pruneBackups(dir, gsync::kBackupPrefix, gsync::kBackupKeep);
        outName = name;
        return core::Result<void>::ok();
    }

    // 拉取覆盖前整目录备份笔记到 backups/notes-<ts>/（与库备份同目录、
    // 前缀隔离、独立计数，各保 5 份）。只拷 .md；笔记目录不存在 → 无可备份 = ok。
    core::Result<void> backupNotesDir(std::string& outName) {
        const auto srcDir = ::core::paths::markdownDir();
        std::error_code ec;
        if (!std::filesystem::is_directory(srcDir, ec)) {
            return core::Result<void>::ok();
        }
        const auto dir = ::core::paths::appDataDir() / gsync::kBackupDirName;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
                                            "创建备份目录失败：" + ec.message());
        }
        const std::string name = gsync::notesBackupDirName(nowSec());
        const auto target = dir / name;
        std::filesystem::create_directories(target, ec);
        if (ec) {
            return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
                                            "创建笔记备份目录失败：" +
                                                ec.message());
        }
        for (std::filesystem::directory_iterator it(srcDir, ec), end;
             it != end; it.increment(ec)) {
            if (ec) break;
            std::error_code fec;
            if (!it->is_regular_file(fec)) continue;
            const std::string fname = it->path().filename().string();
            if (fname.size() < 3 ||
                fname.compare(fname.size() - 3, 3, ".md") != 0) {
                continue;
            }
            std::filesystem::copy_file(
                it->path(), target / fname,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                return core::Result<void>::fail(
                    core::ErrorCode::FileWriteFailed,
                    "拷贝笔记备份失败：" + fname + ": " + ec.message());
            }
        }
        gsync::pruneBackups(dir, gsync::kNotesBackupPrefix, gsync::kBackupKeep,
                            "");
        outName = name;
        return core::Result<void>::ok();
    }

    // 本地笔记快照（文件名 → 内容）。
    static std::map<std::string, std::string> collectLocalNotes() {
        std::map<std::string, std::string> out;
        std::error_code ec;
        const auto dir = ::core::paths::markdownDir();
        if (!std::filesystem::is_directory(dir, ec)) return out;
        for (std::filesystem::directory_iterator it(dir, ec), end; it != end;
             it.increment(ec)) {
            if (ec) break;
            std::error_code fec;
            if (!it->is_regular_file(fec)) continue;
            const std::string name = it->path().filename().string();
            if (name.size() < 3 || name.compare(name.size() - 3, 3, ".md") != 0) {
                continue;
            }
            std::string content;
            if (readFileToString(it->path(), content)) {
                out.emplace(name, std::move(content));
            }
        }
        return out;
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
        if (githubSyncStagedDbImported_.exchange(false,
                                                 std::memory_order_acq_rel)) {
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
        githubSyncErrorDialog_.variant =
            conflict ? GitErrorDialogVariant::Conflict409
                     : GitErrorDialogVariant::Error;
        // 手动操作失败 → 弹窗；自动同步失败只留卡片红字 + 详情入口。
        // 启用流程的校验失败同自动路径（用户刚拨开关，此刻弹窗等于打断）。
        const bool silentFailure =
            githubSyncLastAction_ == SyncAction::ValidateFirstSync;
        if (githubSyncLastAction_ != SyncAction::None && githubSyncLastManual_ &&
            !silentFailure) {
            githubSyncErrorDialog_.open = true;
        }
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
        const float pad = t.sizes.spacingXXLarge;
        const float contentW = screen.width - pad * 2.0f;

        // 页面底色 = 主窗口 clearColor，不自铺不透明底。

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

                // tab 栏：components::tabs 无 .position，套定位 stack。
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
                                if (idx == kTabLog) logTailDirty_ = true;
                                if (idx == kTabModules) refreshExternalState();
                            })
                            .build();
                    })
                    .build();

                // 当前 tab 的卡区仍套 scrollView（4+ 卡总高可超小窗口可视高）。
                const float cardsTopY = pad + headerH +
                    t.sizes.spacingLarge + tabH + t.sizes.spacingLarge;
                const float cardsH = screen.height - cardsTopY - pad;
                components::scrollView(ui, "settings.cards.scroll")
                    .size(contentW, cardsH)
                    .gap(t.sizes.spacingLarge)
                    .step(60.0f)
                    .theme(compTheme)
                    // contentKey 带 tab 索引 + 代理卡高 + 卡片显隐签名：切 tab / 卡片增减时缓存失效重测。
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
                        drawCardRegion(inner, contentWidth, cardsTopY, cardsH,
                                       screen.height, t, proxyH, svc);
                    })
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
        a.applyFontPreset = [this](int idx) { applyFontPreset(idx); };
        a.applyWindowEffect = [this](int idx) { applyWindowEffectPreset(idx); };
        a.applyModuleEnabled = [this](const std::string& moduleId, bool on) {
            applyModuleEnabled(moduleId, on);
        };
        a.setDropdownOpen = [this](Dropdown d, bool v) { setDropdownOpen(d, v); };
        a.changePetSkinAt = [this](int idx) { changePetSkinAt(idx); };
        a.changePetScaleAt = [this](int idx) { changePetScaleAt(idx); };
        a.resetPetPosition = [this] { resetPetPosition(); };
        a.showPetWindow = [this] { showPetWindow(); };
        a.openNotesDirectory = [this] { openNotesDirectory(); };
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

        return a;
    }

    // 注册表卡高解析：Fixed = 表内常量；ProxyWarnings = 警告行数推导；FillRemainder = 剩余视口高。
    static float cardHeight(const CardSpec& spec, float proxyH, float remain,
                            bool githubSyncEnabled) {
        switch (spec.heightKind) {
        case CardHeightKind::Fixed:
            // spec.height 是 100% 档基准，运行期随生效字号放大：卡高不同步放大就会把尾行挤出卡外。
            return roundFs(spec.height);
        case CardHeightKind::ProxyWarnings:
            return proxyH;
        case CardHeightKind::FillRemainder:
            return std::max(260.0f, remain);
        case CardHeightKind::GithubSync:
            // 未启用时卡片早退（不画表单）→ 用折叠高，否则留一大片空面板。
            return roundFs(githubSyncEnabled ? kGithubSyncCardHeight
                                             : kGithubSyncCollapsedHeight);
        }
        return spec.height;  // 兜底：枚举穷举，仅为满足「所有路径有返回」
    }

    // 遍历注册表画本 tab 的卡；纵向游标只累计参与绘制的卡，带下拉的卡需页面绝对 Y（openUp）。
    void drawCardRegion(eui::Ui& ui, float contentWidth, float cardsTopY,
                        float cardsH, float screenH,
                        const core::theme::Tokens& t, float proxyH,
                        ::core::IClaudeProxyService* svc) {
        SettingsCardsIn in{cfg_, petSkins_, petSkinName_, proxyPortDraft_,
                           logTail_, petScaleLabels_};
        in.logTailVersion = logTailVersion_;
        in.logViewScrollOffset = logViewScrollOffset_;
        in.takeoverManaged = takeoverManaged_;
        in.notesDirExists = notesDirExists_;
        in.openDropdown = openDropdown_;
        in.petScaleIndex = petScaleIdx_;
        in.proxy = svc;
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
        const SettingsCardActions a = cardActions();
        const float gapY = t.sizes.spacingLarge;
        float cardY = 0.0f;
        // 越界 tab（不可达）回落通用页
        const int tab = (settingsTab_ >= 0 && settingsTab_ < kSettingTabCount)
                            ? settingsTab_
                            : kTabGeneral;
        for (const CardSpec& spec : kCardSpecs) {
            if (spec.tab != tab) continue;
            if (spec.gate != nullptr && !moduleEnabled(cfg_, spec.gate)) {
                continue;  // 模块关 → 整卡隐藏（不显示故障态）
            }
            const float h =
                cardHeight(spec, proxyH, cardsH - cardY, githubSyncCfg_.enabled);
            CardDrawArgs args{ui, spec.id, contentWidth, h, t,
                              cardsTopY + cardY - cardsScrollOffset_, screenH,
                              in, a};
            spec.draw(args);
            cardY += h + gapY;
        }
    }

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
                            ? "所有变更即时保存到本地数据库 · 左侧侧边栏切换页面"
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
        // 「打开目录」是唯一创建笔记目录的动作——刷新存在性快照。
        refreshExternalState();
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
        std::error_code ec;
        notesDirExists_ =
            std::filesystem::exists(::core::paths::markdownDir(), ec) && !ec;
        petSkins_ = ::module::pet::discoverSkins();
        petSkinName_ = ::module::pet::currentSkinName();
        // 显示大小三档：文案 + 当前档下标快照（下拉 items/selected 渲染源）
        petScaleLabels_ = ::module::pet::petScalePresetLabels();
        petScaleIdx_ = ::module::pet::currentPetScaleIndex();
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
    // GitHub Sync 后台任务：照 quotaTestThread_ 模式（原子完成位 + 每帧 compose 收割）。
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
    std::string githubSyncDeleteSkipHint_;   // 删远端跳过提示（UI 侧）
    // EventBus 订阅 token 必须持有：token 析构即退订（丢弃 = 订阅立刻失效）。
    std::vector<::core::event::EventBus::Subscription> githubSyncSubs_;
    // app.read_settings 工具：onStart 注册 / onStop 注销；与注册表同寿（注销对未注册名是 no-op）。
    std::shared_ptr<core::ITool> appReadSettingsTool_;
    // 当前打开的下拉（7 个 bool 收成一个索引，-1 = 全关）；取值 = static_cast<int>(Dropdown)。
    int openDropdown_ = -1;
    std::vector<std::string> petSkins_;    // 发现到的皮肤（快照见 refreshExternalState）
    std::string petSkinName_;              // 当前皮肤名（同上）
    std::vector<std::string> petScaleLabels_;  // 显示大小三档文案（桥快照）
    int petScaleIdx_ = 1;                  // 当前显示大小档下标（同上）
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
    bool notesDirExists_ = false;         // Markdown 笔记目录存在性
    float logViewScrollOffset_ = 0.0f;    // 日志查看卡内滚动位置
    std::string statusMessage_;
    bool statusIsError_ = false;
    // toast 单实例状态位（EUI-NEO toast 无队列，见 ui_kit/toast.h）
    ui_kit::ToastState toast_;
};

} // namespace module::settings

namespace module::settings {

void runGithubSyncExitPush() {
    if (gExitHookTarget != nullptr) gExitHookTarget->runExitSyncPush();
}

}  // namespace module::settings

extern "C" ::core::Module* create_settings_module() {
    return new module::settings::SettingsModule();
}
