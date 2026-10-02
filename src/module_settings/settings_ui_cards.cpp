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
// panelShadow/圆角）；其余节点 id、文案、字号、颜色、几何、控制流逐字保留。
// 同日第二轮（用户真机反馈「设置页面最外层的卡片还没删除啊」）：连页面主体
// 的 M3-4 渐变底板（settings_module.cpp 的 settings.body.gradient/bg）一起删，
// 故卡内容现在直接铺**页面底**——关档由主窗 clearColor 当底、半透/磨砂档露出
// 窗口背景（frost spec 2026-09-22 §53 口径）。
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
    // 行几何随生效字号派生（2026-09-30 spec 2026-09-30-batch-fix-design §2.3
    // 本期修；roundFs 口径见 settings_ui_logic.h）：标题盒/描述行/标签行/降级
    // 提示行的盒高与 lineHeight 从 100% 字面改为派生，后续行的 y 沿派生盒底
    // 累加（spec §2.7「优先把链改派生而非钳制」）。100% 档 roundFs(x)==x，全部
    // 值回到旧字面（零回归锚）。字面锚：18 = 标题顶（卡顶 padding，不缩放）；
    // 40 = 下拉控件高（组件字面 chrome，字段文案不走 token，不随缩放）
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
            ui.text(id + ".title")
                .position(28.0f, 18.0f)
                .size(w - 56.0f, titleH)
                .text("外观")
                .fontSize(t.sizes.fontSizeLarge)
                .lineHeight(titleH)
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            ui.text(id + ".desc")
                .position(28.0f, descY)
                .size(w - 56.0f, descH)
                .text("主题与窗口底色即时生效；字号缩放全局生效")
                .fontSize(fs(12.0f))
                .lineHeight(descH)
                .color(t.colors.textTertiary)
                .build();

            // 左：主题切换
            ui.text(id + ".theme.label")
                .position(28.0f, labelY)
                .size(200.0f, labelH)
                .text("主题")
                .fontSize(fs(12.0f))
                .lineHeight(labelH)
                .color(t.colors.textSecondary)
                .build();
            // 主题模式下拉（spec 缩水 A）：跟随系统/浅色/深色三档，替代
            // 原「深色/浅色」双按钮（原按钮只能表达显式档，无法表达 auto）。
            // 右缘 28+240=268，窄窗自适应后 fontX≥324，不碰撞（见
            // settings_effect_dropdown_fits_min_window）
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

            // 右：字号缩放 + 右二：窗口效果三档（磨砂 Phase D），即时生效。
            // 终验复审：固定 x=380/680 在 1200 最小窗（页面宽 940、卡内宽
            // 860）下效果下拉右缘到 920，超出卡片 60px——右缘自适应：
            // 宽窗保持原位，窄窗右对齐收进卡内（列宽 240、列距 28、右缘留 28）
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

            // 右二：窗口效果三档（磨砂 Phase D），即时生效（effectX 见上）
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
                // ⚠ 字符换 FA：图标/文字拆两个元素（icon 字体通道）
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
                    .size(w - effectX - 16.0f - 28.0f, hintH)
                    .text(degraded)
                    .fontSize(fs(11.0f))
                    .lineHeight(hintH)
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
            // 下拉 y/高随生效字号派生（2026-09-30 merge-review R1 本期修，
            // 单一来源 logFieldHeight / editorFontDdOffsetY，见
            // settings_ui_logic.h）；100% 档与旧字面 42/36 逐值相等
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
            // 描述行盒随生效字号派生（2026-09-30 spec §2.3 本期修）；y=48 与
            // 左侧下拉（字面控件）并排、下方无行，链上无后续行需挪
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

