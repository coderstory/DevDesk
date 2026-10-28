// markdown_module_internal.h —— MarkdownModule 类定义 + 模块内共享助手（仅本模块内包含）。
// 拆分第一步（抽编译单元）产物：markdown_module.cpp 原文件在此聚齐「共享常量/助手 +
// 类定义」，四个成员函数块（文件树/编辑器/预览/菜单轮询）在各自 md_module_*.cpp
// 出行定义；对外仍只有 create_markdown_module() 与 markdown_module_test_seam.h，
// 本头文件不许被模块外包含（消费方零改动的边界）。
#pragma once
// 键盘流纯函数在 md_keyboard.*，语法高亮在 md_highlight.*。

#include "core/database.h"
#include "core/logging.h"
#include "core/markdown_notes_source.h"
#include "core/module_interface.h"
#include "core/service_locator.h"
#include "core/paths.h"
#include "core/router.h"
#include "core/theme.h"
#include "core/ui_geometry.h"
#include "core/component_theme.h"
#include "core/event_bus.h"
#include "core/time.h"
#include <ui_kit/button.h>
#include <ui_kit/card.h>
#include <ui_kit/dialog_form.h>
#include <ui_kit/metrics.h>
#include <ui_kit/toast.h>
#include <ui_kit/wtab.h>
#include "markdown_service.h"
#include "markdown_module_test_seam.h"
#include "markdown_tools.h"
#include "md_block_model.h"
#include "md_block_interact.h"
#include "md_highlight.h"
#include "md_ids.h"
#include "md_keyboard.h"
#include "md_line_scanner.h"
#include "md_live_edit.h"
#include "md_preview_highlight.h"
#include "md_note_git.h"
#include "md_sync_map.h"
#include "md_tree.h"
#include "md_view_state.h"

#include <eui/dsl.h>

#include <components/button.h>
#include <components/contextmenu.h>
#include <components/dialog.h>
#include <components/dropdown.h>
#include <components/input.h>
#include <components/markdown.h>
#include <components/mousearea.h>
#include <components/scrollview.h>
#include <components/segmented.h>
#include <components/switch.h>
#include <components/theme.h>
#include <components/toast.h>
#include <components/tooltip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <ratio>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace module::markdown {

namespace fs = std::filesystem;

// 树行几何常量；改动须同步 tests/test_modules/test_markdown.cpp 的数值断言。
// UAT2 F批（目录树大气）：行高 32→36、条目字号 13→14（专用常量，
// 不改 fontSizeSmall token——它波及全 UI）。
// W3.5批6 H12-1：箭头 12px（三级文字色，S2 列表规范）
constexpr float kTreeRowHeight = 36.0f;
constexpr float kTreeArrowFontSize = 12.0f;
constexpr float kTreeFontSize = 14.0f;

// UAT3 D-R3-3（目录树折叠重做）：面板宽动画与收起形态常量。
constexpr float kTreePanelWidth = 224.0f;         // 展开宽（S4.4 规格 224，收窄最小 200）
constexpr float kTreePanelAnimK = 0.18f;          // 每帧向目标靠拢的固定 lerp 系数
constexpr float kTreePanelSnapEps = 1.0f;         // 距目标 <1px 视为收敛，直接钉住
constexpr float kTreePanelContentMinW = 220.0f;   // 内容回画门槛（动画中只画面板壳）
// UAT6 MDA（方案 A 边界把手）：一颗把手管两端——树展开时把手位于树右缘垂直居中，
// 树收起后同颗把手出现在编辑区左缘同一垂直位置；绿底白箭头，箭头方向随两态翻转。
// 取代旧 drawTreeExpandFab 浮钮 + md.tree.collapse 面板头折叠钮。
constexpr float kTreeHandleW = 24.0f;          // 把手宽
constexpr float kTreeHandleH = 48.0f;          // 把手高
constexpr float kTreeHandleArrowSize = 14.0f;  // 箭头字号

// UAT6 字号联动：markdown 的两条 token 系外旁路在此收口进全局缩放档
//（core::theme::fontScale；token 系字号已由 theme 的 applyFontScale 统一乘）。
// 编辑器不再直读 core::theme::editorFontSize()、预览不再裸构造 MarkdownStyle，
// 一律经下方两个函数取生效值；settings 系仍写/显示基础值，语义不变。

// 编辑器生效字号：编辑器 input、实时块贴合高度、同步滚动换算全部经本函数取值
// ⇒ 行几何（lineHeight = 字号 × kInputLineHeightFactor，与 EUI input 内部 ×1.2
// 同系数）随档位整链自洽；任何一处绕开本函数直读基础值会与渲染行高错位。
inline float mdEditorFontSize() {
    return core::theme::editorFontSize() * core::theme::fontScale();
}

// 预览生效样式：字号族（body/h1/h2/h3/code + bodyLineHeight）× 档位。
// blockGap 必须同乘——它同时是 syncMap 缓存键项（rev/width/gap/块数），不乘则
// 切档时键不变、块高备忘不重建，新字号配旧几何会裁字。标题/代码块行高在 EUI
// 渲染器 = 字号+6 ⇒ 随字号自洽；bodyLineHeight 是唯一独立行高字段，与 bodySize
// 同乘防行叠。间距/缩进/圆角不动（与 token 系同口径：只缩字号与行盒）。
inline components::MarkdownStyle scaledPreviewStyle(
    const components::theme::ThemeColorTokens& componentTheme) {
    components::MarkdownStyle style(componentTheme);
    const float k = core::theme::fontScale();
    style.bodySize *= k;
    style.bodyLineHeight *= k;
    style.h1Size *= k;
    style.h2Size *= k;
    style.h3Size *= k;
    style.codeSize *= k;
    style.blockGap *= k;
    return style;
}

