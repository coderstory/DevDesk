// src/module_settings/settings_ui_cards.cpp
// settings 模块 8 张卡：每个可见区域一个自由函数，吃 CardDrawArgs。元素 event 回调一律按值捕获 std::function——元素树比一帧活得久，捕引用会悬垂。
// 卡内容直接铺页面底：关档靠主窗 clearColor 当底，半透/磨砂档露出窗口背景。
#include "settings_ui_cards.h"

#include <core/component_theme.h>
#include <core/paths.h>

#include <platform/file_open.h>

#include <eui/app.h>
#include <eui/dsl.h>

#include <components/button.h>
#include <components/dropdown.h>
#include <components/input.h>
#include <components/scrollview.h>
#include <components/switch.h>

#include <ui_kit/button.h>
#include <ui_kit/form_row.h>
#include <ui_kit/wswitch.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace module::settings {

namespace {

// themeModeIndex 已随主题选择器删除（D-12/E3）移除——WeChat 皮肤即主题。

int windowEffectIndex(const std::string& mode) {
    if (mode == kWindowEffectTranslucent) return 1;
    if (mode == kWindowEffectFrosted) return 2;
    return 0;
}

int logLevelIndex(const std::string& level) {
    if (level == kLogLevelDebug) return 0;
    if (level == kLogLevelWarn) return 2;
    if (level == kLogLevelError) return 3;
    return 1;  // INFO 默认档
}

// 卡内标题 / 描述行 helper：左缩进 28、字体与颜色在所有调用点一致；标题字重 600。

void cardTitle(eui::Ui& ui, const std::string& id, const core::theme::Tokens& t,
               float y, float w, float h, float lineHeight,
               const std::string& text) {
    ui.text(id + ".title")
        .position(28.0f, y)
        .size(w, h)
        .text(text)
        .fontSize(t.sizes.fontSizeLarge)
        .lineHeight(lineHeight)
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();
}

// 卡描述行：与标题同左缘(28)、同行高口径（lineHeight 恒 = 盒高 h）。
void cardDesc(eui::Ui& ui, const std::string& id, const core::theme::Tokens& t,
              float y, float w, float h, const std::string& text) {
    ui.text(id + ".desc")
        .position(28.0f, y)
        .size(w, h)
        .text(text)
        .fontSize(fs(12.0f))
        .lineHeight(h)
        .color(t.colors.textTertiary)
        .build();
}

} // namespace

