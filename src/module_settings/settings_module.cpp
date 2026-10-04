// src/module_settings/settings_module.cpp
// 设置页模块：Module 接口实现、生命周期、设置变更动作、日志 tail 与外部状态快照、
// 卡片注册表（kCardSpecs）；卡片绘制在 settings_ui_cards.*，纯逻辑在 settings_ui_logic.*。
// 页面 = 通用 / 模块 / 日志 三 tab。
// 依赖边界：module_* 互不依赖 —— claude 经 core 窄接口、pet 经 module_pet_bridge.h、
// 模块开关经 module_toggle_bridge.h，均单向无环。
// 渲染路径不做 fs 查询：接管状态 / 笔记目录存在性 / 皮肤清单统一在 refreshExternalState
// 装载，compose 只读快照。

#include "app_tools.h"
#include "settings_config.h"
#include "settings_ui_cards.h"
#include "settings_ui_logic.h"

#include "core/claude_proxy_service.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/service_locator.h"
#include "core/theme.h"
#include "core/tool.h"
#include "core/component_theme.h"
#include "platform/file_open.h"
#include <core/platform/platform.h>   // core::platform::requestUiUpdate（唤醒主循环）
#include "platform/system_theme.h"

// module_pet 解耦桥（中立桥头，src 根、应用层）：settings 不 include module_pet 头、
// CMake 不 link，符号在最终可执行文件链接期解析；签名漂移 static_assert 见该头。
#include "module_pet_bridge.h"

// 模块开关即时生效桥（module_toggle_bridge.h，同 module_pet_bridge 模式）：实现 =
// 函数指针缝 + 默认 no-op，真 handler 由 main.cpp 装配段注入。
#include "module_toggle_bridge.h"

#include <ui_kit/metrics.h>
#include <ui_kit/toast.h>

#include <eui/dsl.h>

#include <components/scrollview.h>
#include <components/tabs.h>
#include <components/theme.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace module::settings {

namespace {

// 卡片注册表（R6）：每行 = 一张卡（tab 归属 / 门控 / 卡高种类 / 是否吃页面几何 /
// 绘制函数）；顺序 = 绘制顺序。加卡只改此表。
enum class CardHeightKind {
    Fixed,          // 用 spec.height
    ProxyWarnings,  // 代理卡：由同步警告行数推导（compose 顶部算好的 proxyH）
    FillRemainder,  // 日志查看卡：吃 scrollView 剩余视口高（外层负责滚动）
};

struct CardSpec {
    int tab;                // 所属 tab（kTabGeneral / kTabModules / kTabLog）
    const char* id;         // 卡根元素 id
    const char* gate;       // nullptr = 恒显；否则 = 模块开关门控键（插拔方案 A §5.4）
    CardHeightKind heightKind;
    float height;           // Fixed 档生效
    bool pageGeom;          // 下发卡顶页面绝对 y + 屏高（下拉 openUp 几何）
    void (*draw)(const CardDrawArgs&);
};

// 门控读的是「下次启动的卡布局」的同一份配置（cfg_）——拨开关即时预览
const CardSpec kCardSpecs[] = {
    // 通用 tab：外观（主题 + 字号缩放 + 窗口效果）+ 编辑器字号
    {kTabGeneral, "settings.card.appearance", nullptr, CardHeightKind::Fixed,
     kAppearanceCardHeight, false, &drawAppearanceCard},
    {kTabGeneral, "settings.card.editor", nullptr, CardHeightKind::Fixed,
     kEditorFontCardHeight, true, &drawEditorFontCard},
    // 模块 tab：插拔开关 + 桌宠 + 目录 + 代理（后三者按模块开关整卡隐藏）
    {kTabModules, "settings.card.modules", nullptr, CardHeightKind::Fixed,
     kModulesCardHeight, false, &drawModulesCard},
    {kTabModules, "settings.card.pet", kModuleIdPet, CardHeightKind::Fixed,
     kPetCardHeight, false, &drawPetCard},
    {kTabModules, "settings.card.directory", kModuleIdMarkdown,
     CardHeightKind::Fixed, kDirectoryCardHeight, false, &drawDirectoryCard},
    {kTabModules, "settings.card.proxy", kModuleIdClaudeProxy,
     CardHeightKind::ProxyWarnings, 0.0f, false, &drawProxyCard},
    // 日志 tab：保留天数 + 级别卡 + tail 查看卡（后者吃剩余视口高）
    {kTabLog, "settings.card.log", nullptr, CardHeightKind::Fixed,
     kLogSettingsCardHeight, true, &drawLogCard},
    {kTabLog, "settings.tab.log", nullptr, CardHeightKind::FillRemainder,
     0.0f, false, &drawLogViewCard},
};

} // namespace

class SettingsModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::Settings; }
    std::string displayName() const override { return "设置"; }
    std::string shortName() const override { return "设置"; }
    std::string iconGlyph() const override { return eui::utf8(0xF013); }  // fa-gear

    void onStart() override {
        store_ = std::make_unique<SettingsStore>(::core::appDatabase());
        cfg_ = store_->load();
        // 注册 app.read_settings（agent 只经 ToolRegistry 按名取用；工具自持
        // SettingsStore 纯读，agent 工作线程并发安全）。name 冲突记日志不中断。
        appReadSettingsTool_ =
            makeAppReadSettingsTool(::core::appDatabase());
        auto toolReg =
            ::core::ToolRegistry::instance().registerTool(appReadSettingsTool_);
        if (toolReg.isFail()) {
            LOG_ERROR("Settings: app.read_settings 注册失败: " +
                      toolReg.errorMessage());
        }
        // 启动即应用持久化主题 + 字号缩放（main.cpp 也做过同样恢复，这里幂等重放）；
        // applyThemeRuntime 在 auto 模式按 platform 检测结果解析深浅。
        applyThemeRuntime();
        ::core::theme::setFontScale(
            static_cast<float>(cfg_.fontScalePercent) / 100.0f);
        // 编辑器字号经 core::theme 通道传给 markdown（模块隔离：markdown 不 include 本模块）。
        ::core::theme::setEditorFontSize(
            static_cast<float>(cfg_.editorFontSize));
        // 把持久化日志级别应用到运行时（core::log::setLevel 已存在，即时生效）。
        ::core::log::setLevel(parseLogLevel(cfg_.logLevel));
        LOG_INFO("Settings 模块启动（主题=" + ::core::theme::currentThemeName() +
                 "（模式=" + cfg_.themeMode + "）" +
                 ", 字号=" + std::to_string(cfg_.fontScalePercent) + "%" +
                 ", 编辑器字号=" + std::to_string(cfg_.editorFontSize) + "px" +
                 ", 日志级别=" + cfg_.logLevel +
                 ", 日志保留=" + retentionLabel(cfg_.logRetentionDays) + "）");
        // 日志 tail 首次装载同步做一次（onStart 非渲染路径）：compose 恒读快照，
        // 首帧必须有值——异步首刷会让日志查看卡先闪一帧「暂无日志内容」。
        applyLogTail(readLogTailLines());
        // 外部状态（接管 / 笔记目录存在性 / 皮肤清单）同点首次装载，compose 只读快照。
        refreshExternalState();
    }

    void onStop() override {
        // 工具先注销再释放（teardown 契约：收尾最后一步注销工具，registry find()
        // 返回 shared_ptr keep-alive，在跑 run 不会悬垂）。
        ::core::ToolRegistry::instance().unregisterTool("app.read_settings");
        appReadSettingsTool_.reset();
        store_.reset();
        LOG_INFO("Settings 模块停止");
    }

    // 重新进入设置页（auto 模式回到本页重新解析系统深浅）
    void onPageEnter() override {
        // auto 模式的跟随点：系统深浅运行中变更后回到本页即重新解析（EUI-NEO 无
        // 系统主题变更回调，无法任意页面实时跟随——已登记为限制）。
        if (cfg_.themeMode == kThemeModeAuto) {
            applyThemeRuntime();
        }
        // 进页面 = 外部状态刷新点；变化频率低，不需要每帧重查。
        refreshExternalState();
    }

    // Ctrl+S 显式保存：设置页除端口草稿外全部即时落盘，故 Ctrl+S = 提交唯一待决
    // 变更（端口草稿非空且 ≠ 已应用端口 → applyProxyPort；否则重写配置 + 回显
    //「已保存」）。「重新生成 Token」确认弹窗打开时不响应。
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

    // 按 themeMode 解析并应用深浅（系统检测经 platform::prefersDarkTheme，模块层
    // 可依赖 platform 而 core 不可，故解析在此）。结果写回 cfg_.darkTheme 供落盘。
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
        // 写入 core::theme 全局缩放：token 读取层生效，所有模块零改动受益
        ::core::theme::setFontScale(
            static_cast<float>(cfg_.fontScalePercent) / 100.0f);
        persist();
        setStatus("字号缩放：" + std::to_string(cfg_.fontScalePercent) +
                  "%（下一帧全局生效）");
        LOG_INFO("Settings: 字号缩放 → " + std::to_string(cfg_.fontScalePercent) +
                 "%（core::theme token 层全局缩放）");
    }

    // 编辑器字号档位切换：写 core::theme 全局通道即时生效，persist 供下次启动恢复。
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

    // 窗口效果三档切换：clearColor alpha + backdrop 请求即时生效；系统不支持时
    // compose 每帧回查 activeWindowEffect 显示降级提示行（常驻比 toast 稳）。
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

    // 应用代理端口：解析 draft → 写 app_config → 同步运行面端口镜像 → 运行中自动
    // stop→start 重启。重启失败时代理已停、sidecar 已还原，状态栏提示可换端口重试。
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
            // 生效时机有两个（勿改回单一「下次启动」口径）：① 启动期装配注入
            //（main.cpp assembleClaudeServiceNow）；② 会话内开启 claude 模块时的
            // 惰性装配。下一个生效点出现时，这里与 main.cpp 那处必须同步更新。
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
        // stop→start 会改写/删除 sidecar；本页动作引起的状态变化就地刷新快照（成败都刷）。
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

    // 日志级别切档：core::log::setLevel 即时应用，persist 供下次启动恢复。切档同时
    // 置 logTailDirty_——否则同屏 tail 快照不动，用户看不到切档效果。
    void applyLogLevelPreset(int index) {
        if (index < 0 || index >= kLogLevelPresetCount) return;
        cfg_.logLevel = kLogLevelPresets[index];
        ::core::log::setLevel(parseLogLevel(cfg_.logLevel));
        persist();
        logTailDirty_ = true;   // 可见面即时跟随（C-5）
        setStatus("日志级别：" + cfg_.logLevel + "（即时生效）");
        LOG_INFO("Settings: 日志级别 → " + cfg_.logLevel + "（core::log::setLevel）");
    }

    // 端口输入变更：写草稿 + 校验错误随下一次输入清除。
    void setProxyPortDraft(const std::string& v) {
        proxyPortDraft_ = v;
        // 校验错误（红字）随下一次输入清除；成功提示（非红字）保留到下一次操作。
        if (statusIsError_) {
            statusMessage_.clear();
            statusIsError_ = false;
        }
    }

    // 桌宠换肤：下标越界守卫（含空清单）→ 桥调用。热切换即时可见，不补 toast。
    void changePetSkinAt(int idx) {
        if (idx < 0 || idx >= static_cast<int>(petSkins_.size())) {
            return;
        }
        petSkinName_ = petSkins_[static_cast<size_t>(idx)];
        ::module::pet::applyPetSkin(petSkinName_);
    }

    // 显示大小切档：下标守卫读同一份文案快照；选择即生效，不补 toast。
    void changePetScaleAt(int idx) {
        if (idx < 0 || idx >= static_cast<int>(petScaleLabels_.size())) {
            return;
        }
        ::module::pet::applyPetScaleIndex(idx);
        petScaleIdx_ = idx;
        LOG_INFO("Settings: 桌宠显示大小 → " +
                 petScaleLabels_[static_cast<size_t>(idx)]);
    }

    // 位置重置：回主屏右下角，持久化坐标清回未设置。
    void resetPetPosition() {
        ::module::pet::resetPetPosition();
        setStatus("桌宠位置已重置（主屏右下角）");
    }

    // 显示桌宠（PAB-13）：桥已收成四态枚举，按原因给 toast（未装配 = 中性提示非错误，
    // 启动失败 = 错误样式，已开着 / 成功各一句反馈）——避免「点了没效果」。
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

    // 桌宠开关即时可见性：桌宠是唯一有独立可见载体（无边框子窗）的模块——注册门控
    // 要等重启，但窗口不必：关 → 立即关窗，开 → 立即原位重开（未装配则提示重启生效）。
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
        // AlreadyOpen 与重开成功对用户是同一结果：窗口即在屏幕上，状态行收底即可。
        using Outcome = ::module::pet::PetShowOutcome;
        switch (::module::pet::showPetWindow()) {
            case Outcome::Shown:
            case Outcome::AlreadyOpen:
                setStatus("桌宠已开启并立即显示");
                LOG_INFO("Settings: 桌宠模块开启 → 窗口已重新入队开窗");
                break;
            case Outcome::NotAssembled:
                // 本会话未装配 = 窗口无从谈起，toast 给可见反馈（中性提示非错误，重启生效）。
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

    // 模块开关：只写 app_config + 经 app::toggle 桥发「开关意图」，即时生效经桥路由
    //（真 handler 由 main.cpp 装配段注入）。home/settings 恒启用；桌宠例外——额外做
    // 一次即时窗口同步（见 applyPetModuleVisibility）。
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

    // ---- Toast 反馈 ----
    // 页头副标题小字（setStatus）对「点卡片按钮」反馈太弱，桥失败分支改走 toast；
    // 状态位与绘制接线走 ui_kit（节点 id = settings.toast）。
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

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        const auto& t = core::theme::current();
        const float pad = t.sizes.spacingXXLarge;  // 40
        const float contentW = screen.width - pad * 2.0f;

        // 页面底色 = 主窗口 clearColor（随主题/窗口效果运行时跟随）——不自铺不透明底。

        const float headerH = ui_kit::kHeaderH;
        const float tabH = kSettingsTabHeight;
        // 日志 tail 读盘已移出渲染路径（后台线程）；渲染恒读快照 → 切 tab / 首帧不闪空。
        if (settingsTab_ == kTabLog) {
            refreshLogTailCache();
        }
        // contentKey 带 proxyH：警告行数变化时滚动高度缓存失效重测
        auto* svc = claudeProxy();
        // 接管状态读快照（原每帧 stat sidecar）。冷启动兜底：快照从未装载过时本帧同步刷一次。
        if (!externalStateLoaded_) {
            refreshExternalState();
        }
        size_t warnLines = 0;
        if (svc) {
            // 警告行不以「已接管」为展示门：takeOver 写 sidecar 前失败时 sidecar 不存在，
            // 但失败原因正是最该看见的一条（与详情页 claude_module.cpp 同口径）。
            warnLines = std::min<size_t>(
                svc->lastSyncWarnings().size(), 2);
        }
        // 代理卡高随生效字号派生：proxyCardHeightFor 是接管/警告行几何的单一来源。
        const float proxyH = proxyCardHeightFor(warnLines);
        // 模块插拔门控：claude 关 → 代理卡整卡隐藏，markdown 关 → 目录卡整卡隐藏。
        // 读 cfg_（= 启动注册状态的来源），拨开关即时预览下次启动的布局。
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
                // 已删 M3-4 主体渐变底板（settings.body.gradient/bg）：9 卡卡框早先已删，
                // 这就是用户眼中「最外层卡片」。删除后关档靠 clearColor 当底、半透/磨砂档
                // 露出窗口背景（回归反断言见 tests/test_ui_modern.cpp）。
                drawHeader(ui, contentW, headerH, t);

                // tab 栏：components::tabs 无 .position，套定位 stack（仿 claude detail tabs 写法）。
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
                                // 切到日志 tab：下一帧重读 tail；切到模块 tab：刷新外部状态快照。
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
                    // contentKey 带 tab 索引 + 代理卡高 + 卡片显隐签名：切 tab / 卡片增减时
                    // 滚动高度缓存失效重测（不带 tab 索引会沿用上个 tab 的高度）。
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

        // toast 每帧无条件绘制（显隐由状态位控制，同 claude/markdown 各站点）
        drawToast(ui, screen, t);

        LOG_DEBUG("Settings 模块 compose 完成");
    }

