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
//      数字输入（总设计 §8.5 第 5 条，审计 [14]：默认 core::kDefaultProxyPort，
//      改后运行中自动重启）+ Claude Code 接管状态（Phase D UI 接入，数据源 =
//      core 只读服务定位器取的代理运行面，见下「依赖说明」）。
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
//   6. 模块管理（模块插拔方案 A，设计 2026-09-22 §5.4；活门控 2026-10-01
//      起**即时生效**）：四个工具模块
//      （markdown/todo/claude_proxy/agent）+ 桌宠各一行开关；home/settings
//      恒启用不
//      展示开关。开关写 config.json + 经 app::toggle 桥发意图（handler 由
//      main.cpp 装配段注入，未注入时桥内 no-op 仅写盘）；
//      对应模块关闭时其专属卡片整卡隐藏（代理卡/目录卡，不显示故障态）
//   7. 页面结构（Task B tab 化）：通用 / 模块 / 日志 三 tab
//      （components::tabs）；「日志」tab 含日志查看卡（core::paths::logFile()
//      末尾 500 行 tail + 刷新按钮；审计 H2 后读盘在后台线程，compose 只读
//      快照——首次装载在 onStart，此后切 tab / 点刷新都经 refreshLogTailCache
//      发意图，结果下一帧收割）
//      关于信息已拆到独立模块 module_about（2026-09-28 about 页设计 D-02：
//      本模块不再承担关于 tab）
//   8b. 渲染路径不做文件系统查询（审计 THS-01，与 H2 同款「读一次 → 存成员
//      → compose 只读」）：接管状态（sidecar stat）/ 笔记目录存在性 /
//      桌宠皮肤清单（目录遍历）统一收在 refreshExternalState，触发点 =
//      onStart 首次装载 + 进页面 / 切模块 tab / 打开目录后各一次，compose
//      只读快照（不再 stat / getcwd / 扫目录）
//
// 文件结构（R6 拆分，spec 2026-09-27-refactor-incremental-design §5.2）：
//   settings_module.cpp    本文件 —— Module 接口实现（pageId/displayName/
//                          compose 骨架）、生命周期、设置变更动作、日志 tail
//                          与外部状态快照、**卡片注册表**（kCardSpecs）
//   settings_ui_cards.*    8 张卡的绘制（自由函数 + 只读入参/回调显式入参）
//   settings_ui_logic.*    不碰 Ui / 不碰 DB 的纯件：档位表 / 卡片高度 /
//                          openUp 几何常量 / nearestIndex / fs / 下拉持有者语义
// 卡片绘制按 kCardSpecs 表遍历（原 compose 内 switch(settingsTab_) 的 9 个
// 分支）；卡片高度常量与 openUp 几何同源于 settings_ui_logic.h。
//
// 入口说明：设置入口在 main.cpp 的常驻侧边栏（app.sidebar，spec §2/§6.5.2），
// 与其他四个模块同列。
//
// 依赖说明：module_* 之间不互相依赖（防循环）。claude 侧（R0 / 审计 D6 起）
// **零 include、零 CMake 边**：本页要用的代理运行面（是否在跑 / 实际端口 /
// 接管状态 / 上层覆盖警告 / 改端口时 stop→start）经 core 只读服务定位器取
// ——core/claude_proxy_service.h 的窄接口，由装配段 main.cpp 注册的适配器
// 转发到它持有的 ClaudeService 单例；本页不再 include 兄弟模块的头、不再直呼
// ClaudeService::global()（该单例的另一消费方 claude_module 属同模块内读取，
// 不在 R0 范围）。pet 侧（审计 [5][36]）
// 同样不 include、不 link：桌宠卡全部调用（discoverSkins/applyPetSkin/
// resetPetPosition/currentSkinName/showPetWindow）走中立桥头
// module_pet_bridge.h 的自由函数，实现在 pet 模块内，**调用点集中在本文
// 件**（卡片单元只经回调触达，不知道 pet 模块存在）。两条边均单向
//（claude/pet 不反向依赖 settings），无环；桌宠非 Router 页面模块，
// 设计 2026-09-22 §4.7 接线表明确设置页承载桌宠卡。