// zIndex(40)：下拉弹层需整卡抬层才能盖过下方卡片（zIndex 只在兄弟间排序），同一 tab 带下拉的卡靠前者必须 z 更高。
void drawAppearanceCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const float h = args.h;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;

    const auto compTheme = core::theme::componentTokensWeChat(t);
    // 行几何随生效字号派生（roundFs 口径见 settings_ui_logic.h）；18 = 标题顶字面锚。
    // 主题选择器删除后字号/效果改 FormRow 排列，行高与日志卡同口径（logFieldHeight=36）。
    const float titleH = roundFs(24.0f);
    const float descY = 18.0f + titleH + roundFs(2.0f);
    const float descH = roundFs(18.0f);
    const float hintH = roundFs(16.0f);
    ui.stack(id)
        .size(w, h)
        .zIndex(40)
        .content([&] {
            cardTitle(ui, id, t, 18.0f, w - 56.0f, titleH, titleH, "外观");
            cardDesc(ui, id, t, descY, w - 56.0f, descH,
                     "主题与窗口底色即时生效；字号缩放全局生效");

            // 主题选择器已删除（D-12/E3 去功能例外——WeChat 皮肤即主题、暗色延后 D-04）。
            // 字号缩放（§6 item 3 确认在通用 tab）+ 窗口效果用 FormRow 排列（§6 item 5）。
            const float rowH = logFieldHeight();  // 36px（与日志卡同口径）
            const float fontRowY = descY + descH + roundFs(6.0f);
            const float effectRowY = fontRowY + rowH + roundFs(6.0f);
            const float formX = 28.0f;
            const float formW = w - 56.0f;  // 卡内左右各 28 padding

            // 字号缩放 FormRow：label 110px + dropdown control
            ui_kit::formRow(ui, id + ".font", "字号缩放", formX, fontRowY,
                            formW, rowH,
                [&](eui::Ui& ui, float cx, float cy, float cw, float ch) {
                    const float ddW = std::min(240.0f, cw);
                    ui.stack(id + ".font.ddwrap")
                        .position(cx, cy)
                        .size(ddW, ch)
                        .content([&] {
                            components::dropdown(ui, id + ".font.dd")
                                .size(ddW, ch)
                                .theme(compTheme)
                                .items(std::vector<std::string>(
                                    kFontPresetLabels,
                                    kFontPresetLabels + kFontPresetCount))
                                .selected(nearestIndex(in.cfg.fontScalePercent,
                                                       kFontPresets, kFontPresetCount))
                                .open(isDropdownOpen(in.openDropdown, Dropdown::Font))
                                .onOpenChange([f = a.setDropdownOpen](bool v) {
                                    f(Dropdown::Font, v);
                                })
                                .onChange([f = a.applyFontPreset](int idx) { f(idx); })
                                .build();
                        })
                        .build();
                }, t);

            // 窗口效果 FormRow
            ui_kit::formRow(ui, id + ".effect", "窗口效果", formX, effectRowY,
                            formW, rowH,
                [&](eui::Ui& ui, float cx, float cy, float cw, float ch) {
                    const float ddW = std::min(240.0f, cw);
                    ui.stack(id + ".effect.ddwrap")
                        .position(cx, cy)
                        .size(ddW, ch)
                        .content([&] {
                            components::dropdown(ui, id + ".effect.dd")
                                .size(ddW, ch)
                                .theme(compTheme)
                                .items(std::vector<std::string>(
                                    kWindowEffectPresetLabels,
                                    kWindowEffectPresetLabels +
                                        kWindowEffectPresetCount))
                                .selected(windowEffectIndex(in.cfg.windowEffect))
                                .open(isDropdownOpen(in.openDropdown, Dropdown::Effect))
                                .onOpenChange([f = a.setDropdownOpen](bool v) {
                                    f(Dropdown::Effect, v);
                                })
                                .onChange([f = a.applyWindowEffect](int idx) { f(idx); })
                                .build();
                        })
                        .build();
                }, t);

            // 降级提示 y（条件渲染——仅窗口效果不支持时显示）
            const float hintY = effectRowY + rowH + roundFs(6.0f);
            const std::string degraded = windowEffectDegradedHint(
                in.cfg.windowEffect, app::activeWindowEffect());
            if (!degraded.empty()) {
                const float hintW = w - formX - 16.0f - 28.0f;
                // 图标 / 文字拆两个元素（FA 图标字体通道）
                ui.text(id + ".effect.hint.icon")
                    .position(formX, hintY)
                    .size(14.0f, hintH)
                    .icon(0xF071)  // fa-triangle-exclamation
                    .fontSize(fs(11.0f))
                    .lineHeight(hintH)
                    .color(t.colors.warning)
                    .build();
                ui.text(id + ".effect.hint")
                    .position(formX + 16.0f, hintY)
                    .size(hintW, hintH)
                    .text(degraded)
                    .fontSize(fs(11.0f))
                    .lineHeight(hintH)
                    .color(t.colors.warning)
                    // 单行省略号需 overflow==Ellipsis && maxWidth>0 && !wrap 三件套；clip 作几何兜底（ellipsis 管排版宽、clip 管绘制范围）。
                    .maxWidth(hintW)
                    .overflow(core::TextOverflow::Ellipsis)
                    .clip()
                    .build();
            }
        })
        .build();
}

