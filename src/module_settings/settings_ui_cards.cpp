// src/module_settings/settings_ui_cards.cpp
// R6（spec 2026-09-27-refactor-incremental-design §5.2）：settings 模块 9 张卡 ——
// 逐字自 settings_module.cpp 搬出（原 9 个私有 draw*Card + 其调用的私有
// cardFrame/fs/nearestIndex/下拉持有者取值）。
//
// 每个可见区域一个自由函数，吃 CardDrawArgs（只读入参 + 回调 + 几何）。
// 元素 event 回调一律按值捕获用到的 std::function（见头文件说明）——元素树比
// 一帧活得久，捕引用会悬垂。
//
// 2026-09-28 变更：用户反馈「设置页面最外层的卡片没删除……底色是白色？不需要
// 啊」→ 删全部 9 张卡的 ui_kit::cardFrame 调用（含其 <id>.bg 矩形/1px 边框/
// panelShadow/圆角），卡内容直接铺 M3-4 主体渐变底（settings.body.bg）；其余
// 节点 id、文案、字号、颜色、几何、控制流逐字保留。
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

} // namespace

// ---------- 卡片 1：外观（主题 + 字号 + 窗口效果） ----------
// zIndex(40)：字号下拉的弹出层要盖过下方卡片（zIndex 只在兄弟间排序，
// 弹层在卡片子树内，需整卡抬层）。**抬层规则**：同一 tab 内带下拉的卡，
// 靠前者必须 z 高于其后**所有**卡 —— 外观卡之后紧邻的编辑器字号卡同为
// 带下拉卡（z30），等值 z 时稳定排序保持 DOM 序 → 后绘的编辑器字号卡会
// 盖住外观卡向下铺开的弹层（回归 1379cc1；EUI-NEO subtreeMaxZIndex 逃逸
// 只解决「高 z 盖低 z」，救不了等值碰撞）。40 = 最前卡档位，后续带下拉卡
// 一律 ≤30。
void drawAppearanceCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const float h = args.h;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;

    const auto compTheme = core::theme::componentTokens(t);
    ui.stack(id)
        .size(w, h)
        .zIndex(40)
        .content([&] {
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
                    // 类 6(d)（probes 证伪门禁落点）：窄窗可用宽不足时截断
                    // 而非溢出（完整文案在宽窗可见）
                    .clip()
                    .build();
            }
        })
        .build();
}

// ---------- 编辑器字号卡（Task B：自日志卡行 2 迁入「通用」tab） ----------
// spec §8.5「编辑器字体大小」（spec 缩水 B）：档位写 core::theme::
// editorFontSize 通道，markdown 下一帧生效；默认 15px = fontSizeBody
// token 基准。pageY/screenH：下拉弹层 openUp 几何估算（同款日志卡）
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
                    const bool efontOpenUp = dropdownPopupOverflows(
                        pageY, kEditorFontDdOffsetY, kLogEditorFontHeight,
                        kEditorFontPresetCount, screenH);
                    components::dropdown(ui, id + ".dd")
                        .size(150.0f, kLogEditorFontHeight)
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
                .size(w - 218.0f, 18.0f)
                .text("Markdown 编辑器正文字号；默认 15px（标准）。切换即时生效。")
                .fontSize(fs(12.0f))
                .lineHeight(18.0f)
                .color(t.colors.textTertiary)
                .build();
        })
        .build();
}

