// src/module_settings/settings_ui_cards.cpp
// settings 模块 8 张卡：每个可见区域一个自由函数，吃 CardDrawArgs（只读入参 +
// 回调 + 几何）。元素 event 回调一律按值捕获用到的 std::function——元素树比一帧
// 活得久，捕引用会悬垂。
//
// 卡内容直接铺页面底（9 张卡的 cardFrame 与主体渐变底板均已删）：关档靠主窗
// clearColor 当底，半透/磨砂档露出窗口背景。
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

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace module::settings {

namespace {

// cfg_.themeMode（三档字符串）→ 下拉索引；sanitized 保证只会是三档之一
int themeModeIndex(const std::string& mode) {
    if (mode == kThemeModeLight) return 1;
    if (mode == kThemeModeDark) return 2;
    return 0;  // auto
}

// cfg_.windowEffect（三档字符串）→ 下拉索引；sanitized 保证只会是三档之一
int windowEffectIndex(const std::string& mode) {
    if (mode == kWindowEffectTranslucent) return 1;
    if (mode == kWindowEffectFrosted) return 2;
    return 0;
}

// cfg_.logLevel（四档字符串）→ 下拉索引；sanitized 保证只会是四档之一
int logLevelIndex(const std::string& level) {
    if (level == kLogLevelDebug) return 0;
    if (level == kLogLevelWarn) return 2;
    if (level == kLogLevelError) return 3;
    return 1;  // INFO 默认档
}

// 卡内标题 / 描述行 helper：左缩进 28（对齐 kDialogInsetX 语系）、字体与颜色在
// 所有调用点一致；标题字重 600。

// 卡标题：y = 标题顶，w = 标题盒宽，h = 盒高，lineHeight 通常 = h。
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

// ---------- 卡片 1：外观（主题 + 字号 + 窗口效果） ----------
// zIndex(40)：下拉弹层在卡片子树内，需整卡抬层才能盖过下方卡片（zIndex 只在兄弟间
// 排序）。同一 tab 内带下拉的卡，靠前者必须 z 高于其后所有卡——等值 z 时后绘者
//（同 tab 的编辑器字号卡 z30）会盖住前者的弹层。40 = 最前卡档位，后续带下拉卡 ≤30。
void drawAppearanceCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const float h = args.h;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;

    const auto compTheme = core::theme::componentTokens(t);
    // 行几何随生效字号派生（roundFs 口径见 settings_ui_logic.h）：盒高与后续行 y 沿
    // 派生盒底累加。字面锚不缩放：18 = 标题顶（卡顶 padding），40 = 下拉控件高（组件 chrome）。
    const float titleH = roundFs(24.0f);
    const float descY = 18.0f + titleH + roundFs(2.0f);
    const float descH = roundFs(18.0f);
    const float labelY = descY + descH + roundFs(10.0f);
    const float labelH = roundFs(16.0f);
    const float ddY = labelY + labelH + roundFs(6.0f);
    const float hintY = ddY + 40.0f + roundFs(6.0f);
    const float hintH = roundFs(16.0f);
    ui.stack(id)
        .size(w, h)
        .zIndex(40)
        .content([&] {
            cardTitle(ui, id, t, 18.0f, w - 56.0f, titleH, titleH, "外观");
            cardDesc(ui, id, t, descY, w - 56.0f, descH,
                     "主题与窗口底色即时生效；字号缩放全局生效");

            // 主题模式下拉：跟随系统 / 浅色 / 深色三档（替代只能表达显式档的双按钮）。
            ui.text(id + ".theme.label")
                .position(28.0f, labelY)
                .size(200.0f, labelH)
                .text("主题")
                .fontSize(fs(12.0f))
                .lineHeight(labelH)
                .color(t.colors.textSecondary)
                .build();
            ui.stack(id + ".theme.ddwrap")
                .position(28.0f, ddY)
                .size(240.0f, 40.0f)
                .content([&] {
                    components::dropdown(ui, id + ".theme.dd")
                        .size(240.0f, 40.0f)
                        .theme(compTheme)
                        .items(std::vector<std::string>(
                            kThemeModePresetLabels,
                            kThemeModePresetLabels + kThemeModePresetCount))
                        .selected(themeModeIndex(in.cfg.themeMode))
                        .open(isDropdownOpen(in.openDropdown, Dropdown::Theme))
                        .onOpenChange([f = a.setDropdownOpen](bool v) {
                            f(Dropdown::Theme, v);
                        })
                        .onChange([f = a.applyThemeMode](int idx) { f(idx); })
                        .build();
                })
                .build();

            // 字号缩放 + 窗口效果两列，右缘自适应：宽窗保持原位，窄窗右对齐收进卡内
            //（列宽 240、列距 28、右缘留 28），避免最小窗（1200）下溢出卡片。
            const float effectX = std::min(680.0f, w - 28.0f - 240.0f);
            const float fontX = std::min(380.0f, effectX - 240.0f - 28.0f);
            ui.text(id + ".font.label")
                .position(fontX, labelY)
                .size(240.0f, labelH)
                .text("字号缩放（正文基准 15px）")
                .fontSize(fs(12.0f))
                .lineHeight(labelH)
                .color(t.colors.textSecondary)
                .build();
            ui.stack(id + ".font.ddwrap")
                .position(fontX, ddY)
                .size(240.0f, 40.0f)
                .content([&] {
                    components::dropdown(ui, id + ".font.dd")
                        .size(240.0f, 40.0f)
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

            // 窗口效果三档，即时生效（effectX 见上）。
            ui.text(id + ".effect.label")
                .position(effectX, labelY)
                .size(240.0f, labelH)
                .text("窗口效果")
                .fontSize(fs(12.0f))
                .lineHeight(labelH)
                .color(t.colors.textSecondary)
                .build();
            ui.stack(id + ".effect.ddwrap")
                .position(effectX, ddY)
                .size(240.0f, 40.0f)
                .content([&] {
                    components::dropdown(ui, id + ".effect.dd")
                        .size(240.0f, 40.0f)
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
            // 降级提示行（activeWindowEffect 回查实际生效值；一致时为空不显示）
            const std::string degraded = windowEffectDegradedHint(
                in.cfg.windowEffect, app::activeWindowEffect());
            if (!degraded.empty()) {
                // 可用宽：28 = 右内缩，16 = 图标让位
                const float hintW = w - effectX - 16.0f - 28.0f;
                // 图标 / 文字拆两个元素（FA 图标字体通道）
                ui.text(id + ".effect.hint.icon")
                    .position(effectX, hintY)
                    .size(14.0f, hintH)
                    .icon(0xF071)  // fa-triangle-exclamation
                    .fontSize(fs(11.0f))
                    .lineHeight(hintH)
                    .color(t.colors.warning)
                    .build();
                ui.text(id + ".effect.hint")
                    .position(effectX + 16.0f, hintY)
                    .size(hintW, hintH)
                    .text(degraded)
                    .fontSize(fs(11.0f))
                    .lineHeight(hintH)
                    .color(t.colors.warning)
                    // 窄窗可用宽不足时截断而非溢出（完整文案在宽窗可见）：单行省略号需
                    // overflow==Ellipsis && maxWidth>0 && !wrap 三件套，显式 maxWidth 是
                    // 生效前提（runtime 在 maxWidth==0 时回退 frame.width，与 DSL 分叉）；
                    // clip 作几何兜底（ellipsis 管排版宽，clip 管绘制范围）。
                    .maxWidth(hintW)
                    .overflow(core::TextOverflow::Ellipsis)
                    .clip()
                    .build();
            }
        })
        .build();
}

// ---------- 编辑器字号卡（自日志卡迁入「通用」tab） ----------
// 档位写 core::theme::editorFontSize 通道，markdown 下一帧生效；默认 15px = fontSizeBody。
// pageY/screenH：下拉弹层 openUp 几何估算。
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

    const auto compTheme = core::theme::componentTokens(t);
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
            // 描述行与左侧下拉并排（y=48，字面控件），下方无行。
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

// ---------- 卡片 2：模块管理 ----------
// 四个工具模块（markdown/todo/claude_proxy/agent）+ 桌宠各一行开关；关闭 = 下次启动
// 不注册进 Router（不执行代码、不显示侧边栏入口）。home/settings 恒启用，不提供开关。
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
        {kModuleIdPet, "桌宠",
         "桌面小宠物：透明置顶小窗，可拖拽/点击互动（默认关闭）"},
        {kModuleIdAgent, "智能体",
         "任务列表 + 手动触发运行（依赖 Claude 代理提供模型通道）"},
    };
    constexpr int kToggleCount = static_cast<int>(sizeof(kToggleable) / sizeof(kToggleable[0]));
    const auto compTheme = core::theme::componentTokens(t);
    ui.stack(id)
        .size(w, h)
        .content([&] {
            cardTitle(ui, id, t, 16.0f, w - 56.0f, 22.0f, 22.0f, "模块");
            cardDesc(ui, id, t, 40.0f, w - 56.0f, roundFs(16.0f),
                     "开关即时生效：关闭后该模块代码不再执行，侧边栏入口与"
                     "页面当帧更新");

            // 开关行行几何随生效字号派生：名称/描述行盒、行距（100% 档 = 56）；行 1 顶
            // y=68 锚在字面说明行下方。开关控件（32 高字面组件）随行顶走，相对偏移 -2。
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
                // SwitchBuilder 无 position，套 stack 定位（与代理卡同款）
                ui.stack(id + ".sw.wrap." + suffix)
                    .position(w - 28.0f - 80.0f, rowY - 2.0f)
                    .size(80.0f, 32.0f)
                    .content([&] {
                        components::toggleSwitch(ui, id + ".sw." + suffix)
                            .size(80.0f, 32.0f)
                            .checked(moduleEnabled(in.cfg, meta.moduleId))
                            .theme(compTheme)
                            .onChange([f = a.applyModuleEnabled,
                                       mid = meta.moduleId](bool v) {
                                f(mid, v);
                            })
                            .build();
                    })
                    .build();
            }
        })
        .build();
}

// ---------- 卡片 3：桌宠 ----------
// 皮肤下拉 + 位置重置按钮，两者即时生效；模块开关也即时生效（app::toggle 桥）。
void drawPetCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const float h = args.h;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;
    const float pageY = args.pageY;    // 卡顶页面绝对 y（皮肤下拉 openUp 几何）
    const float screenH = args.screenH;

    // 皮肤清单（discoverSkins）与当前皮肤名已移出渲染路径——快照由 refreshExternalState
    // 装载，这里只读入参。
    const auto compTheme = core::theme::componentTokens(t);
    // 本卡抬到 z40（仿外观卡）：皮肤下拉弹层向下铺开时越出卡底沿盖住后续目录卡。
    ui.stack(id)
        .size(w, h)
        .zIndex(40)
        .content([&] {
            cardTitle(ui, id, t, 16.0f, w - 56.0f, 22.0f, 22.0f, "桌宠");
            cardDesc(ui, id, t, 40.0f, w - 56.0f, roundFs(16.0f),
                     "换肤与位置重置即时生效；关闭桌宠立即隐藏、重新开启立即恢复");
            // 皮肤下拉（列表为空时占位一项，选择被忽略）
            std::vector<std::string> items = in.petSkins;
            if (items.empty()) {
                items.emplace_back("（未发现皮肤）");
            }
            int selected = 0;
            for (size_t i = 0; i < in.petSkins.size(); ++i) {
                if (in.petSkins[i] == in.petSkinName) {
                    selected = static_cast<int>(i);
                    break;
                }
            }
            // 皮肤名超宽时 EUI-NEO 的 text 不折行、直接画出控件右缘：① 控件宽按最长皮肤名
            // 放大（240 起、上限 420，字段与弹层同步）；② 仍放不下的由 fitControlLabel 截断
            // 显示（末尾 '…'，只改显示串，取值仍是原下标）。字号口径 = EUI-NEO 默认 metrics
            //（字段 16px + kDropdownFieldChrome=62），不受 DevDesk 全局字号缩放影响。
            constexpr float kDdFieldFont = 16.0f;
            const float ddW = petSkinFieldWidth(items, kDdFieldFont);
            // 每项显示串（截断到字段可用宽；实际取值仍是原下标）
            std::vector<std::string> itemLabels;
            itemLabels.reserve(items.size());
            for (const std::string& name : items) {
                itemLabels.push_back(
                    fitControlLabel(name, ddW - kDropdownFieldChrome,
                                    kDdFieldFont));
            }
            // 行几何随生效字号派生：这两行的行 y 与控件高必须在派生池内——85% 档时
            // 卡高会小于字面内容底沿，「显示大小」行会溢出卡外与目录卡叠印。
            const float ddH = roundFs(40.0f);        // 下拉/按钮控件高（100% = 40）
            const float rowLabelH = roundFs(20.0f);  // 行标签盒高（100% = 20）
            const float skinTop =
                40.0f + roundFs(16.0f) + roundFs(8.0f);  // 100% = 64
            const float scaleTop = skinTop + ddH + roundFs(8.0f);  // 100% = 112
            // 皮肤行：标签左、控件右同一基线——标签列 28/72，控件列 104/40（与代理卡同列
            // 口径，卡内左边缘 28）；标签在行内垂直居中（100% 档 y=64，标签中心对齐控件中心）。
            ui.text(id + ".skin.label")
                .position(28.0f, skinTop + (ddH - rowLabelH) * 0.5f)
                .size(72.0f, rowLabelH)
                .text("皮肤")
                .fontSize(fs(12.0f))
                .lineHeight(rowLabelH)
                .color(t.colors.textSecondary)
                .build();
            ui.stack(id + ".skin.ddwrap")
                .position(104.0f, skinTop)
                .size(ddW, ddH)
                .content([&] {
                    // 皮肤下拉是六处下拉里唯一需补 openUp 防护的：≥3 皮肤时弹层向下铺开
                    // 越出 scrollView 视口底被 clip（几何判定同编辑器字号/日志卡）。
                    const bool skinOpenUp = dropdownPopupOverflows(
                        pageY, skinTop, ddH,
                        static_cast<int>(items.size()), screenH);
                    components::dropdown(ui, id + ".skin.dd")
                        .size(ddW, ddH)
                        .theme(compTheme)
                        .items(itemLabels)
                        .selected(selected)
                        .openUp(skinOpenUp)
                        .open(isDropdownOpen(in.openDropdown, Dropdown::PetSkin))
                        .onOpenChange([f = a.setDropdownOpen](bool v) {
                            f(Dropdown::PetSkin, v);
                        })
                        // 换肤按下标回模块：下标与 items 一一对应，截断只影响显示串。
                        .onChange([f = a.changePetSkinAt](int idx) { f(idx); })
                        .build();
                })
                .build();
            // 显示大小行：三档下拉（小 0.75× / 中 1.0× / 大 1.5×，默认中），排法照皮肤行同款。
            // 档位文案单源在 pet 侧（petScalePresetLabels 桥快照），选择经桥切档（写盘 + 热切换）。
            ui.text(id + ".scale.label")
                .position(28.0f, scaleTop + (ddH - rowLabelH) * 0.5f)
                .size(72.0f, rowLabelH)
                .text("大小")
                .fontSize(fs(12.0f))
                .lineHeight(rowLabelH)
                .color(t.colors.textSecondary)
                .build();
            ui.stack(id + ".scale.ddwrap")
                .position(104.0f, scaleTop)
                .size(240.0f, ddH)
                .content([&] {
                    // openUp 防护与皮肤下拉同款几何判定（项数 = petScaleLabels.size()）。
                    const bool scaleOpenUp = dropdownPopupOverflows(
                        pageY, scaleTop, ddH,
                        static_cast<int>(in.petScaleLabels.size()), screenH);
                    components::dropdown(ui, id + ".scale.dd")
                        .size(240.0f, ddH)
                        .theme(compTheme)
                        .items(in.petScaleLabels)
                        .selected(in.petScaleIndex)
                        .openUp(scaleOpenUp)
                        .open(isDropdownOpen(in.openDropdown, Dropdown::PetSize))
                        .onOpenChange([f = a.setDropdownOpen](bool v) {
                            f(Dropdown::PetSize, v);
                        })
                        .onChange([f = a.changePetScaleAt](int idx) { f(idx); })
                        .build();
                })
                .build();
            // 位置重置按钮：回主屏右下角，持久化坐标清回未设置；与皮肤下拉同一行。
            components::button(ui, id + ".reset")
                .position(w - 28.0f - 160.0f, skinTop)
                .size(160.0f, ddH)
                .text("位置重置")
                .fontSize(14.0f)
                .theme(compTheme, /*primary=*/false)
                .textColor(t.colors.textPrimary)
                .border(1.0f, t.colors.borderDefault)
                .onClick([f = a.resetPetPosition] { f(); })
                .build();
            // 显示桌宠：隐藏后的恢复入口（隐藏走右键「隐藏桌宠」菜单项）；同排、居
            //「位置重置」左侧 8px。100% 档卡高保持 168，contentKey 不变。
            components::button(ui, id + ".show")
                .position(w - 28.0f - 160.0f - 168.0f, skinTop)
                .size(160.0f, ddH)
                .text("显示桌宠")
                .fontSize(14.0f)
                .theme(compTheme, /*primary=*/false)
                .textColor(t.colors.textPrimary)
                .border(1.0f, t.colors.borderDefault)
                .onClick([f = a.showPetWindow] { f(); })
                .build();
        })
        .build();
}

// ---------- 卡片 4：Markdown 笔记目录（只读 + 打开目录） ----------
void drawDirectoryCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const float h = args.h;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;

    // .colors() 单独用会让 ButtonStyle 落到默认深色主题；补 .theme(compTheme) 后边框 /
    // 阴影 / 文字 / 半径随主题，.colors() 仍覆盖三态底色。
    const auto compTheme = core::theme::componentTokens(t);
    ui.stack(id)
        .size(w, h)
        .content([&] {
            cardTitle(ui, id, t, 16.0f, w - 56.0f, 22.0f, 22.0f,
                      "Markdown 笔记目录（只读）");

            // 存在性读快照（原每帧 fs::exists(mdDir)），刷新点见 refreshExternalState。
            const std::filesystem::path mdDir =
                ::core::paths::markdownDir();
            const bool exists = in.notesDirExists;
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
            // 「打开目录」按钮：路径在 APPDATA 深处，此前用户只能手拼路径。
            components::button(ui, id + ".open")
                .position(w - 28.0f - dirBtnW, 46.0f)
                .size(dirBtnW, 34.0f)
                .text("打开目录")
                .icon(0xF07C)  // fa-folder-open
                .fontSize(fs(13.0f))
                .theme(compTheme, /*primary=*/false)
                .colors(t.colors.surfaceElevated, t.colors.hover,
                        t.colors.active)
                .textColor(t.colors.textPrimary)
                .radius(t.sizes.radiusSmall)
                .border(1.0f, t.colors.borderDefault)
                .onClick([f = a.openNotesDirectory] { f(); })
                .build();
            // 提示行：14px 盒 + fs(11) 文本的墨迹在 130% 档会越盒，故盒高派生。
            ui.text(id + ".hint")
                .position(28.0f, 86.0f)
                .size(w - 56.0f, roundFs(14.0f))
                .text("修改笔记目录属于后续功能，当前仅展示（可用「打开目录」"
                      "定位后迁移笔记）")
                .fontSize(fs(11.0f))
                .lineHeight(roundFs(14.0f))
                .color(t.colors.textTertiary)
                .build();

            // 数据根忽略规则路径：凭据存在 devdesk.db（WAL 伴生件会复制凭据页），数据根又
            // 要进 git 仓库——必须让用户看得见「哪一份规则在挡 git add .」。本行只展示。
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

// ---------- 卡片 5：Claude 代理（claude 模块关闭时整卡隐藏） ----------
void drawProxyCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const float h = args.h;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;

    // 行几何随生效字号派生（单一来源 helper 见 settings_ui_logic.h，compose 的卡高公式
    // proxyCardHeightFor 与这里同源）。字面锚 y40/62/98 在其上（标题与端口 input 28 不缩放）；
    // 端口标签 lineHeight 保持 28 = 与 28 高输入控件的垂直居中口径，不是行盒。
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
                        .checked(in.cfg.claudeProxyAutoStart)
                        .theme(compTheme)
                        .onChange([f = a.applyProxyAutoStart](bool v) { f(v); })
                        .build();
                })
                .build();

            cardDesc(ui, id, t, 40.0f, w - 56.0f, descH,
                     running
                         ? "本地代理 http://127.0.0.1:" + std::to_string(port) +
                               "（仅本机可访问，无需凭据）"
                         : "本地代理未运行（默认不随应用启动，可在 Claude 页手动启动）");

            // ---- 代理端口行（总设计 §8.5 第 5 条，审计 2026-09-23 [14]）----
            // 数字输入 + Enter/应用按钮提交；代理运行中改端口自动 stop→start
            // 重启（保持 token，只换端口），未运行则下次启动生效
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
                // 审计 [21]：Esc=丢弃端口草稿（回落已应用端口，不再误触发
                // applyProxyPort——旧映射下 Esc 会提交端口并重启代理）
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

            // Claude Code 接管状态：sidecar 是否存在推导，让还原机制对用户可见；上层覆盖警告逐行展示。
            const bool managed = in.takeoverManaged;
            ui.text(id + ".takeover.label")
                .position(28.0f, 98.0f)
                .size(150.0f, takeoverH)
                .text("Claude Code 接管")
                .fontSize(fs(12.0f))
                .lineHeight(takeoverH)
                .color(t.colors.textSecondary)
                .build();
            // 路径读 Claude 运行面内存态（main 启动期注入，即接管实际使用的路径），
            // 不再每帧调 defaultPaths()（内含 fs::current_path / getcwd）。
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
            // 渲染门与卡高计算同源（svc 而非 managed）：未接管但警告非空（接管失败）时
            // 行必须画出来，否则卡高留了位。
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

// ---------- 卡片 6：日志（保留天数 + 级别） ----------
// 行内下拉的弹层用 pageY/screenH 几何估算判断向下展开是否出界，出界则 openUp 向上
//（常量与 dropdown 内部 metrics 一致）。两行统一「标签列 28/84、控件列 124/240、
// 说明列 388」同列口径（常量见 settings_ui_logic.h），行高恒 = 单行 20 + fitControlLabel 兜底。
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

    const auto compTheme = core::theme::componentTokens(t);
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
            // ---- 行 1：日志保留（标签列 + 控件列 + 说明列）----
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

            // ---- 行 2：日志级别：切档即时 core::log::setLevel（DEBUG 起打详录），
            // 重启由 onStart 恢复持久化档 ----
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

// ---------- 卡片 8：日志查看：日志文件 tail + 刷新 ----------
// logTail 是内存快照（读盘在后台线程），本方法只渲染；逐行一个 text，超宽行由
// scrollView clip；刷新按钮只置 dirty 标记，实际重读由 refreshLogTailCache 发意图。
void drawLogViewCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const float h = args.h;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;

    const auto compTheme = core::theme::componentTokens(t);
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
                            .text("（暂无日志内容，或日志文件尚未生成）")
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