// 档位写 core::theme::editorFontSize 通道；pageY/screenH 用于下拉弹层 openUp 几何估算。
void drawEditorFontCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const float h = args.h;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;
    const float pageY = args.pageY;
    const float screenH = args.screenH;

    const auto compTheme = core::theme::componentTokensWeChat(t);
    ui.stack(id)
        .size(w, h)
        .zIndex(30)
        .content([&] {
            cardTitle(ui, id, t, 16.0f, w - 56.0f, 22.0f, 22.0f, "编辑器字号");
            // 下拉 y/高随生效字号派生（单一来源 editorFontDdOffsetY / logFieldHeight）。
            ui.stack(id + ".ddwrap")
                .position(28.0f, editorFontDdOffsetY())
                .size(150.0f, logFieldHeight())
                .content([&] {
                    const bool efontOpenUp = dropdownPopupOverflows(
                        pageY, editorFontDdOffsetY(), logFieldHeight(),
                        kEditorFontPresetCount, screenH);
                    components::dropdown(ui, id + ".dd")
                        .size(150.0f, logFieldHeight())
                        .theme(compTheme)
                        .items(std::vector<std::string>(
                            kEditorFontPresetLabels,
                            kEditorFontPresetLabels + kEditorFontPresetCount))
                        .selected(nearestIndex(in.cfg.editorFontSize,
                                               kEditorFontPresets,
                                               kEditorFontPresetCount))
                        .openUp(efontOpenUp)
                        .open(isDropdownOpen(in.openDropdown,
                                             Dropdown::EditorFont))
                        .onOpenChange([f = a.setDropdownOpen](bool v) {
                            f(Dropdown::EditorFont, v);
                        })
                        .onChange([f = a.applyEditorFontPreset](int idx) {
                            f(idx);
                        })
                        .build();
                })
                .build();
            ui.text(id + ".hint")
                .position(190.0f, 48.0f)
                .size(w - 218.0f, roundFs(18.0f))
                .text("Markdown 编辑器正文字号；默认 15px（标准）。切换即时生效。")
                .fontSize(fs(12.0f))
                .lineHeight(roundFs(18.0f))
                .color(t.colors.textTertiary)
                .build();
        })
        .build();
}

// 各模块一行开关；关闭 = 下次启动不注册进 Router。home/settings 恒启用，不提供开关。
void drawModulesCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const float h = args.h;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;

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
        {kModuleIdAgent, "智能体",
         "任务列表 + 手动触发运行（依赖 Claude 代理提供模型通道）"},
    };
    constexpr int kToggleCount = static_cast<int>(sizeof(kToggleable) / sizeof(kToggleable[0]));
    ui.stack(id)
        .size(w, h)
        .content([&] {
            cardTitle(ui, id, t, 16.0f, w - 56.0f, 22.0f, 22.0f, "模块");
            cardDesc(ui, id, t, 40.0f, w - 56.0f, roundFs(16.0f),
                     "开关即时生效：关闭后该模块代码不再执行，侧边栏入口与"
                     "页面当帧更新");

            // 开关行行几何随生效字号派生；开关控件（32 高字面组件）随行顶走，相对偏移 -2。
            const float nameH = roundFs(18.0f);
            const float rowDescDy = nameH + roundFs(2.0f);
            const float rowDescH = roundFs(16.0f);
            const float rowPitch = roundFs(56.0f);
            for (int i = 0; i < kToggleCount; ++i) {
                const ModuleMeta& meta = kToggleable[i];
                const float rowY = 68.0f + static_cast<float>(i) * rowPitch;
                const std::string suffix = std::to_string(i);
                ui.text(id + ".name." + suffix)
                    .position(28.0f, rowY)
                    .size(w - 160.0f, nameH)
                    .text(meta.name)
                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                    .lineHeight(nameH)
                    .fontWeight(500)
                    .color(t.colors.textPrimary)
                    .build();
                ui.text(id + ".rowdesc." + suffix)
                    .position(28.0f, rowY + rowDescDy)
                    .size(w - 160.0f, rowDescH)
                    .text(meta.desc)
                    .fontSize(fs(12.0f))
                    .lineHeight(rowDescH)
                    .color(t.colors.textTertiary)
                    .build();
                // 开关换微信绿 wswitch（§6 item 7）：绿轨道+白滑块（§4.4）。
                // wswitch 的 clickable 元素 id = <id>.track（probe 按 .track 定位）。
                ui_kit::wswitch(ui, id + ".sw." + suffix,
                    moduleEnabled(in.cfg, meta.moduleId),
                    w - 28.0f - 80.0f, rowY - 2.0f, 80.0f, 32.0f, t,
                    [f = a.applyModuleEnabled,
                     mid = meta.moduleId](bool v) {
                        f(mid, v);
                    });
            }
        })
        .build();
}

void drawDirectoryCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const float h = args.h;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;

    ui.stack(id)
        .size(w, h)
        .content([&] {
            cardTitle(ui, id, t, 16.0f, w - 56.0f, 22.0f, 22.0f,
                      "Markdown 笔记目录（只读）");

            const std::filesystem::path mdDir =
                ::core::paths::markdownDir();
            const bool exists = in.notesDirExists;
            const float dirBtnW = 110.0f;
            // 路径只读灰底（§6 item 9）：wechat.dividerSecondary 底
            ui.rect(id + ".path.bg")
                .position(28.0f, 46.0f)
                .size(w - 56.0f - dirBtnW - 12.0f, 34.0f)
                .color(t.colors.wechat.dividerSecondary)
                .radius(t.sizes.radiusWeChat)
                .build();
            ui.text(id + ".path.value")
                .position(40.0f, 46.0f)
                .size(w - 80.0f - dirBtnW - 12.0f, 34.0f)
                .text(mdDir.string() + (exists ? "" : "（尚未创建）"))
                .fontSize(fs(13.0f))
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.wechat.textSecondary)
                .build();
            // 「打开目录」浅灰次按钮（§6 item 9）：ui_kit::button Secondary 级
            ui_kit::button(ui, id + ".open", "打开目录",
                ui_kit::ButtonLevel::Secondary,
                w - 28.0f - dirBtnW, 46.0f, dirBtnW, 34.0f, t,
                {.onClick = [f = a.openNotesDirectory] { f(); }});
            // 说明小字（§6 item 9）：wechat.textTertiary
            ui.text(id + ".hint")
                .position(28.0f, 86.0f)
                .size(w - 56.0f, roundFs(14.0f))
                .text("目录暂不支持修改，打开后可手动迁移文件")
                .fontSize(fs(11.0f))
                .lineHeight(roundFs(14.0f))
                .color(t.colors.wechat.textTertiary)
                .build();

            // 数据根忽略规则路径：凭据在 devdesk.db（WAL 伴生件会复制凭据页）且数据根要进 git——本行只展示，让用户看见是哪份规则在挡 git add。
            ui.text(id + ".gitignore.label")
                .position(28.0f, 108.0f)
                .size(w - 56.0f, roundFs(14.0f))
                .text("数据根忽略规则（防凭据误提交）")
                .fontSize(fs(11.0f))
                .lineHeight(roundFs(14.0f))
                .color(t.colors.textTertiary)
                .build();
            ui.rect(id + ".gitignore.path.bg")
                .position(28.0f, 126.0f)
                .size(w - 56.0f, 34.0f)
                .color(t.colors.surfaceElevated)
                .radius(t.sizes.radiusSmall)
                .border(1.0f, t.colors.borderMuted)
                .build();
            ui.text(id + ".gitignore.path.value")
                .position(40.0f, 126.0f)
                .size(w - 80.0f, 34.0f)
                .text(::core::paths::dataRootGitignoreFile().string())
                .fontSize(fs(13.0f))
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textSecondary)
                .build();
        })
        .build();
}

void drawProxyCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const float h = args.h;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;

    // 行几何随生效字号派生（helper 见 settings_ui_logic.h）；字面锚 y40/62/98 不缩放，端口标签 lineHeight 28 = 与输入控件垂直居中。
    const float descH = roundFs(16.0f);
    const float portRowH = roundFs(18.0f);
    const float takeoverH = proxyTakeoverRowHeight();
    const float warnY0 = proxyWarnRowTop();
    const float warnH = proxyWarnRowHeight();

    ui.stack(id)
        .size(w, h)
        .content([&] {
            auto* svc = in.proxy;
            const bool running = svc && svc->isProxyRunning();
            const int port = running ? svc->proxyPort() : 0;

            cardTitle(ui, id, t, 16.0f, 240.0f, 22.0f, 22.0f, "Claude 代理");

            // 随应用启动代理（默认关）放卡片右上。
            const auto compTheme = core::theme::componentTokensWeChat(t);
            components::toggleSwitch(ui, id + ".autostart")
                .position(w - 28.0f - 240.0f, 12.0f)
                .size(240.0f, 28.0f)
                .text("随应用启动代理")
                .fontSize(fs(13.0f))
                .checked(in.cfg.claudeProxyAutoStart)
                .theme(compTheme)
                .onChange([f = a.applyProxyAutoStart](bool v) { f(v); })
                .build();

            cardDesc(ui, id, t, 40.0f, w - 56.0f, descH,
                     running
                         ? "本地代理 http://127.0.0.1:" + std::to_string(port) +
                               "（仅本机可访问，无需凭据）"
                         : "本地代理未运行（默认不随应用启动，可在 Claude 页手动启动）");

            // 代理运行中改端口自动 stop→start 重启，未运行则下次启动生效
            const std::string portText = in.proxyPortDraft.empty()
                ? std::to_string(in.cfg.proxyPort)
                : in.proxyPortDraft;
            ui.text(id + ".port.label")
                .position(28.0f, 62.0f)
                .size(72.0f, portRowH)
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
                .onChange([f = a.setProxyPortDraft](const std::string& v) {
                    f(v);
                })
                .onEnter([f = a.applyProxyPort] { f(); })
                // Esc=丢弃端口草稿（回落已应用端口，不触发 applyProxyPort）
                .onCancel([f = a.discardProxyPortDraft] { f(); })
                .build();
            components::button(ui, id + ".port.apply")
                .position(208.0f, 60.0f)
                .size(64.0f, 28.0f)
                .text("应用")
                .fontSize(fs(13.0f))
                .theme(compTheme)
                .colors(t.colors.primary, t.colors.primaryHover,
                        t.colors.primaryPressed)
                .textColor(t.colors.onPrimary)
                .radius(t.sizes.radiusSmall)
                .onClick([f = a.applyProxyPort] { f(); })
                .build();
            ui.text(id + ".port.hint")
                .position(282.0f, 62.0f)
                .size(w - 282.0f - 28.0f, portRowH)
                .text("改后回车或点「应用」；代理运行中自动重启")
                .fontSize(fs(12.0f))
                .lineHeight(portRowH)
                .color(t.colors.textTertiary)
                .build();

            // Claude Code 接管状态：sidecar 是否存在推导，让还原机制对用户可见。
            const bool managed = in.takeoverManaged;
            ui.text(id + ".takeover.label")
                .position(28.0f, 98.0f)
                .size(150.0f, takeoverH)
                .text("Claude Code 接管")
                .fontSize(fs(12.0f))
                .lineHeight(takeoverH)
                .color(t.colors.textSecondary)
                .build();
            // 路径读 Claude 运行面内存态（main 启动期注入，即接管实际使用的路径）。
            const std::string ccPath =
                svc ? svc->settingsFilePath() : std::string();
            ui.text(id + ".takeover.value")
                .position(190.0f, 98.0f)
                .size(w - 190.0f - 28.0f, takeoverH)
                .text(managed
                      ? ("● 已接管 " + ccPath)
                      : std::string("○ 未接管（启动代理后自动接管，停止时还原）"))
                .fontSize(fs(12.0f))
                .lineHeight(takeoverH)
                .color(managed ? t.colors.success : t.colors.textTertiary)
                .build();
            // 渲染门与卡高计算同源（svc 而非 managed）：未接管但警告非空时行必须画出来，否则卡高留了位。
            if (svc) {
                // 按值快照（写者可能是 agent 线程），不是活引用
                const std::vector<std::string> warns = svc->lastSyncWarnings();
                // 最多展示 2 条（卡片高度约束），更多警告在日志可查；图标 / 文字拆两个元素。
                const size_t shown =
                    std::min<size_t>(warns.size(), 2);
                for (size_t i = 0; i < shown; ++i) {
                    const float warnY =
                        warnY0 + static_cast<float>(i) * warnH;
                    ui.text(id + ".takeover.warn.icon." + std::to_string(i))
                        .position(28.0f, warnY)
                        .size(12.0f, warnH)
                        .icon(0xF071)  // fa-triangle-exclamation
                        .fontSize(fs(11.0f))
                        .lineHeight(warnH)
                        .color(t.colors.warning)
                        .build();
                    ui.text(id + ".takeover.warn." + std::to_string(i))
                        .position(42.0f, warnY)
                        .size(w - 42.0f - 28.0f, warnH)
                        .text(warns[i])
                        .fontSize(fs(11.0f))
                        .lineHeight(warnH)
                        .color(t.colors.warning)
                        .build();
                }
            }
        })
        .build();
}