private:
    // 跨模块服务取用（审计 D6）：本页对 Claude 运行时的引用都经 core 只读服务定位器；
    // 未注册 = claude 模块关闭 / 服务未装配 → nullptr，调用点按「服务不可用」降级。
    static ::core::IClaudeProxyService* claudeProxy() {
        auto r =
            ::core::ServiceLocator::requireService<::core::IClaudeProxyService>();
        return r.isOk() ? r.value() : nullptr;
    }

    // 7 个下拉共用一个「谁打开着」索引（取值语义见 settings_ui_logic.h 的
    // isDropdownOpen / applyDropdownOpen）。
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
        return a;
    }

    // 注册表卡高解析：Fixed = 表内常量；ProxyWarnings = 代理卡警告行数推导；
    // FillRemainder = 日志查看卡吃剩余视口高（极小窗口兜底 260 由外层 scrollView 滚动）。
    static float cardHeight(const CardSpec& spec, float proxyH, float remain) {
        switch (spec.heightKind) {
        case CardHeightKind::Fixed:
            // spec.height 是 100% 档基准，运行期随生效字号放大（roundFs）：卡内行盒
            // 派生后内容底沿增长，卡高不同步放大就会把尾行挤出卡外。100% 档 roundFs(x)==x。
            return roundFs(spec.height);
        case CardHeightKind::ProxyWarnings:
            return proxyH;
        case CardHeightKind::FillRemainder:
            return std::max(260.0f, remain);
        }
        return spec.height;  // 兜底：枚举穷举，仅为满足「所有路径有返回」
    }

    // 卡区（R6）：遍历注册表画本 tab 的卡。纵向游标只累计本 tab 参与绘制的卡
    //（全量累加会让下拉方向算错）；带下拉的卡算 openUp 需页面绝对 Y。
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
        const SettingsCardActions a = cardActions();
        const float gapY = t.sizes.spacingLarge;
        float cardY = 0.0f;
        // 原 switch 的 `case kTabGeneral: default:`——越界 tab（不可达）回落通用页
        const int tab = (settingsTab_ >= 0 && settingsTab_ < kSettingTabCount)
                            ? settingsTab_
                            : kTabGeneral;
        for (const CardSpec& spec : kCardSpecs) {
            if (spec.tab != tab) continue;
            if (spec.gate != nullptr && !moduleEnabled(cfg_, spec.gate)) {
                continue;  // 模块关 → 整卡隐藏（不显示故障态）
            }
            const float h = cardHeight(spec, proxyH, cardsH - cardY);
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

    // 打开笔记目录：目录不存在先创建，再交平台文件管理器打开（Win/macOS/xdg-open）。
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
        // 「打开目录」是唯一创建笔记目录的动作——刷新存在性快照，目录卡本帧起不再显示「尚未创建」。
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

    // 纯读盘（static、不碰成员 → 可后台跑）：只读文件末尾窗口防大日志进内存，
    // 保留末尾 kLogTailMaxLines 行（滑动裁剪，避免逐行 erase 的 O(n²)）。
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

    // compose 只读快照 + 只发意图：① 上一轮读就绪则搬进快照（wait_for(0) 不阻塞）；
    // ② 脏标记置位且无在途读则发一次后台读意图。可见时机最多晚一个 tick。
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
        // 先就绪、后唤醒（与 claude_ui_logs.cpp 同一形态）：std::async 在 callable
        // 返回后才发布值，任务内先唤醒会让 UI 抢跑（收割看到未就绪 → 结果没人上屏）。
        auto promise =
            std::make_shared<std::promise<std::vector<std::string>>>();
        logTailFuture_ = promise->get_future().share();
        logTailWorker_ = std::async(std::launch::async, [promise] {
            try {
                promise->set_value(readLogTailLines());   // ① 结果落到共享状态
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
            core::platform::requestUiUpdate();            // ② 就绪之后才唤醒
        });
    }

    // 外部状态只读快照的刷新点（读一次 → 存成员 → compose 只读）：原先落在渲染路径的
    // 三处 fs 查询（sidecar stat / markdownDir stat / discoverSkins）全部移出。调用点全在
    // 非渲染路径（onStart / onPageEnter / 切模块 tab / 打开目录后）+ compose 冷启动兜底。
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
    // ---- tab 化 + 日志查看 ----
    int settingsTab_ = 0;                 // 当前 tab（通用/模块/日志）
    std::vector<std::string> logTail_;    // 日志文件末尾行内存快照（compose 只渲染）
    unsigned logTailVersion_ = 0;         // tail 读取版本号（进日志查看卡 contentKey）
    bool logTailDirty_ = true;            // 待重读标记（切日志 tab / 点刷新置位）
    bool logTailLoaded_ = false;          // 快照是否装载过（冷启动兜底，见 refreshLogTailCache）
    // 后台读盘结果槽 + worker（结果槽 = shared_future，worker set_value 发布 → 再
    // requestUiUpdate 唤醒）；在途判据 = logTailFuture_.valid()。
    std::shared_future<std::vector<std::string>> logTailFuture_;
    std::future<void> logTailWorker_;
    // ---- 外部状态只读快照（compose 不做 fs 查询）----
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

extern "C" ::core::Module* create_settings_module() {
    return new module::settings::SettingsModule();
}