// ---------- 卡片 2：模块管理（插拔方案 A §5.4） ----------
// 三个工具模块各一行开关；关闭 = 下次启动不注册进 Router（不执行代码、
// 不显示侧边栏入口）。home/settings 恒启用，不提供开关
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
    };
    constexpr int kToggleCount = static_cast<int>(sizeof(kToggleable) / sizeof(kToggleable[0]));
    const auto compTheme = core::theme::componentTokens(t);
    ui.stack(id)
        .size(w, h)
        .content([&] {
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

// ---------- 卡片 3：桌宠（M2 多皮肤 UI，设计 §4.5/§4.6） ----------
// 皮肤下拉（discoverSkins 发现列表）+ 位置重置按钮；两者即时生效
//（换肤热切换/位置直接挪窗），与开关不同（开关重启生效）
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

    // 审计 THS-01：皮肤清单（discoverSkins = 目录遍历 + 每皮肤 pet.json
    // stat）与当前皮肤名（读 pet_config.json）已移出渲染路径——快照由
    // refreshExternalState 装载（原为「首帧惰性加载」，仍在渲染路径上），
    // 这里只读入参；审计 [5][36] 的 pet 侧只读桥调用点随之收在模块文件
    const auto compTheme = core::theme::componentTokens(t);
    // 跨模块剩余审计修（2026-09-28）：本卡抬到 z40（仿外观卡）——皮肤下拉
    // 弹层向下铺开时越出卡底沿盖住后续目录卡，z 逃逸语义与外观/日志卡一致
    ui.stack(id)
        .size(w, h)
        .zIndex(40)
        .content([&] {
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
            ui.stack(id + ".skin.ddwrap")
                .position(28.0f, 64.0f)
                .size(240.0f, 40.0f)
                .content([&] {
                    // 跨模块剩余审计修：皮肤下拉是六处下拉里唯一无 openUp
                    // 防护的——≥3 皮肤时弹层（3×34+12=114）向下铺开越出
                    // scrollView 视口底被 clip 裁掉；与编辑器字号/日志卡同款
                    // 几何判定（卡顶 + 字段偏移 + 弹层高 vs 屏高）
                    const bool skinOpenUp = dropdownPopupOverflows(
                        pageY, 64.0f, 40.0f,
                        static_cast<int>(items.size()), screenH);
                    components::dropdown(ui, id + ".skin.dd")
                        .size(240.0f, 40.0f)
                        .theme(compTheme)
                        .items(items)
                        .selected(selected)
                        .openUp(skinOpenUp)
                        .open(isDropdownOpen(in.openDropdown, Dropdown::PetSkin))
                        .onOpenChange([f = a.setDropdownOpen](bool v) {
                            f(Dropdown::PetSkin, v);
                        })
                        // 换肤按下标回模块（越界守卫在模块侧，读的是同一份
                        // 皮肤清单快照）
                        .onChange([f = a.changePetSkinAt](int idx) { f(idx); })
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
                .onClick([f = a.resetPetPosition] { f(); })
                .build();
            // 显示桌宠（PAB-13）：隐藏后的恢复入口（隐藏走右键「隐藏桌宠」
            // 菜单项，之后本按钮即可重新开窗）。同排 y=64、居「位置重置」
            // 左侧 8px；卡高保持 120（内容底沿 104 不变，故 contentKey 不变）
            components::button(ui, id + ".show")
                .position(w - 28.0f - 160.0f - 168.0f, 64.0f)
                .size(160.0f, 40.0f)
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

    // 跨模块剩余审计修（2026-09-28）：.colors() 单独用 = ButtonStyle 落到
    // 默认深色主题（浅色模式蓝描边 + 重阴影）——补 .theme(compTheme) 后
    // 边框/阴影/文字/半径随主题，.colors() 仍覆盖三态底色
    const auto compTheme = core::theme::componentTokens(t);
    ui.stack(id)
        .size(w, h)
        .content([&] {
            ui.text(id + ".title")
                .position(28.0f, 16.0f)
                .size(w - 56.0f, 22.0f)
                .text("Markdown 笔记目录（只读）")
                .fontSize(t.sizes.fontSizeLarge)
                .lineHeight(22.0f)
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();

            // 审计 THS-01：存在性读快照（原每帧 fs::exists(mdDir)），
            // 刷新点见 refreshExternalState
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
            // 「打开目录」按钮（审计 [15]：路径在 APPDATA 深处，此前用户
            // 只能手拼路径；改目录本身仍属后续功能，登记 KNOWN_LIMITATIONS）
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

// ---------- 卡片 5：Claude 代理 token（claude 模块关闭时整卡隐藏） ----------
void drawProxyCard(const CardDrawArgs& args) {
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
            auto* svc = in.proxy;
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
                        .checked(in.cfg.claudeProxyAutoStart)
                        .theme(compTheme)
                        .onChange([f = a.applyProxyAutoStart](bool v) { f(v); })
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
            const std::string portText = in.proxyPortDraft.empty()
                ? std::to_string(in.cfg.proxyPort)
                : in.proxyPortDraft;
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
                .size(w - 282.0f - 28.0f, 18.0f)
                .text("改后回车或点「应用」；代理运行中自动重启")
                .fontSize(fs(12.0f))
                .lineHeight(18.0f)
                .color(t.colors.textTertiary)
                .build();

            // Claude Code 接管状态（Phase D，§6.3）：sidecar 是否存在推导，
            // 让还原机制对用户可见；上层覆盖警告（§5.3 检测命中时）逐行展示
            const bool managed = in.takeoverManaged;  // 审计 THS-01：读快照
            ui.text(id + ".takeover.label")
                .position(28.0f, 98.0f)
                .size(150.0f, 18.0f)
                .text("Claude Code 接管")
                .fontSize(fs(12.0f))
                .lineHeight(18.0f)
                .color(t.colors.textSecondary)
                .build();
            // 审计 THS-01：路径读 Claude 运行面内存态（main 启动期注入的
            // defaultPaths()，正是接管实际使用的路径）——不再每帧调
            // defaultPaths()（内含 fs::current_path / getcwd）
            const std::string ccPath =
                svc ? svc->settingsFilePath() : std::string();
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
            // 审计 CPX-02：渲染门与卡高计算同源（svc 而非 managed）——
            // 未接管但警告非空（接管失败）时行必须画出来，否则卡高留了位
            if (svc) {
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

// ---------- 卡片 6：日志（保留天数 + 级别，日志 tab） ----------
// 行内下拉的弹层用 pageY/screenH 几何估算判断向下展开是否出界，出界则
// openUp 向上（修复菜单被窗体裁剪）。spec 缩水 B 的编辑器字号 Task B 起
// 迁到「通用」tab 独立卡（不同 tab 的卡不同屏，原「末卡 z 逃逸宿主」
// 理由失效）。pageY/screenH：弹层几何用 settings_ui_logic.h 的 kDropdown* /
// kLogDropdown* 常量估算（与 dropdown 内部 metrics 一致）
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
    ui.stack(id)
        .size(w, h)
        .zIndex(30)
        .content([&] {
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
                    const bool openUp = dropdownPopupOverflows(
                        pageY, kLogDropdownOffsetY, kLogDropdownHeight,
                        kRetentionPresetCount, screenH);
                    components::dropdown(ui, id + ".dd")
                        .size(240.0f, kLogDropdownHeight)
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
                    const bool levelOpenUp = dropdownPopupOverflows(
                        pageY, kLogLevelOffsetY, kLogLevelHeight,
                        kLogLevelPresetCount, screenH);
                    components::dropdown(ui, id + ".level.dd")
                        .size(150.0f, kLogLevelHeight)
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

// ---------- 卡片 8：日志查看（Task B，日志 tab）：日志文件 tail + 刷新 ----------
// logTail 是内存快照（读盘见 readLogTailLines，跑在后台线程，见审计 H2），
// 本方法只渲染字符串。逐行一个 text（固定行高，超宽行由 scrollView clip
// 右侧裁切）；刷新按钮只置 dirty 标记，实际重读由 refreshLogTailCache 发意图
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