// 下拉出界则 openUp 向上；两行统一「标签列/控件列/说明列」同列口径（常量见 settings_ui_logic.h）。
void drawLogCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const float h = args.h;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;
    const float pageY = args.pageY;
    const float screenH = args.screenH;

    const auto compTheme = core::theme::componentTokensWeChat(t);
    const float labelFont = fs(12.0f);
    const float hintFontSize = fs(12.0f);
    const float hintW = std::max(0.0f, w - kLogHintX - 28.0f);
    ui.stack(id)
        .size(w, h)
        .zIndex(30)
        .content([&] {
            cardTitle(ui, id, t, 16.0f, w - 56.0f, 22.0f, 22.0f, "日志");

            // 档位标签（0 = 永久）；存的是原始天数 int，清理任务按值读取
            static const char* kRetentionLabels[] = {
                "永久保留", "7 天", "30 天", "90 天", "180 天", "365 天"};
            ui.text(id + ".label")
                .position(kLogLabelX, logRow1LabelY())
                .size(kLogLabelWidth, logLabelHeight())
                .text(fitControlLabel("日志保留", kLogLabelWidth, labelFont))
                .fontSize(labelFont)
                .lineHeight(logLabelHeight())
                .color(t.colors.textSecondary)
                .build();
            ui.stack(id + ".ddwrap")
                .position(kLogFieldX, logDropdownOffsetY())
                .size(kLogFieldWidth, logFieldHeight())
                .content([&] {
                    const bool openUp = dropdownPopupOverflows(
                        pageY, logDropdownOffsetY(), logFieldHeight(),
                        kRetentionPresetCount, screenH);
                    components::dropdown(ui, id + ".dd")
                        .size(kLogFieldWidth, logFieldHeight())
                        .theme(compTheme)
                        .items(std::vector<std::string>(
                            kRetentionLabels,
                            kRetentionLabels + kRetentionPresetCount))
                        .selected(nearestIndex(in.cfg.logRetentionDays,
                                               kRetentionPresets,
                                               kRetentionPresetCount))
                        .open(isDropdownOpen(in.openDropdown,
                                             Dropdown::Retention))
                        .openUp(openUp)
                        .onOpenChange([f = a.setDropdownOpen](bool v) {
                            f(Dropdown::Retention, v);
                        })
                        .onChange([f = a.applyRetentionPreset](int idx) {
                            f(idx);
                        })
                        .build();
                })
                .build();
            ui.text(id + ".hint")
                .position(kLogHintX, logRow1LabelY())
                .size(hintW, logLabelHeight())
                .text("0 = 永久保留；30 天为默认。切换档位立即清理一次。")
                .fontSize(hintFontSize)
                .lineHeight(logLabelHeight())
                .color(t.colors.textTertiary)
                .build();

            // 行 2 日志级别：切档即时 core::log::setLevel（DEBUG 起打详录），重启由 onStart 恢复持久化档。
            ui.text(id + ".level.label")
                .position(kLogLabelX, logRow2LabelY())
                .size(kLogLabelWidth, logLabelHeight())
                .text(fitControlLabel("日志级别", kLogLabelWidth, labelFont))
                .fontSize(labelFont)
                .lineHeight(logLabelHeight())
                .color(t.colors.textSecondary)
                .build();
            ui.stack(id + ".level.ddwrap")
                .position(kLogFieldX, logLevelOffsetY())
                .size(kLogFieldWidth, logFieldHeight())
                .content([&] {
                    const bool levelOpenUp = dropdownPopupOverflows(
                        pageY, logLevelOffsetY(), logFieldHeight(),
                        kLogLevelPresetCount, screenH);
                    components::dropdown(ui, id + ".level.dd")
                        .size(kLogFieldWidth, logFieldHeight())
                        .theme(compTheme)
                        .items(std::vector<std::string>(
                            kLogLevelPresetLabels,
                            kLogLevelPresetLabels + kLogLevelPresetCount))
                        .selected(logLevelIndex(in.cfg.logLevel))
                        .openUp(levelOpenUp)
                        .open(isDropdownOpen(in.openDropdown,
                                             Dropdown::LogLevel))
                        .onOpenChange([f = a.setDropdownOpen](bool v) {
                            f(Dropdown::LogLevel, v);
                        })
                        .onChange([f = a.applyLogLevelPreset](int idx) {
                            f(idx);
                        })
                        .build();
                })
                .build();
            ui.text(id + ".level.hint")
                .position(kLogHintX, logRow2LabelY())
                .size(hintW, logLabelHeight())
                .text("DEBUG 最详尽（排障）；默认 INFO。切档即时生效。")
                .fontSize(hintFontSize)
                .lineHeight(logLabelHeight())
                .color(t.colors.textTertiary)
                .build();
        })
        .build();
}