// 单行树条目：整行包一层 stack —— position() 只在 stack 内生效，流式子元素会被静默忽略。
// S2.5：选中态 = primarySoftBg 底 + 行左缘 3px 绿竖线 + primary 文字 / 图标。
// 搜索命中时高亮关键词（§3.2.4 补充项）。
inline void drawFileTreeItem(eui::Ui& ui,
                      const std::string& id,
                      float width,
                      const core::theme::Tokens& t,
                      unsigned int icon,
                      const std::string& name,
                      bool active,
                      float indent,
                      std::function<void()> onClick,
                      std::function<void(const eui::PointerEvent&, const eui::Rect&)>
                          onContextMenu = {},
                      const std::string& highlight = {}) {
    ui.stack(id)
        .size(width, kTreeRowHeight)
        .content([&] {
            // 2026-10-25 用户拍板「目录树 item 样式错乱」收口：未选行不再画 cardSurface
            // 白卡 chip（浮在共面底上呈白点列），改侧栏同款无底色行 + hover；选中仍
            // S2.5 primarySoftBg + 3px 绿线
            ui.rect(id + ".row")
                .size(width, kTreeRowHeight)
                .color(active ? t.colors.primarySoftBg
                              : eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                .hoverColor(ui_kit::cardSurface(t))  // 灰面板上行悬停提亮（面板=hover 令牌）
                .radius(t.sizes.radiusSmall)
                .onClick(std::move(onClick))
                .onContextMenu(std::move(onContextMenu))
                .build();
            // 选中态左 3px 绿竖线（S2.5 selectionBar）
            if (active) {
                ui.rect(id + ".accent")
                    .position(0.0f, 0.0f)
                    .size(3.0f, kTreeRowHeight)
                    .color(t.colors.primary)
                    .build();
            }
            // 文件行列位对齐目录行（2026-10-25 错乱收口）：icon x=20 / name x=44
            //（原 0/24，与目录 ficon 20 / name 44 错开 20px，同层文件名偏左）
            ui.text(id + ".icon")
                .position(20.0f + indent, 0.0f)
                .size(16.0f, kTreeRowHeight)
                .icon(icon)
                .fontSize(core::theme::scaled(kTreeFontSize))
                .lineHeight(kTreeRowHeight)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(active ? t.colors.primary : t.colors.textTertiary)
                .build();
            // 文件名：搜索命中时高亮关键词段（wechat.primary 着色）
            auto nameBuilder = ui.text(id + ".name")
                .position(44.0f + indent, 0.0f)
                .size(std::max(0.0f, width - 52.0f - indent), kTreeRowHeight)
                .text(name)
                .fontSize(core::theme::scaled(kTreeFontSize))
                .lineHeight(kTreeRowHeight)
                .verticalAlign(eui::VerticalAlign::Center)
                // 选中/未选中同色（深字）=与侧栏一致；S2.5「primary 文字」冲突待用户裁决
                .color(t.colors.textPrimary)
                .fontWeight(active ? 700 : 400);
            if (!highlight.empty()) {
                std::vector<core::TextRun> runs;
                size_t pos = 0;
                while (pos < name.size()) {
                    size_t hit = name.find(highlight, pos);
                    if (hit == std::string::npos) break;
                    if (hit > pos) {
                        runs.push_back({static_cast<int>(pos),
                                        static_cast<int>(hit),
                                        t.colors.textPrimary});
                    }
                    runs.push_back({static_cast<int>(hit),
                                    static_cast<int>(hit + highlight.size()),
                                    t.colors.primary});
                    pos = hit + highlight.size();
                }
                if (!runs.empty() && pos < name.size()) {
                    runs.push_back({static_cast<int>(pos),
                                    static_cast<int>(name.size()),
                                    t.colors.textPrimary});
                }
                if (!runs.empty()) {
                    nameBuilder.runs(std::move(runs));
                }
            }
            nameBuilder.build();
        })
        .build();
}

inline std::string noteDisplayName(const NoteMeta& n) {
    return n.relativePath.filename().string();
}

inline std::string noteDisplayPath(const NoteMeta& n) {
    return n.relativePath.generic_string();
}


// 四态视图枚举。值 = 视图状态持久化口径（非 segmented 下标）
enum class ViewMode { Edit = 0, Split = 1, Preview = 2, Live = 3 };

constexpr ViewMode kModeOrder[] = {ViewMode::Edit, ViewMode::Live,
                                   ViewMode::Split, ViewMode::Preview};
constexpr int kModeCount = 4;

inline int modeOrderIndex(ViewMode m) {
    for (int i = 0; i < kModeCount; ++i) {
        if (kModeOrder[i] == m) return i;
    }
    return 0;
}

inline ViewMode modeFromOrderIndex(int index) {
    return (index >= 0 && index < kModeCount) ? kModeOrder[index] : ViewMode::Edit;
}

inline ViewMode nextMode(ViewMode m) {
    return kModeOrder[(modeOrderIndex(m) + 1) % kModeCount];
}

// [[maybe_unused]] 不可删：本 TU 无引用，clang -Wunused-const-variable 会报。
[[maybe_unused]] constexpr float kDefaultEditorFontSize = 15.0f;
constexpr float kTextColumnInsetX = 8.0f;
constexpr float kPreviewColumnWidth = 760.0f;   // 仅预览态的居中阅读列宽

// 实时态单输入（活动块）：全局只有一个 input，切块换文本 + 搬位置，焦点永不迁移
//（每块一个 input 会在拆/并块瞬间丢焦点）。id 不得落在 releaseStateScope 释放的前缀下。
constexpr const char* kLiveInputId = "md.live.input";
constexpr const char* kLiveInputHitId = "md.live.input.hit";  // 聚焦只发 .hit
constexpr float kLiveInputInset = 10.0f;   // 与编辑态 input 同款内边距
constexpr float kLiveActiveFrameAlpha = 0.6f;
// 高度公式的测量输入必须与 input 内部同源：同字体族、同字号、同文本宽。
constexpr const char* kLiveInputFontFamily = "Microsoft YaHei";

inline std::string formatSaveTime(std::time_t t) {
    if (t <= 0) return {};
    std::tm tm{};
    ::core::time::localTime(t, &tm);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

// 粗略字宽估算（chip 布局用）：ASCII ≈ 0.55×字号，非 ASCII ≈ 字号/3
inline float estimateTextWidth(const std::string& s, float fontSize) {
    float w = 0.0f;
    for (unsigned char ch : s) {
        w += (ch < 0x80) ? fontSize * 0.55f : fontSize / 3.0f;
    }
    return w;
}

// 指针坐标是窗口坐标，浮层构建在页面子树内 → 经 menuAnchorForPointer 统一换算。
inline core::MenuAnchor menuAnchorForPointerInPage(const eui::PointerEvent& ev,
                                            float scrollOffsetY = 0.0f) {
    const ::core::Router* router = ::core::appRouter();
    return ::core::menuAnchorForPointer(
        static_cast<float>(ev.x), static_cast<float>(ev.y),
        router ? router->pageOriginX() : 0.0f, scrollOffsetY, 1.0f);
}

class MarkdownModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::Markdown; }
    std::string displayName() const override { return "Markdown 笔记本"; }
    std::string shortName() const override { return "笔记"; }
    std::string iconGlyph() const override { return eui::utf8(0xF044); }  // fa-pen-to-square

    // 测试缝（声明与理由见 markdown_module_test_seam.h）：只给「注入轮询周期」这一项
    // 开私有访问，不给测试留后门集合。
    friend void mdSetPollIntervalMsForTest(::core::Module* mod, int64_t ms);


    void onStart() override ;

    void onStop() override ;


    void compose(eui::Ui& ui, const eui::Screen& screen) override ;

private:
    // Esc 关最上层弹窗/浮层（顺序 = z 序倒序）。返回 true = 本键已消费，无弹窗返回 false。
    bool closeTopmostPopup() ;

    void drawHeader(eui::Ui& ui,
                    float contentW,
                    float contentH,
                    float bodyH,
                    const core::theme::Tokens& t) ;

    void drawBody(eui::Ui& ui,
                  float contentW,
                  float bodyH,
                  float sideW,
                  float midW,
                  float gap,
                  const core::theme::Tokens& t,
                  const components::theme::ThemeColorTokens& componentTheme) ;

    // 方案 A 边界把手：一颗把手管两端——树展开时把手位于树与编辑区之间（树右缘）垂直居中，
    // 树收起后同颗把手出现在编辑区左缘同一垂直位置。绿底白箭头门把手样式，箭头方向随
    // 两态翻转形成镜像对称。页面级元素（zIndex 6，低于大纲 10 / 弹窗 1000），
    // 常驻 id md.tree.handle（含 .bg 子件）。取代旧 drawTreeExpandFab + md.tree.collapse。
    void drawTreeHandle(eui::Ui& ui,
                        float pad,
                        float bodyH,
                        float sideW,
                        float gap,
                        const core::theme::Tokens& t) ;

    // 方案 A 笔记标签条：已打开的笔记以 tab 行展示在头部与编辑器之间。
    // 每 tab = 笔记名 + 关闭钮；激活 tab = 块面式（primarySoftBg 底+primary 字+8px 圆角，S2.2）。
    // 关闭最后一个 tab 回到空态；数据源 = openNotes_ 向量（会话内存态）。
    void drawNoteTabBar(eui::Ui& ui,
                        float contentW,
                        const core::theme::Tokens& t,
                        const components::theme::ThemeColorTokens& componentTheme) ;

    // 关闭一个标签页：从 openNotes_ 移除，若关闭的是当前活跃笔记则切换到最后剩余项
    void closeTab(const std::string& path) ;

    void drawTreeColumn(eui::Ui& ui,
                        float bodyH,
                        float sideW,
                        const core::theme::Tokens& t,
                        const components::theme::ThemeColorTokens& componentTheme) ;

    void drawNoteList(eui::Ui& ui,
                      float listY,
                      float listH,
                      float sideW,
                      const core::theme::Tokens& t,
                      const components::theme::ThemeColorTokens& componentTheme) ;

    void drawTreeList(eui::Ui& ui,
                      float listY,
                      float listH,
                      float sideW,
                      const core::theme::Tokens& t,
                      const components::theme::ThemeColorTokens& componentTheme) ;

    void drawOutlinePanel(eui::Ui& ui,
                          const eui::Screen& screen,
                          const core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& componentTheme) ;

    // 两态共用的列壳（单点维护）：面板底 + 空态提示 + 标签 chip 行 + 文本列 + 状态栏，
    // 几何经 TextColumnGeometry 回传给调用方。
    struct TextColumnGeometry {
        float x = 0.0f;  // 文本列左缘
        float y = 0.0f;  // 文本列顶缘（16 顶距 + 24 chip 行 + 8 间距）
        float w = 0.0f;  // 文本列宽
        float h = 0.0f;  // 文本列高（已扣状态栏与底部间距，钳 >= 0）
    };

    // 未选笔记（空文档）只出提示条 + 状态栏，正文回调不执行
    template <typename Body>
    void drawTextColumnChrome(eui::Ui& ui,
                              const std::string& columnId,
                              float bodyH,
                              float midW,
                              const core::theme::Tokens& t,
                              const components::theme::ThemeColorTokens& componentTheme,
                              Body&& body) {
        ui.stack(columnId).size(midW, bodyH).content([&] {
            // 列底 = 页面底（浅灰）；有笔记时正文区浮白纸卡（批M：灰底托纸卡分层）
            ui.rect(columnId + ".bg")
                .size(midW, bodyH)
                .color(t.colors.background)
                .build();
            if (currentPath_.empty()) {
                ui.text(columnId + ".hint")
                    .position(24.0f, 50.0f)
                    .size(midW - 48.0f, 40.0f)
                    .text("从左栏选一篇笔记开始编辑")
                    .fontSize(t.sizes.fontSizeBody)
                    .lineHeight(20.0f)
                    .color(t.colors.textTertiary)
                    .build();
                drawStatusBar(ui, 0.0f, bodyH - 22.0f, midW, t);
                return;
            }

            // 白纸卡：包 tag 栏 + 正文（状态栏留在卡下灰底）
            ui.rect(columnId + ".paper")
                .position(8.0f, 8.0f)
                .size(midW - 16.0f, std::max(0.0f, bodyH - 8.0f - 30.0f))
                .color(t.colors.surface)
                .radius(t.sizes.radiusLarge)
                .border(1.0f, t.colors.borderDefault)
                .build();
            float colX = kTextColumnInsetX;
            float colW = midW - kTextColumnInsetX * 2.0f;
            // TODO(面包屑)：展示 currentPath_ 路径（§3.2.4 补充项，签认可划）。
            // 数据链待核——审计未覆盖路径读取链，暂不硬做。
            drawTagBar(ui, colX, colW, t, componentTheme);
            const float colY = 48.0f;  // 16 顶距 + 24 chip 行 + 8 间距
            const float colH = std::max(0.0f, bodyH - colY - 22.0f - 8.0f);

            body(TextColumnGeometry{colX, colY, colW, colH});

            drawStatusBar(ui, 0.0f, bodyH - 22.0f, midW, t);
        }).build();
    }

    // 中栏：编辑器（真实读 / 写文件）
    void drawEditorColumn(eui::Ui& ui,
                          float bodyH,
                          float midW,
                          const core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& componentTheme) ;

    // 键盘流接线（纯函数在 md_keyboard）
    // 键位：Tab/Shift+Tab 缩进、Enter 列表续行、Ctrl+B/I 包裹、Ctrl+K 链接。
    // 只做 IME 防御 → 纯函数 → 写回 InputState；无变化的拦截只消费不动状态。
    bool handleEditorKey(eui::Ui& ui, const eui::KeyEvent& event) ;

    // 键盘流与「插入表格」共用写回。无变化只消费不动状态，不制造空 undo / 脏标记。
    void commitEditBuffer(eui::Ui& ui, EditBuffer& buf) ;

    // onChange 与键盘流共用的副作用（内存缓冲 + 脏标记）
    void onEditorTextChanged(const std::string& v) ;

    // editorText_ 唯一写点。内容变化时推进修订号 + 刷新预览 hash，下游缓存
    //（几何表 / 预览 contentKey / 状态栏）以 rev 为键跨帧复用，省 O(doc) 重算。
    void setEditorText(const std::string& v) ;

    // 标签 chip 行（显示 + × 删除）+ 内置添加输入（回车提交）。溢出直接截断。
    // W8：标签添加从红点加号改为输入框内置（§3.3.4），移除独立 addbtn 按钮。
    // chip 样式：浅灰底 + 标签绿字（wechat 令牌）。
    void drawTagBar(eui::Ui& ui,
                    float x,
                    float w,
                    const core::theme::Tokens& t,
                    const components::theme::ThemeColorTokens& componentTheme) ;

    // 底部状态栏（§3.3.4）：12px 浅灰右对齐，字数 + 保存状态。
    void drawStatusBar(eui::Ui& ui, float x, float y, float w, const core::theme::Tokens& t) ;

    // 右栏：预览（components::markdown 渲染成 eui 节点树）
    // 双栏态：编辑器单向驱动预览滚动；仅预览态：手动滚动，进入时对齐编辑器位置。
    void drawPreviewColumn(eui::Ui& ui,
                           float bodyH,
                           float midW,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& componentTheme,
                           float gap) ;

    // 块流注入点
    // 视图态对单块的特判经本注入点传入，新增视图态只传自己的 hooks。
    // 三个回调均可空（空 = 走默认）；k = 块下标、b = 块、wrapId = 包裹 stack id。
    struct BlockFlowHooks {
        std::function<float(size_t k, const MdBlock& b, float geometryHeight,
                            float contentWidth)>
            wrapHeight;
        std::function<bool(eui::Ui& ui, size_t k, const MdBlock& b,
                           const std::string& wrapId, float contentWidth,
                           float wrapHeight)>
            drawContent;
        std::function<void(eui::Ui& ui, size_t k, const std::string& hitId)>
            afterHitOverlay;
    };

    // 实时态块级特判上下文（活动块锚 + input 主题），只在实时态内部传递。
    struct LiveBlockCtx {
        size_t activeBlock = static_cast<size_t>(-1);  // 活动块下标（其余值不命中）
        const components::theme::ThemeColorTokens* theme = nullptr;  // input 主题
    };

    // 逐块渲染（预览 / 实时共用同一条通路）
    // 切分器保证块区间连续覆盖文档，与整篇渲染等价；prefix 决定键控 id。
    // ⚠ ui 必须是 scrollView content 回调传入的 Ui：测量 pass 也走同一回调，
    // 建到外层 Ui 会让测量量到空内容（可滚高度恒 0）。
    void drawBlockFlow(eui::Ui& ui,
                       float contentWidth,
                       const core::theme::Tokens& t,
                       const components::MarkdownStyle& style,
                       const std::string& prefix,
                       const std::function<void(size_t, float)>& onBlockTap,
                       const BlockFlowHooks& hooks) ;

    // 滚动锚定与光标跟随
    // 外层 scrollView 是唯一滚动权威。用户滚轮 → liveFollow_ = false；
    // 光标事件 → true；跟随只在 caret 越出安全带时拉一次，目标 == 记账值则 no-op。

    // 块内光标的显示行号（软折行参与计数），与 input 内部 layout.cursorLine 同源；
    // 逻辑行口径会低估折行段落的 caret y，不采用。
    int liveCaretDisplayLine(const MdBlock& b, const std::string& blockText,
                             int local, float textWidth, float fontSize) ;

    // 越界才拉。caretDocY = 活动块顶 + input 内边距 + 显示行 × 行高。
    // 落地走 Ui::setScrollOffset（直写实例 offset），调用点必须在 scrollView
    // build + 回读钳位之后：程序化标志不跨帧残留。
    void liveScrollFollowMaybe(eui::Ui& ui, const MdBlock& b,
                               const std::string& blockText, int localCursor,
                               float contentWidth, float viewportH) ;

    // 第四态：实时（块级实时渲染 + 活动块输入）
    // 单栏，块级渲染走 drawBlockFlow（与预览同一通路、同一张几何表）。
    void drawLiveColumn(eui::Ui& ui,
                        float bodyH,
                        float midW,
                        const core::theme::Tokens& t,
                        const components::theme::ThemeColorTokens& componentTheme) ;

    // CodeFence 块预览自绘：几何逐项对齐上游 renderCodeBlock，唯一差异是 text
    // 带着色 runs。hovered 时把 codeBackground 混向 hover 色（块 bg 不透明）。
    void drawPreviewCodeBlock(eui::Ui& ui,
                              const std::string& partId,
                              const MdPreviewCodeHighlight& hl,
                              float contentWidth,
                              const components::MarkdownStyle& style,
                              bool hovered,
                              const eui::Color& hoverTint) ;

    // 实时态活动块输入

    // 块源码文本（块区间连续覆盖文档）
    std::string liveBlockText(const MdBlock& b) const ;

    // 活动块 stableId（渲染键 / contentKey 用；无块 → -1）。
    // stableId 按内容身份继承，比下标更适合当缓存键。
    int liveActiveBlockStableId() const ;

    // 活动块内容贴合高度：测量与 input 内部排版同源（公式值 +1px 余量
    // ⇒ maxVerticalScroll == 0）；备忘键避免整块 shaping 逐帧重付。
    float liveFitHeight(const MdBlock& b, const std::string& blockText,
                        float contentWidth) ;

    // 上行链路：活动块局部文本 → 拼回整篇 → 唯一文档写点。
    // 分块器保证块区间连续覆盖，拼回 ≡ 编辑态对同一处的同一编辑。
    void syncDocumentFromLiveInput(const std::string& blockText) ;

    // 活动块源码态 input：建在块包裹 stack 内（0,0），尺寸 = 列宽 × 贴合高。
    // 元素必须建在 scrollView content 回调的 Ui 上（否则测量量到空内容）。
    void drawLiveInputBlock(eui::Ui& ui,
                            const MdBlock& b,
                            const std::string& blockText,
                            const std::string& wrapId,
                            float contentWidth,
                            float fitHeight,
                            const components::MarkdownStyle& style,
                            const LiveBlockCtx& live) ;

    // 光标写点 / 块点击定位 / 失焦提交

    // 实时态唯一光标写点：文档字节偏移 = 活动块身份锚。一次做完：锚重算、
    // 记账 liveCaretPending_ 与 liveFocusPending_（文本未变须显式写光标 /
    // 指针按下摘掉焦点须补发），两个一次性请求都在下次 compose 同帧消费。
    void setLiveCursor(size_t docByte, const char* reason) ;

    // 点击渲染块 → 该块变源码态 + 光标落到点击行。只服务非活动块；
    // 组字在飞时拒绝切块并补发聚焦。
    void handleLiveBlockClick(eui::Ui& ui, size_t blockIndex, float localY) ;

    // 失焦提交两条收口：① 清在飞组字（不清则冻结谓词恒真、切块被永久拒绝）；
    // ② 只在输入框与文档对齐到同一修订号时上行未提交文本，否则旧文本
    // 灌回新文档 = 数据丢失。
    void commitLiveInputOnBlur(eui::Ui& ui) ;

    // 块边界键
    // 块边界不做块结构操作，只做普通文本编辑，块身份始终跟随光标：
    // 块内按键 → md_keyboard 纯函数写回活动 input；块边界键 → md_live_edit
    // 纯函数改文档 + setLiveCursor。组字在飞一律不拦截（会撕裂预编辑）。
    bool handleLiveKey(eui::Ui& ui, const eui::KeyEvent& event) ;

    // 块内编辑键写回（写活动 input）：纯函数改 InputState 不触发 onChange，
    // 故文本变化时须显式上行。
    void commitLiveEditBuffer(eui::Ui& ui, EditBuffer& buf) ;

    // 边界删除：删的是块外接缝换行，直接改整篇文档再按删除点重定光标。
    // 顺序不能反（先 onEditorTextChanged 再 setLiveCursor）；撤销由模块层
    // 快照栈接管，须先 push 编辑前整篇文本。
    void applyLiveBoundaryEdit(const MdLiveBoundaryEdit& edit, const char* reason) ;

    // 块级 undo / redo

    // 推栈：快照 = 编辑前的整篇文本 + 文档光标偏移，在写文档之前推。
    void pushLiveUndoSnapshot() ;

    // 撤销 / 重做落地：改文档 + 光标两处唯一写点。无可用快照时静默返回
    //（调用侧已消费按键，不回退框架栈）。
    void applyLiveUndo() ;

    void applyLiveRedo() ;

    // 撤销栈基线换代（以当前文档为基线重开栈）。调用点 = 整篇文档换了一版
    //（进实时态首帧 / 外部重载）；不换代时 Ctrl+Z 会把别处改动整篇盖回旧版并写盘。
    void resetLiveUndoBaseline() ;

    // 双栏同步滚动：编辑器顶行 → 块定位 → 预览侧块几何（块内按编辑器行线性插值；
    // 反向映射是同一函数的反函数）。缓存键 = 修订 + 宽度 + blockGap + 块数。
    float computeSyncOffset(eui::Ui& ui,
                            float pvW,
                            float pvH,
                            const components::MarkdownStyle& style) ;

    // 几何表按需重建（键未变 = 零成本；变了只对 memo 未命中的块付 estimateHeight）。
    void rebuildSyncMap(float width, const components::MarkdownStyle& style) ;

    // 预览块点击 → 编辑器光标定位 + 聚焦（mdResolvePreviewJump 反查行首字节）。
    // 行内列不映射：预览与编辑器布局不同源，行首是唯一无歧义落点。
    void handlePreviewBlockClick(eui::Ui& ui, size_t blockIndex, float localY) ;

    // 新建笔记弹窗
    void drawNewNoteDialog(eui::Ui& ui,
                           const eui::Screen& screen,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& componentTheme) ;

    // 新建文件夹弹窗（含父目录下拉）
    void drawNewFolderDialog(eui::Ui& ui,
                             const eui::Screen& screen,
                             const core::theme::Tokens& t,
                             const components::theme::ThemeColorTokens& componentTheme) ;

    // 外部修改冲突 toast：action「重载磁盘版本」= 重读，✕ 关闭 = 保留本地编辑；
    // duration(0) 不自动超时（二选一须显式决定）。


    // Toast 反馈
    // 普通提示走 ui_kit；外部冲突就地绘制；二者共用 md.toast 单实例且互斥。
    // M-1（UAT2 一审）：toast 属 popup，两分支统一 wechat 皮肤；页面暖橙
    // componentTheme 只喂页面主体，themeTokens 形参随之闲置（先例 drawDeleteDialog）。
    void drawToast(eui::Ui& ui, const eui::Screen& screen,
                   const components::theme::ThemeColorTokens& themeTokens) ;

    void showToast(const std::string& title, const std::string& msg, bool isError) ;

    // 四态切换 / 大纲

    // 实时态状态位群（文档锚 / 首帧聚焦 / 滚动位置）的单一复位点，
    // 进实时态三条路径（setMode / openNote / loadViewState）统一调用。
    void enterLiveState(const char* reason) ;

    void setMode(ViewMode m) ;

    // 全局快捷键：Ctrl+Shift+P 循环四态、Ctrl+N 新建笔记、Ctrl+S 保存、Esc 关浮层。
    // 通道 = app 级 onKeyEvent → Router 派发（input 未消费的键回调出来）。
    bool onKeyEvent(const eui::KeyEvent& key) override ;

    // 新建笔记弹窗统一入口：打开前重置草稿名与错误行。
    void openNewNoteDialog() ;

    // 大纲当前节：光标行所在的上一个标题；预览态沿用 activeOutlineIdx_，
    // 实时态用 liveCursorByte_（编辑器输入不在实时态帧树）。
    int activeOutlineIndex(eui::Ui& ui) ;

    // 大纲点击跳转：编辑/双栏态写 cursor 到标题行首；仅预览态直接滚预览。
    // 只改 cursor 不改文本 → 不动 undo 栈。
    void jumpToHeading(eui::Ui& ui, size_t outlineIndex,
                       const components::theme::ThemeColorTokens& componentTheme) ;

    // 消费挂在本页的跳转负载（取出即清空，每帧重复调用不重复触发）：
    // "action:new_note" → 新建笔记弹窗；"open:<相对路径>" → 打开指定笔记
    void consumeNavPayload() ;

    // 收割「库被 GitHub Sync 导入覆盖」标记（compose 主线程）：库表被 DROP 重建，
    // notes_/allFolders_ 指向旧行 → 重查列表。
    // 当前笔记不在本函数里无条件重载——库被导入但该笔记文件没被 pull 时磁盘内容没变，
    // 无条件重载会弹假冲突，用户点「重载磁盘版本」还会用未变内容覆盖缓冲、静默丢未保存
    // 编辑。改走 checkCurrentNoteOnDisk()：只在 fileMtime > lastKnownMtime_（磁盘真变过）
    // 时才重载或弹冲突，与 5 秒磁盘轮询同一口径。
    void harvestDbImport() ;

    void refreshList() ;

    // 全部标签（左栏筛选区数据源）
    void refreshTags() ;

    // 标签动作（数据全在 DB）

    void toggleTagFilter(const std::string& name) ;

    // 标签写的**单一出口**：成功即发 md.tags_changed。标签只落 DB 两张表
    // （markdown_tags / markdown_note_tags，都在整库 dump 里）而**不碰笔记文件**，
    // 所以 md.saved 兜不住「只加/删标签」这类改动 —— 必须自有一条事件。
    // 加/删两个 UI 入口都走这里；将来新增第三个入口若绕过本函数直调
    // service_->setTags，同步侧就会漏掉那类改动（见 settings_module 订阅清单的注释：
    // 漏订一个 = 那类改动永远不自动同步）。调用方保证 currentPath_ 非空。
    core::Result<void> writeTags(const std::vector<std::string>& tags) ;

    // 「＋」/回车：给当前笔记加标签
    void commitAddTag() ;

    // 标签 chip ×：移除标签
    void removeTag(const std::string& name) ;

    void openNote(const std::string& relPath) ;

    // 视图状态持久化

    // 恢复上次选中笔记 / 四态视图 / 收起目录；笔记已被外部删除时静默跳过。
    void loadViewState() ;

    // 视图状态写失败只记日志，不影响编辑主流程
    void saveViewState() const ;

    // 有未落盘改动时写回文件 + 索引（每帧调用，自带防抖）；
    // 保存失败走指数退避，避免 dirty 期间每帧重试打失败 IO/日志。
    // conflictOpen_ 时不写：磁盘版本已变（外部修改 / 库导入拉回新内容），此刻落盘
    // 就是用本地旧缓冲盖掉刚 pull 下来的内容——等用户在冲突弹窗里选完再写。
    void flushEditorIfIdle() ;

    void saveEditor() ;

    // 文件变化自动感知（5 秒轮询）

    // 每 5 秒一次（compose 每帧调用，steady_clock 节流）
    void pollDiskChangesIfDue() ;

    void checkCurrentNoteOnDisk() ;

    // 从磁盘重读当前笔记（不走 openNote 的「先保存旧笔记」路径）。
    // 重载后按内容身份映射旧活动块：三元组命中 = 精确实例；回退 (kind,hash)
    // 取 |start 差| 最小者；零命中钳位到旧光标对应新块。
    void reloadCurrentNote(bool silent) ;

    void resolveConflict(bool reloadFromDisk) ;

    // lastKnownMtime_ ← 磁盘当前 mtime
    void syncCurrentMtime() ;

    void commitNewNote(eui::Ui& ui) ;

    // 新建文件夹（左栏「+ 文件夹」入口）

    // 相对路径的父目录（POSIX；无 '/' → "" 即根目录）
    static std::string parentDirOf(const std::string& relPath) ;

    // 目录是否存在于当前磁盘清单（"" = 根目录，恒存在）
    bool dirExistsOnDisk(const std::string& dir) const ;

    // 新建条目的父目录 = newItemTargetDir_（由点击文件夹 / 打开笔记同步更新）；
    // 已被外部删除 → 回根目录 + WARN。
    // "" 既表「根目录」也表兜底，区别只在日志。
    std::string newItemParentDir() const ;

    // 选中目录变化 → 记住 + 落盘（跨会话）
    // 用户直接点文件夹时视觉选中与功能目标同步更新
    void selectDir(const std::string& dir) ;

    // 面板折叠/展开（UAT2 F批）：翻转 + 落盘（与目录折叠同口径，跨会话记忆）
    void toggleTreePanel() ;

    // 文件名规范化：以 .md 结尾（rescan 只索引 .md，否则笔记从树里消失）
    static std::string withMdExtension(std::string name) ;

    // 「上级目录」候选：根目录 + 磁盘上每一级目录（层级不设上限）。
    // 返回 (显示名, 相对路径)，显示名 = 相对路径（根目录显示为「根目录」）。
    std::vector<std::pair<std::string, std::string>> folderParentCandidates() const ;

    // 候选里某目录的下标（找不到 → 0 = 根目录，避免「显示 A 却建到 B」）
    static int folderParentIndexFor(
        const std::vector<std::pair<std::string, std::string>>& candidates,
        const std::string& dir) ;

    void commitNewFolder() ;

// 生命周期：右键菜单 / 删除 / 重命名

// 右键文件行：指针逻辑坐标经 menuAnchorForPointerInPage 换算成页面坐标
//（runtime 已做 /dpiScale；scrollOffsetY=0 = 指针即视觉位）。
    void openFileContextMenu(const std::string& relPath, const eui::PointerEvent& ev) ;

    void drawNoteContextMenu(eui::Ui& ui,
                             const eui::Screen& screen,
                             const core::theme::Tokens& t,
                             const components::theme::ThemeColorTokens& componentTheme) ;

// 编辑器右键「插入表格」：行→列两级级联菜单；行数含表头行。
    static std::vector<components::ContextMenuItem> buildTableMenuItems() ;

    void drawEditorContextMenu(eui::Ui& ui,
                               const eui::Screen& screen,
                               const core::theme::Tokens& t,
                               const components::theme::ThemeColorTokens& componentTheme) ;

    // 光标处插表格模板（与键盘流同一写回：可 undo、走脏标记链路）
    void insertTableAtCursor(eui::Ui& ui, int rows, int cols) ;

    // 删除确认弹窗（不可恢复文案）
    void drawDeleteDialog(eui::Ui& ui,
                          const eui::Screen& screen,
                          const core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& componentTheme) ;

    // 重命名对话框（预填当前相对路径；填含 / 的路径 = 同时移动）
    void drawRenameDialog(eui::Ui& ui,
                          const eui::Screen& screen,
                          const core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& componentTheme) ;

    void doDeleteNote() ;

    void commitRenameNote() ;


    static constexpr int64_t kAutoSaveDelayMs = 400;
    static constexpr int64_t kMaxSaveBackoffMs = 60000;  // 保存失败退避封顶
    static constexpr int64_t kPollIntervalMs = 5000;     // 文件变化轮询周期（默认值）

    std::unique_ptr<MarkdownService> service_;
    std::unique_ptr<class NoteGitAutoCommit> noteGit_;  // 笔记目录 git 自动提交
    // 引用 service_，onStop 须先注销再放 service
    std::unique_ptr<MarkdownTools> markdownTools_;
    // GitHub Sync 导入成功 → 置脏，compose 收割重载（导入线程只碰原子，不碰 UI 成员）。
    // 闸 = 存活守卫，onStop 先关闸再退订（token 析构即退订）。
    std::atomic<bool> dbImportedDirty_{false};
    std::atomic<bool> dbImportHandlersEnabled_{false};
    ::core::event::EventBus::Subscription dbImportedSub_;
    // 重入幂等守卫：double-onStart / double-onStop / 未启动的 off = no-op
    bool started_ = false;
    // ServiceLocator 提供方（UAT6.2 H31）：onStart 注册、onStop 注销（set(nullptr)）。
    // 生命周期 = service_（适配器持 MarkdownService&）；reset 必须晚于注销。
    std::unique_ptr<::core::IMarkdownNotesSource> notesSource_;
    // 未创建 = 不持久化，saveViewState 静默跳过
    std::unique_ptr<MarkdownViewStateStore> viewStore_;
    std::vector<NoteMeta> notes_;      // 当前展示（全部或搜索结果）
    std::string searchQuery_;          // 搜索框内容
    std::string currentPath_;          // 打开的笔记（POSIX 相对路径）
    // 方案 A：笔记标签页列表（会话内存态，顺序 = tab 顺序；持久化仅记 lastPath）
    std::vector<std::string> openNotes_;
    std::string editorText_;           // 编辑器缓冲
    // 内容修订号（唯一写点 = setEditorText），下游缓存以它为键跨帧复用
    uint64_t editorTextRev_ = 0;
    uint64_t previewTextHash_ = std::hash<std::string>{}(std::string());
    // 同步滚动几何表（块高按 stableId 备忘，编辑只重测脏块）
    MdSyncMap syncMap_;
    // 预览逐块 hover 状态；点击定位纯数学在 mdResolvePreviewJump
    MdPreviewHoverState previewHover_;
    // 状态栏 (行, 列, 码点数) 缓存（键 = 修订 + 光标 + 模式）
    uint64_t statusCacheRev_ = ~0ull;
    size_t statusCacheCursor_ = ~0ull;
    int statusCacheMode_ = -1;
    int statusCacheLine_ = 0;
    int statusCacheCol_ = 1;
    size_t statusCacheChars_ = 0;
    std::string lastSavedText_;        // 上次落盘内容（用于跳过冗余写）
    bool editorDirty_ = false;
    bool saveFailed_ = false;          // 上次保存尝试失败（状态栏 ✕ 提示）
    std::chrono::steady_clock::time_point lastEditAt_{};
    std::chrono::steady_clock::time_point lastSaveAttemptAt_{};  // 退避计时基准
    int64_t saveBackoffMs_ = 0;        // 0 = 无退避；失败后指数增长
    std::string listError_;            // 左栏错误态（空 = 正常）
    bool newNoteOpen_ = false;
    std::string newNoteName_;
    // 弹窗内嵌红字错误行（校验/创建失败不走 toast）
    std::string newNoteError_;
    bool newFolderOpen_ = false;
    std::string newFolderName_;
    std::string newFolderError_;       // 内嵌错误行
    // 新建文件夹的「上级目录」（POSIX 相对路径；"" = 根目录）。
    // 存路径而非候选下标：候选列表每帧由 allFolders_ 重建，下标会漂移。
    std::string newFolderParentDir_;
    bool newFolderParentOpen_ = false; // 下拉展开态
    // 磁盘全层目录（POSIX 相对路径，含空目录；树渲染 + 父目录下拉用）
    std::set<std::string> allFolders_;
    // 目录树选中态（POSIX 相对路径；"" = 根目录，是合法值也是初值）。
    // 选中不级联：仅由用户直接点击文件夹行更新；打开笔记不上溯选中父目录。
    std::string selectedDir_;
    // 新建条目的目标目录（POSIX 相对路径；"" = 根目录）。
    // 由 selectDir（点击文件夹）和 openNote（打开笔记）同步更新。
    // 跨会话由 selectedDir_ 初始化，其后跟笔记打开实时变化。
    std::string newItemTargetDir_;

    // 生命周期：树行右键菜单 + 删除确认 + 重命名
    bool ctxMenuOpen_ = false;
    float ctxMenuX_ = 0.0f;
    float ctxMenuY_ = 0.0f;
    std::string ctxMenuPath_;        // 右键目标（POSIX 相对路径）
    // 编辑器右键「插入表格」级联菜单
    bool editorCtxOpen_ = false;
    float editorCtxX_ = 0.0f;
    float editorCtxY_ = 0.0f;
    bool deleteConfirmOpen_ = false;
    std::string deleteTargetPath_;   // 待删笔记（确认弹窗展示 + 删除目标）
    bool renameOpen_ = false;
    std::string renameTargetPath_;   // 重命名原路径
    std::string renameInput_;        // 重命名输入框内容
    std::string renameError_;        // 弹窗内嵌红字错误行

    // Toast（单实例）
    ui_kit::ToastState toast_;

    // 视图状态
    ViewMode mode_ = ViewMode::Edit;
    std::vector<MdHeading> outline_;   // 大纲（标题扫描结果）
    bool outlineDirty_ = true;
    int activeOutlineIdx_ = -1;        // 仅预览态用（无编辑器光标可读）
    float previewOffset_ = 0.0f;       // 仅预览态的手动滚动位置
    float liveScrollY_ = 0.0f;         // 实时态滚动偏移（记账值，每帧按声明值回读钳位）
    // 滚动锚定与光标跟随
    bool liveFollow_ = false;          // 跟随开关：滚轮 → false，光标事件 → true
    bool liveProgrammaticScroll_ = false;  // 程序化滚动来源标记（同帧消费）
    float liveMaxOffset_ = 0.0f;       // 上帧 scrollView 声明的 maxOffset
    int liveCursorSeen_ = -1;          // 上帧活动 input 的光标（变化 = 一次光标事件）
    float liveContentWidth_ = 0.0f;    // 上帧内容宽（跟随判据的排版宽，滞后一帧）
    // 光标显示行备忘（键 = stableId + hash + 块内光标 + 文本宽 + 字号）
    struct LiveCaretMemo {
        int stableId = -1;
        uint64_t hash = 0;
        int local = 0;
        float width = -1.0f;
        float fontSize = 0.0f;
        int line = 0;
    };
    LiveCaretMemo liveCaret_;
    // 实时态活动块（单输入跟随 + 内容贴合高度）
    size_t liveCursorByte_ = 0;        // 文档字节偏移 = 活动块身份锚
    size_t liveActiveBlock_ = 0;       // 本帧活动块下标（每帧由锚重算）
    size_t liveInputStart_ = 0;        // 活动 input 文本在文档中的起点（锚推导基准）
    bool liveCursorValid_ = false;     // 锚是否已按当前文档初始化
    bool liveFocusPending_ = false;    // 进实时态首帧同帧补发 requestFocus
    // 焦点/切块状态机
    bool liveInputStateFresh_ = false; // 输入框状态待换代（enterLiveState 置位）
    bool liveCaretPending_ = false;    // 待写块局部光标（文本未变须显式写）
    uint64_t liveSyncedRev_ = 0;       // 输入框与文档对齐的修订号（失焦提交安全判据）
    // 块级 undo/redo（模块层快照栈）
    MdLiveUndoStack liveUndo_;         // 快照 = (整篇文本, 文档光标偏移)
    size_t liveUndoBlock_ = 0;         // 上次推栈时的活动块（合并判据基准）
    bool liveUndoSeeded_ = false;      // 本会话基线是否已就位
    // 贴合高度备忘（键 = stableId + hash + 文本宽 + 字号）
    struct LiveHeightMemo {
        int stableId = -1;
        uint64_t hash = 0;
        float width = -1.0f;
        float fontSize = 0.0f;
        float height = 0.0f;
    };
    LiveHeightMemo liveHeight_;
    // 本帧活动块是否命中高度封顶（0.7×视口高）；
    // over 探针凭它区分「封顶块内滚动（预期）」与「贴合失败（回归）」。
    bool liveInputCapped_ = false;
    bool syncPreviewOnEnter_ = false;  // 刚切入预览态 → 对齐编辑器位置
    float lastPvContentWidth_ = 0.0f;  // 预览内容宽（estimateHeight 用，滞后一帧）
    std::time_t lastSavedAt_ = 0;      // 上次落盘时刻（状态栏 "✓ 已保存 HH:MM"）

    std::set<std::string> collapsedDirs_;             // 已收起的一级目录（内存态）
    // 目录树面板折叠态（UAT3 D-R3-3）：true = 完全收起（宽 0，编辑区左上角浮展开钮）。
    // 纯 UI 态（先例 ViewMode/selectedDir），跨会话经 md_view_state 持久化（仍存 bool）。
    bool treePanelCollapsed_ = false;
    // 面板当前宽（D-R3-3 c 动画值）：每帧向目标（0 / kTreePanelWidth）lerp，收敛即钉住；
    // 持久化只存 treePanelCollapsed_，本值启动时按折叠态直接落位（loadViewState）。
    float treePanelWidth_ = kTreePanelWidth;
    bool outlineOpen_ = false;                        // 大纲浮动面板开关
    int64_t lastKnownMtime_ = 0;                      // 当前笔记已知磁盘 mtime（-1=已删除）
    bool conflictOpen_ = false;                       // 外部修改冲突 toast（action 二选一）
    std::chrono::steady_clock::time_point lastPollAt_{};  // 轮询节流基准
    // 轮询节流周期（默认 5s；测试缝可注入，见 markdown_module_test_seam.h）
    int64_t pollIntervalMs_ = kPollIntervalMs;

    // 标签状态
    std::vector<std::string> currentTags_;  // 当前打开笔记的标签
    std::vector<TagCount> allTags_;         // 左栏筛选区的全部标签（含笔记数）
    std::string tagFilter_;                 // 生效中的标签筛选（空 = 无）
    std::string tagInput_;                  // 添加标签输入框内容

    // 语法高亮状态
    MdHighlighter highlighter_;        // 行级高亮器（围栏状态随 display 文本重建）
    // 实时态高亮独立实例：编辑态喂整篇（全文档坐标）、实时态喂块文本
    //（块局部坐标）；两套坐标系交替喂同一实例会抖动围栏缓存。
    MdHighlighter liveHighlighter_;
    std::string highlightThemeName_;   // 上次注入色表的主题名（变化 = 重高亮）

    // 块级元素模型（预览逐块增量渲染的键控源）
    MdBlockModel blockModel_;

    // 预览代码块高亮（CodeFence 块自绘的脏块缓存）
    MdPreviewCodeCache previewCodeCache_;
};

} // namespace module::markdown