// app_tools.h：本模块的智能体工具集（agent M2 Task 9，app.read_settings）。
// 工具自持 SettingsStore 纯读、不摸模块成员——注册进 core::ToolRegistry 供
// module_agent 按名调用（工具归各自模块，见 M2 计划 Global Constraints）。
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

// ---- 审计 [5][36] 解耦桥：module_pet 自由函数声明 ----
// 声明已收口到中立桥头 module_pet_bridge.h（src 根、应用层，审计 M1 短期
// 项）：settings 仍不 include module_pet 任何头、CMake 不 link
// devdesk_module_pet——符号在最终可执行文件（DevDesk / devdesk_tests 均直接
// 链接全部 module_*）链接期解析。只读查询 / 改对方状态两组边界与签名漂移
// static_assert 见该头。
#include "module_pet_bridge.h"

// ---- 模块开关即时生效桥：app::toggle 自由函数声明 ----
// 与 module_pet_bridge.h 同模式的中立桥头（src 根、应用层）：settings 不
// include 任何 app/兄弟模块头；实现（函数指针缝 + 默认 no-op）落
// module_toggle_bridge.cpp（与头成对的 src 根 TU，probe 链接面可解析），真
// handler 由 main.cpp 装配段注入。签名漂移 static_assert 见该头。
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