// logTail 是内存快照（读盘在后台线程），本方法只渲染；刷新按钮只置 dirty 标记。
void drawLogViewCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const float h = args.h;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;

    const auto compTheme = core::theme::componentTokensWeChat(t);
    const float lineH = 16.0f;
    ui.stack(id)
        .size(w, h)
        .content([&] {
            const float btnW = 96.0f;
            cardTitle(ui, id, t, 16.0f, w - 56.0f - btnW - 12.0f, 22.0f, 22.0f,
                      "运行日志（末尾 500 行）");
            components::button(ui, id + ".refresh")
                .position(w - 28.0f - btnW, 14.0f)
                .size(btnW, 28.0f)
                .text("刷新")
                .icon(0xF021)  // fa-arrows-rotate
                .fontSize(fs(12.0f))
                .theme(compTheme, /*primary=*/false)
                .colors(t.colors.surfaceElevated, t.colors.hover,
                        t.colors.active)
                .textColor(t.colors.textPrimary)
                .radius(t.sizes.radiusSmall)
                .border(1.0f, t.colors.borderDefault)
                .onClick([f = a.requestLogTailReload] { f(); })
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
                            std::to_string(in.logTailVersion))
                .offset(in.logViewScrollOffset)
                .onChange([f = a.logViewScrolled](float v) { f(v); })
                .content([&](eui::Ui& inner, float cw, float vh) {
                    (void)vh;
                    if (in.logTail.empty()) {
                        inner.text(id + ".empty")
                            .position(8.0f, 8.0f)
                            .size(cw - 16.0f, 20.0f)
                            .text("（暂无日志内容，或日志文件尚未生成，或均低于所选级别）")
                            .fontSize(fs(12.0f))
                            .lineHeight(20.0f)
                            .color(t.colors.textTertiary)
                            .build();
                        return;
                    }
                    for (size_t i = 0; i < in.logTail.size(); ++i) {
                        inner.text(id + ".line." + std::to_string(i))
                            .position(
                                8.0f, static_cast<float>(i) * lineH)
                            .size(cw - 16.0f, lineH)
                            .text(in.logTail[i])
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

// 供应商余量功能总开关（单行小卡）。默认关闭；开启后首页余量卡、profile 余量配置入口、
// 后台刷新线程才激活。关闭不删数据。放置位置：模块 tab，在模块插拔卡下方——余量是功能开关，
// 与模块开关并列（影响多个模块的 UI 可见面 + 后台行为，但不卸载模块本身）。
// 卡高固定（不随字号缩放），下缘与模块卡保持标准间距。
void drawQuotaCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;

    const float rowH = roundFs(20.0f);
    ui.stack(id)
        .size(w, 48.0f)
        .content([&] {
            ui.text(id + ".label")
                .position(28.0f, 4.0f)
                .size(104.0f, rowH)
                .text("供应商余量")
                .fontSize(t.sizes.fontSizeBody - 1.0f)
                .lineHeight(rowH)
                .fontWeight(500)
                .color(t.colors.textPrimary)
                .build();
            // 「实验功能」灰色小字标注（§6 item 8）：wechat.textTertiary
            ui.text(id + ".experimental")
                .position(136.0f, 4.0f)
                .size(72.0f, rowH)
                .text("实验功能")
                .fontSize(fs(11.0f))
                .lineHeight(rowH)
                .color(t.colors.wechat.textTertiary)
                .build();
            // 问号图标（§6 item 10）：0xF059 fa-circle-question，hover tooltip 待定
            ui.text(id + ".help.icon")
                .position(212.0f, 4.0f)
                .size(16.0f, rowH)
                .icon(0xF059)
                .fontSize(fs(11.0f))
                .lineHeight(rowH)
                .color(t.colors.wechat.textTertiary)
                .build();
            ui.text(id + ".desc")
                .position(28.0f, 28.0f)
                .size(w - 160.0f, roundFs(16.0f))
                .text("关闭后首页余量卡隐藏、配置入口消失（已存数据保留）；开启即时生效")
                .fontSize(fs(12.0f))
                .lineHeight(roundFs(16.0f))
                .color(t.colors.textTertiary)
                .build();
            // 开关换微信绿 wswitch（§6 item 7）
            ui_kit::wswitch(ui, id + ".sw",
                in.cfg.quotaEnabled,
                w - 28.0f - 80.0f, 4.0f, 80.0f, 32.0f, t,
                [f = a.applyQuotaEnabled](bool v) { f(v); });
        })
        .build();
}

} // namespace module::settings