// ---------- 卡片 2：模块管理（插拔方案 A §5.4） ----------
// 四个工具模块（markdown/todo/claude_proxy/agent）+ 桌宠各一行开关；关闭 =
// 下次启动不注册进 Router（不执行代码、
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
        {kModuleIdAgent, "智能体",
         "任务列表 + 手动触发运行（依赖 Claude 代理提供模型通道）"},
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
                .size(w - 56.0f, roundFs(16.0f))
                .text("开关即时生效：关闭后该模块代码不再执行，侧边栏入口与"
                      "页面当帧更新")
                .fontSize(fs(12.0f))
                .lineHeight(roundFs(16.0f))
                .color(t.colors.textTertiary)
                .build();

            // 开关行行几何随生效字号派生（2026-09-30 spec §2.3 本期修）：
            // 名称/描述行盒、行内 y 偏移与行距（100% 档 = 56，零回归锚）；
            // 行 1 顶 y=68 锚在字面说明行下方（说明行 y40 字面、盒派生后
            // 130% 档底 61 仍 < 68，链上无碰撞）。开关控件（32 高字面组件）
            // 随行顶走，相对偏移 -2 保持字面
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

// ---------- 卡片 3：桌宠（M2 多皮肤 UI，设计 §4.5/§4.6） ----------
// 皮肤下拉（discoverSkins 发现列表）+ 位置重置按钮；两者即时生效
//（换肤热切换/位置直接挪窗）；模块开关自活门控起也即时生效（app::toggle 桥）
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
                .size(w - 56.0f, roundFs(16.0f))
                .text("换肤与位置重置即时生效；关闭桌宠立即隐藏、重新开启立即恢复")
                .fontSize(fs(12.0f))
                .lineHeight(roundFs(16.0f))
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
            // ③ 2026-09-29 真机反馈「桌宠设置的下拉框不应该换行」。EUI-NEO 的
            // text 默认 wrap=false（core/dsl.h:149）：皮肤名超宽时**不折行**，
            // 而是画到控件右边缘之外（真机复现：25 字符的皮肤名在 240 宽下拉里
            // 文本压过 chevron 跑出控件），读起来就是「换行/串行」。两条一起上：
            //   1) 控件宽度按最长皮肤名放大（240 起、上限 420）——常见长名直接
            //      放得下，字段与弹层同步变宽（components/dropdown.h 里弹层宽 =
            //      字段宽），选项也一并能读全；
            //   2) 仍放不下的名字由 fitControlLabel 截断显示（末尾 '…'），
            //      **只改显示串，不改取值**（回传的仍是原始下标）。
            // 字号口径 = EUI-NEO 默认 metrics（components/theme.h）：
            // 字段文案 typography.body=16、弹层项 typography.option=15。这两个
            // 值不受 DevDesk 全局字号缩放影响（core::theme::fontScale 只缩放
            // 自家 token，见 core/theme.cpp:115-120）。截断按**字段**口径
            //（16px + kDropdownFieldChrome=62）：字段文案框比弹层项框窄，
            // 按窄的那个截，两边都放得下。
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
            // 行几何随生效字号派生（2026-09-30 merge-review R1 本期修）：皮肤/
            // 显示大小两行的行 y 与控件高原为字面（64/74/112/122/40），不在
            // roundFs 派生池 —— 85% 字号档（fontScale=0.85）卡高
            // roundFs(168)=143 < 字面内容底沿 152，「显示大小」行溢出卡外 9px
            // 与目录卡叠印。派生口径与外观卡同款：行顶沿说明行**派生盒底**
            // 累加（说明行 y=40 字面锚、盒 roundFs(16)），控件高随字号派生，
            // 标签在行内垂直居中；100% 档 roundFs(x)==x，全部回到旧字面
            // 64/74/112/122/40（零回归锚）
            const float ddH = roundFs(40.0f);        // 下拉/按钮控件高（100% = 40）
            const float rowLabelH = roundFs(20.0f);  // 行标签盒高（100% = 20）
            const float skinTop =
                40.0f + roundFs(16.0f) + roundFs(8.0f);  // 100% = 64
            const float scaleTop = skinTop + ddH + roundFs(8.0f);  // 100% = 112
            // 皮肤行（PET-ROW，2026-09-29 真机反馈「设置-模块-桌宠 下拉框
            // 不应该另起一行」）：标签左、控件右、**同一基线**——标签列
            // 28/72，控件列 104/40（与代理卡「代理端口 [输入]」同款列口径，
            // 卡内左边缘 28 对齐）。改前只有控件、无标签，控件独占一行，读起来
            // 就是「下拉另起一行」。行顶 skinTop（100% 档 = 64，控件高 40）——
            // 标签 20 高需在行内垂直居中 → y=74（中心 84 == 控件中心），皮肤
            // 下拉的 openUp 几何（dropdownPopupOverflows 的字段偏移）随行顶走
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
                    // 跨模块剩余审计修：皮肤下拉是六处下拉里唯一无 openUp
                    // 防护的——≥3 皮肤时弹层（3×34+12=114）向下铺开越出
                    // scrollView 视口底被 clip 裁掉；与编辑器字号/日志卡同款
                    // 几何判定（卡顶 + 字段偏移 + 弹层高 vs 屏高）
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
                        // 换肤按下标回模块（越界守卫在模块侧，读的是同一份
                        // 皮肤清单快照）——下标与 items 一一对应，截断只影响
                        // 显示串
                        .onChange([f = a.changePetSkinAt](int idx) { f(idx); })
                        .build();
                })
                .build();
            // 显示大小行（2026-09-30 用户需求「桌宠显示大小」）：三档下拉
            //（小 0.75× / 中 1.0× / 大 1.5×，默认中）。排法照皮肤行同款
            // 「标签列 28/72 + 控件列 104」同基线口径，行顶 scaleTop =
            // skinTop + ddH + roundFs(8)（100% 档：皮肤行控件底 104 + 8 = 本行
            // 顶 112；卡高 168 见 settings_ui_logic.h）。行几何随字号派生，
            // 2026-09-30 merge-review R1 本期修（见上 ddH/skinTop 注）。
            // 文案/档位单源在 pet 侧（petScalePresetLabels 桥快照），选择
            // 经桥切档：写盘 + 热切换（窗口关旧开新，尺寸同步缩放，点单位）
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
                    // openUp 防护与皮肤下拉同款几何判定（卡顶 + 字段偏移 +
                    // 弹层高 vs 屏高；项数 = petScaleLabels.size()，当前 3×34+12）
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
            // 位置重置按钮（回主屏右下角，持久化坐标清回未设置）——随皮肤行
            // 顶/控件高派生走（100% 档 = 64/40，与皮肤下拉同一行）
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
            // 显示桌宠（PAB-13）：隐藏后的恢复入口（隐藏走右键「隐藏桌宠」
            // 菜单项，之后本按钮即可重新开窗）。同排、居「位置重置」
            // 左侧 8px；100% 档卡高保持 168（内容底沿 152 不变，故 contentKey
            // 不变；行几何 2026-09-30 R1 起随字号派生，见上 ddH/skinTop 注）
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
            // 提示行盒随生效字号派生（2026-09-30 spec §2.3 本期修）——本行
            // 14px 盒 + fs(11) 文本是 130% 档实测越盒点（14.3px 墨迹 > 14 盒）；
            // y=86 字面（上方路径行是字面控件行），下方无行
            ui.text(id + ".hint")
                .position(28.0f, 86.0f)
                .size(w - 56.0f, roundFs(14.0f))
                .text("修改笔记目录属于后续功能，当前仅展示（可用「打开目录」"
                      "定位后迁移笔记）")
                .fontSize(fs(11.0f))
                .lineHeight(roundFs(14.0f))
                .color(t.colors.textTertiary)
                .build();

            // 数据根忽略规则路径（2026-10-02 存储统一 §6.3 硬约束 1 的 UI 面）：
            // 凭据现在存在 devdesk.db 里（WAL 伴生件会把凭据页复制进去），数据根
            // 又要进 git 仓库——必须让用户看得见「哪一份规则在挡 `git add .`」。
            // 与应用启动时写入的同一个路径（core::paths::dataRootGitignoreFile）；
            // 本行只展示，不提供编辑（内容由应用维护，用户要加规则直接编辑该文件）
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