// ---- 卡片注册表（R6，spec §5.2）----
// 每行 = 一张卡：tab 归属 / 门控 / 卡高种类 / 是否吃页面几何 / 绘制函数。
// 顺序 = 绘制顺序 = 兄弟绘制序（含下拉的卡整卡 z30，见各卡实现）。
// 原来 compose 内 switch(settingsTab_) 的 9 个分支收成这一张表——加卡只改表。
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

    // ============ 生命周期 ============

    void onStart() override {
        store_ = std::make_unique<SettingsStore>(::core::paths::configFile());
        cfg_ = store_->load();
        // app.read_settings（agent M2 Task 9）：store 就绪后把只读设置工具注册
        // 进全局注册表（模块隔离：agent 只经 core::ToolRegistry 按名取用，不
        // include 本模块）。工具自持 SettingsStore(path) 纯读、不引用本对象，
        // agent 工作线程并发调用安全。name 冲突属装配 bug（无错误回传通道），
        // 日志留痕不中断——照 agent_module.cpp:143 statsTool_ 注册同款
        appReadSettingsTool_ =
            makeAppReadSettingsTool(::core::paths::configFile());
        auto toolReg =
            ::core::ToolRegistry::instance().registerTool(appReadSettingsTool_);
        if (toolReg.isFail()) {
            LOG_ERROR("Settings: app.read_settings 注册失败: " +
                      toolReg.errorMessage());
        }
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
                 ", 日志保留=" + retentionLabel(cfg_.logRetentionDays) + "）");
        // 审计 H2：日志 tail 首次装载同步做一次（onStart = 启动期，非渲染路径）。
        // compose 恒读快照，首帧必须有值——异步首刷会让日志查看卡先闪一帧
        // 「暂无日志内容」占位（硬约束：不得空帧/闪空）。此后每次重读（切 tab /
        // 点刷新）都走 refreshLogTailCache 的后台线程
        applyLogTail(readLogTailLines());
        // 审计 THS-01：外部状态（接管 / 笔记目录存在性 / 皮肤清单）同点首次
        // 装载——compose 只读快照，首帧即有值（不闪空）
        refreshExternalState();
    }

    void onStop() override {
        // 工具先注销再释放持有物（M2 teardown 契约：模块收尾最后一步注销工具，
        // 在跑 run 不会悬垂——registry find() 返回 shared_ptr keep-alive）。
        // app.read_settings 无在跑资源，但顺序与 agent_module.cpp:172 对齐
        ::core::ToolRegistry::instance().unregisterTool("app.read_settings");
        appReadSettingsTool_.reset();
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
        // 审计 THS-01：进页面 = 外部状态（接管 / 笔记目录 / 皮肤清单）的刷新
        // 点；变化频率低，不需要每帧重查（原实现每帧 stat）
        refreshExternalState();
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
        // 审计 THS-05：Esc 关当前打开的下拉（下拉独占打开 → 关它 = 关「最上层」）。
        // 全关时不消费（本页其余 Esc 语义属于焦点元素，走 input.onCancel 通道）
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
    // 解析 draft → 写 config.json → 同步运行面端口镜像
    //（service.setConfiguredProxyPort；Claude 页手动启动 / main 自启读它）
    // → 运行中自动 stop→start 重启。
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

        auto* svc = claudeProxy();
        if (svc) {
            svc->setConfiguredProxyPort(port);
        }
        if (!svc || !svc->isProxyRunning()) {
            // 生效时机有**两个**（审计 ARCH-09，勿改回单一「下次启动」口径）：
            //   ① 启动期装配注入 —— main.cpp assembleClaudeServiceNow（读本页
            //      落盘的 config.json proxyPort 喂 service.configuredProxyPort）；
            //   ② 会话内开启 claude 模块时的惰性装配 —— 同函数经桥
            //      app::toggle::ensureClaudeServiceAssembled 再读一次现值。
            // 下一个生效点出现时，这里与 main.cpp 那处必须同步更新。
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
        // 审计 THS-01：stop→start 会改写/删除 sidecar（= 接管状态变化）——
        // 本页动作引起的状态变化就地刷新快照，卡片不会停在旧值（成败都刷：
        // 重启失败时代理已停、接管已还原）
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

    // 端口输入变更（原代理卡 onCh 内联体）：写草稿 + 校验错误随下一次输入清除
    void setProxyPortDraft(const std::string& v) {
        proxyPortDraft_ = v;
        // 审计 [19]：校验错误（红字状态行）随下一次输入清除，对齐 claude
        // formError_ 的「就地红字 + 输入即清」语义；成功提示（非红字）不打断，
        // 保留到下一次操作
        if (statusIsError_) {
            statusMessage_.clear();
            statusIsError_ = false;
        }
    }

    // 桌宠换肤（原桌宠卡 onChange 内联体）：下标越界守卫（含空清单）→ 桥
    // 调用。守卫读的就是卡片渲染用的同一份皮肤清单快照。不补状态行 toast
    //（2026-09-30 用户点名删除：换肤热切换即时生效、界面肉眼立即可见，
    // toast 冗余）
    void changePetSkinAt(int idx) {
        if (idx < 0 || idx >= static_cast<int>(petSkins_.size())) {
            return;
        }
        petSkinName_ = petSkins_[static_cast<size_t>(idx)];
        ::module::pet::applyPetSkin(petSkinName_);
    }

    // 显示大小切档（2026-09-30 用户需求「桌宠显示大小」）：下标守卫读的
    // 就是下拉渲染用的同一份文案快照（桥取自 pet 侧，越界静默拒绝在 pet 侧
    // 再兜一道）。选择即生效——桌宠窗关旧开新、尺寸同步缩放，肉眼立即可见，
    // 不补状态行 toast（与切皮肤同一冗余口径）
    void changePetScaleAt(int idx) {
        if (idx < 0 || idx >= static_cast<int>(petScaleLabels_.size())) {
            return;
        }
        ::module::pet::applyPetScaleIndex(idx);
        petScaleIdx_ = idx;
        LOG_INFO("Settings: 桌宠显示大小 → " +
                 petScaleLabels_[static_cast<size_t>(idx)]);
    }

    // 位置重置（原桌宠卡 onClick 内联体）：回主屏右下角，持久化坐标清回未设置
    void resetPetPosition() {
        ::module::pet::resetPetPosition();
        setStatus("桌宠位置已重置（主屏右下角）");
    }

    // 显示桌宠（PAB-13，Bug B 2026-09-30）。桥已收成四态枚举：false 三义
    // 折一个「未启用，重启后生效」既误报「已开着」，又只有页头副标题小字
    // 这一可见通道（用户视线在卡片上）——真机反馈「点了没效果」。现在按
    // 原因给 toast：未装配 = 中性提示非错误（开关已写盘，重启生效）；
    // 启动失败 = 错误样式；已开着 / 成功各给一句如实反馈
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

    // 桌宠开关的即时可见性（PET-CLOSE，2026-09-29 真机反馈「设置-模块-桌宠
    // 取消勾选，桌宠没消失」）：桌宠是七个模块里**唯一**有独立可见载体
    //（非 Router 页面的无边框子窗）的模块——注册门控确实要等重启，但窗口
    // 本身不必：关 → 桥调用立即关窗（与右键「隐藏桌宠」同一条路径）；
    // 开 → 桥调用立即原位重开（皮肤/位置/气泡桥沿用上次装配）。
    // 反向口径：本会话 main 装配过桌宠（启动时开关是开的）才能即时重现；
    // 启动时桌宠就是关的会话里模块从未装配，桥返回 NotAssembled → 如实提示重启生效
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
        // 已开着（AlreadyOpen，如关→开快速往返窗口尚未销毁）与重开成功对
        // 用户是同一结果：窗口此刻就在屏幕上，本体即反馈，状态行收底即可
        //（与换肤/切大小「即时肉眼可见不补 toast」同一口径）
        using Outcome = ::module::pet::PetShowOutcome;
        switch (::module::pet::showPetWindow()) {
            case Outcome::Shown:
            case Outcome::AlreadyOpen:
                setStatus("桌宠已开启并立即显示");
                LOG_INFO("Settings: 桌宠模块开启 → 窗口已重新入队开窗");
                break;
            case Outcome::NotAssembled:
                // Bug B 2026-09-30：本会话未装配 = 窗口无从谈起，toast 给可见
                // 反馈（改前只有页头副标题小字，用户日志 00:32:32 后感知「点了
                // 没效果」）；中性提示非错误——开关已写盘，重启后生效
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

    // 模块开关（插拔方案 A §5.4 / 活门控 2026-10-01）：本函数只写 config.json
    // + 经 app::toggle 桥发「开关意图」，即时生效经桥路由（真 handler 由
    // main.cpp 装配段注入）。home/settings 恒启用，UI 不提供开关。
    // 桌宠例外（PET-CLOSE）：写盘 + 桥之外额外做一次**即时窗口同步**（见
    // applyPetModuleVisibility）——桌宠有自己的可见载体，直接走老路。
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

    // ---- Toast 反馈（Bug B 2026-09-30 起接入）----
    // settings 是四个页面模块里最后一个接 toast 的（R3 备注原话「无 toast /
    // 无对话框」）：页头副标题小字（setStatus）对「点卡片按钮」这类动作
    // 反馈太弱（真机反馈「点了没效果」），桥失败分支改走 toast。状态位与
    // 绘制接线走 ui_kit（D1 去重），形态与 claude 同款：3.5s + 成功
    // fa-circle-check / 失败 fa-triangle-exclamation，节点 id = settings.toast
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

    // ============ UI ============

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        const auto& t = core::theme::current();
        const float pad = t.sizes.spacingXXLarge;  // 40
        const float contentW = screen.width - pad * 2.0f;

        // 页面底色 = 主窗口 clearColor（随主题/窗口效果档运行时跟随，
        // 磨砂 Phase B/D）——不自铺不透明底，半透/磨砂档主体才生效

        const float headerH = ui_kit::kHeaderH;
        const float tabH = kSettingsTabHeight;
        // 审计 H2：日志 tail 读盘已移出渲染路径（refreshLogTailCache 只发意图 /
        // 搬已就绪结果，读盘在后台线程；快照 logTail_ 由 onStart 首次装载）。
        // 渲染路径恒读快照 → 切 tab / 首帧不会闪空
        if (settingsTab_ == kTabLog) {
            refreshLogTailCache();
        }
        // contentKey 带 proxyH：警告行数变化时滚动高度缓存失效重测
        auto* svc = claudeProxy();
        // 审计 THS-01：接管状态读快照（原 isClaudeCodeManaged → fs::exists
        // 每帧 stat sidecar）。冷启动兜底：快照从未装载过时本帧同步刷新一次
        //（与 H2 的 logTail 同款，只发生一次）——首帧必须有值，不闪空
        if (!externalStateLoaded_) {
            refreshExternalState();
        }
        // 审计 CPX-02 残：本处不再预读 managed（卡高只由警告行数决定；接管状态
        // 行的文本/色在 drawProxyCard 内自读快照）
        size_t warnLines = 0;
        if (svc) {
            // 审计 CPX-02：警告行不再以「已接管」为展示门——takeOver 在写
            // sidecar 之前失败（解析失败/写失败）时 sidecar 不存在 ⇒ 未接管，
            // 但失败原因正是最该看见的一条。与详情页（claude_module.cpp 同款
            // 收敛）同口径
            warnLines = std::min<size_t>(
                svc->lastSyncWarnings().size(), 2);
        }
        // 代理卡高（2026-09-30 起随生效字号派生）：proxyCardHeightFor 是
        // 接管/警告行几何的单一来源（drawProxyCard 同源），100% 档公式与旧字面
        // 逐一相等（无警告 116+16=132 / n 条警告 118+16n+16）
        const float proxyH = proxyCardHeightFor(warnLines);
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
                // 2026-09-28 用户真机反馈（原话「设置页面最外层的卡片还没删除
                // 啊，让你删除好多次了」）：删 M3-4 加的主体渐变底板
                // （settings.body.gradient > settings.body.bg，surface →
                // surfaceElevated 垂直）。9 卡卡框早先已删，此底板就是用户眼中
                // 剩下的「最外层卡片」。删除后回到 frost spec 口径
                // （docs/superpowers/specs/2026-09-22-eui-window-frost-design.md:53
                // 「各模块页面要么靠 clearColor 当底、要么自铺不透明 surface
                // 色」）：关档靠主窗 clearColor 当底，半透/磨砂档本就该露出窗口
                // 背景——回归断言见 tests/test_ui_modern.cpp
                // uiModernSettingsBodyGradientRemoved（反断言：加回即红）
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
                                // 切到模块 tab：刷新外部状态快照（审计 THS-01；
                                // 代理 / 目录 / 桌宠三卡都在这 tab，进 tab 即取新值）
                                if (idx == kTabModules) refreshExternalState();
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
    // ============ 跨模块服务取用（R0 / 审计 D6） ============
    // 本页对 Claude 运行时的全部引用都经 core 只读服务定位器：不再 include
    // 兄弟模块的头、不再直呼 ClaudeService::global()（装配段 main.cpp 注册的
    // 适配器负责转发到它持有的单例）。未注册 = claude 模块关闭 / 服务未装配
    //（与解耦前 `ClaudeService::global() == nullptr` 同义）→ 返回 nullptr，
    // 各调用点按「服务不可用」降级，结果与解耦前逐字一致（判空口径只此一处）
    static ::core::IClaudeProxyService* claudeProxy() {
        auto r =
            ::core::ServiceLocator::requireService<::core::IClaudeProxyService>();
        return r.isOk() ? r.value() : nullptr;
    }

    // ============ 下拉「谁打开着」的单一持有者（审计 THS-05） ============
    // 7 个下拉共用一个索引（取值语义见 settings_ui_logic.h 的
    // isDropdownOpen / applyDropdownOpen）；卡片侧只读入参 + 回写回调
    void setDropdownOpen(Dropdown d, bool v) {
        openDropdown_ = applyDropdownOpen(openDropdown_, d, v);
    }

    // 卡片回调装配（compose 每帧）。lambda 只捕获 this（SBO 无堆分配）；
    // 卡片把它们**按值**拷进元素回调 → 元素树比一帧活得久也不悬垂
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
        // 审计 [21]：Esc=丢弃端口草稿（不清红字状态——与输入变更的语义分开）
        a.discardProxyPortDraft = [this] { proxyPortDraft_.clear(); };
        a.applyRetentionPreset = [this](int idx) { applyRetentionPreset(idx); };
        a.applyLogLevelPreset = [this](int idx) { applyLogLevelPreset(idx); };
        a.applyEditorFontPreset = [this](int idx) { applyEditorFontPreset(idx); };
        a.requestLogTailReload = [this] { logTailDirty_ = true; };
        a.logViewScrolled = [this](float v) { logViewScrollOffset_ = v; };
        return a;
    }

    // 注册表卡高解析：Fixed = 表内常量；ProxyWarnings = 代理卡（警告行数推导，
    // 公式见 compose 顶部）；FillRemainder = 日志查看卡吃掉剩余视口高（内容
    // 恰好一屏，避免内外双层滚动条并存；极小窗口兜底 260 由外层 scrollView
    // 负责滚动）
    static float cardHeight(const CardSpec& spec, float proxyH, float remain) {
        switch (spec.heightKind) {
        case CardHeightKind::Fixed:
            // 2026-09-30 缩放派生审查（spec §2.3 本期修）：spec.height 是
            // 100% 档基准，运行期随生效字号放大（roundFs）——卡内行盒/行距
            // 派生后内容底沿增长，卡高不同步放大就会把尾行挤出卡外（与下一
            // 张卡的内容叠印）。100% 档 roundFs(x)==x，与旧字面逐值相等
            return roundFs(spec.height);
        case CardHeightKind::ProxyWarnings:
            return proxyH;
        case CardHeightKind::FillRemainder:
            return std::max(260.0f, remain);
        }
        return spec.height;  // 兜底：枚举穷举，仅为满足「所有路径有返回」
    }

    // 卡区（R6）：遍历注册表画本 tab 的卡（原 switch(settingsTab_) 的 9 个
    // 分支）。卡片纵向游标（scrollView 内容空间，顶 = 0）只累计**本 tab
    // 参与绘制**的卡（tab 过滤后卡序列变了，全量累加会让下拉方向算错）；
    // 带下拉的卡算 openUp 需页面绝对 Y：字段页内位置
    // = cardsTopY + 内容Y - 滚动偏移
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

    // ---------- Markdown 笔记目录：打开目录（卡片 4 的动作） ----------
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
        // 审计 THS-01：「打开目录」是唯一创建笔记目录的动作——刷新存在性
        // 快照，目录卡本帧起不再显示「尚未创建」（不必等下次进页面）
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

    // ---------- 日志 tail 读取（Task B，日志 tab；审计 H2 迁出渲染路径） ----------
    // readLogTailLines 是纯读盘（static、不碰成员 → 可跑在后台线程）：只读文件
    // 末尾窗口（kLogTailReadWindow）防大日志整档进内存；保留末尾
    // kLogTailMaxLines 行（滑动裁剪，避免逐行 erase 的 O(n²)）
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

    // 审计 H2：compose 只读快照 + 只发意图。本函数每帧只做两件无 IO 的事：
    // ①上一轮读已就绪 → 搬进快照（wait_for(0) 不阻塞，未就绪直接跳过，快照
    //   沿用上次值，不丢不改）；②脏标记置位且无在途读 → 发一次读意图
    //   （std::async 后台线程跑 readLogTailLines，结果经 future 值回渲染线程）。
    // 等价性：读盘失败/文件缺失与旧实现同样落成空快照；可见时机最多晚一个
    // tick（结果下一帧收割），交互语义不变（切 tab / 点刷新仍是「置脏 → 重读」）
    void refreshLogTailCache() {
        if (logTailFuture_.valid() &&
            logTailFuture_.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready) {
            auto lines = logTailFuture_.get();   // shared_future::get 不失效
            logTailFuture_ = {};                 // 显式清空 = 让出「在途」判据
            applyLogTail(std::move(lines));
        }
        if (!logTailLoaded_) {
            // 冷启动兜底：快照从未装载过（调用方未走 onStart）时本帧同步读一次。
            // 首帧必须有值（硬约束：不得空帧）——异步首刷会先渲染一帧「暂无日志
            // 内容」占位文案；这一帧的同步读是刻意的、只发生一次，装载即转后台
            applyLogTail(readLogTailLines());
            logTailDirty_ = false;
            return;
        }
        if (!logTailDirty_ || logTailFuture_.valid()) {
            return;
        }
        logTailDirty_ = false;
        // 批次 D 评审收尾（2026-10-02）：**先就绪、后唤醒**（与 claude_ui_logs.cpp /
        // claude_ui_state.cpp 同一形态）。std::async 在 callable 返回之后才发布值，
        // 任务内先唤醒会让 UI 抢跑（compose 收割看到「未就绪」→ 唤醒位已被消费 →
        // 结果没人上屏，本卡停在旧快照）。原先完全没唤醒：日志 tail 要等用户下一次
        // 动鼠标才被 compose 收割。
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

    // 审计 THS-01：外部状态只读快照的刷新点（H2 同款：读一次 → 存成员 →
    // compose 只读）。原先直接落在渲染路径上的三处文件系统查询：
    //   ① isClaudeCodeManaged → hasPendingSnapshot → fs::exists(sidecar)
    //      （compose 顶部 + 代理卡，每帧 stat）
    //   ② fs::exists(markdownDir())（目录卡，每帧 stat）
    //   ③ discoverSkins（目录遍历 + 每皮肤 pet.json stat）+ currentSkinName
    //      （读 pet_config.json）（桌宠卡，改动前为首帧一次性）
    // 调用点全在非渲染路径：onStart / onPageEnter / 切模块 tab / 打开目录后，
    // 另有 compose 顶部一次性冷启动兜底（见 compose）——四处覆盖了「进页面 /
    // 相关操作后」的全部可见变化时机；单次成本几十微秒，远低于原先每帧一次
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

    // ---------- 状态 ----------
    std::unique_ptr<SettingsStore> store_;
    SettingsConfig cfg_;
    // app.read_settings 工具（agent M2 Task 9）：onStart 注册 / onStop 注销。
    // 持 shared_ptr 与注册表同寿（registerTool 失败也留成员，注销对未注册名
    // 是 no-op——见 core/tool.cpp unregisterTool 语义）
    std::shared_ptr<core::ITool> appReadSettingsTool_;
    // 当前打开的下拉（审计 THS-05：7 个 bool 收成一个索引，-1 = 全关）。
    // 取值 = static_cast<int>(Dropdown)，判读/开关走 settings_ui_logic.h 的
    // isDropdownOpen / applyDropdownOpen
    int openDropdown_ = -1;
    std::vector<std::string> petSkins_;    // 发现到的皮肤（M2，快照见 THS-01）
    std::string petSkinName_;              // 当前皮肤名（M2，快照见 THS-01）
    std::vector<std::string> petScaleLabels_;  // 显示大小三档文案（桥快照）
    int petScaleIdx_ = 1;                  // 当前显示大小档下标（快照见 THS-01）
    // 代理端口输入草稿（审计 [14]）：空 = 未编辑，compose 显示 cfg_.proxyPort；
    // applyProxyPort 成功后由 persist 侧刷新（下次进页恢复显示配置值）
    std::string proxyPortDraft_;
    float cardsScrollOffset_ = 0.0f;      // 卡片区 scrollView 滚动位置
    // ---- Task B：tab 化 + 日志查看 ----
    int settingsTab_ = 0;                 // 当前 tab（通用/模块/日志）
    std::vector<std::string> logTail_;    // 日志文件末尾行内存快照（compose 只渲染）
    unsigned logTailVersion_ = 0;         // tail 读取版本号（进日志查看卡 contentKey）
    bool logTailDirty_ = true;            // 待重读标记（切日志 tab / 点刷新置位）
    bool logTailLoaded_ = false;          // 快照是否装载过（冷启动兜底，见 refreshLogTailCache）
    // 审计 H2：后台读盘结果槽 + worker 生命周期（批次 D 评审收尾：结果槽改
    // shared_future，worker set_value 发布 → 再 requestUiUpdate 唤醒；
    // std::async 的 future 只作 join 句柄，不留它 = 同步派发）。在途判据 =
    // logTailFuture_.valid()（派发即有效、收割即清空，语义同旧版）。
    std::shared_future<std::vector<std::string>> logTailFuture_;
    std::future<void> logTailWorker_;
    // ---- 审计 THS-01：外部状态只读快照（compose 不做 fs 查询）----
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
