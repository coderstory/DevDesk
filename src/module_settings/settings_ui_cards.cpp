// src/module_settings/settings_ui_cards.cpp
// settings 模块 6 张卡（+ GitHub Sync 卡在 github_sync_card.cpp，共 7 卡）：每个可见
// 区域一个自由函数，吃 CardDrawArgs。元素 event 回调一律按值捕获 std::function——
// 元素树比一帧活得久，捕引用会悬垂。
// 卡内容直接铺页面底：关档靠主窗 clearColor 当底，半透/磨砂档露出窗口背景。
// UAT5：目录卡（数据目录/忽略规则展示）整卡删除，相关 includes 一并清退。
#include "settings_ui_cards.h"

#include <core/component_theme.h>
#include <core/render/text.h>  // core::TextPrimitive::measureTextWidth

#include <eui/app.h>
#include <eui/dsl.h>

#include <components/button.h>
#include <components/dropdown.h>
#include <components/input.h>
#include <components/scrollview.h>
#include <components/switch.h>

#include <components/theme.h>
#include <ui_kit/card.h>
#include <ui_kit/columns.h>
#include <ui_kit/form_row.h>
#include <ui_kit/slider.h>
#include <ui_kit/wswitch.h>

#include <algorithm>
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

// 字号行右侧实时百分比文本盒宽（"130%" fs(13) ≈ 33px，余量防手滑拖出四位数）
constexpr float kFontScalePctLabelW = 44.0f;

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
// UAT3 多列分栏：卡宽可收窄到半宽列，desc 超宽一律省略号兜底（放得下时视觉不变）。
void cardDesc(eui::Ui& ui, const std::string& id, const core::theme::Tokens& t,
              float y, float w, float h, const std::string& text) {
    ui.text(id + ".desc")
        .position(28.0f, y)
        .size(w, h)
        .text(text)
        .fontSize(fs(12.0f))
        .lineHeight(h)
        .color(t.colors.textTertiary)
        // 单行省略号三件套（wrap 默认 false）：ellipsis 管排版宽、clip 管绘制范围。
        .maxWidth(w)
        .overflow(core::TextOverflow::Ellipsis)
        .clip()
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
    // E 批布局统一：标题 16/22 + desc y=40/h=16 + 内容起始 y=68（全卡统一口径，
    // roundFs 派生见 settings_ui_logic.h；标题顶 16 = 字面锚）。
    const float descH = roundFs(16.0f);
    const float hintH = roundFs(16.0f);
    // 「动态背景」开关（UAT6 主窗活体流质）落位：标题带右贴（代理卡「随应用启动
    // 代理」同款先例），**行盒零增量**——通用 tab 零滚动预算逐值保持（0/1/2 条
    // 代理警告 = 338/356/372 对可用 372；任何 +30~40 的卡内新行都会破 1/2 警告档
    // 的 372 上限，加行方案已在预算表排除）。标题盒相应收窄（w-56-150），与
    // 开关盒 x 相接不叠印（settings_card_rows_scale_derived 行盒池的严格重叠
    // 判定按 < 0.01 收口，贴边合法）。desc 槽让给开关说明（规格原文），字号/
    // 窗口效果两行自带 FormRow 标签不再重复占 desc。
    constexpr float kFluidToggleW = 150.0f;
    ui.stack(id)
        .size(w, h)
        .zIndex(40)
        .content([&] {
            // S6.1 玻璃退役→白卡 + Sm 阴影 + H10 描边 + H15 轻微半透（kCardGlassAlpha）
            ui.rect(id + ".bg")
                .position(0.0f, 0.0f).size(w, h)
                .color(ui_kit::cardSurface(t))
                .radius(t.sizes.radiusLarge)
                .shadow(components::theme::shadow(compTheme, 3.0f, 1.0f, 0.04f, 0.02f))
                .border(1.0f, t.colors.borderDefault)
                .build();

            cardTitle(ui, id, t, 16.0f, w - 56.0f - kFluidToggleW, 22.0f,
                      22.0f, "外观");
            cardDesc(ui, id, t, 40.0f, w - 56.0f, descH,
                     "主窗口背景液态流质动画；关闭恢复静态");

            // 动态背景开关（标题带右贴，y 12..40 与 desc y 40 相接不叠）
            components::toggleSwitch(ui, id + ".fluid")
                .position(w - 28.0f - kFluidToggleW, 12.0f)
                .size(kFluidToggleW, 28.0f)
                .text("动态背景")
                .fontSize(fs(13.0f))
                .checked(in.cfg.fluidBackgroundEnabled)
                .theme(compTheme)
                .onChange([f = a.applyFluidBackgroundEnabled](bool v) { f(v); })
                .build();

            // 主题选择器已删除（D-12/E3 去功能例外——WeChat 皮肤即主题、暗色延后 D-04）。
            // 字号缩放（§6 item 3 确认在通用 tab）+ 窗口效果用 FormRow 排列（§6 item 5）。
            const float rowH = logFieldHeight();  // 36px（与日志卡同口径）
            const float fontRowY = 40.0f + descH + roundFs(12.0f);  // 100% = 68
            const float effectRowY = fontRowY + rowH + roundFs(6.0f);
            const float formX = 28.0f;
            const float formW = w - 56.0f;  // 卡内左右各 28 padding

            // 字号缩放 FormRow：label 110px + dropdown control
            ui_kit::formRow(ui, id + ".font", "字号缩放", formX, fontRowY,
                            formW, rowH,
                [&](eui::Ui& ui, float cx, float cy, float cw, float ch) {
                    // UAT6.2 无级调节：四档下拉 → 滑杆（80-130% 连续取值，1% 分辨率
                    // 量化落 cfg）+ 右侧实时百分比文本。行几何不变 → 卡高公式不受档位影响。
                    const float wrapW = cw;
                    const float pctW = kFontScalePctLabelW;
                    const float sliderW = wrapW - pctW - roundFs(8.0f);
                    const float ratio =
                        static_cast<float>(in.cfg.fontScalePercent - kMinFontScalePercent) /
                        static_cast<float>(kMaxFontScalePercent - kMinFontScalePercent);
                    ui.stack(id + ".font.ddwrap")
                        .position(cx, cy)
                        .size(wrapW, ch)
                        .content([&] {
                            ui_kit::slider(ui, id + ".font.slider", 0.0f, 0.0f,
                                           sliderW, ch, ratio, t,
                                [f = a.applyFontScalePercent](float v) {
                                    f(fontScalePercentFromRatio(v));
                                });
                            ui.text(id + ".font.pct")
                                .position(sliderW + roundFs(8.0f), 0.0f)
                                .size(pctW, ch)
                                .text(std::to_string(in.cfg.fontScalePercent) + "%")
                                .fontSize(fs(13.0f))
                                .lineHeight(ch)
                                .color(t.colors.textSecondary)
                                .build();
                        })
                        .build();
                }, t);

            // 窗口效果 FormRow
            ui_kit::formRow(ui, id + ".effect", "窗口效果", formX, effectRowY,
                            formW, rowH,
                [&](eui::Ui& ui, float cx, float cy, float cw, float ch) {
                    const float ddW = cw;
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
// UAT3 多列分栏：本卡为半宽配对右列，行内尾注放不下（formW ≈292、控件区仅 ≈174）
// → hint 迁下拉下方第二行（行 y 单一来源 hintRowBelowY）。UAT6：卡内第二段加
// 「开机自启动」开关行，卡高 144→184（与外观卡同高，配对行高不变，见行内注释）。
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
    // E 批布局统一：裸 dropdown 行迁 FormRow（label 110 + dropdown），
    // 行 y = editorFontRowY（100% = 68 内容起始口径），行高与日志卡同源（logFieldHeight）。
    const float rowH = logFieldHeight();
    const float rowY = editorFontRowY();
    const float hintH = roundFs(16.0f);
    const float hintY = hintRowBelowY(rowY);
    ui.stack(id)
        .size(w, h)
        .zIndex(30)
        .content([&] {
            // S6.1 玻璃退役→白卡 + Sm 阴影 + H10 描边 + H15 轻微半透（kCardGlassAlpha）
            ui.rect(id + ".bg")
                .position(0.0f, 0.0f).size(w, h)
                .color(ui_kit::cardSurface(t))
                .radius(t.sizes.radiusLarge)
                .shadow(components::theme::shadow(compTheme, 3.0f, 1.0f, 0.04f, 0.02f))
                .border(1.0f, t.colors.borderDefault)
                .build();

            cardTitle(ui, id, t, 16.0f, w - 56.0f, 22.0f, 22.0f, "编辑器字号");
            ui_kit::formRow(ui, id + ".font", "字号", 28.0f, rowY,
                            w - 56.0f, rowH,
                [&](eui::Ui& ui, float cx, float cy, float cw, float ch) {
                    // UAT4 自适应：下拉填满控件区（替代 min(240,cw) 固定帽）——
                    // 窄窗 cw 本身已收缩（让位保留），宽窗不再停在 240 留大段空白。
                    const float ddW = cw;
                    ui.stack(id + ".ddwrap")
                        .position(cx, cy)
                        .size(ddW, ch)
                        .content([&] {
                            const bool efontOpenUp = dropdownPopupOverflows(
                                pageY, rowY, rowH,
                                kEditorFontPresetCount, screenH);
                            components::dropdown(ui, id + ".dd")
                                .size(ddW, ch)
                                .theme(compTheme)
                                .items(std::vector<std::string>(
                                    kEditorFontPresetLabels,
                                    kEditorFontPresetLabels +
                                        kEditorFontPresetCount))
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
                }, t);
            // hint 第二行：下拉下方全卡宽。UAT6「字号说明没显示全」：旧文案
            // 30.7em 在 115%/130% 档（fs(12)=13.8/15.6）实宽 423/479px 超出
            // 最小窗 1000 的 hintW 396（colW 452-56）被省略号截断。两案中取
            // 「缩短」：换行要 +16px 行盒，配对行 184 被外观卡钉死（通用 tab
            // 零滚动预算 372@640 无余量），只能挪 68 内容锚破坏与外观卡并排
            // 对齐、且会顶掉 UAT6 自启动行（autostart 批 autostartRowBelowY
            // 按 hint 单行高推导）——缩短零几何影响。「默认 15px（标准）」与
            // 下拉选中档文案「标准 15px」重复，删之不损信息。新文案 16.6em，
            // 四档实宽 169/199/229/259px 全档放得下（回退锁见 hint 适配用例）。
            const float hintW = w - 56.0f;
            ui.text(id + ".hint")
                .position(28.0f, hintY)
                .size(hintW, hintH)
                .text("Markdown 正文字号；切换即时生效")
                .fontSize(fs(12.0f))
                .lineHeight(hintH)
                .color(t.colors.textTertiary)
                .maxWidth(hintW)
                .overflow(core::TextOverflow::Ellipsis)
                .clip()
                .build();

            // ── 开机自启动行（UAT6）：落位在本卡的配对行阴影里 ──
            // 通用 tab 1000×640 档零滚动预算已满（配对行 184 + 代理卡 138~172 =
            // 内容 338~372，可用 372），新建卡/加高外观卡都会破预算；唯一零增长
            // 空位 = 配对行内矮卡（本卡原 144 → 现 184）的 40px 阴影，行落此 →
            // 配对行高恒 max(184,184)=184 不变（85%/130% 同由 roundFs(184) 掌行高）。
            // 几何单一来源 settings_ui_logic.h（autostartRowBelowY/autostartRowHeight/
            // kAutostartSwitch*）；POSIX（不支持）整行不建，卡高不变（阴影留白）。
            if (in.autostartSupported) {
                // 行盒两变量刻意异名（startRowY/startRowH）：本卡上方已有字号段的
                // rowY/rowH（editorFontRowY/logFieldHeight），lambda 内同名会遮蔽捕获
                const float startRowY = autostartRowBelowY(hintY);
                const float startRowH = autostartRowHeight();
                const float swY = startRowY + (startRowH - kAutostartSwitchH) * 0.5f;
                const float labelW = roundFs(92.0f);
                const float descX = 28.0f + labelW + roundFs(8.0f);
                const float descW = w - 28.0f - kAutostartSwitchW -
                                    roundFs(12.0f) - descX;
                // 标签 + 说明一行 + 右贴 44×24 微信开关（行几何 = 开关右贴卡内
                // 边距 28、行盒内垂直居中；说明超宽省略号兜底）
                ui.text(id + ".autostart.label")
                    .position(28.0f, startRowY)
                    .size(labelW, startRowH)
                    .text("开机自启动")
                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                    .lineHeight(startRowH)
                    .fontWeight(500)
                    .color(t.colors.textPrimary)
                    .maxWidth(labelW)
                    .overflow(core::TextOverflow::Ellipsis)
                    .clip()
                    .build();
                ui.text(id + ".autostart.desc")
                    .position(descX, startRowY)
                    .size(std::max(0.0f, descW), startRowH)
                    .text("开机后自动运行并最小化到托盘")
                    .fontSize(fs(12.0f))
                    .lineHeight(startRowH)
                    .color(t.colors.textTertiary)
                    .maxWidth(std::max(0.0f, descW))
                    .overflow(core::TextOverflow::Ellipsis)
                    .clip()
                    .build();
                // wswitch 命中元素 = <id>.track（回调断言按此定位）；开关显示
                // 注册真值（in.autostartRegistered），拨动 → applyAutostartEnabled
                ui_kit::wswitch(ui, id + ".autostart.sw", in.autostartRegistered,
                                w - 28.0f - kAutostartSwitchW, swY,
                                kAutostartSwitchW, kAutostartSwitchH, t,
                                [f = a.applyAutostartEnabled](bool v) { f(v); });
            }
        })
        .build();
}

// 各模块一格开关（UAT3 多列分栏：半宽卡 + 卡内两列网格，行数随开关数派生）；关闭 = 下次启动不注册进
// Router。home/settings 恒启用，不提供开关。格内 name/desc 在窄列（最小窗 colW=348）
// 必然超宽 → 省略号兜底（标准降级，用户拍板接受）。
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
        // 上线：一个开关控制「任务」+「基础配置」两个子页（多页模块共用门控键）
        {kModuleIdRelease, "上线",
         "任务执行与基础配置（两个子页同开同关）"},
    };
    constexpr int kToggleCount = static_cast<int>(sizeof(kToggleable) / sizeof(kToggleable[0]));
    ui.stack(id)
        .size(w, h)
        .content([&] {
            const auto compThemeTop = core::theme::componentTokensWeChat(t);
            // S6.1 玻璃退役→白卡 + Sm 阴影 + H10 描边 + H15 轻微半透（kCardGlassAlpha）
            ui.rect(id + ".bg")
                .position(0.0f, 0.0f).size(w, h)
                .color(ui_kit::cardSurface(t))
                .radius(t.sizes.radiusLarge)
                .shadow(components::theme::shadow(compThemeTop, 3.0f, 1.0f, 0.04f, 0.02f))
                .border(1.0f, t.colors.borderDefault)
                .build();

            cardTitle(ui, id, t, 16.0f, w - 56.0f, 22.0f, 22.0f, "模块");
            cardDesc(ui, id, t, 40.0f, w - 56.0f, roundFs(16.0f),
                     "开关即时生效：关闭后该模块代码不再执行，侧边栏入口与"
                     "页面当帧更新");

            // 开关两列网格：行主序（0/1 同行异列，2/3 次行，4/5 …）；开关控件几何沿用
            // 44×24（UAT3 由 80×32 收敛口径），在 name+desc 36 高块内垂直居中
            //（相对偏移 +6）。格宽/右格 x 走 ui_kit::columnX 单一来源（gap 16）。
            const float nameH = roundFs(18.0f);
            const float rowDescDy = nameH + roundFs(2.0f);
            const float rowDescH = roundFs(16.0f);
            const float rowPitch = roundFs(56.0f);
            const float switchW = 44.0f;
            const float switchH = 24.0f;
            const float switchGap = 12.0f;   // 格内文案与右贴开关的间距
            float cellW = 0.0f;
            float cell2X = 0.0f;
            ui_kit::columnX(w - 56.0f, 16.0f, &cellW, &cell2X);
            for (int i = 0; i < kToggleCount; ++i) {
                const ModuleMeta& meta = kToggleable[i];
                const int col = i % 2;   // 行主序：0/1 同行，2/3 次行
                const int row = i / 2;   // 行数由 kToggleCount 派生（release 为第 5 格）
                const float cellX = 28.0f + (col == 0 ? 0.0f : cell2X);
                const float rowY = 68.0f + static_cast<float>(row) * rowPitch;
                const float textW = cellW - switchW - switchGap;
                const std::string suffix = std::to_string(i);
                ui.text(id + ".name." + suffix)
                    .position(cellX, rowY)
                    .size(textW, nameH)
                    .text(meta.name)
                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                    .lineHeight(nameH)
                    .fontWeight(500)
                    .color(t.colors.textPrimary)
                    .maxWidth(textW)
                    .overflow(core::TextOverflow::Ellipsis)
                    .clip()
                    .build();
                ui.text(id + ".rowdesc." + suffix)
                    .position(cellX, rowY + rowDescDy)
                    .size(textW, rowDescH)
                    .text(meta.desc)
                    .fontSize(fs(12.0f))
                    .lineHeight(rowDescH)
                    .color(t.colors.textTertiary)
                    .maxWidth(textW)
                    .overflow(core::TextOverflow::Ellipsis)
                    .clip()
                    .build();
                // 开关换微信绿 wswitch（§6 item 7）：绿轨道+白滑块（§4.4）。
                // wswitch 的 clickable 元素 id = <id>.track（probe 按 .track 定位）。
                // UAT3：80×32 显示过大 → 44×24（agent_module 详情弹窗同款）；
                // 格视觉块 = name 18 + 2 + desc 16 = 36 高，24 开关居中偏移 +6。
                ui_kit::wswitch(ui, id + ".sw." + suffix,
                    moduleEnabled(in.cfg, meta.moduleId),
                    cellX + cellW - switchW, rowY + 6.0f, switchW, switchH, t,
                    [f = a.applyModuleEnabled,
                     mid = meta.moduleId](bool v) {
                        f(mid, v);
                    });
            }
        })
        .build();
}

// 壁纸卡（模块 tab，全宽）：标题 + 说明 + 右侧 44×24 开关。平台能力门控在注册表
// 的 visible 函数（unsupported 平台整卡不建、不占高度），本函数无平台分支。
// 开关行几何与卡高同源（settings_ui_logic.h 的 kWallpaperCardHeight 注释）：
// 行 y = roundFs(68) 内容起始锚、行盒 roundFs(44)，44×24 字面开关行内垂直居中。
void drawWallpaperCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const float h = args.h;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;

    constexpr float kSwitchW = 44.0f;   // §4.4 微信开关字面值（不随字号缩放）
    constexpr float kSwitchH = 24.0f;
    constexpr float kSidePad = 28.0f;   // 卡内左右边距（与各卡同款）

    ui.stack(id)
        .size(w, h)
        .content([&] {
            const auto compThemeTop = core::theme::componentTokensWeChat(t);
            // 白卡 + Sm 阴影 + 描边（与模块插拔卡同款，S6.1 玻璃退役口径）
            ui.rect(id + ".bg")
                .position(0.0f, 0.0f).size(w, h)
                .color(ui_kit::cardSurface(t))
                .radius(t.sizes.radiusLarge)
                .shadow(components::theme::shadow(compThemeTop, 3.0f, 1.0f,
                                                  0.04f, 0.02f))
                .border(1.0f, t.colors.borderDefault)
                .build();

            cardTitle(ui, id, t, 16.0f, w - 56.0f, 22.0f, 22.0f, "壁纸");
            cardDesc(ui, id, t, 40.0f, w - 56.0f, roundFs(16.0f),
                     "动态桌面壁纸（视频 / 图片），开启后随 DevDesk 启动");

            // 开关行：内容起始锚 68（与日志/编辑器卡同口径）→ 行盒 roundFs(44)
            // → 下边距 roundFs(16)，三者之和即 kWallpaperCardHeight 基准。
            const float rowY = roundFs(68.0f);
            const float rowH = roundFs(44.0f);
            const float swY = rowY + (rowH - kSwitchH) * 0.5f;
            // wswitch 命中元素 = <id>.track；初始值读 cfg 模块态，拨动走既有
            // applyModuleEnabled → onModuleToggled 链路（与模块插拔卡同一入口）。
            ui_kit::wswitch(
                ui, id + ".sw", moduleEnabled(in.cfg, kModuleIdWallpaper),
                w - kSidePad - kSwitchW, swY, kSwitchW, kSwitchH, t,
                [f = a.applyModuleEnabled](bool v) {
                    f(kModuleIdWallpaper, v);
                });
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

    // 行几何随生效字号派生（helper 见 settings_ui_logic.h）；E 批统一——标题 16/22、
    // desc y=40/h=16、端口行迁 FormRow（行 y=68 内容起始口径），接管行从端口行盒底顺延。
    const float descH = roundFs(16.0f);
    const float rowH = logFieldHeight();
    const float portRowY = proxyPortRowY();
    const float takeoverY = proxyTakeoverRowY();
    const float hintH = roundFs(16.0f);
    const float takeoverH = proxyTakeoverRowHeight();
    const float warnY0 = proxyWarnRowTop();
    const float warnH = proxyWarnRowHeight();

    // H18 横向弹性锚（S4.1：1000×640 为锚点，向上按伸缩公式）：
    // 固定控件（开关/输入/按钮）各持定宽，控件组右缘钉卡内容右缘（w − 28）；
    // 说明列（端口提示 / 接管状态值）宽 = 卡内可用宽 − label 宽 − 控件宽 − 间距，
    // 宽窗增量只进说明列——各列宽与改前逐值一致（仅控件组按右锚重排次序），
    // 不产生右侧死区。
    constexpr float kPortInputW = 96.0f;         // 端口输入框（数值定宽）
    constexpr float kPortApplyW = 64.0f;         // 「应用」按钮
    constexpr float kPortCtrlGap = 8.0f;         // 输入框 → 按钮
    constexpr float kPortHintGap = 12.0f;        // 说明列 → 控件组
    constexpr float kTakeoverLabelW = 150.0f;    // 接管行 label（固定列）
    constexpr float kTakeoverGap = 12.0f;        // label → 状态值（说明列）

    ui.stack(id)
        .size(w, h)
        .content([&] {
            const auto compTheme = core::theme::componentTokensWeChat(t);
            // S6.1 玻璃退役→白卡 + Sm 阴影 + H10 描边 + H15 轻微半透（kCardGlassAlpha）
            ui.rect(id + ".bg")
                .position(0.0f, 0.0f).size(w, h)
                .color(ui_kit::cardSurface(t))
                .radius(t.sizes.radiusLarge)
                .shadow(components::theme::shadow(compTheme, 3.0f, 1.0f, 0.04f, 0.02f))
                .border(1.0f, t.colors.borderDefault)
                .build();

            auto* svc = in.proxy;
            const bool running = svc && svc->isProxyRunning();
            const int port = running ? svc->proxyPort() : 0;

            cardTitle(ui, id, t, 16.0f, 240.0f, 22.0f, 22.0f, "Claude 代理");

            // 随应用启动代理（默认关）放卡片右上：SwitchBuilder 盒内左排（轨左字右）
            // 且不感知盒宽，盒宽须=内容实宽（左内距+轨道+间距+文字实宽），文字右缘
            // 才恰钉卡内容右缘 28（H18）；盒宽超出即右侧死区、控件组视觉脱离锚线。
            // 文字实宽随生效字号缩放，须运行期测量（同 SwitchBuilder 度量口径）。
            const float autoStartTextW = core::TextPrimitive::measureTextWidth(
                "随应用启动代理", {}, fs(13.0f), 400);
            const float autoStartBoxW = compTheme.metrics.spacing.control
                + compTheme.metrics.control.switchWidth
                + compTheme.metrics.spacing.control
                + autoStartTextW;
            components::toggleSwitch(ui, id + ".autostart")
                .position(w - 28.0f - autoStartBoxW, 12.0f)
                .size(autoStartBoxW, 28.0f)
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
            // E 批统一：端口行 = FormRow（label 110）+ 说明列 + 右侧固定控件组
            //（input/应用按钮，H18 右锚）。
            ui_kit::formRow(ui, id + ".port", "代理端口", 28.0f, portRowY,
                            w - 56.0f, rowH,
                [&](eui::Ui& ui, float cx, float cy, float cw, float ch) {
                    const float inputH = 28.0f;
                    const float ctrlY = cy + (ch - inputH) * 0.5f;
                    // H18：固定控件组（输入 96 + 间距 8 + 应用 64）右锚控件区
                    // 右缘（cx+cw = 卡内容右缘 w−28）；说明列 flex 从 label 右缘
                    // 起，宽 = cw − 96 − 8 − 64 − 12（任一宽度下均与改前盒宽同值，
                    // 宽窗增量全归说明列）。
                    const float applyX = cx + cw - kPortApplyW;
                    const float inputX = applyX - kPortCtrlGap - kPortInputW;
                    components::input(ui, id + ".port.input")
                        .position(inputX, ctrlY)
                        .size(kPortInputW, inputH)
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
                    // UAT3 W2-M1：core 橘（#D97757 三态）残留 → 与 github 卡主按钮
                    //（btn.push「立即同步」）完全同款：componentTokensWeChat 的
                    // primary=#36D399 绿，不再覆写 colors/textColor
                    components::button(ui, id + ".port.apply")
                        .position(applyX, ctrlY)
                        .size(kPortApplyW, inputH)
                        .text("应用")
                        .fontSize(fs(13.0f))
                        .theme(compTheme, /*primary=*/true)
                        .radius(t.sizes.radiusSmall)
                        .onClick([f = a.applyProxyPort] { f(); })
                        .build();
                    const float hintX = cx;
                    const float hintW =
                        std::max(0.0f, inputX - kPortHintGap - hintX);
                    ui.text(id + ".port.hint")
                        .position(hintX, cy + (ch - hintH) * 0.5f)
                        .size(hintW, hintH)
                        .text("改后回车或点「应用」；代理运行中自动重启")
                        .fontSize(fs(12.0f))
                        .lineHeight(hintH)
                        .color(t.colors.textTertiary)
                        .build();
                }, t);

            // Claude Code 接管状态：sidecar 是否存在推导，让还原机制对用户可见。
            const bool managed = in.takeoverManaged;
            // H18：label 150 为固定列；状态值列（说明列）起点由 28 + label + 12
            // 派生（旧写死 190，改 label 宽即漂），宽 = w − 起点 − 28（与原式
            // 等价；宽窗增量同进状态值列）。
            const float takeoverValueX =
                28.0f + kTakeoverLabelW + kTakeoverGap;
            ui.text(id + ".takeover.label")
                .position(28.0f, takeoverY)
                .size(kTakeoverLabelW, takeoverH)
                .text("Claude Code 接管")
                .fontSize(fs(12.0f))
                .lineHeight(takeoverH)
                .color(t.colors.textSecondary)
                .build();
            // 路径读 Claude 运行面内存态（main 启动期注入，即接管实际使用的路径）。
            const std::string ccPath =
                svc ? svc->settingsFilePath() : std::string();
            ui.text(id + ".takeover.value")
                .position(takeoverValueX, takeoverY)
                .size(w - takeoverValueX - 28.0f, takeoverH)
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

// 下拉出界则 openUp 向上；UAT3 多列分栏：两行改卡内双列同排（日志保留 | 日志级别，
// 列几何走 ui_kit::columnX），hint 从行内尾注并一行下移（hintRowBelowY），
// 卡高 170→144。行 y 单一来源 settings_ui_logic.h（logRow1LabelY，100% = 68）。
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
    const float hintFontSize = fs(12.0f);
    const float rowH = logFieldHeight();
    const float hintH = roundFs(16.0f);
    const float row1Y = logRow1LabelY();        // 两下拉同排行锚
    const float hintY = hintRowBelowY(row1Y);   // hint 并一行（两 hint 同 y）
    const float innerX = 28.0f;
    float colW = 0.0f;
    float col2X = 0.0f;
    ui_kit::columnX(w - 56.0f, 16.0f, &colW, &col2X);
    ui.stack(id)
        .size(w, h)
        .zIndex(30)
        .content([&] {
            // S6.1 玻璃退役→白卡 + Sm 阴影（日志卡原本无 bg，新增）+ H10 描边 + H15 轻微半透（kCardGlassAlpha）
            ui.rect(id + ".bg")
                .position(0.0f, 0.0f).size(w, h)
                .color(ui_kit::cardSurface(t))
                .radius(t.sizes.radiusLarge)
                .shadow(components::theme::shadow(compTheme, 3.0f, 1.0f, 0.04f, 0.02f))
                .border(1.0f, t.colors.borderDefault)
                .build();

            cardTitle(ui, id, t, 16.0f, w - 56.0f, 22.0f, 22.0f, "日志");

            // 档位标签（0 = 永久）；存的是原始天数 int，清理任务按值读取
            static const char* kRetentionLabels[] = {
                "永久保留", "7 天", "30 天", "90 天", "180 天", "365 天"};
            // 左列：日志保留
            ui_kit::formRow(ui, id, "日志保留", innerX, row1Y, colW, rowH,
                [&](eui::Ui& ui, float cx, float cy, float cw, float ch) {
                    // UAT4 自适应：下拉填满控件区（kLogFieldWidth 240 固定帽退位）——
                    // 窄列 cw 已让位收缩，宽窗填满列内可用宽（与同步卡撑满同原则）。
                    const float ddW = cw;
                    ui.stack(id + ".ddwrap")
                        .position(cx, cy)
                        .size(ddW, ch)
                        .content([&] {
                            const bool openUp = dropdownPopupOverflows(
                                pageY, row1Y, rowH,
                                kRetentionPresetCount, screenH);
                            components::dropdown(ui, id + ".dd")
                                .size(ddW, ch)
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
                }, t);

            // 右列：日志级别（切档即时 core::log::setLevel，DEBUG 起打详录；重启由 onStart 恢复持久化档）
            ui_kit::formRow(ui, id + ".level", "日志级别", innerX + col2X,
                            row1Y, colW, rowH,
                [&](eui::Ui& ui, float cx, float cy, float cw, float ch) {
                    // UAT4 自适应：同左列——填满控件区，固定帽退位（两下拉同宽口径不变）。
                    const float ddW = cw;
                    ui.stack(id + ".level.ddwrap")
                        .position(cx, cy)
                        .size(ddW, ch)
                        .content([&] {
                            const bool levelOpenUp = dropdownPopupOverflows(
                                pageY, row1Y, rowH,
                                kLogLevelPresetCount, screenH);
                            components::dropdown(ui, id + ".level.dd")
                                .size(ddW, ch)
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
                }, t);

            // hint 并一行下移：各随其列、同一行 y（窄列超宽省略号兜底）。
            ui.text(id + ".hint")
                .position(innerX, hintY)
                .size(colW, hintH)
                .text("0 = 永久保留；30 天为默认。切换档位立即清理一次。")
                .fontSize(hintFontSize)
                .lineHeight(hintH)
                .color(t.colors.textTertiary)
                .maxWidth(colW)
                .overflow(core::TextOverflow::Ellipsis)
                .clip()
                .build();
            ui.text(id + ".level.hint")
                .position(innerX + col2X, hintY)
                .size(colW, hintH)
                .text("DEBUG 最详尽（排障）；默认 INFO。切档即时生效。")
                .fontSize(hintFontSize)
                .lineHeight(hintH)
                .color(t.colors.textTertiary)
                .maxWidth(colW)
                .overflow(core::TextOverflow::Ellipsis)
                .clip()
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
            // S6.1 玻璃退役→白卡 + Sm 阴影（日志查看卡原本无 bg，新增）+ H10 描边 + H15 轻微半透（kCardGlassAlpha）
            ui.rect(id + ".bg")
                .position(0.0f, 0.0f).size(w, h)
                .color(ui_kit::cardSurface(t))
                .radius(t.sizes.radiusLarge)
                .shadow(components::theme::shadow(compTheme, 3.0f, 1.0f, 0.04f, 0.02f))
                .border(1.0f, t.colors.borderDefault)
                .build();

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

} // namespace module::settings