// ---------- 卡片 5：Claude 代理 token（claude 模块关闭时整卡隐藏） ----------
void drawProxyCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const float h = args.h;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;

    // 行几何随生效字号派生（2026-09-30 spec §2.3 本期修；roundFs 与接管/警告
    // 行的单一来源 helper 见 settings_ui_logic.h，compose 的卡高公式
    // proxyCardHeightFor 与这里同源）。字面锚：y40/62/98 —— 上方标题与端口
    // 控件行（input 28 字面组件）不缩放，锚在其下；警告行 y 从接管行派生盒底
    // 起算（100% 档 = 118 与旧字面一致）。端口标签 lineHeight 保持 28 字面：
    // 它是与 28 高输入控件的垂直居中对齐口径，不是行盒
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
                .size(w - 56.0f, descH)
                .text(running
                      ? "本地代理 http://127.0.0.1:" + std::to_string(port) +
                            "（仅本机可访问，无需凭据）"
                      : "本地代理未运行（默认不随应用启动，可在 Claude 页手动启动）")
                .fontSize(fs(12.0f))
                .lineHeight(descH)
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

            // Claude Code 接管状态（Phase D，§6.3）：sidecar 是否存在推导，
            // 让还原机制对用户可见；上层覆盖警告（§5.3 检测命中时）逐行展示
            const bool managed = in.takeoverManaged;  // 审计 THS-01：读快照
            ui.text(id + ".takeover.label")
                .position(28.0f, 98.0f)
                .size(150.0f, takeoverH)
                .text("Claude Code 接管")
                .fontSize(fs(12.0f))
                .lineHeight(takeoverH)
                .color(t.colors.textSecondary)
                .build();
            // 审计 THS-01：路径读 Claude 运行面内存态（main 启动期注入的
            // defaultPaths()，正是接管实际使用的路径）——不再每帧调
            // defaultPaths()（内含 fs::current_path / getcwd）
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
            // 审计 CPX-02：渲染门与卡高计算同源（svc 而非 managed）——
            // 未接管但警告非空（接管失败）时行必须画出来，否则卡高留了位
            if (svc) {
                // F2：按值快照（写者可能是 agent 线程），不是活引用
                const std::vector<std::string> warns = svc->lastSyncWarnings();
                // ponytail: 最多展示 2 条（卡片高度约束），更多警告在
                // 日志（LOG_WARN 全量）可查；⚠ 换 FA（图标/文字拆两个元素）
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

// ---------- 卡片 6：日志（保留天数 + 级别，日志 tab） ----------
// 行内下拉的弹层用 pageY/screenH 几何估算判断向下展开是否出界，出界则
// openUp 向上（修复菜单被窗体裁剪）。spec 缩水 B 的编辑器字号 Task B 起
// 迁到「通用」tab 独立卡（不同 tab 的卡不同屏，原「末卡 z 逃逸宿主」
// 理由失效）。pageY/screenH：弹层几何用 settings_ui_logic.h 的 kDropdown* /
// kLogDropdown* 常量估算（与 dropdown 内部 metrics 一致）
//
// 2026-09-29 用户真机反馈「日志和日志级别这 2 个条件 错位了啊 日志显示 2 行」：
// 改前行 1（保留天数）只有控件、**没有标签**，控件左边缘 = 卡内 padding 28；
// 行 2（日志级别）有 84 宽标签 + 控件，控件左边缘 = 124 —— 两行控件左边缘差
// 96px、行 1 还缺一列，视觉上就是「错位 + 像多出来一行」。现在两行统一
// 「标签列 28/84、控件列 124/240、说明列 388」同列口径（常量见
// settings_ui_logic.h 的 kLogLabel* / kLogField* / kLogRow*LabelY），行 1 补上
// 缺失的「日志保留」标签；标签行高恒 = 单行高度（20），叠加 fitControlLabel
// 兜底（长档位文案也只画一行）。
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
                    // 字段绝对底边 = 卡顶 + 卡内 y 偏移 + 字段高
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

            // ---- 行 2：日志级别（spec §8.5「日志级别」，spec 缩水 D；
            // Task B：原行 2 编辑器字号迁至「通用」tab 独立卡后上移）：
            // 切档即时 core::log::setLevel（DEBUG 起打 LOG_DEBUG 详录），
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
