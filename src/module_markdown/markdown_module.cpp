// Markdown 笔记本模块：三栏 UI（目录树 / 编辑器 / 预览），四态可切换。
// 所有 I/O 经 MarkdownService，失败走 core::Result；本文件只接线，
// 键盘流纯函数在 md_keyboard.*，语法高亮在 md_highlight.*。

#include "core/database.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/router.h"
#include "core/theme.h"
#include "core/ui_geometry.h"
#include "core/component_theme.h"
#include "core/event_bus.h"
#include "core/time.h"
#include <ui_kit/button.h>
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

namespace {

namespace fs = std::filesystem;

// 树行几何常量；改动须同步 tests/test_modules/test_markdown.cpp 的数值断言。
// UAT2 F批（目录树大气）：行高 32→36、箭头 18→20、条目字号 13→14（专用常量，
// 不改 fontSizeSmall token——它波及全 UI）。
constexpr float kTreeRowHeight = 36.0f;
constexpr float kTreeArrowFontSize = 20.0f;
constexpr float kTreeFontSize = 14.0f;

// UAT3 D-R3-3（目录树折叠重做）：面板宽动画与收起形态常量。
constexpr float kTreePanelWidth = 240.0f;         // 展开宽（沿用 UAT2 F批 240）
constexpr float kTreePanelAnimK = 0.18f;          // 每帧向目标靠拢的固定 lerp 系数
constexpr float kTreePanelSnapEps = 1.0f;         // 距目标 <1px 视为收敛，直接钉住
constexpr float kTreePanelContentMinW = 220.0f;   // 内容回画门槛（动画中只画面板壳）

// 单行树条目：整行包一层 stack —— position() 只在 stack 内生效，流式子元素会被静默忽略。
// W8：选中态改 wechat 风格（§4.3）——浅灰底 #E5E5E5 + 左 2px 绿竖线。
// 搜索命中时高亮关键词（§3.2.4 补充项）。
void drawFileTreeItem(eui::Ui& ui,
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
    const auto& wc = t.colors.wechat;
    ui.stack(id)
        .size(width, kTreeRowHeight)
        .content([&] {
            ui.rect(id + ".row")
                .size(width, kTreeRowHeight)
                .color(active ? wc.selectedBg : wc.cardBg)
                .hoverColor(wc.dividerSecondary)
                .radius(t.sizes.radiusWeChat)
                .onClick(std::move(onClick))
                .onContextMenu(std::move(onContextMenu))
                .build();
            // 选中态左 2px 绿竖线（§4.3）
            if (active) {
                ui.rect(id + ".accent")
                    .position(indent, 0.0f)
                    .size(2.0f, kTreeRowHeight)
                    .color(wc.primary)
                    .build();
            }
            ui.text(id + ".icon")
                .position(12.0f + indent, 0.0f)
                .size(22.0f, kTreeRowHeight)
                .icon(icon)
                .fontSize(kTreeFontSize)
                .lineHeight(kTreeRowHeight)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(active ? wc.primary : wc.textTertiary)
                .build();
            // 文件名：搜索命中时高亮关键词段（wechat.primary 着色）
            auto nameBuilder = ui.text(id + ".name")
                .position(36.0f + indent, 0.0f)
                .size(std::max(0.0f, width - 48.0f - indent), kTreeRowHeight)
                .text(name)
                .fontSize(kTreeFontSize)
                .lineHeight(kTreeRowHeight)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(active ? wc.textPrimary : wc.textPrimary)
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
                                        wc.textPrimary});
                    }
                    runs.push_back({static_cast<int>(hit),
                                    static_cast<int>(hit + highlight.size()),
                                    wc.primary});
                    pos = hit + highlight.size();
                }
                if (!runs.empty() && pos < name.size()) {
                    runs.push_back({static_cast<int>(pos),
                                    static_cast<int>(name.size()),
                                    wc.textPrimary});
                }
                if (!runs.empty()) {
                    nameBuilder.runs(std::move(runs));
                }
            }
            nameBuilder.build();
        })
        .build();
}

std::string noteDisplayName(const NoteMeta& n) {
    return n.relativePath.filename().string();
}

std::string noteDisplayPath(const NoteMeta& n) {
    return n.relativePath.generic_string();
}


// 四态视图枚举。值 = 视图状态持久化口径（非 segmented 下标）
enum class ViewMode { Edit = 0, Split = 1, Preview = 2, Live = 3 };

constexpr ViewMode kModeOrder[] = {ViewMode::Edit, ViewMode::Live,
                                   ViewMode::Split, ViewMode::Preview};
constexpr int kModeCount = 4;

int modeOrderIndex(ViewMode m) {
    for (int i = 0; i < kModeCount; ++i) {
        if (kModeOrder[i] == m) return i;
    }
    return 0;
}

ViewMode modeFromOrderIndex(int index) {
    return (index >= 0 && index < kModeCount) ? kModeOrder[index] : ViewMode::Edit;
}

ViewMode nextMode(ViewMode m) {
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

std::string formatSaveTime(std::time_t t) {
    if (t <= 0) return {};
    std::tm tm{};
    ::core::time::localTime(t, &tm);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

// 粗略字宽估算（chip 布局用）：ASCII ≈ 0.55×字号，非 ASCII ≈ 字号/3
float estimateTextWidth(const std::string& s, float fontSize) {
    float w = 0.0f;
    for (unsigned char ch : s) {
        w += (ch < 0x80) ? fontSize * 0.55f : fontSize / 3.0f;
    }
    return w;
}

// 指针坐标是窗口坐标，浮层构建在页面子树内 → 经 menuAnchorForPointer 统一换算。
core::MenuAnchor menuAnchorForPointerInPage(const eui::PointerEvent& ev,
                                            float scrollOffsetY = 0.0f) {
    const ::core::Router* router = ::core::appRouter();
    return ::core::menuAnchorForPointer(
        static_cast<float>(ev.x), static_cast<float>(ev.y),
        router ? router->pageOriginX() : 0.0f, scrollOffsetY, 1.0f);
}

} // namespace

class MarkdownModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::Markdown; }
    std::string displayName() const override { return "Markdown 笔记本"; }
    std::string shortName() const override { return "笔记"; }
    std::string iconGlyph() const override { return eui::utf8(0xF044); }  // fa-pen-to-square

    // 测试缝（声明与理由见 markdown_module_test_seam.h）：只给「注入轮询周期」这一项
    // 开私有访问，不给测试留后门集合。
    friend void mdSetPollIntervalMsForTest(::core::Module* mod, int64_t ms);


    void onStart() override {
        if (started_) return;
        started_ = true;
        // GitHub Sync 拉取会 DROP 重建整个库：订阅导入成功事件，导入线程同步回调里
        // 只置原子脏标记（不得摸 UI 线程成员），compose 收割重载。
        // 闸 = 存活守卫：onStop 先关闸再退订，挡住在途 handler（emit 已把 handler
        // 拷出后才调用，退订拦不住在途那一发）。
        dbImportHandlersEnabled_.store(true, std::memory_order_release);
        dbImportedSub_ = ::core::event::bus().subscribe(
            ::core::event::kDbImported, [this](const std::string&) {
                if (dbImportHandlersEnabled_.load(std::memory_order_acquire)) {
                    dbImportedDirty_.store(true, std::memory_order_release);
                }
            });
        auto* db = ::core::appDatabase();
        if (db == nullptr) {
            LOG_WARN("Markdown 模块启动：appDatabase() 为空，跳过真实索引");
            return;
        }
        // 服务复用而非重建：agent run 线程可能正持 md.* 工具引用旧 service，
        // off→on 窗口重建 = 先析构旧对象 → UAF。前提：db 指针与 notesDir 运行期不变。
        if (!service_) {
            service_ = std::make_unique<MarkdownService>(*db,
                                                         ::core::paths::markdownDir());
        }
        markdownTools_ = std::make_unique<MarkdownTools>(*service_);
        markdownTools_->registerAll();
        auto r = service_->rescan();
        if (r.isFail()) {
            LOG_ERROR("Markdown rescan 失败: " + r.errorMessage());
            showToast("笔记目录加载失败", "重扫失败：" + r.errorMessage(), true);
        } else {
            LOG_INFO("已加载笔记目录：" + service_->notesDir().filename().string());
        }
        refreshList();
        loadViewState();  // 恢复上次的选中笔记 / 视图模式 / 收起目录

        // 笔记目录 git 自动提交：后台启动（异步 init，不阻塞 onStart）
        if (!noteGit_) {
            noteGit_ = std::make_unique<NoteGitAutoCommit>(
                service_->notesDir());
        }
        noteGit_->start();

        LOG_INFO("Markdown 模块启动，共 " + std::to_string(notes_.size()) + " 篇笔记");
    }

    void onStop() override {
        if (!started_) return;
        // 先关原子闸再退订：闸挡在途 handler，退订挡后续 emit。
        dbImportHandlersEnabled_.store(false, std::memory_order_release);
        dbImportedSub_ = {};
        if (markdownTools_) {
            markdownTools_->unregisterAll();
            markdownTools_.reset();
        }
        // 退出前落盘：无退出拦截 API，只能同步有限重试 + 日志兜底。
        // 退出时冲突未决（toast 随页面消失、没有让用户选择的时机）：只能按「保留本地」
        // 落盘 —— 未保存的键入不可恢复，外部版本可从笔记 git 历史 / 远端找回，所以这个
        // 默认方向是安全侧；但不能静默，WARN 留痕（saveEditor 成功时会顺带 resolve）。
        if (editorDirty_ && conflictOpen_) {
            LOG_WARN("Markdown: 退出模块时存在未决冲突，按「保留本地」落盘（外部版本可在笔记 git 历史 / 远端找回）: " +
                     currentPath_);
        }
        for (int attempt = 0; attempt < 3 && editorDirty_; ++attempt) {
            saveEditor();
            if (!editorDirty_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (editorDirty_ && !currentPath_.empty()) {
            LOG_ERROR("退出时保存笔记失败，未落盘的编辑已丢失: " + currentPath_ +
                      "（EUI-NEO 暂无退出拦截 API，无法阻断退出）");
        }
        // 笔记 git 自动提交：析构 join 后台线程（已在 onFileSaved 推进时间戳，
        // 本 onStop 也会经循环 saveEditor → onFileSaved 触发最后一次 commit）
        noteGit_.reset();
        started_ = false;
        LOG_INFO("Markdown 模块停止");
    }


    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        harvestDbImport();

        flushEditorIfIdle();

        pollDiskChangesIfDue();

        if (outlineDirty_) {
            outline_ = scanHeadings(editorText_);
            outlineDirty_ = false;
        }

        // 消费跨模块跳转负载（一次性语义）
        consumeNavPayload();

        const auto& t = core::theme::current();
        // UAT3 T1（D-R3-1 米黄清除）：页面主体皮肤整体切 wechat（与 todo/home 同口径）。
        // 本变量只喂页面主体（树/编辑/实时/预览/大纲/状态栏链）；popup 作用域
        // （弹窗/右键菜单/toast）各自就地取 componentTokensWeChat，不经本变量。
        const auto componentTheme =
            core::theme::componentTokensWeChat(t);

        // 主题名变化 = 换色表 = bump styleRevision 触发全部可视行重高亮
        if (const std::string themeName = core::theme::currentThemeName();
            themeName != highlightThemeName_) {
            highlightThemeName_ = themeName;
            highlighter_.setColors(mdHighlightColors(t));
            ++ui.state<components::input_detail::InputModel::InputState>(
                      kEditorInputId)
                 .styleRevision;
            liveHighlighter_.setColors(mdHighlightColors(t));
            ++ui.state<components::input_detail::InputModel::InputState>(
                      kLiveInputId)
                 .styleRevision;
        }

        const float pad = t.sizes.spacingXXLarge; // 40
        const float contentW = screen.width - pad * 2.0f;
        const float contentH = screen.height - pad * 2.0f;
        // UAT3 D-R3-3 c：面板宽每帧向目标 lerp（固定系数），距目标 <1px 即钉住；
        // treePanelCollapsed_（bool）仍为唯一持久化口径，本值启动时按折叠态直接落位。
        // sideW 取连续值 → midW/列宽随动画平滑过渡，窄窗下不再二值跳变。
        const float treeTargetW = treePanelCollapsed_ ? 0.0f : kTreePanelWidth;
        treePanelWidth_ += (treeTargetW - treePanelWidth_) * kTreePanelAnimK;
        if (std::fabs(treeTargetW - treePanelWidth_) < kTreePanelSnapEps) {
            treePanelWidth_ = treeTargetW;
        }
        const float sideW = treePanelWidth_;
        const float gap = t.sizes.spacingLarge;
        const float midW = (contentW - sideW - gap * 2.0f) / 2.0f;
        const float bodyH = contentH - ui_kit::kHeaderH - t.sizes.spacingLarge;

        // 页面底色 = 主窗口 clearColor，不自铺不透明底（半透/磨砂档才生效）

        ui.column("md.root")
            .size(screen.width, screen.height)
            .padding(pad)
            .gap(t.sizes.spacingLarge)
            .content([&] {
                drawHeader(ui, contentW, contentH, bodyH, t);
                drawBody(ui, contentW, bodyH, sideW, midW, gap, t, componentTheme);
            })
            .build();

        // UAT3 D-R3-3 b：完全收起后，编辑区左上角常显的圆形展开浮钮
        drawTreeExpandFab(ui, screen, pad, t);

        // UAT5 真机收敛泵：lerp 只在 compose 里推进，而真机主循环是事件驱动——无输入
        // 即 glfwWaitEvents 睡眠，且 runtime 的 animating 位只认 DSL 管理的动画
        //（onFrame/过渡/GIF/滚动惯性），本手搓 treePanelWidth_ lerp 对它不可见。
        // 后果（真机两报同根因）：点折叠/展开 → 点击帧只推进一步（240→~197）即冻结，
        // 面板壳仍 ~240、内容空（<220 不画）、浮钮（>1px）不上屏；其后每次任意输入
        //（如反复开关大纲）各供一帧，面板再向目标爬一步 → 编辑列 x 随之逐轮漂移。
        // headless 直泵 compose 40 帧绕过主循环策略，故既有测试恒绿、真机分叉。
        // 修法 = carousel 同款模式：动画在途挂 1x1 空 onFrame 元素——runtime 每帧经
        // updateFrameCallback 回调它并置 animating_/composeRequested_/paintRequested_
        // → 主循环持续出帧推进 compose 直至钉住；收敛后本元素不再挂载即被剪除，
        // 循环回睡，零常驻功耗。条件与上方钉住判定互补（<eps 钉住 / >=eps 挂泵），
        // 不存在「在途无泵」或「收敛常驻泵」的缝隙。
        if (std::fabs(treeTargetW - treePanelWidth_) >= kTreePanelSnapEps) {
            ui.stack("md.tree.anim.driver")
                .size(1.0f, 1.0f)
                .onFrame([](float) {})
                .build();
        }

        drawOutlinePanel(ui, screen, t, componentTheme);

        drawNewNoteDialog(ui, screen, t, componentTheme);
        drawNewFolderDialog(ui, screen, t, componentTheme);
        // 右键菜单 zIndex 1050 盖过 dialog 默认 1000；菜单项点击先 dismiss 再开弹窗
        drawNoteContextMenu(ui, screen, t, componentTheme);
        drawEditorContextMenu(ui, screen, t, componentTheme);
        drawDeleteDialog(ui, screen, t, componentTheme);
        drawRenameDialog(ui, screen, t, componentTheme);
        drawToast(ui, screen, componentTheme);
    }

private:
    // Esc 关最上层弹窗/浮层（顺序 = z 序倒序）。返回 true = 本键已消费，无弹窗返回 false。
    bool closeTopmostPopup() {
        if (renameOpen_) { renameOpen_ = false; return true; }
        if (deleteConfirmOpen_) { deleteConfirmOpen_ = false; return true; }
        if (editorCtxOpen_) { editorCtxOpen_ = false; return true; }
        if (ctxMenuOpen_) { ctxMenuOpen_ = false; return true; }
        if (newFolderOpen_) {
            newFolderOpen_ = false;
            newFolderParentOpen_ = false;
            return true;
        }
        if (newNoteOpen_) { newNoteOpen_ = false; return true; }
        return false;
    }

    void drawHeader(eui::Ui& ui,
                    float contentW,
                    float contentH,
                    float bodyH,
                    const core::theme::Tokens& t) {
        (void)contentH;
        (void)bodyH;
        // UAT3 T1：页面头部控件（大纲开关）随页面主体切 wechat；开关主色 = 微信绿。
        const auto componentTheme =
            core::theme::componentTokensWeChat(t);
        const std::string subtitle =
            service_
                ? "Ctrl+Shift+P 切换视图 · 按 ? 查看快捷键"
                : "尚未初始化笔记目录";

        ui.row("md.header")
            .size(contentW, ui_kit::kHeaderH)
            .alignItems(eui::Align::CENTER)
            .content([&] {
                const float segW = 264.0f;
                const float actionsW = 304.0f;
                ui.column("md.title")
                    .size(contentW - actionsW - segW, ui_kit::kHeaderH)
                    .gap(2.0f)
                    .content([&] {
                        ui.text("md.h1")
                            .size(contentW - actionsW - segW, 32.0f)
                            .text("Markdown 笔记本")
                            .fontSize(t.sizes.fontSizeTitle)
                            .lineHeight(32.0f)
                            .fontWeight(700)
                            .color(t.colors.wechat.textPrimary)
                            .build();
                        // subtitle 包 clip 栈：内层省略号管排版宽，栈 .clip() 兜底绘制范围
                        const float subW = contentW - actionsW - segW;
                        ui.stack("md.subtitle.clip")
                            .size(subW, 18.0f)
                            .clip()
                            .content([&] {
                                ui.text("md.subtitle")
                                    .size(subW, 18.0f)
                                    .text(subtitle)
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .lineHeight(18.0f)
                                    .color(t.colors.wechat.textSecondary)
                                    .maxWidth(subW)
                                    .overflow(core::TextOverflow::Ellipsis)
                                    .build();
                            })
                            .build();
                    })
                    .build();

                // 微信风格 Tab（§4.2）：2px 绿下划线激活、灰字未激活。
                // 保 md.mode 作 wrapper stack id（ui.find("md.mode") 仍绿）；
                // 子 Tab id = md.mode.0..3，hit/text/underline 按 wtab 约定。
                // 标签逐字「编辑/实时/双栏/预览」不变（modeOrderIndex 逆映射）。
                const float tabW = segW / 4.0f;
                ui.stack("md.mode")
                    .size(segW, 36.0f)
                    .content([&] {
                        for (int i = 0; i < 4; ++i) {
                            ui_kit::wtab(
                                ui,
                                std::string("md.mode.") + std::to_string(i),
                                std::vector<std::string>{"编辑", "实时", "双栏",
                                                         "预览"}[i],
                                modeOrderIndex(mode_) == i,
                                static_cast<float>(i) * tabW, 0.0f, tabW,
                                36.0f, t,
                                [this, i] { setMode(modeFromOrderIndex(i)); });
                        }
                    })
                    .build();

                ui.row("md.actions")
                    .size(actionsW, 40.0f)
                    .justifyContent(eui::Align::END)
                    .alignItems(eui::Align::CENTER)
                    .gap(t.sizes.spacingSmall)
                    .content([&] {
                        // 快捷键帮助入口（§5.4）：「?」图标，待 EUI tooltip 绑定后
                        // hover 弹快捷键面板。0xF059 = fa-circle-question 占位。
                        ui.text("md.help.icon")
                            .size(24.0f, 24.0f)
                            .icon(0xF059)
                            .fontSize(16.0f)
                            .lineHeight(24.0f)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .color(t.colors.wechat.textSecondary)
                            .build();

                        components::toggleSwitch(ui, "md.outline.toggle")
                            .size(112.0f, 36.0f)
                            .checked(outlineOpen_)
                            .text("大纲")
                            .fontSize(t.sizes.fontSizeSmall)
                            .theme(componentTheme)
                            .onChange([this](bool v) { outlineOpen_ = v; })
                            .build();

                        // 微信绿主按钮（§4.1 Primary）：wechat 令牌填充 + 白字。
                        // 用 componentTokensWeChat 给 button 组件绿底；row 布局定位不受 ui_kit::button position 干扰。
                        const auto wechatTheme = core::theme::componentTokensWeChat(t);
                        components::button(ui, "md.btn.new")
                            .size(124.0f, 36.0f)
                            .text("+ 新建笔记")
                            .fontSize(t.sizes.fontSizeBody)
                            .theme(wechatTheme, true)
                            .textColor(t.colors.wechat.onPrimary)
                            .radius(t.sizes.radiusWeChat)
                            .onClick([this] { openNewNoteDialog(); })
                            .build();
                    })
                    .build();
            })
            .build();
    }

    void drawBody(eui::Ui& ui,
                  float contentW,
                  float bodyH,
                  float sideW,
                  float midW,
                  float gap,
                  const core::theme::Tokens& t,
                  const components::theme::ThemeColorTokens& componentTheme) {
        const float fullW = contentW - sideW - gap;
        ui.row("md.body")
            .size(contentW, bodyH)
            .gap(gap)
            .content([&] {
                drawTreeColumn(ui, bodyH, sideW, t, componentTheme);
                if (mode_ == ViewMode::Live) {
                    drawLiveColumn(ui, bodyH, fullW, t, componentTheme);
                    return;
                }
                if (mode_ != ViewMode::Preview) {
                    drawEditorColumn(ui, bodyH, mode_ == ViewMode::Split ? midW : fullW,
                                     t, componentTheme);
                }
                if (mode_ != ViewMode::Edit) {
                    drawPreviewColumn(ui, bodyH, mode_ == ViewMode::Split ? midW : fullW,
                                      t, componentTheme, gap);
                }
            })
            .build();
    }

    // D-R3-3 b：收起态展开浮钮——面板完全收起（宽 0）后，编辑区左上角常显的
    // 圆形 28px 微信风格钮（chevron-right = 向右展开）。页面级浮层（zIndex 5，
    // 低于大纲面板 10 / 弹窗 1000），常驻 id md.tree.expand.btn（含 .bg/.hit 子件）。
    void drawTreeExpandFab(eui::Ui& ui,
                           const eui::Screen& screen,
                           float pad,
                           const core::theme::Tokens& t) {
        (void)screen;
        // 收起动画未收敛（宽 >1px）不画：避免收起途中浮钮与残影同屏。
        if (treePanelWidth_ > kTreePanelSnapEps) return;
        // 位置：body 左上角——横跨页边留白/树列空槽缝，右缘轻搭编辑列左缘
        //（收起后编辑列 x = pad + gap，浮钮右缘 62 < 标签 chip 起点 64，互不压）。
        const float fabY = pad + ui_kit::kHeaderH + t.sizes.spacingLarge + 8.0f;
        ui.stack("md.tree.expand")
            .position(pad - 6.0f, fabY)
            .size(28.0f, 28.0f)
            .zIndex(5)
            .content([&] {
                components::button(ui, "md.tree.expand.btn")
                    .size(28.0f, 28.0f)
                    .icon(0xF054)  // fa-chevron-right
                    .fontSize(14.0f)
                    .theme(core::theme::componentTokensWeChat(t), false)
                    .textColor(t.colors.wechat.textSecondary)
                    .radius(t.sizes.radiusPill)  // 圆形
                    .onClick([this] { toggleTreePanel(); })
                    .build();
            })
            .build();
    }

    void drawTreeColumn(eui::Ui& ui,
                        float bodyH,
                        float sideW,
                        const core::theme::Tokens& t,
                        const components::theme::ThemeColorTokens& componentTheme) {
        // 单 stack 列 = bg rect + 绝对定位内容；bg 与 stack 两侧尺寸必须精确重合，
        // 测试 md_tree_column_layout_semantics 锁定（错位会静默溢出）。
        // D-R3-3 b：完全收起时本列仍以 0 宽占位（保持 md.body 行间隙公式与右缘
        // 对齐不变），但不画 bg/内容——展开入口移交 drawTreeExpandFab 浮钮。
        ui.stack("md.tree").size(sideW, bodyH).content([&] {
            if (sideW <= kTreePanelSnapEps) return;
            // 白底面板（§4.14：wechat.cardBg #FFFFFF）+ 右侧 1px 分割线（§4.7）。
            // 去圆角/去 border：flat 白卡，右分割线单独画。
            ui.rect("md.tree.bg")
                .size(sideW, bodyH)
                .color(t.colors.wechat.cardBg)
                .build();
            ui.rect("md.tree.divider")
                .position(sideW - 1.0f, 0.0f)
                .size(1.0f, bodyH)
                .color(t.colors.wechat.dividerPrimary)
                .build();
            // 收起中 / 展开动画未到门槛：只画面板壳（bg/divider），内容不画——
            // 窄板上搜索框/按钮的固定几何会破版；内容左锚定，展开末段自然滑入。
            if (treePanelCollapsed_ || sideW < kTreePanelContentMinW) return;
            // D-R3-3 a：面板头——「目录」标题 + 右缘独立折叠钮（不再塞进搜索框行
            // 当第三键）。头行 y 8..40；搜索框行随之下移 16→48，标签/列表基准 +32。
            ui.text("md.tree.head.title")
                .position(16.0f, 8.0f)
                .size(sideW - 60.0f, 32.0f)
                .text("目录")
                .fontSize(kTreeFontSize)
                .lineHeight(32.0f)
                .fontWeight(700)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.wechat.textPrimary)
                .build();
            components::button(ui, "md.tree.collapse")
                .position(sideW - 44.0f, 10.0f)
                .size(28.0f, 28.0f)
                .icon(0xF053)  // fa-chevron-left（收起 = 面板向左收）
                .fontSize(14.0f)
                .theme(componentTheme, false)
                .textColor(t.colors.wechat.textSecondary)
                .radius(t.sizes.radiusWeChat)
                .onClick([this] { toggleTreePanel(); })
                .build();
            // 搜索框 + 右侧两个图标按钮（折叠键已上移面板头；§3.3.3 新建按钮上移）。
            // 搜索框窄至留 64px 给两个 28px 图标按钮 + 间距。
            const float searchW = sideW - 96.0f;  // 16 左 + 4 gap + 60 btns + 16 右
            components::input(ui, "md.search")
                .position(16.0f, 48.0f)
                .size(searchW, 40.0f)
                .value(searchQuery_)
                .placeholder("搜索笔记…")
                .fontSize(kTreeFontSize)
                .inset(32.0f)
                .theme(componentTheme)
                .onChange([this](const std::string& v) {
                    searchQuery_ = v;
                    refreshList();
                })
                .build();
            ui.text("md.search.icon")
                .position(28.0f, 59.0f)
                .size(18.0f, 18.0f)
                .icon(0xF002)  // fa-magnifying-glass
                .fontSize(kTreeFontSize)
                .lineHeight(18.0f)
                .color(t.colors.wechat.textTertiary)
                .build();
            // 清空按钮（补充项 §3.2.4）：搜索非空时显示「×」，点击清空。
            if (!searchQuery_.empty()) {
                ui.rect("md.search.clear")
                    .position(16.0f + searchW - 28.0f, 48.0f)
                    .size(28.0f, 40.0f)
                    .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                    .hoverColor(t.colors.wechat.dividerSecondary)
                    .onClick([this] {
                        searchQuery_.clear();
                        refreshList();
                    })
                    .build();
                ui.text("md.search.clear.icon")
                    .position(16.0f + searchW - 28.0f, 48.0f)
                    .size(28.0f, 40.0f)
                    .text("×")
                    .fontSize(14.0f)
                    .lineHeight(40.0f)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.wechat.textTertiary)
                    .build();
            }
            // +笔记 / +文件夹 图标按钮（从面板底部上移到搜索框右侧，§3.3.3）
            // 0xF067 = fa-plus；onClick 接原回调不变。UAT3 D-R3-3：折叠键上移面板头后
            // 两键贴右缘（右 padding 16 与左对称），行内不再有第三键。
            const float btnX0 = sideW - 76.0f;
            const float btnX1 = sideW - 44.0f;
            components::button(ui, "md.tree.new")
                .position(btnX0, 48.0f)
                .size(28.0f, 40.0f)
                .text("+")
                .fontSize(t.sizes.fontSizeBody)
                .theme(componentTheme, false)
                .textColor(t.colors.wechat.textSecondary)
                .radius(t.sizes.radiusWeChat)
                .onClick([this] { openNewNoteDialog(); })
                .build();
            components::button(ui, "md.tree.newfolder")
                .position(btnX1, 48.0f)
                .size(28.0f, 40.0f)
                .text("📁")
                .fontSize(12.0f)
                .theme(componentTheme, false)
                .textColor(t.colors.wechat.textSecondary)
                .radius(t.sizes.radiusWeChat)
                .onClick([this] {
                    newFolderOpen_ = true;
                    newFolderName_.clear();
                    newFolderError_.clear();
                    newFolderParentDir_ = newItemParentDir();
                    newFolderParentOpen_ = false;
                })
                .build();

            // D-R3-3 a：搜索框行高 40（y48..88），标签区下移保持 8px 呼吸
            const float tagY = 96.0f;
            float tagAreaH = 0.0f;
            if (!allTags_.empty()) {
                ui.text("md.tags.title")
                    .position(16.0f, tagY)
                    .size(sideW - 32.0f, 14.0f)
                    .text("标签")
                    .fontSize(10.0f)
                    .lineHeight(14.0f)
                    .fontWeight(700)
                    .color(t.colors.wechat.textTertiary)
                    .build();
                const float fs = 11.0f;
                const float rowH = 24.0f;
                // UAT2 F批：chip 左/右缘与面板 padding 16 对齐；间距 4→6
                float cx = 16.0f;
                float cy = tagY + 18.0f;
                int row = 0;
                for (size_t i = 0; i < allTags_.size(); ++i) {
                    const auto& tc = allTags_[i];
                    const std::string label =
                        tc.name + "·" + std::to_string(tc.noteCount);
                    const std::string id = "md.tags.filter." + std::to_string(i);
                    const float chipW = 16.0f + estimateTextWidth(label, fs);
                    if (cx + chipW > sideW - 16.0f) {
                        cx = 16.0f;
                        cy += rowH + 6.0f;
                        if (++row >= 3) break;
                    }
                    const bool active = tagFilter_ == tc.name;
                    ui.stack(id)
                        .position(cx, cy)
                        .size(chipW, rowH)
                        .content([&] {
                            ui.rect(id + ".bg")
                                .size(chipW, rowH)
                                .color(active ? t.colors.wechat.primary : t.colors.wechat.dividerSecondary)
                                .hoverColor(t.colors.wechat.selectedBg)
                                .radius(t.sizes.radiusWeChat)
                                .onClick([this, name = tc.name] {
                                    toggleTagFilter(name);
                                })
                                .build();
                            ui.text(id + ".name")
                                .position(0.0f, 0.0f)
                                .size(chipW, rowH)
                                .text(label)
                                .fontSize(fs)
                                .lineHeight(rowH)
                                .horizontalAlign(eui::HorizontalAlign::Center)
                                // 文本盒 == 胶囊高，须显式垂直居中（缺它墨迹从行框顶起排）
                                .verticalAlign(eui::VerticalAlign::Center)
                                .color(active ? t.colors.wechat.onPrimary
                                              : t.colors.wechat.textSecondary)
                                .build();
                        })
                        .build();
                    cx += chipW + 6.0f;
                }
                tagAreaH = 18.0f + static_cast<float>(row + 1) * (rowH + 6.0f);
            }

            // 搜索时树展平为命中文件列表；空搜索 = 二级目录树
            // D-R3-3 a：搜索框行下移后基准 68→100（保持与搜索框底 12px 呼吸）
            const float listY = 100.0f + tagAreaH;
            // 新建按钮已上移到搜索框右侧，底部不再预留按钮区——只留 16px 底 padding。
            const float listH = std::max(0.0f, bodyH - listY - 16.0f);

            if (!listError_.empty()) {
                ui.text("md.tree.error")
                    .position(16.0f, listY + 8.0f)
                    .size(sideW - 32.0f, 60.0f)
                    .text("列表加载失败：\n" + listError_ +
                          "\n每 5 秒自动重试")
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(18.0f)
                    .wrap()
                    .color(t.colors.wechat.warningRed)
                    .build();
            } else if (notes_.empty()) {
                if (searchQuery_.empty() && tagFilter_.empty()) {
                    drawTreeList(ui, listY, listH, sideW, t, componentTheme);
                    // 有文件夹时只留树：提示行会压住第一个文件夹行，空文件夹本身就是内容
                    if (allFolders_.empty()) {
                        ui.text("md.tree.empty")
                            .position(36.0f, listY + 40.0f)
                            .size(sideW - 48.0f, 40.0f)
                            .text("还没有笔记\n点右上角 + 开始")
                            .fontSize(t.sizes.fontSizeSmall)
                            .lineHeight(18.0f)
                            .wrap()
                            .color(t.colors.wechat.textTertiary)
                            .build();
                    }
                } else {
                    ui.text("md.tree.empty")
                        .position(16.0f, listY + 8.0f)
                        .size(sideW - 32.0f, 40.0f)
                        .text("没有匹配的笔记")
                        .fontSize(t.sizes.fontSizeSmall)
                        .lineHeight(18.0f)
                        .wrap()
                        .color(t.colors.wechat.textTertiary)
                        .build();
                }
            } else if (!searchQuery_.empty()) {
                drawNoteList(ui, listY, listH, sideW, t, componentTheme);
            } else {
                drawTreeList(ui, listY, listH, sideW, t, componentTheme);
            }
            // +笔记/+文件夹 已上移到搜索框右侧（见上方），底部不再有按钮。
        }).build();
    }

    void drawNoteList(eui::Ui& ui,
                      float listY,
                      float listH,
                      float sideW,
                      const core::theme::Tokens& t,
                      const components::theme::ThemeColorTokens& componentTheme) {
        const float rowW = sideW - 16.0f;
        // contentKey 用逐条路径组合 hash：同数量替换内容也触发重建
        size_t listKey = 0;
        for (const auto& n : notes_) {
            listKey = listKey * 1315423911u +
                      std::hash<std::string>{}(noteDisplayPath(n));
        }
        components::scrollView(ui, "md.tree.scroll")
            .position(8.0f, listY)
            .size(sideW - 16.0f, listH)
            .gap(4.0f)  // UAT2 F批：行间留白 0→4
            .step(40.0f)
            .theme(componentTheme)
            .contentKey("md.tree.scroll." + std::to_string(listKey))
            .content([&](eui::Ui& inner, float contentWidth, float viewportHeight) {
                (void)viewportHeight;
                const float w = std::min(contentWidth, rowW);
                for (size_t i = 0; i < notes_.size(); ++i) {
                    const NoteMeta& n = notes_[i];
                    const std::string path = noteDisplayPath(n);
                    const std::string id = "md.tree.item." + std::to_string(i);
                    drawFileTreeItem(inner, id, w, t, 0xF15C, noteDisplayName(n),
                                     path == currentPath_, 0.0f,
                                     [this, path] { openNote(path); },
                                     [this, path](const eui::PointerEvent& ev,
                                                  const eui::Rect&) {
                                         openFileContextMenu(path, ev);
                                     }, searchQuery_);
                }
            })
            .build();
    }

    void drawTreeList(eui::Ui& ui,
                      float listY,
                      float listH,
                      float sideW,
                      const core::theme::Tokens& t,
                      const components::theme::ThemeColorTokens& componentTheme) {
        const std::vector<MdTreeRow> rows = buildTreeRows(notes_, allFolders_,
                                                          collapsedDirs_);
        const float rowW = sideW - 16.0f;
        // contentKey：行内容（类型 + 名字 + 路径）组合 hash，内容变即重建
        size_t treeKey = 0;
        for (const auto& r : rows) {
            treeKey = treeKey * 1315423911u +
                      std::hash<std::string>{}(r.name + "\x1f" + r.relPath) +
                      (r.isFolder ? 1u : 0u) + static_cast<size_t>(r.depth);
        }
        components::scrollView(ui, "md.tree.scroll")
            .position(8.0f, listY)
            .size(sideW - 16.0f, listH)
            .gap(4.0f)  // UAT2 F批：行间留白 0→4
            .step(40.0f)
            .theme(componentTheme)
            .contentKey("md.tree.scroll." + std::to_string(treeKey))
            .content([&](eui::Ui& inner, float contentWidth, float viewportHeight) {
                (void)viewportHeight;
                const float w = std::min(contentWidth, rowW);
                for (size_t i = 0; i < rows.size(); ++i) {
                    const MdTreeRow& r = rows[i];
                    const std::string id = "md.tree.node." + std::to_string(i);
                    if (r.isFolder) {
                        // 目录行：行体点击 = 选中目录，箭头命中区 = 收起/展开
                        //（命中取最上层兄弟，故命中区须叠在行体之上）。缩进每级 16px。
                        const float indent = static_cast<float>(r.depth) * 16.0f;
                        const bool expanded = collapsedDirs_.count(r.relPath) == 0;
                        const bool selected = r.relPath == selectedDir_;
                        inner.stack(id)
                            .size(w, kTreeRowHeight)
                            .content([&] {
                                inner.rect(id + ".row")
                                    .size(w, kTreeRowHeight)
                                    .color(selected ? t.colors.wechat.selectedBg
                                                    : t.colors.wechat.cardBg)
                                    .hoverColor(t.colors.wechat.dividerSecondary)
                                    .radius(t.sizes.radiusWeChat)
                                    .onClick([this, dir = r.relPath] { selectDir(dir); })
                                    .build();
                                // 选中态左 2px 绿竖线（§4.3）
                                if (selected) {
                                    inner.rect(id + ".accent")
                                        .position(indent, 0.0f)
                                        .size(2.0f, kTreeRowHeight)
                                        .color(t.colors.wechat.primary)
                                        .build();
                                }
                                // 展开/收起命中区（叠在行体之上，命中取最上层）。
                                // UAT2 F批：26→46（箭头+文件夹图标整区可点）+ hover
                                // 反馈（原全透明无提示；三态同款 app.sidebar.toggle）。
                                // 宽度止于名称区起点（46+indent）：命中取最上层兄弟，
                                // 再宽会把行体的 selectDir（点名称选中目录）整个吞掉。
                                inner.rect(id + ".toggle")
                                    .position(indent, 0.0f)
                                    .size(46.0f, kTreeRowHeight)
                                    .states(eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                                            t.colors.wechat.dividerSecondary,
                                            t.colors.wechat.selectedBg)
                                    .radius(t.sizes.radiusWeChat)
                                    .onClick([this, dir = r.relPath] {
                                        if (collapsedDirs_.count(dir) != 0) {
                                            collapsedDirs_.erase(dir);
                                        } else {
                                            collapsedDirs_.insert(dir);
                                        }
                                        saveViewState();  // 目录折叠跨会话记忆
                                    })
                                    .build();
                                inner.text(id + ".arrow")
                                    .position(6.0f + indent, 0.0f)
                                    .size(16.0f, kTreeRowHeight)
                                    .text(expanded ? "▾" : "▸")
                                    .fontSize(kTreeArrowFontSize)
                                    .lineHeight(kTreeRowHeight)
                                    .verticalAlign(eui::VerticalAlign::Center)
                                    .color(t.colors.wechat.textTertiary)
                                    .build();
                                inner.text(id + ".ficon")
                                    .position(26.0f + indent, 0.0f)
                                    .size(18.0f, kTreeRowHeight)
                                    .icon(0xF07B)  // fa-folder
                                    .fontSize(kTreeFontSize)
                                    .lineHeight(kTreeRowHeight)
                                    .verticalAlign(eui::VerticalAlign::Center)
                                    .color(selected ? t.colors.wechat.primary
                                                    : t.colors.wechat.textTertiary)
                                    .build();
                                inner.text(id + ".name")
                                    .position(46.0f + indent, 0.0f)
                                    .size(std::max(0.0f, w - 58.0f - indent),
                                          kTreeRowHeight)
                                    .text(r.name)
                                    .fontSize(kTreeFontSize)
                                    .lineHeight(kTreeRowHeight)
                                    .verticalAlign(eui::VerticalAlign::Center)
                                    .color(selected ? t.colors.wechat.textPrimary
                                                    : t.colors.wechat.textPrimary)
                                    .fontWeight(700)
                                    .build();
                            })
                            .build();
                    } else {
                        // 文件行缩进 = 32 + (depth-1)*16；文件名与文件夹名错开 22px
                        const float indent =
                            32.0f + static_cast<float>(std::max(0, r.depth - 1)) * 16.0f;
                        drawFileTreeItem(inner, id, w, t, 0xF15C, r.name,
                                         r.relPath == currentPath_,
                                         indent,
                                         [this, path = r.relPath] { openNote(path); },
                                         [this, path = r.relPath](
                                             const eui::PointerEvent& ev,
                                             const eui::Rect&) {
                                             openFileContextMenu(path, ev);
                                         });
                    }
                }
            })
            .build();
    }

    void drawOutlinePanel(eui::Ui& ui,
                          const eui::Screen& screen,
                          const core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& componentTheme) {
        if (!outlineOpen_) return;
        const float pad = t.sizes.spacingXXLarge; // 与 compose 同基准（40）
        const float panelW = 264.0f;
        const float panelH = screen.height - pad * 2.0f - ui_kit::kHeaderH -
                             t.sizes.spacingLarge;
        const float x = screen.width - pad - panelW;
        const float y = pad + ui_kit::kHeaderH + t.sizes.spacingLarge;

        ui.stack("md.outline.panel")
            .position(x, y)
            .size(panelW, panelH)
            .zIndex(10)
            .content([&] {
                // UAT3 T1：大纲浮面板原为暖橙 surfaceElevated 米色底（米黄根源之一），
                // 随页面主体整体 wechat 化——白卡底 + 一级分割线边框 + wechat 文字档。
                ui.rect("md.outline.panel.bg")
                    .size(panelW, panelH)
                    .color(t.colors.wechat.cardBg)
                    .radius(t.sizes.radiusLarge)
                    .border(1.0f, t.colors.wechat.dividerPrimary)
                    .build();
                ui.text("md.outline.panel.title")
                    .position(16.0f, 10.0f)
                    .size(120.0f, 24.0f)
                    .text("大纲")
                    .fontSize(t.sizes.fontSizeBody)
                    .lineHeight(24.0f)
                    .fontWeight(700)
                    .color(t.colors.wechat.textPrimary)
                    .build();
                components::button(ui, "md.outline.panel.close")
                    .position(panelW - 44.0f, 8.0f)
                    .size(32.0f, 28.0f)
                    .text("✕")
                    .fontSize(t.sizes.fontSizeSmall)
                    .theme(componentTheme, false)
                    .textColor(t.colors.wechat.textSecondary)
                    .radius(t.sizes.radius)
                    .onClick([this] { outlineOpen_ = false; })
                    .build();

                if (outline_.empty()) {
                    ui.text("md.outline.panel.empty")
                        .position(16.0f, 52.0f)
                        .size(panelW - 32.0f, 60.0f)
                        .text(currentPath_.empty() ? "先打开一篇笔记"
                                                   : "没有标题\n用 # 开始一行即可加入大纲")
                        .fontSize(t.sizes.fontSizeSmall)
                        .lineHeight(18.0f)
                        .wrap()
                        .color(t.colors.wechat.textTertiary)
                        .build();
                    return;
                }

                // 当前节：光标行所在的上一个标题
                const int active = activeOutlineIndex(ui);

                size_t outlineKey = 0;
                for (const auto& h : outline_) {
                    outlineKey = outlineKey * 1315423911u +
                                 std::hash<std::string>{}(h.title) +
                                 static_cast<size_t>(h.level);
                }
                components::scrollView(ui, "md.outline.scroll")
                    .position(8.0f, 44.0f)
                    .size(panelW - 16.0f, std::max(0.0f, panelH - 52.0f))
                    .gap(0.0f)
                    .step(40.0f)
                    .theme(componentTheme)
                    .contentKey("md.outline.scroll." + std::to_string(outlineKey))
                    .content([&](eui::Ui& inner, float contentWidth,
                                 float viewportHeight) {
                        (void)viewportHeight;
                        const float rowW = std::min(contentWidth, panelW - 16.0f);
                        for (size_t i = 0; i < outline_.size(); ++i) {
                            const MdHeading& h = outline_[i];
                            const std::string id =
                                "md.outline.item." + std::to_string(i);
                            const bool current = static_cast<int>(i) == active;
                            inner.stack(id)
                                .size(rowW, 30.0f)
                                .content([&] {
                                    // UAT3 T1：行底/悬停/文字全部 wechat 化
                                    //（选中 = 浅灰 selectedBg + 绿字，未选 = 白卡底 + 一级文字）
                                    inner.rect(id + ".row")
                                        .size(rowW, 30.0f)
                                        .color(current ? t.colors.wechat.selectedBg
                                                       : t.colors.wechat.cardBg)
                                        .hoverColor(t.colors.wechat.dividerSecondary)
                                        .radius(t.sizes.radiusSmall)
                                        .onClick([this, i, &ui, componentTheme] {
                                            jumpToHeading(ui, i, componentTheme);
                                        })
                                        .build();
                                    inner.text(id + ".label")
                                        .position(12.0f + static_cast<float>(h.level - 1) * 14.0f,
                                                  0.0f)
                                        .size(std::max(0.0f, rowW - 20.0f -
                                                       static_cast<float>(h.level - 1) * 14.0f),
                                              30.0f)
                                        .text(h.title.empty() ? "(无标题)" : h.title)
                                        .fontSize(t.sizes.fontSizeSmall)
                                        .lineHeight(30.0f)
                                        .verticalAlign(eui::VerticalAlign::Center)
                                        .color(current ? t.colors.wechat.primary
                                                       : t.colors.wechat.textPrimary)
                                        .fontWeight(current ? 700 : 400)
                                        .build();
                                })
                                .build();
                        }
                    })
                    .build();
            })
            .build();
    }

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
            // 右区底色 = wechat.pageBg（§4.14：浅灰底 #F7F7F7，白卡浮于其上）。
            // 旧「不铺列底」已废：D-12 强制内容区底为浅灰，编辑器/预览白卡浮起。
            ui.rect(columnId + ".bg")
                .size(midW, bodyH)
                .color(t.colors.wechat.pageBg)
                .build();
            if (currentPath_.empty()) {
                ui.text(columnId + ".hint")
                    .position(24.0f, 50.0f)
                    .size(midW - 48.0f, 40.0f)
                    .text("从左栏选一篇笔记开始编辑")
                    .fontSize(t.sizes.fontSizeBody)
                    .lineHeight(20.0f)
                    .color(t.colors.wechat.textTertiary)
                    .build();
                drawStatusBar(ui, 0.0f, bodyH - 22.0f, midW, t);
                return;
            }

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
                          const components::theme::ThemeColorTokens& componentTheme) {
        (void)componentTheme;
        // 布局语义与耦合点见 drawTreeColumn 注释。
        drawTextColumnChrome(
            ui, "md.editor", bodyH, midW, t,
            componentTheme, [&](const TextColumnGeometry& g) {
                components::input(ui, kEditorInputId)
                    .position(g.x, g.y)
                    .size(g.w, g.h)
                    .value(editorText_)
                    .placeholder("在这里写 Markdown…")
                    .multiline(true)
                    .scrollbar(true)
                    .fontSize(core::theme::editorFontSize())
                    .inset(10.0f)
                    .theme(componentTheme)
                    .onChange([this](const std::string& v) {
                        // 输入更新内存缓冲；落盘走防抖（见 flushEditorIfIdle）
                        onEditorTextChanged(v);
                    })
                    // 聚焦时先于内置处理收到按键，返回 true = 拦截。
                    .onKeyEvent([this, &ui](const eui::KeyEvent& event) {
                        return handleEditorKey(ui, event);
                    })
                    // 行级高亮（styled-runs）：回调只给行文本/行首偏移，围栏状态需全文 → 从
                    // InputState 取 display 文本（IME 期间是 preedit 快照，字节偏移不会错位）。
                    .lineStyles([this, &ui](const std::string& lineText,
                                            int lineStartByte, int lineNo) {
                        (void)lineNo;
                        using InputState =
                            components::input_detail::InputModel::InputState;
                        auto& st = ui.state<InputState>(kEditorInputId);
                        return highlighter_.runsForLine(
                            st.preedit ? st.preedit->text : st.text, lineText,
                            lineStartByte);
                    })
                    // 输入框右键菜单直挂 input（onContextMenu），坐标经统一换算（窗口 → 页面坐标）。
                    .onContextMenu([this](const eui::PointerEvent& ev,
                                          const eui::Rect&) {
                        const core::MenuAnchor a =
                            menuAnchorForPointerInPage(ev);
                        editorCtxX_ = a.x;
                        editorCtxY_ = a.y;
                        editorCtxOpen_ = true;
                    })
                    .build();
            });
    }

    // 键盘流接线（纯函数在 md_keyboard）
    // 键位：Tab/Shift+Tab 缩进、Enter 列表续行、Ctrl+B/I 包裹、Ctrl+K 链接。
    // 只做 IME 防御 → 纯函数 → 写回 InputState；无变化的拦截只消费不动状态。
    bool handleEditorKey(eui::Ui& ui, const eui::KeyEvent& event) {
        using InputState = components::input_detail::InputModel::InputState;
        if (!event.isDown()) return false;
        auto& st = ui.state<InputState>(kEditorInputId);
        // IME 预编辑中不拦截（Ctrl 类修饰键 IME 也可能用）
        if (!st.compositionText.empty()) return false;

        EditBuffer buf{st.text, st.cursor, st.selectionStart, st.selectionEnd};
        const bool shortcut = event.modifiers.shortcut();
        const bool plain = !shortcut && !event.modifiers.shift &&
                           !event.modifiers.alt && !event.modifiers.super;
        bool handled = false;
        if (event.key == eui::InputKey::Tab && !shortcut && !event.modifiers.alt) {
            if (!applyTabTable(buf, event.modifiers.shift)) {
                applyTab(buf, event.modifiers.shift);
            }
            handled = true;
        } else if (event.key == eui::InputKey::Enter && plain) {
            handled = applyEnterList(buf);
        } else if (shortcut && !event.modifiers.shift && !event.modifiers.alt) {
            if (event.key == eui::InputKey::B) {
                applyWrapAround(buf, "**");
                handled = true;
            } else if (event.key == eui::InputKey::I) {
                applyWrapAround(buf, "*");
                handled = true;
            } else if (event.key == eui::InputKey::K) {
                applyLinkSkeleton(buf);
                handled = true;
            }
        }
        if (!handled) return false;
        commitEditBuffer(ui, buf);
        return true;
    }

    // 键盘流与「插入表格」共用写回。无变化只消费不动状态，不制造空 undo / 脏标记。
    void commitEditBuffer(eui::Ui& ui, EditBuffer& buf) {
        using InputState = components::input_detail::InputModel::InputState;
        auto& st = ui.state<InputState>(kEditorInputId);
        if (buf.text == st.text) {
            // 纯光标/选区移动：只更新位置，不进 undo / 不 bump revision / 不打脏。
            if (buf.cursor == st.cursor && buf.selStart == st.selectionStart &&
                buf.selEnd == st.selectionEnd) {
                return;
            }
            st.cursor = buf.cursor;
            st.selectionStart = buf.selStart;
            st.selectionEnd = buf.selEnd;
            st.hasPreferredCursorX = false;
            st.followCaret = true;
            return;
        }
        components::input_detail::InputModel::pushUndoState(st);
        st.text = std::move(buf.text);
        ++st.textRevision;
        st.cursor = buf.cursor;
        st.selectionStart = buf.selStart;
        st.selectionEnd = buf.selEnd;
        st.hasPreferredCursorX = false;
        st.followCaret = true;  // 编辑后滚到光标（同内置行为）
        onEditorTextChanged(st.text);
    }

    // onChange 与键盘流共用的副作用（内存缓冲 + 脏标记）
    void onEditorTextChanged(const std::string& v) {
        setEditorText(v);
        editorDirty_ = true;
        outlineDirty_ = true;
        lastEditAt_ = std::chrono::steady_clock::now();
    }

    // editorText_ 唯一写点。内容变化时推进修订号 + 刷新预览 hash，下游缓存
    //（几何表 / 预览 contentKey / 状态栏）以 rev 为键跨帧复用，省 O(doc) 重算。
    void setEditorText(const std::string& v) {
        if (editorText_ == v) return;
        editorText_ = v;
        ++editorTextRev_;
        previewTextHash_ = std::hash<std::string>{}(editorText_);
        // 块模型：唯一文本写点顺带 update（同文零重算），编辑只脏受影响块。
        // 分块重算耗时记录式日志（无硬断言，扫日志复核）；零重算路径不打。
        const auto tSeg0 = std::chrono::steady_clock::now();
        const MdBlockUpdate upd = blockModel_.update(editorText_);
        const auto tSeg1 = std::chrono::steady_clock::now();
        if (upd.changed) {
            char msBuf[32];
            std::snprintf(msBuf, sizeof(msBuf), "%.2f",
                          std::chrono::duration<double, std::milli>(tSeg1 - tSeg0)
                              .count());
            LOG_DEBUG("[live] segment n=" +
                      std::to_string(blockModel_.blocks().size()) + " ms=" + msBuf +
                      " dirty=[" + std::to_string(upd.dirtyFirst) + "," +
                      std::to_string(upd.dirtyLast) + "], 复用 " +
                      std::to_string(upd.reused) + " 块");
        }
    }

    // 标签 chip 行（显示 + × 删除）+ 内置添加输入（回车提交）。溢出直接截断。
    // W8：标签添加从红点加号改为输入框内置（§3.3.4），移除独立 addbtn 按钮。
    // chip 样式：浅灰底 + 标签绿字（wechat 令牌）。
    void drawTagBar(eui::Ui& ui,
                    float x,
                    float w,
                    const core::theme::Tokens& t,
                    const components::theme::ThemeColorTokens& componentTheme) {
        const auto& wc = t.colors.wechat;
        const float y = 16.0f;
        const float h = 24.0f;
        const float fs = 11.0f;
        float cx = x;
        for (size_t i = 0; i < currentTags_.size(); ++i) {
            const std::string& name = currentTags_[i];
            const std::string id = "md.tags.chip." + std::to_string(i);
            const float chipW = 10.0f + estimateTextWidth(name, fs) + 26.0f;
            if (cx + chipW > x + w) break;  // 溢出截断（见函数头注释）
            ui.stack(id)
                .position(cx, y)
                .size(chipW, h)
                .content([&] {
                    ui.rect(id + ".bg")
                        .size(chipW, h)
                        .color(wc.dividerSecondary)
                        .radius(t.sizes.radiusWeChat)
                        .build();
                    ui.text(id + ".name")
                        .position(10.0f, 0.0f)
                        .size(std::max(0.0f, chipW - 30.0f), h)
                        .text(name)
                        .fontSize(fs)
                        .lineHeight(h)
                        // 文本盒 == 胶囊高，须显式垂直居中（缺它 ink 从行框顶沿起排 → 字偏上）
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(wc.primary)
                        .fontWeight(600)
                        .build();
                    // × 删除热区（底色同 chip 不额外可见）
                    ui.rect(id + ".del")
                        .position(chipW - 24.0f, 0.0f)
                        .size(24.0f, h)
                        .color(wc.dividerSecondary)
                        .hoverColor(wc.selectedBg)
                        .radius(t.sizes.radiusWeChat)
                        .onClick([this, name] { removeTag(name); })
                        .build();
                    ui.text(id + ".x")
                        .position(chipW - 24.0f, 0.0f)
                        .size(24.0f, h)
                        .text("×")
                        .fontSize(fs)
                        .lineHeight(h)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        // 文本盒 == 热区高，须显式垂直居中
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(wc.primary)
                        .build();
                })
                .build();
            cx += chipW + 6.0f;
        }
        // 内置添加输入（§3.3.4：从红点加号改为输入框内置「添加标签」入口）。
        // 回车提交（onEnter 已接 commitAddTag）；移除独立 addbtn 按钮。
        const float inW = 120.0f;
        if (cx + inW <= x + w) {
            components::input(ui, "md.tags.add")
                .position(cx, y)
                .size(inW, h)
                .value(tagInput_)
                .placeholder("添加标签")
                .fontSize(fs)
                .inset(8.0f)
                .theme(componentTheme)
                .onChange([this](const std::string& v) { tagInput_ = v; })
                .onEnter([this] { commitAddTag(); })
                .onCancel([this] { tagInput_.clear(); })
                .build();
        }
    }

    // 底部状态栏（§3.3.4）：12px 浅灰右对齐，字数 + 保存状态。
    void drawStatusBar(eui::Ui& ui, float x, float y, float w, const core::theme::Tokens& t) {
        ui.rect("md.status.bg")
            .position(x, y)
            .size(w, 22.0f)
            .color(t.colors.wechat.cardBg)
            .build();

        std::string left = "未选择笔记";
        eui::Color leftColor = t.colors.wechat.textTertiary;
        if (!currentPath_.empty()) {
            const std::string name = fs::path(currentPath_).filename().string();
            if (saveFailed_) {
                left = name + "  ✕ 保存失败，自动重试中";
                leftColor = t.colors.wechat.warningRed;
            } else if (editorDirty_) {
                left = name + "  ● 未保存";
                // UAT3 T1：未保存圆点改用 wechat 警示红（旧注释称「wechat 无对应槽」
                // 有误——warningRed #FA5151 即警示档），暖橙黄退出 md 页主体。
                leftColor = t.colors.wechat.warningRed;
            } else {
                left = name + "  ✓ 已保存 " + formatSaveTime(lastSavedAt_);
                leftColor = t.colors.wechat.primary;  // 保存成功 = 绿
            }
        }
        // 左框右缘 = 右串左缘留 24px 间距
        const float statusLeftW = w - 24.0f - 220.0f - 24.0f;
        ui.text("md.status.save")
            .position(x + 24.0f, y)
            .size(statusLeftW, 22.0f)
            // 超长文件名用单行省略号；显式 maxWidth 是生效前提（==0 时 runtime 回退
            // frame.width，DSL 按 maxWidth 走，两条路会分叉）；clip 兜底绘制范围。
            .maxWidth(statusLeftW)
            .overflow(core::TextOverflow::Ellipsis)
            .clip()
            .text(left)
            .fontSize(11.0f)
            .lineHeight(22.0f)
            // 文本盒 == 状态栏底高，须显式垂直居中
            .verticalAlign(eui::VerticalAlign::Center)
            .color(leftColor)
            .build();

        // 行:列 · N 字 · N 篇笔记（列/字数按 UTF-8 码点计）
        std::string right;
        if (!currentPath_.empty()) {
            // (行, 列, 字数) 按 (修订, 光标, mode) 缓存，未变帧直接复用。实时态光标权威 =
            // liveCursorByte_、文本 = 整篇 editorText_（活动 input 只持块局部文本）。
            const auto& st = ui.state<components::input_detail::InputModel::InputState>(kEditorInputId);
            const std::string* text = &st.text;
            size_t cursor = static_cast<size_t>(st.cursor);
            if (mode_ == ViewMode::Live) {
                text = &editorText_;
                cursor = std::min(liveCursorByte_, editorText_.size());
            }
            if (statusCacheRev_ != editorTextRev_ ||
                statusCacheCursor_ != cursor ||
                statusCacheMode_ != static_cast<int>(mode_)) {
                const int line = byteOffsetToLine(*text, cursor);
                const size_t lineStart = lineStartOffset(*text, line);
                const int col = static_cast<int>(utf8Length(
                    text->substr(lineStart, cursor - lineStart))) + 1;
                statusCacheLine_ = line;
                statusCacheCol_ = col;
                statusCacheChars_ = utf8Length(*text);
                statusCacheRev_ = editorTextRev_;
                statusCacheCursor_ = cursor;
                statusCacheMode_ = static_cast<int>(mode_);
            }
            right = std::to_string(statusCacheLine_ + 1) + ":" +
                    std::to_string(statusCacheCol_) + " · " +
                    std::to_string(statusCacheChars_) + " 字";
        }
        if (service_ != nullptr) {
            if (!right.empty()) right += " · ";
            right += std::to_string(notes_.size()) + " 篇笔记";
        }
        ui.text("md.status.pos")
            .position(x + w - 24.0f - 220.0f, y)
            .size(220.0f, 22.0f)
            .text(right)
            .fontSize(11.0f)
            .lineHeight(22.0f)
            .horizontalAlign(eui::HorizontalAlign::Right)
            // 文本盒 == 状态栏底高，须显式垂直居中
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.wechat.textTertiary)
            .build();
    }

    // 右栏：预览（components::markdown 渲染成 eui 节点树）
    // 双栏态：编辑器单向驱动预览滚动；仅预览态：手动滚动，进入时对齐编辑器位置。
    void drawPreviewColumn(eui::Ui& ui,
                           float bodyH,
                           float midW,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& componentTheme,
                           float gap) {
        (void)gap;
        ui.stack("md.preview").size(midW, bodyH).content([&] {
            // W8：移除渐变（§8 #4 禁大面积渐变），改为 flat wechat.pageBg 底色。
            // 预览白卡浮于 pageBg 之上（§4.14）。
            ui.rect("md.preview.bg")
                .size(midW, bodyH)
                .color(t.colors.wechat.pageBg)
                .build();

            float pvX = 20.0f;
            float pvW = midW - 40.0f;
            if (mode_ == ViewMode::Preview) {
                // 阅读态：内容列 clamp 760px 居中
                pvW = std::min(pvW, kPreviewColumnWidth);
                pvX = std::max(0.0f, (midW - pvW) * 0.5f);
            }
            const float pvY = 16.0f;
            const float pvH = std::max(0.0f, bodyH - pvY - 16.0f);

            if (currentPath_.empty()) {
                ui.text("md.preview.hint")
                    .position(pvX, pvY)
                    .size(pvW, 40.0f)
                    .text("选择笔记后在这里看到渲染结果")
                    .fontSize(t.sizes.fontSizeBody)
                    .lineHeight(20.0f)
                    .color(t.colors.wechat.textTertiary)
                    .build();
                return;
            }

            const components::MarkdownStyle style(componentTheme);

            // 滚动位置来源：Split = 同步滚动目标（编辑器驱动）；Preview = previewOffset_
            float scrollOffset = previewOffset_;
            if (mode_ == ViewMode::Split) {
                scrollOffset = computeSyncOffset(ui, pvW, pvH, style);
            } else if (syncPreviewOnEnter_) {
                previewOffset_ = computeSyncOffset(ui, pvW, pvH, style);
                scrollOffset = previewOffset_;
                syncPreviewOnEnter_ = false;
            }

            auto scroll = components::scrollView(ui, "md.preview.scroll")
                .position(pvX, pvY)
                .size(pvW, pvH)
                .gap(t.sizes.spacing)
                .step(56.0f)
                .theme(componentTheme)
                .offset(scrollOffset)
                .contentKey("md.preview.scroll." + currentPath_ + "." +
                            std::to_string(previewTextHash_));
            if (mode_ == ViewMode::Preview) {
                // 仅预览态允许手动滚动（滚轮写回 previewOffset_）
                scroll.onChange([this](float v) { previewOffset_ = v; });
            }
            scroll.content([&](eui::Ui& inner, float contentWidth, float viewportHeight) {
                    (void)viewportHeight;
                    lastPvContentWidth_ = contentWidth;  // 供同步滚动 estimateHeight 用
                    // 逐块渲染与实时态共用 drawBlockFlow；点击定位要写模块 Ui，
                    // 故闭包捕获外层 ui。预览/双栏态注入点传空实现。
                    drawBlockFlow(inner, contentWidth, t, style, "md.preview",
                                  [this, &ui](size_t k, float y) {
                                      handlePreviewBlockClick(ui, k, y);
                                  },
                                  {});
                })
                .build();
            // 直写实例 offset（绕开 .offset() 的 init-only 守卫）：
            // Split 每帧推送 computeSyncOffset，Preview 推送 previewOffset_（滚轮已同值回写）。
            if (mode_ == ViewMode::Split || mode_ == ViewMode::Preview) {
                ui.setScrollOffset("md.preview.scroll", scrollOffset);
            }
        }).build();
    }

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
                       const BlockFlowHooks& hooks) {
        if (blockModel_.text() != editorText_) {
            blockModel_.update(editorText_);  // 防御：理论不可达
        }
        // 色表注入（每帧）：主题切换时缓存旧色条目自动作废
        previewCodeCache_.setColors(mdHighlightColors(t));
        // 逐块 hit/hover 几何表（与同步滚动同一张）。键全等时零成本；Preview/Live 态是唯一重建点。
        rebuildSyncMap(contentWidth, style);
        const auto& blocks = blockModel_.blocks();
        std::vector<int> activeCodeIds;  // 本帧 CodeFence 块，供缓存回收
        // UAT3 T1：块 hover 底衬随页面主体 wechat 化（浅灰，同 surfaceHover 口径）。
        const eui::Color hoverTint = t.colors.wechat.dividerSecondary;
        ui.column(prefix + ".blocks")
            .width(contentWidth)
            .gap(style.blockGap)
            .content([&] {
                for (size_t k = 0; k < blocks.size(); ++k) {
                    const MdBlock& b = blocks[k];
                    const bool hovered = previewHover_.hovered(b.stableId);
                    const float hk = syncMap_.blockHeight(k);
                    // 包裹高默认取几何表 H_k，注入点可覆盖：实时态活动块改取 input
                    // 贴合高（源码行数与渲染高不等，用 H_k 会切成输入框内部滚动）。
                    const float wrapH =
                        hooks.wrapHeight ? hooks.wrapHeight(k, b, hk, contentWidth)
                                         : hk;
                    // 逐块交互包裹
                    // 上游无逐块命中/指针事件，这里给每块包固定高 stack：hover 底衬最下、
                    // 内容居中、透明 MouseArea 覆盖层最上；透明 mouseArea 不劫持滚动。
                    const std::string wrapId =
                        prefix + ".blk." + std::to_string(b.stableId);
                    ui.stack(wrapId)
                        .width(contentWidth)
                        .height(wrapH)
                        .content([&] {
                            // hover 底衬（CodeFence bg 不透明，走混色参数由块自身呈现）
                            ui.rect(wrapId + ".hl")
                                .size(contentWidth, wrapH)
                                .radius(style.radius)
                                .color(hovered
                                           ? hoverTint
                                           : eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                                .build();
                            // 注入点：视图态可自绘本块内容（返回 true = 已画，跳过下方渲染通路）。
                            bool contentDrawn =
                                hooks.drawContent &&
                                hooks.drawContent(ui, k, b, wrapId, contentWidth,
                                                  wrapH);
                            if (!contentDrawn &&
                                b.kind == MdBlockKind::CodeFence) {
                                // CodeFence 自绘：上游代码块单色且不暴露预解析注入。
                                // 未变块命中 (stableId, hash) 缓存零重算。
                                const MdPreviewCodeHighlight* hl =
                                    previewCodeCache_.get(b.stableId, b.hash);
                                if (!hl) {
                                    MdPreviewCodeHighlight fresh =
                                        mdPreviewCodeHighlight(
                                            blockModel_.text().substr(
                                                b.start, b.end - b.start),
                                            mdHighlightColors(t));
                                    if (fresh.ok) {
                                        previewCodeCache_.put(
                                            b.stableId, b.hash, std::move(fresh));
                                        hl = previewCodeCache_.get(b.stableId,
                                                                   b.hash);
                                    }
                                }
                                if (hl) {
                                    activeCodeIds.push_back(b.stableId);
                                    drawPreviewCodeBlock(ui, wrapId + ".code", *hl,
                                                         contentWidth, style,
                                                         hovered, hoverTint);
                                    contentDrawn = true;
                                }
                            }
                            if (!contentDrawn) {
                                components::markdown(
                                    ui,
                                    prefix + ".block." +
                                        std::to_string(b.stableId))
                                    .markdown(blockModel_.text().substr(
                                        b.start, b.end - b.start))
                                    .width(contentWidth)
                                    .wrapContentHeight()
                                    .style(style)
                                    .build();
                            }
                            // 透明 hit 覆盖层；onTap 的 y 为区域局部坐标，反查回编辑器行
                            auto hit =
                                components::mouseArea(ui, wrapId + ".hit")
                                    .size(contentWidth, wrapH);
                            if (onBlockTap) {
                                hit.onTap([onBlockTap, k](
                                              const components::MouseEvent& ev) {
                                    onBlockTap(k, static_cast<float>(ev.y));
                                });
                            }
                            hit.onHover([this, sid = b.stableId](bool h) {
                                   if (h) previewHover_.enter(sid);
                                   else previewHover_.leave(sid);
                               })
                                .build();
                            // 注入点收尾：实时态活动块在此关覆盖层命中，
                            // 否则覆盖层（命中取最上层）会抢走 input 的点击与焦点。
                            if (hooks.afterHitOverlay) {
                                hooks.afterHitOverlay(ui, k, wrapId + ".hit");
                            }
                        })
                        .build();
                }
            })
            .build();
        previewCodeCache_.pruneActive(activeCodeIds);
    }

    // 滚动锚定与光标跟随
    // 外层 scrollView 是唯一滚动权威。用户滚轮 → liveFollow_ = false；
    // 光标事件 → true；跟随只在 caret 越出安全带时拉一次，目标 == 记账值则 no-op。

    // 块内光标的显示行号（软折行参与计数），与 input 内部 layout.cursorLine 同源；
    // 逻辑行口径会低估折行段落的 caret y，不采用。
    int liveCaretDisplayLine(const MdBlock& b, const std::string& blockText,
                             int local, float textWidth, float fontSize) {
        if (liveCaret_.stableId == b.stableId && liveCaret_.hash == b.hash &&
            liveCaret_.local == local && liveCaret_.width == textWidth &&
            liveCaret_.fontSize == fontSize) {
            return liveCaret_.line;
        }
        using InputModel = components::input_detail::InputModel;
        const auto lines = InputModel::measureLines(blockText, kLiveInputFontFamily,
                                                    fontSize, textWidth);
        liveCaret_ = LiveCaretMemo{
            b.stableId, b.hash, local, textWidth, fontSize,
            InputModel::lineIndexFor(lines, local)};
        return liveCaret_.line;
    }

    // 越界才拉。caretDocY = 活动块顶 + input 内边距 + 显示行 × 行高。
    // 落地走 Ui::setScrollOffset（直写实例 offset），调用点必须在 scrollView
    // build + 回读钳位之后：程序化标志不跨帧残留。
    void liveScrollFollowMaybe(eui::Ui& ui, const MdBlock& b,
                               const std::string& blockText, int localCursor,
                               float contentWidth, float viewportH) {
        if (!liveFollow_) return;                       // 用户滚动后：不干预
        if (viewportH <= 0.0f || contentWidth <= 0.0f) return;
        const float fontSize = core::theme::editorFontSize();
        const float textWidth =
            std::max(0.0f, contentWidth - kLiveInputInset * 2.0f);
        const float lineH = fontSize * kInputLineHeightFactor;
        const size_t local =
            localCursor <= 0
                ? 0
                : std::min(static_cast<size_t>(localCursor), blockText.size());
        const int caretLine = liveCaretDisplayLine(
            b, blockText, static_cast<int>(local), textWidth, fontSize);
        const float caretDocY = syncMap_.blockTopY(liveActiveBlock_) +
                                kLiveInputInset +
                                static_cast<float>(caretLine) * lineH;
        const MdLiveFollowDecision d = mdLiveFollowOffset(
            caretDocY, viewportH, liveScrollY_, liveMaxOffset_);
        if (!d.pull || d.offset == liveScrollY_) return;   // no-op 抑制
        liveScrollY_ = d.offset;
        // 程序化来源标记：onChange 凭它区分跟随落地与用户滚轮（回写不关 liveFollow_）；
        // 同帧消费，漏清由 drawLiveColumn 开头清零兜底。
        liveProgrammaticScroll_ = true;
        ui.setScrollOffset("md.live.scroll", d.offset);
        LOG_DEBUG("[live] follow: prog=1 applied offset=" +
                  std::to_string(d.offset));
        LOG_DEBUG("[live] follow pull: caret=" + std::to_string(caretDocY) +
                  " block=" + std::to_string(liveActiveBlock_) +
                  " line=" + std::to_string(caretLine) +
                  " offset=" + std::to_string(d.offset) + " viewport=" +
                  std::to_string(viewportH) + " max=" +
                  std::to_string(liveMaxOffset_));
    }

    // 第四态：实时（块级实时渲染 + 活动块输入）
    // 单栏，块级渲染走 drawBlockFlow（与预览同一通路、同一张几何表）。
    void drawLiveColumn(eui::Ui& ui,
                        float bodyH,
                        float midW,
                        const core::theme::Tokens& t,
                        const components::theme::ThemeColorTokens& componentTheme) {
        // 整列 compose 耗时日志（DEBUG 级）
        const auto tCompose0 = std::chrono::steady_clock::now();
        // 程序化标记漏清在下帧开头清零（残留会把用户滚轮误判成程序化来源）。
        liveProgrammaticScroll_ = false;
        // 列壳与编辑态共用同一实现；两态都不钳列宽：正文列 = 栏宽 − 2×8px 留白。
        drawTextColumnChrome(
            ui, "md.live", bodyH, midW, t, componentTheme,
            [&](const TextColumnGeometry& g) {
                const components::MarkdownStyle style(componentTheme);
                // 输入框状态换代（释放点）
                // enterLiveState 置位，本帧先释放再建 input；释放会清焦点，
                // 必须与 liveFocusPending_ 同帧补发配对。id 落在被释放前缀下
                // 会每次切块清焦点 + 清 InputState。
                if (liveInputStateFresh_) {
                    liveInputStateFresh_ = false;
                    ui.releaseStateScope(kLiveInputId);
                    LOG_DEBUG("[live] release scope=md.live.input");
                }
                // 活动块锚（文档字节偏移）
                // 进实时态以编辑器文档光标为初始锚；此后每帧跟着活动 input 真实光标推进。
                LiveBlockCtx live;
                live.theme = &componentTheme;
                bool hasActive = false;
                if (!blockModel_.blocks().empty()) {
                    const std::vector<MdBlock>& bs = blockModel_.blocks();
                    const auto& lst = ui.state<
                        components::input_detail::InputModel::InputState>(
                        kLiveInputId);
                    if (!liveCursorValid_) {
                        const auto& ed = ui.state<
                            components::input_detail::InputModel::InputState>(
                            kEditorInputId);
                        liveCursorByte_ =
                            ed.cursor > 0 ? static_cast<size_t>(ed.cursor) : 0;
                        liveCursorValid_ = true;
                        liveActiveBlock_ =
                            mdLiveActiveBlockIndex(blockModel_, liveCursorByte_);
                        // 本帧 input 尚未与本块同步，基准取块起点；真实落点由换文本帧反向写入。
                        liveInputStart_ = bs[liveActiveBlock_].start;
                        resetLiveUndoBaseline();
                    } else if (liveCaretPending_) {
                        // 本帧锚由 setLiveCursor 显式定下；此刻 input 光标还是上一块
                        // 局部坐标，拿它反推会错位，本帧跳过重推。
                    } else if (!mdLiveCompositionFrozen(lst.compositionText)) {
                        // 锚必须跟着真实光标走（文档字节 = 输入框文本起点 + 块内光标），
                        // 冻结在进态旧值会让重切后光标跳块首。
                        // 基准取上一帧的 liveInputStart_（重切后文本可跨块）。
                        const size_t local =
                            lst.cursor > 0 ? static_cast<size_t>(lst.cursor) : 0;
                        liveCursorByte_ = liveInputStart_ + local;
                        liveActiveBlock_ =
                            mdLiveActiveBlockIndex(blockModel_, liveCursorByte_);
                        liveInputStart_ = bs[liveActiveBlock_].start;
                    }
                    // caret 变了 = 一次光标事件（打字 / 方向键 / 点进输入框）→ 恢复跟随；
                    // 用户滚轮已关掉它。
                    if (lst.cursor != liveCursorSeen_) {
                        liveCursorSeen_ = lst.cursor;
                        liveFollow_ = true;
                    }
                    // 跟随判定 + 落地须在 scrollView build 之后（见 liveScrollFollowMaybe）。
                    live.activeBlock = liveActiveBlock_;
                    hasActive = true;
                }
                // 实时态块级特判经注入点传入
                // 活动块 = 源码态 input：wrapHeight 改贴合高、drawContent 改由 input
                // 呈现、afterHitOverlay 关掉覆盖层命中；非活动块走默认路。
                BlockFlowHooks hooks;
                // 单块高度上限：贴合高超过视口 0.7 倍时封顶（封顶后 input 内部滚动，
                // 外层滚轮在该块成死区）。liveInputCapped_ 供 over 探针区分预期与回归。
                hooks.wrapHeight = [this, &live, viewportH = g.h](
                                       size_t k, const MdBlock& b,
                                       float geometryHeight,
                                       float contentWidth) {
                    if (k != live.activeBlock) return geometryHeight;
                    const float fit =
                        liveFitHeight(b, liveBlockText(b), contentWidth);
                    const float h = mdLiveCappedInputHeight(fit, viewportH);
                    liveInputCapped_ = h < fit;
                    if (liveInputCapped_) {
                        LOG_DEBUG("[live] input cap fit=" +
                                  std::to_string(fit) + " h=" + std::to_string(h) +
                                  " viewport=" + std::to_string(viewportH) +
                                  " stableId=" + std::to_string(b.stableId));
                    }
                    return h;
                };
                hooks.drawContent = [this, &live, &style](
                                        eui::Ui& inner, size_t k,
                                        const MdBlock& b,
                                        const std::string& wrapId,
                                        float contentWidth,
                                        float wrapHeight) {
                    if (k != live.activeBlock) return false;
                    drawLiveInputBlock(inner, b, liveBlockText(b), wrapId,
                                       contentWidth, wrapHeight, style, live);
                    return true;  // 已画：跳过下方渲染通路
                };
                // 不捕获 this：clang -Wunused-lambda-capture（GCC 不报）。
                hooks.afterHitOverlay = [&live](eui::Ui& inner, size_t k,
                                                      const std::string& hitId) {
                    if (k != live.activeBlock) return;
                    if (auto* hitEl = inner.find(hitId)) {
                        hitEl->hitTestMode = core::dsl::HitTestMode::None;
                    }
                };
                // 单一滚动权威 = 本 scrollView；活动块 input 内容贴合不吃滚轮
                components::scrollView(ui, "md.live.scroll")
                    .position(g.x, g.y)
                    .size(g.w, g.h)
                    .gap(t.sizes.spacing)
                    .step(56.0f)
                    .theme(componentTheme)
                    .offset(liveScrollY_)
                    // contentKey 不用：实态点击切块会换 key → scrollView 视为全新内容 → 滚动归零。
                    // 若用 contentKey 带 activeBlockStableId，cache 只在点击帧 miss；不带则
                    // cache 永久返回旧活动块高度、maxOffset 永误。空字符串走每帧实量，O(blocks)
                    // 成本可忽略（rebuildSyncMap 同文零成本、仅 estimateHeight 逐块重付）。
                    // 规则性注释：实时模式点击不得重建编辑器子树 / id 必须稳定，否则滚动重置。
                    .onChange([this](float v) {
                        // 程序化来源（跟随落地 echo）：只记账、不关跟随
                        if (liveProgrammaticScroll_) {
                            liveProgrammaticScroll_ = false;
                            liveScrollY_ = v;
                            return;
                        }
                        // 用户滚动：记账 + 关跟随（直到下一次光标事件）
                        liveScrollY_ = v;
                        liveFollow_ = false;
                        LOG_DEBUG("[live] follow: wheel disarmed");
                    })
                    .content([&](eui::Ui& inner, float contentWidth, float viewportHeight) {
                        (void)viewportHeight;
                        // 跟随判据的排版宽（滞后一帧，窗口缩放时才变）
                        liveContentWidth_ = contentWidth;
                        drawBlockFlow(inner, contentWidth, t, style, "md.live",
                                      [this, &ui](size_t k, float y) {
                                          handleLiveBlockClick(ui, k, y);
                                      },
                                      hooks);
                    })
                    .build();
                // 按框架声明值回读钳位：内容变矮时记账值随之收敛
                if (const auto* svEl = ui.find("md.live.scroll")) {
                    liveMaxOffset_ = svEl->scrollMaxOffset;
                    liveScrollY_ = svEl->scrollOffset;
                }
                // 跟随判定 + 落地须在 scrollView build + 回读钳位之后。评估守卫：
                // 组字在飞不评、liveCaretPending_ 帧不评、几何表滞后一帧（跨块错位下帧自纠）。
                if (liveFollow_ && !liveCaretPending_ &&
                    liveActiveBlock_ < blockModel_.blocks().size()) {
                    const auto& lstFollow = ui.state<
                        components::input_detail::InputModel::InputState>(
                        kLiveInputId);
                    if (!mdLiveCompositionFrozen(lstFollow.compositionText)) {
                        const MdBlock& ab =
                            blockModel_.blocks()[liveActiveBlock_];
                        liveScrollFollowMaybe(ui, ab, liveBlockText(ab),
                                              lstFollow.cursor,
                                              liveContentWidth_, g.h);
                    }
                }
                // 写光标一次性请求已消费，帧末清位（下帧锚由真实光标重推，写了也是同值）。
                liveCaretPending_ = false;

                // 进实时态首帧同帧补发 requestFocus（目标须已在当帧树中），只发
                // <input id>.hit（组件按 .hit 判聚焦，发基 id 会不画 caret）。
                if (liveFocusPending_) {
                    liveFocusPending_ = false;
                    if (hasActive) ui.requestFocus(kLiveInputHitId);
                }
                // 整列 compose 耗时日志（含 scrollView 测量 pass）
                const auto tCompose1 = std::chrono::steady_clock::now();
                char msBuf[32];
                std::snprintf(msBuf, sizeof(msBuf), "%.2f",
                              std::chrono::duration<double, std::milli>(
                                  tCompose1 - tCompose0)
                                  .count());
                LOG_DEBUG(std::string("[live] frame compose ms=") + msBuf);
            });
    }

    // CodeFence 块预览自绘：几何逐项对齐上游 renderCodeBlock，唯一差异是 text
    // 带着色 runs。hovered 时把 codeBackground 混向 hover 色（块 bg 不透明）。
    void drawPreviewCodeBlock(eui::Ui& ui,
                              const std::string& partId,
                              const MdPreviewCodeHighlight& hl,
                              float contentWidth,
                              const components::MarkdownStyle& style,
                              bool hovered,
                              const eui::Color& hoverTint) {
        const float textWidth =
            std::max(0.0f, contentWidth - style.codePadding * 2.0f);
        const float textHeight = components::detail::estimateMarkdownTextHeight(
            hl.displayText, textWidth, style.codeSize, style.codeSize + 6.0f);
        const float blockHeight = textHeight + style.codePadding * 2.0f;
        ui.stack(partId)
            .width(contentWidth)
            .height(blockHeight)
            .content([&] {
                ui.rect(partId + ".bg")
                    .size(contentWidth, blockHeight)
                    .color(hovered
                               ? mdHoverMix(style.codeBackground, hoverTint, 0.5f)
                               : style.codeBackground)
                    .radius(style.radius)
                    .build();
                ui.text(partId + ".text")
                    .x(style.codePadding)
                    .y(style.codePadding)
                    .width(textWidth)
                    .height(textHeight)
                    .text(hl.displayText)
                    .fontFamily(style.codeFontFamily)
                    .fontSize(style.codeSize)
                    .lineHeight(style.codeSize + 6.0f)
                    .wrap()
                    .color(style.codeText)
                    .runs(hl.runs)
                    .build();
            })
            .build();
    }

    // 实时态活动块输入

    // 块源码文本（块区间连续覆盖文档）
    std::string liveBlockText(const MdBlock& b) const {
        return blockModel_.text().substr(b.start, b.end - b.start);
    }

    // 活动块 stableId（渲染键 / contentKey 用；无块 → -1）。
    // stableId 按内容身份继承，比下标更适合当缓存键。
    int liveActiveBlockStableId() const {
        const std::vector<MdBlock>& bs = blockModel_.blocks();
        return liveActiveBlock_ < bs.size()
                   ? bs[liveActiveBlock_].stableId
                   : -1;
    }

    // 活动块内容贴合高度：测量与 input 内部排版同源（公式值 +1px 余量
    // ⇒ maxVerticalScroll == 0）；备忘键避免整块 shaping 逐帧重付。
    float liveFitHeight(const MdBlock& b, const std::string& blockText,
                        float contentWidth) {
        const float fontSize = core::theme::editorFontSize();
        const float textWidth =
            std::max(0.0f, contentWidth - kLiveInputInset * 2.0f);
        if (liveHeight_.stableId == b.stableId && liveHeight_.hash == b.hash &&
            liveHeight_.width == textWidth && liveHeight_.fontSize == fontSize) {
            return liveHeight_.height;
        }
        const MdLiveLineCountFn lineCount =
            [fontSize](const std::string& text, float width) {
                using InputModel = components::input_detail::InputModel;
                return static_cast<int>(
                    InputModel::measureLines(text, kLiveInputFontFamily, fontSize,
                                             width).size());
            };
        liveHeight_ = LiveHeightMemo{b.stableId, b.hash, textWidth, fontSize,
                                     mdLiveInputHeight(blockText, textWidth,
                                                       fontSize, kLiveInputInset,
                                                       lineCount)};
        return liveHeight_.height;
    }

    // 上行链路：活动块局部文本 → 拼回整篇 → 唯一文档写点。
    // 分块器保证块区间连续覆盖，拼回 ≡ 编辑态对同一处的同一编辑。
    void syncDocumentFromLiveInput(const std::string& blockText) {
        const std::vector<MdBlock>& blocks = blockModel_.blocks();
        if (liveActiveBlock_ >= blocks.size()) return;   // 防御：无块不写
        const size_t start = blocks[liveActiveBlock_].start;
        const size_t end = blocks[liveActiveBlock_].end;
        const std::string& doc = blockModel_.text();
        if (start > end || end > doc.size()) return;
        // 拼装须在 onEditorTextChanged 之前完成：它进 setEditorText →
        // blockModel_.update，blocks/doc 引用随即失效。
        std::string next;
        next.reserve(doc.size() + blockText.size());
        next.append(doc, 0, start);
        next.append(blockText);
        next.append(doc, end, std::string::npos);
        // 撤销快照须在写文档之前推（= 本次编辑前的那一版）
        pushLiveUndoSnapshot();
        onEditorTextChanged(next);
    }

    // 活动块源码态 input：建在块包裹 stack 内（0,0），尺寸 = 列宽 × 贴合高。
    // 元素必须建在 scrollView content 回调的 Ui 上（否则测量量到空内容）。
    void drawLiveInputBlock(eui::Ui& ui,
                            const MdBlock& b,
                            const std::string& blockText,
                            const std::string& wrapId,
                            float contentWidth,
                            float fitHeight,
                            const components::MarkdownStyle& style,
                            const LiveBlockCtx& live) {
        using InputState = components::input_detail::InputModel::InputState;
        InputState& st = ui.state<InputState>(kLiveInputId);
        // 换文本帧：input 走「外部改值」把光标搬到新文本末尾，须在 build 之后
        // 反向写正确落点（早写会被 build 内重载覆盖）。
        const bool swapped = (st.text != blockText);
        components::input(ui, kLiveInputId)
            .position(0.0f, 0.0f)
            .size(contentWidth, fitHeight)
            .value(blockText)
            .placeholder("")  // 活动块恒有源码（空文档无块 = 不建 input）
            .multiline(true)
            .fontSize(core::theme::editorFontSize())
            .inset(kLiveInputInset)
            .theme(*live.theme)
            .onChange([this](const std::string& v) { syncDocumentFromLiveInput(v); })
            // 块内编辑键 + 块边界键，passthrough 先于内置（返回 true 即拦截）。
            .onKeyEvent([this, &ui](const eui::KeyEvent& event) {
                return handleLiveKey(ui, event);
            })
            // 失焦提交（语义见 commitLiveInputOnBlur）：焦点离开活动 input 时收口。
            .onFocus([this, &ui](bool focused) {
                if (!focused) commitLiveInputOnBlur(ui);
            })
            // 实时态行级高亮：独立实例 liveHighlighter_ 喂块文本（runs 以块首为原点）；
            // 与编辑态混用（交替喂整篇/块文本）会抖动围栏缓存。
            .lineStyles([this, &ui](const std::string& lineText,
                                    int lineStartByte, int lineNo) {
                (void)lineNo;
                using InputState =
                    components::input_detail::InputModel::InputState;
                auto& st = ui.state<InputState>(kLiveInputId);
                return liveHighlighter_.runsForLine(
                    st.preedit ? st.preedit->text : st.text, lineText,
                    lineStartByte);
            })
            .build();
        // 写光标两条来路（都在 build 之后）：swapped（换文本帧）/
        // liveCaretPending_（同块定位帧，文本未变须显式写）。
        if (swapped || liveCaretPending_) {
            st.cursor =
                static_cast<int>(mdLiveBlockLocalOffset(b, liveCursorByte_));
            st.selectionStart = st.cursor;
            st.selectionEnd = st.cursor;
            st.dragAnchor = st.cursor;
            st.hasPreferredCursorX = false;
            st.followCaret = true;     // 落点即目标（贴合高度下框内无滚动）
            st.verticalScroll = 0.0f;  // 切块/定位帧清上一块的滚动残余
            st.horizontalScroll = 0.0f;
        }
        // 失焦提交安全判据：输入框文本 == 当前块源。外部重载/撤销换代帧
        // 不满足，故不会把旧文本灌回新文档。
        if (st.text == blockText) liveSyncedRev_ = editorTextRev_;

        // 活动块边框：主色半透明 1px + 透明填充，不参与命中测试
        ui.rect(wrapId + ".frame")
            .size(contentWidth, fitHeight)
            .radius(style.radius)
            .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
            .border(1.0f, components::theme::withAlpha(
                             live.theme->primary, kLiveActiveFrameAlpha))
            .build();
        // 记录式校验：活动 input 应内容贴合（无内部滚动条）。框架不暴露
        // maxVerticalScroll，用「是否注册滚轮处理器」做 0/1 探针。
        const auto* hit = ui.find(kLiveInputHitId);
        const int over = (hit != nullptr && hit->onScroll) ? 1 : 0;
        if (over > 0) {
            if (liveInputCapped_) {
                LOG_DEBUG("[live] input over=1（封顶块内滚动，预期）stableId=" +
                          std::to_string(b.stableId) +
                          " h=" + std::to_string(fitHeight));
            } else {
                LOG_WARN("[live] input over=1 活动块高度未贴合（应 0）: stableId=" +
                         std::to_string(b.stableId) +
                         " 声明高=" + std::to_string(fitHeight));
            }
        } else {
            LOG_DEBUG("[live] input over=0");
        }
    }

    // 光标写点 / 块点击定位 / 失焦提交

    // 实时态唯一光标写点：文档字节偏移 = 活动块身份锚。一次做完：锚重算、
    // 记账 liveCaretPending_ 与 liveFocusPending_（文本未变须显式写光标 /
    // 指针按下摘掉焦点须补发），两个一次性请求都在下次 compose 同帧消费。
    void setLiveCursor(size_t docByte, const char* reason) {
        const size_t size = editorText_.size();
        liveCursorByte_ = docByte < size ? docByte : size;
        liveCursorValid_ = true;
        const std::vector<MdBlock>& bs = blockModel_.blocks();
        if (!bs.empty()) {
            liveActiveBlock_ = mdLiveActiveBlockIndex(blockModel_, liveCursorByte_);
            liveInputStart_ = bs[liveActiveBlock_].start;
            // 块切换前关掉撤销合并组：否则「切走又切回同一块」会并成一次撤销。
            // 只关组不推条目（多推会多出空撤销中间站并作废 redo 链）。
            if (liveUndoSeeded_ && liveActiveBlock_ != liveUndoBlock_) {
                liveUndo_.markBoundary();
                liveUndoBlock_ = liveActiveBlock_;
            }
        }
        liveCaretPending_ = true;
        liveFocusPending_ = true;
        // 定位即跟随。本帧不评估（input 光标还是上一块局部坐标），下一帧按真实落点判一次。
        liveFollow_ = true;
        LOG_DEBUG(std::string("[live] setLiveCursor(") + reason + ") byte=" +
                  std::to_string(liveCursorByte_) + " block=" +
                  std::to_string(liveActiveBlock_));
    }

    // 点击渲染块 → 该块变源码态 + 光标落到点击行。只服务非活动块；
    // 组字在飞时拒绝切块并补发聚焦。
    void handleLiveBlockClick(eui::Ui& ui, size_t blockIndex, float localY) {
        if (mode_ != ViewMode::Live || editorText_.empty()) return;
        using InputState = components::input_detail::InputModel::InputState;
        if (mdLiveCompositionFrozen(
                ui.state<InputState>(kLiveInputId).compositionText)) {
            liveFocusPending_ = true;
            LOG_DEBUG("[live] 组字中拒绝切块: 块#" + std::to_string(blockIndex));
            return;
        }
        const MdPreviewJump jump = mdResolvePreviewJump(
            editorText_, blockModel_.blocks(), syncMap_, blockIndex, localY);
        if (!jump.ok) return;
        setLiveCursor(jump.cursorByte, "点击渲染块");
        LOG_INFO("实时态块点击: 块#" + std::to_string(jump.blockIndex) +
                 " stableId=" + std::to_string(jump.stableId) +
                 " → 文档字节 " + std::to_string(jump.cursorByte) + "（行 " +
                 std::to_string(jump.targetLine + 1) + "）");
    }

    // 失焦提交两条收口：① 清在飞组字（不清则冻结谓词恒真、切块被永久拒绝）；
    // ② 只在输入框与文档对齐到同一修订号时上行未提交文本，否则旧文本
    // 灌回新文档 = 数据丢失。
    void commitLiveInputOnBlur(eui::Ui& ui) {
        using InputState = components::input_detail::InputModel::InputState;
        InputState& st = ui.state<InputState>(kLiveInputId);
        if (mdLiveCompositionFrozen(st.compositionText)) {
            st.compositionText.clear();
            ++st.compositionRevision;
            LOG_DEBUG("[live] 失焦清组字");
        }
        const std::vector<MdBlock>& bs = blockModel_.blocks();
        if (liveActiveBlock_ >= bs.size()) return;
        if (st.text.empty() || liveSyncedRev_ != editorTextRev_) return;
        if (st.text == liveBlockText(bs[liveActiveBlock_])) return;   // 无未提交内容
        const std::string pending = st.text;   // 取副本：上行会换掉 blockModel_ 内部文本
        LOG_DEBUG("[live] 失焦提交未上行文本 len=" + std::to_string(pending.size()));
        syncDocumentFromLiveInput(pending);
    }

    // 块边界键
    // 块边界不做块结构操作，只做普通文本编辑，块身份始终跟随光标：
    // 块内按键 → md_keyboard 纯函数写回活动 input；块边界键 → md_live_edit
    // 纯函数改文档 + setLiveCursor。组字在飞一律不拦截（会撕裂预编辑）。
    bool handleLiveKey(eui::Ui& ui, const eui::KeyEvent& event) {
        using InputState = components::input_detail::InputModel::InputState;
        if (mode_ != ViewMode::Live) return false;
        if (!event.isDown()) return false;   // 释放/重复：内置只认按下（同 handleEditorKey）
        InputState& st = ui.state<InputState>(kLiveInputId);
        if (mdLiveCompositionFrozen(st.compositionText)) return false;

        const bool shortcut = event.modifiers.shortcut();
        const bool alt = event.modifiers.alt;
        const bool plain = !shortcut && !event.modifiers.shift && !alt &&
                           !event.modifiers.super;

        // 撤销 / 重做走模块层快照栈，无论有无可用快照一律消费（交回内置会走
        // framework per-instance 栈，存块局部文本，单输入下跨块混叠）。
        if (shortcut && !alt) {
            const bool redoKey = event.key == eui::InputKey::Y ||
                                 (event.key == eui::InputKey::Z &&
                                  event.modifiers.shift);
            if (redoKey) {
                applyLiveRedo();
                return true;
            }
            if (event.key == eui::InputKey::Z) {
                applyLiveUndo();
                return true;
            }
        }

        // 块内按键：与编辑态同一批纯函数
        EditBuffer buf{st.text, st.cursor, st.selectionStart, st.selectionEnd};
        bool handled = false;
        if (event.key == eui::InputKey::Tab && !shortcut && !alt) {
            if (!applyTabTable(buf, event.modifiers.shift)) {
                applyTab(buf, event.modifiers.shift);
            }
            handled = true;
        } else if (event.key == eui::InputKey::Enter && plain) {
            handled = applyEnterList(buf);   // 列表行续行；其余交回内置插 '\n'
        } else if (shortcut && !event.modifiers.shift && !alt) {
            if (event.key == eui::InputKey::B) {
                applyWrapAround(buf, "**");
                handled = true;
            } else if (event.key == eui::InputKey::I) {
                applyWrapAround(buf, "*");
                handled = true;
            } else if (event.key == eui::InputKey::K) {
                applyLinkSkeleton(buf);
                handled = true;
            }
        }
        if (handled) {
            commitLiveEditBuffer(ui, buf);
            return true;
        }

        // 块边界键：文档字节偏移 = 输入框文本起点 + 块内光标；
        // 带 Ctrl/Alt/Super 的其它组合一律不碰。
        if (shortcut || alt || event.modifiers.super) return false;
        const std::vector<MdBlock>& blocks = blockModel_.blocks();
        if (blocks.empty()) return false;
        const size_t local = st.cursor > 0 ? static_cast<size_t>(st.cursor) : 0;
        const size_t docByte = liveInputStart_ + local;
        const bool hasSelection = st.selectionStart != st.selectionEnd;

        if (event.key == eui::InputKey::Up || event.key == eui::InputKey::Down) {
            // Shift+↑↓ 与块内选区照旧（跨块选区不做）
            if (event.modifiers.shift || hasSelection) return false;
            const int dir = (event.key == eui::InputKey::Up) ? -1 : 1;
            const size_t target =
                mdLiveVerticalBlockTarget(editorText_, blocks, docByte, dir);
            if (target == kMdLiveNoConsume) return false;   // 首块 ↑ / 末块 ↓：内置
            setLiveCursor(target, dir < 0 ? "跨块上移" : "跨块下移");
            LOG_DEBUG(std::string("[live] 跨块") + (dir < 0 ? "上移" : "下移") +
                      " byte=" + std::to_string(docByte) + " → " +
                      std::to_string(target));
            return true;
        }
        if (event.key == eui::InputKey::Backspace || event.key == eui::InputKey::Delete) {
            if (hasSelection) return false;   // 块内选区删除交回内置
            MdLiveBoundaryEdit edit;
            const int dir = (event.key == eui::InputKey::Backspace) ? -1 : 1;
            if (!mdLiveBoundaryEdit(editorText_, blocks, docByte, dir, edit)) {
                return false;   // 不在块边界 / 首块退格 / 文末删除 / 普通字符删除
            }
            applyLiveBoundaryEdit(edit, dir < 0 ? "块首退格" : "块尾删除");
            return true;
        }
        return false;
    }

    // 块内编辑键写回（写活动 input）：纯函数改 InputState 不触发 onChange，
    // 故文本变化时须显式上行。
    void commitLiveEditBuffer(eui::Ui& ui, EditBuffer& buf) {
        using InputState = components::input_detail::InputModel::InputState;
        InputState& st = ui.state<InputState>(kLiveInputId);
        if (buf.text == st.text) {
            // 纯光标/选区移动：只更新位置，不进 undo / 不 bump 修订号。
            if (buf.cursor == st.cursor && buf.selStart == st.selectionStart &&
                buf.selEnd == st.selectionEnd) {
                return;
            }
            st.cursor = buf.cursor;
            st.selectionStart = buf.selStart;
            st.selectionEnd = buf.selEnd;
            st.dragAnchor = buf.cursor;
            st.hasPreferredCursorX = false;
            st.followCaret = true;
            return;
        }
        components::input_detail::InputModel::pushUndoState(st);
        st.text = std::move(buf.text);
        ++st.textRevision;
        st.cursor = buf.cursor;
        st.selectionStart = buf.selStart;
        st.selectionEnd = buf.selEnd;
        st.dragAnchor = buf.cursor;
        st.hasPreferredCursorX = false;
        st.followCaret = true;
        syncDocumentFromLiveInput(st.text);
    }

    // 边界删除：删的是块外接缝换行，直接改整篇文档再按删除点重定光标。
    // 顺序不能反（先 onEditorTextChanged 再 setLiveCursor）；撤销由模块层
    // 快照栈接管，须先 push 编辑前整篇文本。
    void applyLiveBoundaryEdit(const MdLiveBoundaryEdit& edit, const char* reason) {
        if (edit.eraseEnd <= edit.eraseStart || edit.eraseEnd > editorText_.size()) {
            return;
        }
        std::string next = editorText_;
        next.erase(edit.eraseStart, edit.eraseEnd - edit.eraseStart);
        pushLiveUndoSnapshot();   // 快照 = 改之前的那一版
        onEditorTextChanged(next);
        setLiveCursor(edit.cursor, reason);
        LOG_INFO(std::string("实时态边界键(") + reason + "): 删 [" +
                 std::to_string(edit.eraseStart) + "," +
                 std::to_string(edit.eraseEnd) + ") 光标 " +
                 std::to_string(edit.cursor));
    }

    // 块级 undo / redo

    // 推栈：快照 = 编辑前的整篇文本 + 文档光标偏移，在写文档之前推。
    void pushLiveUndoSnapshot() {
        if (!liveUndoSeeded_) return;   // 基线未就位（空文档 / 尚未进实时态）
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
        const size_t capBefore = liveUndo_.capacity();
        liveUndo_.push(editorText_, liveCursorByte_,
                       liveActiveBlock_ == liveUndoBlock_, nowMs);
        liveUndoBlock_ = liveActiveBlock_;
        if (liveUndo_.capacity() != capBefore) {
            // 文档 > 2MB 时撤销栈降级；只在档位切换那一次记日志
            LOG_WARN("[live] 撤销栈降级：文档 " + std::to_string(editorText_.size()) +
                     " 字节 > 2MB，容量 " + std::to_string(capBefore) + " → " +
                     std::to_string(liveUndo_.capacity()));
        }
    }

    // 撤销 / 重做落地：改文档 + 光标两处唯一写点。无可用快照时静默返回
    //（调用侧已消费按键，不回退框架栈）。
    void applyLiveUndo() {
        std::string text = editorText_;
        size_t cursor = liveCursorByte_;
        if (!liveUndo_.undo(text, cursor)) {
            LOG_DEBUG("[live] Ctrl+Z: 无可撤销快照");
            return;
        }
        LOG_INFO("实时态撤销: " + std::to_string(editorText_.size()) + " → " +
                 std::to_string(text.size()) + " 字节, 光标 " + std::to_string(cursor));
        onEditorTextChanged(text);
        setLiveCursor(cursor, "撤销");
    }

    void applyLiveRedo() {
        std::string text = editorText_;
        size_t cursor = liveCursorByte_;
        if (!liveUndo_.redo(text, cursor)) {
            LOG_DEBUG("[live] Ctrl+Y: 无可重做快照");
            return;
        }
        LOG_INFO("实时态重做: " + std::to_string(editorText_.size()) + " → " +
                 std::to_string(text.size()) + " 字节, 光标 " + std::to_string(cursor));
        onEditorTextChanged(text);
        setLiveCursor(cursor, "重做");
    }

    // 撤销栈基线换代（以当前文档为基线重开栈）。调用点 = 整篇文档换了一版
    //（进实时态首帧 / 外部重载）；不换代时 Ctrl+Z 会把别处改动整篇盖回旧版并写盘。
    void resetLiveUndoBaseline() {
        liveUndo_.reset(editorText_, liveCursorByte_);
        liveUndoBlock_ = mdLiveActiveBlockIndex(blockModel_, liveCursorByte_);
        liveUndoSeeded_ = true;
    }

    // 双栏同步滚动：编辑器顶行 → 块定位 → 预览侧块几何（块内按编辑器行线性插值；
    // 反向映射是同一函数的反函数）。缓存键 = 修订 + 宽度 + blockGap + 块数。
    float computeSyncOffset(eui::Ui& ui,
                            float pvW,
                            float pvH,
                            const components::MarkdownStyle& style) {
        if (editorText_.empty()) return 0.0f;
        const auto& st = ui.state<components::input_detail::InputModel::InputState>(kEditorInputId);
        const float topLine =
            st.verticalScroll /
            (core::theme::editorFontSize() * kInputLineHeightFactor);
        const float width = lastPvContentWidth_ > 0.0f ? lastPvContentWidth_ : pvW;
        rebuildSyncMap(width, style);
        const float y = syncMap_.previewYForEditorLine(topLine);
        return std::max(0.0f, y - pvH * 0.2f);
    }

    // 几何表按需重建（键未变 = 零成本；变了只对 memo 未命中的块付 estimateHeight）。
    void rebuildSyncMap(float width, const components::MarkdownStyle& style) {
        if (blockModel_.text() != editorText_) {
            blockModel_.update(editorText_);  // 防御：理论不可达（同预览路径）
        }
        syncMap_.rebuild(
            editorText_, blockModel_.blocks(), editorTextRev_, width,
            style.blockGap,
            [this, width, &style](const MdBlock& b) {
                // 与预览逐块渲染同一份块源码/宽度/style ⇒ 测量与预览布局同源。
                return components::MarkdownBuilder::estimateHeight(
                    editorText_.substr(b.start, b.end - b.start), width, style);
            });
    }

    // 预览块点击 → 编辑器光标定位 + 聚焦（mdResolvePreviewJump 反查行首字节）。
    // 行内列不映射：预览与编辑器布局不同源，行首是唯一无歧义落点。
    void handlePreviewBlockClick(eui::Ui& ui, size_t blockIndex, float localY) {
        if (mode_ == ViewMode::Preview || editorText_.empty()) return;
        const MdPreviewJump jump = mdResolvePreviewJump(
            editorText_, blockModel_.blocks(), syncMap_, blockIndex, localY);
        if (!jump.ok) return;
        auto& st =
            ui.state<components::input_detail::InputModel::InputState>(kEditorInputId);
        st.cursor = static_cast<int>(jump.cursorByte);
        st.selectionStart = st.cursor;
        st.selectionEnd = st.cursor;
        st.dragAnchor = st.cursor;
        st.hasPreferredCursorX = false;
        st.followCaret = true;
        ui.requestFocus(kEditorInputHitId);  // 帧末消费（Runtime 机制，同回车聚焦）
        LOG_INFO("预览块点击: 块#" + std::to_string(jump.blockIndex) +
                 " stableId=" + std::to_string(jump.stableId) +
                 " kind=" +
                 mdBlockKindName(blockModel_.blocks()[jump.blockIndex].kind) +
                 " → 编辑器行 " + std::to_string(jump.targetLine + 1));
    }

    // 新建笔记弹窗
    void drawNewNoteDialog(eui::Ui& ui,
                           const eui::Screen& screen,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& componentTheme) {
        const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
        const float h = 220.0f;
        // M-1（UAT2 一审）：弹窗内控件统一 wechat 皮肤；页面暖橙 componentTheme
        // 有意只喂页面主体（compose/drawHeader 两处页面变量），popup 控件就地取 wechat 令牌。
        const auto wcTheme = core::theme::componentTokensWeChat(t);

        ui_kit::DialogFrame frame;
        frame.id = "md.dialog.new";
        frame.width = ui_kit::DialogWidth::Standard;
        frame.height = h;
        frame.open = newNoteOpen_;
        frame.onOpenChange = [this](bool v) { newNoteOpen_ = v; };

        ui_kit::drawDialogFrame(ui, screen, frame, core::theme::componentTokensWeChat(t), [&] {
                ui_kit::drawDialogTitle(ui, "md.dialog.new.title", w, "新建笔记",
                                        t.sizes.fontSizeLarge, t.colors.textPrimary);

                // 提示行用就地判据而非 newItemParentDir()（后者每帧 WARN 刷屏）
                const std::string newNoteParent =
                    dirExistsOnDisk(newItemTargetDir_) ? newItemTargetDir_ : std::string();
                ui_kit::drawDialogText(ui, "md.dialog.new.hint", w,
                                       "保存到：" +
                                           (newNoteParent.empty()
                                                ? std::string("根目录")
                                                : newNoteParent) +
                                           " · 名字可带子目录，如 工作/周会.md",
                                       t.sizes.fontSizeSmall, t.colors.textTertiary,
                                       {.y = 56.0f, .height = 18.0f, .lineHeight = 18.0f});

                components::input(ui, "md.dialog.new.input")
                    .position(28.0f, 84.0f)
                    .size(w - 56.0f, 44.0f)
                    .value(newNoteName_)
                    .placeholder("笔记名，可用 / 分文件夹，如 工作/周会")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(wcTheme)
                    .onChange([this](const std::string& v) {
                        newNoteName_ = v;
                        newNoteError_.clear();  // 错误随下一次输入清除
                    })
                    .onEnter([this, &ui] { commitNewNote(ui); })
                    // Esc=取消（关弹窗不落盘，同「取消」按钮）
                    .onCancel([this] { newNoteOpen_ = false; })
                    .build();

                // 内嵌红字错误行（校验/创建失败不走 toast）
                ui_kit::drawDialogText(ui, "md.dialog.new.error", w, newNoteError_, 12.0f,
                                       t.colors.error,
                                       {.y = 134.0f, .height = 18.0f, .lineHeight = 18.0f,
                                        .skipIfEmpty = true});

                ui_kit::DialogButtonSpec cancel;
                cancel.label = "取消";
                cancel.fontSize = t.sizes.fontSizeBody;
                cancel.style = ui_kit::DialogButtonStyle::SecondaryThemed;
                cancel.onClick = [this] { newNoteOpen_ = false; };
                ui_kit::DialogButtonSpec confirm;
                confirm.label = "创建";
                confirm.fontSize = t.sizes.fontSizeBody;
                confirm.style = ui_kit::DialogButtonStyle::PrimaryThemed;
                confirm.onClick = [this, &ui] { commitNewNote(ui); };
                ui_kit::drawDialogButtons(ui, "md.dialog.new", w, h, cancel, confirm, t,
                                          core::theme::componentTokensWeChat(t));
        });
    }

    // 新建文件夹弹窗（含父目录下拉）
    void drawNewFolderDialog(eui::Ui& ui,
                             const eui::Screen& screen,
                             const core::theme::Tokens& t,
                             const components::theme::ThemeColorTokens& componentTheme) {
        const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
        const float h = 320.0f;
        // M-1（UAT2 一审）：弹窗内控件（下拉/输入框）统一 wechat 皮肤。
        const auto wcTheme = core::theme::componentTokensWeChat(t);

        // 父目录候选：根目录 + 磁盘上每一级目录
        const auto candidates = folderParentCandidates();
        std::vector<std::string> parents;
        parents.reserve(candidates.size());
        for (const auto& c : candidates) parents.push_back(c.first);
        // 已不在候选 → 0 = 根目录（显示 == 行为）
        const int parentIdx = folderParentIndexFor(candidates, newFolderParentDir_);

        ui_kit::DialogFrame frame;
        frame.id = "md.dialog.newfolder";
        frame.width = ui_kit::DialogWidth::Standard;
        frame.height = h;
        frame.open = newFolderOpen_;
        frame.onOpenChange = [this](bool v) {
            newFolderOpen_ = v;
            if (!v) newFolderParentOpen_ = false;  // 关弹窗顺手收下拉
        };

        ui_kit::drawDialogFrame(ui, screen, frame, core::theme::componentTokensWeChat(t), [&] {
                ui_kit::drawDialogTitle(ui, "md.dialog.newfolder.title", w, "新建文件夹",
                                        t.sizes.fontSizeLarge, t.colors.textPrimary);

                // 提示行带上实际落点（= 当前上级目录），避免「显示 A 建到 B」。
                ui_kit::drawDialogText(ui, "md.dialog.newfolder.hint", w,
                                       "创建在：" +
                                           parents[static_cast<size_t>(parentIdx)] +
                                           "/ · 空文件夹也会显示在树里",
                                       t.sizes.fontSizeSmall, t.colors.textTertiary,
                                       {.y = 56.0f, .height = 18.0f, .lineHeight = 18.0f});

                ui_kit::drawDialogText(ui, "md.dialog.newfolder.parent.label", w, "父目录",
                                       12.0f, t.colors.textTertiary,
                                       {.y = 84.0f, .height = 16.0f, .lineHeight = 16.0f});

                // 下拉弹层自带 zIndex=20，包一层定位 stack 即可
                ui.stack("md.dialog.newfolder.parent")
                    .position(28.0f, 102.0f)
                    .size(w - 56.0f, 40.0f)
                    .content([&] {
                        components::dropdown(ui, "md.dialog.newfolder.parent.dd")
                            .size(w - 56.0f, 40.0f)
                            .items(parents)
                            .selected(parentIdx)
                            .placeholder("根目录")
                            .theme(wcTheme)
                            .open(newFolderParentOpen_)
                            .onOpenChange(
                                [this](bool v) { newFolderParentOpen_ = v; })
                            // 命中即按当帧候选的路径落状态（capture by value），列表后续变化不影响本次选择
                            .onChange([this, candidates](int idx) {
                                if (idx < 0 ||
                                    static_cast<size_t>(idx) >= candidates.size()) {
                                    return;
                                }
                                newFolderParentDir_ = candidates[static_cast<size_t>(idx)].second;
                            })
                            .build();
                    })
                    .build();

                ui_kit::drawDialogText(ui, "md.dialog.newfolder.name.label", w, "文件夹名",
                                       12.0f, t.colors.textTertiary,
                                       {.y = 156.0f, .height = 16.0f, .lineHeight = 16.0f});

                components::input(ui, "md.dialog.newfolder.input")
                    .position(28.0f, 174.0f)
                    .size(w - 56.0f, 44.0f)
                    .value(newFolderName_)
                    .placeholder("文件夹名，如 工作")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(wcTheme)
                    .onChange([this](const std::string& v) {
                        newFolderName_ = v;
                        newFolderError_.clear();  // 错误随下一次输入清除
                    })
                    .onEnter([this] { commitNewFolder(); })
                    // Esc=取消
                    .onCancel([this] { newFolderOpen_ = false; })
                    .build();

                // 内嵌红字错误行，同新建笔记弹窗
                ui_kit::drawDialogText(ui, "md.dialog.newfolder.error", w, newFolderError_,
                                       12.0f, t.colors.error,
                                       {.y = 228.0f, .height = 18.0f, .lineHeight = 18.0f,
                                        .skipIfEmpty = true});

                ui_kit::DialogButtonSpec cancel;
                cancel.label = "取消";
                cancel.fontSize = t.sizes.fontSizeBody;
                cancel.style = ui_kit::DialogButtonStyle::SecondaryThemed;
                cancel.onClick = [this] { newFolderOpen_ = false; };
                ui_kit::DialogButtonSpec confirm;
                confirm.label = "创建";
                confirm.fontSize = t.sizes.fontSizeBody;
                confirm.style = ui_kit::DialogButtonStyle::PrimaryThemed;
                confirm.onClick = [this] { commitNewFolder(); };
                ui_kit::drawDialogButtons(ui, "md.dialog.newfolder", w, h, cancel, confirm, t,
                                          core::theme::componentTokensWeChat(t));
        });
    }

    // 外部修改冲突 toast：action「重载磁盘版本」= 重读，✕ 关闭 = 保留本地编辑；
    // duration(0) 不自动超时（二选一须显式决定）。


    // Toast 反馈
    // 普通提示走 ui_kit；外部冲突就地绘制；二者共用 md.toast 单实例且互斥。
    // M-1（UAT2 一审）：toast 属 popup，两分支统一 wechat 皮肤；页面暖橙
    // componentTheme 只喂页面主体，themeTokens 形参随之闲置（先例 drawDeleteDialog）。
    void drawToast(eui::Ui& ui, const eui::Screen& screen,
                   const components::theme::ThemeColorTokens& themeTokens) {
        const auto wcTheme =
            core::theme::componentTokensWeChat(core::theme::current());
        if (!conflictOpen_) {
            ui_kit::drawToast(ui, screen, "md.toast", toast_, wcTheme);
            return;
        }
        components::toast(ui, "md.toast")
            .theme(wcTheme)
            .screen(screen.width, screen.height)
            .visible(true)  // 本分支 conflict 必真
            .size(360.0f, 128.0f)  // 带 action 按钮加高一档
            .duration(0.0f)
            .title("文件已被外部修改")
            .message("「" + currentPath_ + "」在磁盘上被其他程序修改，"
                     "与未保存的本地编辑冲突。\n✕ 关闭 = 保留本地编辑")
            .icon(0xF071)
            .action("重载磁盘版本", [this] { resolveConflict(true); })
            .onAutoDismiss([this] {
                if (conflictOpen_) {
                    resolveConflict(false);
                } else {
                    toast_.open = false;
                }
            })
            .onDismiss([this] {
                if (conflictOpen_) {
                    resolveConflict(false);
                } else {
                    toast_.open = false;
                }
            })
            .build();
    }

    void showToast(const std::string& title, const std::string& msg, bool isError) {
        toast_.show(title, msg, isError);
    }

    // 四态切换 / 大纲

    // 实时态状态位群（文档锚 / 首帧聚焦 / 滚动位置）的单一复位点，
    // 进实时态三条路径（setMode / openNote / loadViewState）统一调用。
    void enterLiveState(const char* reason) {
        liveCursorValid_ = false;
        liveFocusPending_ = true;
        liveScrollY_ = 0.0f;
        // 滚动跟随状态位群回初始值：scrollView 本帧重建（offset 取声明值 0）
        liveFollow_ = false;
        liveProgrammaticScroll_ = false;
        liveMaxOffset_ = 0.0f;   // 上一会话的 maxOffset 不属于新文档
        liveCursorSeen_ = -1;    // -1 ≠ 任何真实光标 → 首帧判为一次光标事件
        liveContentWidth_ = 0.0f;
        // 输入框状态也换代（下一帧 release md.live.input）：只复位锚不复位状态时，
        // 上一篇的块文本 / 在飞组字 / framework undo 栈会跟着稳定 id「复活」。
        liveInputStateFresh_ = true;
        // 撤销栈随之换代（下一帧按当前文档重建基线）
        liveUndoSeeded_ = false;
        LOG_DEBUG(std::string("Markdown 实时态状态复位: ") + reason);
    }

    void setMode(ViewMode m) {
        if (m == mode_) return;
        // 离开实时态前关掉撤销合并组：别让跨态两次键入并成同一次撤销
        if (mode_ == ViewMode::Live) liveUndo_.markBoundary();
        if (m == ViewMode::Live) enterLiveState("切换视图");
        mode_ = m;
        // 进预览态时对齐编辑器位置（延迟到 compose，需要列宽与样式）
        if (m == ViewMode::Preview) syncPreviewOnEnter_ = true;
        saveViewState();  // 四态视图跨会话记忆
        LOG_INFO("Markdown 视图切换: " + std::to_string(static_cast<int>(m)));
    }

    // 全局快捷键：Ctrl+Shift+P 循环四态、Ctrl+N 新建笔记、Ctrl+S 保存、Esc 关浮层。
    // 通道 = app 级 onKeyEvent → Router 派发（input 未消费的键回调出来）。
    bool onKeyEvent(const eui::KeyEvent& key) override {
        // 弹窗输入框已挂 onCancel，聚焦时 Esc 被 input 收走，不重复
        if (key.isDown() && key.key == eui::InputKey::Escape) {
            return closeTopmostPopup();
        }
        if (key.isDown() && key.modifiers.shortcut() && key.modifiers.shift &&
            key.key == eui::InputKey::P) {
            setMode(nextMode(mode_));
            return true;
        }
        // Ctrl+S 显式落盘。四态共用 saveEditor 幂等链（无改动零操作）。
        if (key.isDown() && key.modifiers.shortcut() && !key.modifiers.shift &&
            !key.modifiers.alt && key.key == eui::InputKey::S) {
            saveEditor();
            LOG_INFO("Ctrl+S：手动保存 " + currentPath_);
            return true;
        }
        // Ctrl+N 与按钮/跨模块负载同一入口；任一弹窗开着就落空，
        // 避免清掉正在输入的草稿名。
        if (key.isDown() && key.modifiers.shortcut() && !key.modifiers.shift &&
            !key.modifiers.alt && key.key == eui::InputKey::N) {
            if (newNoteOpen_ || newFolderOpen_ || renameOpen_ ||
                deleteConfirmOpen_) {
                return false;
            }
            openNewNoteDialog();
            LOG_INFO("Ctrl+N：打开新建笔记弹窗");
            return true;
        }
        return false;
    }

    // 新建笔记弹窗统一入口：打开前重置草稿名与错误行。
    void openNewNoteDialog() {
        newNoteOpen_ = true;
        newNoteName_.clear();
        newNoteError_.clear();
    }

    // 大纲当前节：光标行所在的上一个标题；预览态沿用 activeOutlineIdx_，
    // 实时态用 liveCursorByte_（编辑器输入不在实时态帧树）。
    int activeOutlineIndex(eui::Ui& ui) {
        if (outline_.empty()) return -1;
        int line;
        if (mode_ == ViewMode::Preview) {
            return activeOutlineIdx_;
        }
        if (mode_ == ViewMode::Live) {
            line = byteOffsetToLine(editorText_, liveCursorByte_);
        } else {
            const auto& st = ui.state<components::input_detail::InputModel::InputState>(kEditorInputId);
            line = byteOffsetToLine(st.text, static_cast<size_t>(st.cursor));
        }
        int idx = -1;
        for (size_t i = 0; i < outline_.size(); ++i) {
            if (outline_[i].lineNo <= line) idx = static_cast<int>(i);
            else break;
        }
        return idx;
    }

    // 大纲点击跳转：编辑/双栏态写 cursor 到标题行首；仅预览态直接滚预览。
    // 只改 cursor 不改文本 → 不动 undo 栈。
    void jumpToHeading(eui::Ui& ui, size_t outlineIndex,
                       const components::theme::ThemeColorTokens& componentTheme) {
        if (outlineIndex >= outline_.size()) return;
        const MdHeading& h = outline_[outlineIndex];
        activeOutlineIdx_ = static_cast<int>(outlineIndex);
        if (mode_ == ViewMode::Preview) {
            const float width = lastPvContentWidth_;
            previewOffset_ = components::MarkdownBuilder::estimateHeight(
                editorText_.substr(0, h.byteOffset), width,
                components::MarkdownStyle(componentTheme));
            syncPreviewOnEnter_ = false;
            return;
        }
        // 实时态唯一文档级光标写点 = setLiveCursor（写 kEditorInputId 是死写：
        // 该输入不在帧树，且绕过换块/写块内光标/补发聚焦整条链路）。
        if (mode_ == ViewMode::Live) {
            setLiveCursor(h.byteOffset, "大纲跳转");
            return;
        }
        auto& st = ui.state<components::input_detail::InputModel::InputState>(kEditorInputId);
        st.cursor = static_cast<int>(h.byteOffset);
        st.selectionStart = st.cursor;
        st.selectionEnd = st.cursor;
        st.dragAnchor = st.cursor;
        st.hasPreferredCursorX = false;
        st.followCaret = true;
    }

    // 消费挂在本页的跳转负载（取出即清空，每帧重复调用不重复触发）：
    // "action:new_note" → 新建笔记弹窗；"open:<相对路径>" → 打开指定笔记
    void consumeNavPayload() {
        auto* router = ::core::appRouter();
        if (router == nullptr) return;
        const std::string payload = router->consumePendingPayload(pageId());
        if (payload.empty()) return;

        if (payload == "action:new_note") {
            openNewNoteDialog();
            LOG_INFO("跨模块负载：打开新建笔记弹窗");
            return;
        }
        static constexpr char kOpenPrefix[] = "open:";
        constexpr size_t kOpenLen = sizeof(kOpenPrefix) - 1;
        if (payload.size() > kOpenLen &&
            payload.compare(0, kOpenLen, kOpenPrefix) == 0) {
            openNote(payload.substr(kOpenLen));
            return;
        }
        LOG_WARN("Markdown: 未识别的跳转负载: " + payload);
    }

    // 收割「库被 GitHub Sync 导入覆盖」标记（compose 主线程）：库表被 DROP 重建，
    // notes_/allFolders_ 指向旧行 → 重查列表。
    // 当前笔记不在本函数里无条件重载——库被导入但该笔记文件没被 pull 时磁盘内容没变，
    // 无条件重载会弹假冲突，用户点「重载磁盘版本」还会用未变内容覆盖缓冲、静默丢未保存
    // 编辑。改走 checkCurrentNoteOnDisk()：只在 fileMtime > lastKnownMtime_（磁盘真变过）
    // 时才重载或弹冲突，与 5 秒磁盘轮询同一口径。
    void harvestDbImport() {
        // 早退必须在 exchange 之前：DB 未就绪时不能把脏标记消费掉丢弃。
        if (!service_) return;
        if (!dbImportedDirty_.exchange(false, std::memory_order_acq_rel)) return;
        refreshList();
        checkCurrentNoteOnDisk();
        LOG_INFO("Markdown: 检测到库导入，已重载笔记列表");
    }

    void refreshList() {
        if (!service_) {
            notes_.clear();
            allFolders_.clear();
            allTags_.clear();
            return;
        }
        // 磁盘全层目录实时枚举（含空目录），树据此显示空文件夹行
        if (auto fr = service_->listAllFolders(); fr.isOk()) {
            allFolders_ = std::set<std::string>(fr.value().begin(), fr.value().end());
        }
        auto r = searchQuery_.empty() ? service_->listAll()
                                      : service_->search(searchQuery_);
        if (r.isFail()) {
            LOG_ERROR("刷新笔记列表失败: " + r.errorMessage());
            showToast("笔记列表加载失败", r.errorMessage(), true);
            listError_ = r.errorMessage();
            notes_.clear();  // 不拿旧数据伪装成当前态
            return;
        }
        listError_.clear();
        notes_ = std::move(r.value());
        // 树只显示含当前筛选标签的笔记
        if (!tagFilter_.empty()) {
            auto fr = service_->notesWithTag(tagFilter_);
            if (fr.isOk()) {
                std::set<std::string> keep;
                for (const auto& n : fr.value()) {
                    keep.insert(n.relativePath.generic_string());
                }
                notes_.erase(
                    std::remove_if(notes_.begin(), notes_.end(),
                                   [&](const NoteMeta& n) {
                                       return keep.count(
                                                  n.relativePath.generic_string()) == 0;
                                   }),
                    notes_.end());
            } else {
                LOG_WARN("标签筛选失败: " + fr.errorMessage());
            }
        }
        refreshTags();
    }

    // 全部标签（左栏筛选区数据源）
    void refreshTags() {
        if (!service_) {
            allTags_.clear();
            return;
        }
        auto r = service_->listAllTags();
        if (r.isOk()) {
            allTags_ = std::move(r.value());
        } else {
            LOG_WARN("加载标签失败: " + r.errorMessage());
        }
        // 筛选标签已无笔记引用 → 自动取消筛选
        if (!tagFilter_.empty()) {
            bool found = false;
            for (const auto& tc : allTags_) {
                if (tc.name == tagFilter_) {
                    found = true;
                    break;
                }
            }
            if (!found) tagFilter_.clear();
        }
    }

    // 标签动作（数据全在 DB）

    void toggleTagFilter(const std::string& name) {
        tagFilter_ = (tagFilter_ == name) ? std::string() : name;
        refreshList();
        LOG_INFO("标签筛选: " + (tagFilter_.empty() ? "(无)" : tagFilter_));
    }

    // 标签写的**单一出口**：成功即发 md.tags_changed。标签只落 DB 两张表
    // （markdown_tags / markdown_note_tags，都在整库 dump 里）而**不碰笔记文件**，
    // 所以 md.saved 兜不住「只加/删标签」这类改动 —— 必须自有一条事件。
    // 加/删两个 UI 入口都走这里；将来新增第三个入口若绕过本函数直调
    // service_->setTags，同步侧就会漏掉那类改动（见 settings_module 订阅清单的注释：
    // 漏订一个 = 那类改动永远不自动同步）。调用方保证 currentPath_ 非空。
    core::Result<void> writeTags(const std::vector<std::string>& tags) {
        auto r = service_->setTags(currentPath_, tags);
        if (r.isOk()) core::event::bus().emit(core::event::kMdTagsChanged);
        return r;
    }

    // 「＋」/回车：给当前笔记加标签
    void commitAddTag() {
        if (!service_ || currentPath_.empty()) return;
        std::string name = tagInput_;
        while (!name.empty() &&
               (name.front() == ' ' || name.front() == '\t')) {
            name.erase(name.begin());
        }
        while (!name.empty() &&
               (name.back() == ' ' || name.back() == '\t' ||
                name.back() == '\r' || name.back() == '\n')) {
            name.pop_back();
        }
        tagInput_.clear();
        if (name.empty()) return;
        for (const auto& existing : currentTags_) {
            if (existing == name) return;  // 已有，不重复
        }
        currentTags_.push_back(name);
        auto r = writeTags(currentTags_);
        if (r.isFail()) {
            currentTags_.pop_back();
            LOG_ERROR("添加标签失败 " + name + ": " + r.errorMessage());
            showToast("添加标签失败", r.errorMessage(), true);
            return;
        }
        refreshTags();
        showToast("已添加标签", name, false);
        LOG_INFO("添加标签 " + name + " → " + currentPath_);
    }

    // 标签 chip ×：移除标签
    void removeTag(const std::string& name) {
        if (!service_ || currentPath_.empty()) return;
        std::vector<std::string> next;
        for (const auto& tg : currentTags_) {
            if (tg != name) next.push_back(tg);
        }
        if (next.size() == currentTags_.size()) return;
        auto r = writeTags(next);
        if (r.isFail()) {
            LOG_ERROR("删除标签失败 " + name + ": " + r.errorMessage());
            showToast("删除标签失败", r.errorMessage(), true);
            return;
        }
        currentTags_ = std::move(next);
        refreshTags();
        refreshList();  // 筛选标签可能随删除失效，树要同步
        showToast("已删除标签", name, false);
        LOG_INFO("删除标签 " + name + " ← " + currentPath_);
    }

    void openNote(const std::string& relPath) {
        saveEditor();
        // saveEditor 失败时 editorDirty_ 保持 true：还有未落盘改动就阻止切换并
        // 提示（保数据优先，改到别处会静默丢弃编辑）。
        if (editorDirty_) {
            showToast("有未保存的改动",
                      "「" + currentPath_ + "」保存失败，切换会丢失这些编辑；"
                      "请先处理保存问题或撤销改动",
                      true);
            return;
        }
        if (!service_) return;

        auto r = service_->read(relPath);
        if (r.isFail()) {
            LOG_ERROR("读取笔记失败 " + relPath + ": " + r.errorMessage());
            showToast("读取失败", "「" + relPath + "」" + r.errorMessage(), true);
            return;
        }
        currentPath_ = relPath;
        setEditorText(r.value());
        lastSavedText_ = editorText_;
        editorDirty_ = false;
        outlineDirty_ = true;
        activeOutlineIdx_ = -1;
        previewHover_.reset();  // 换笔记 = 块身份换代，旧 hover 作废
        // 换笔记 = 实时态状态群换代（见 enterLiveState）
        enterLiveState("换笔记");
        tagInput_.clear();  // 清掉上一篇残留的半截输入
        if (auto tr = service_->tagsForNote(relPath); tr.isOk()) {
            currentTags_ = std::move(tr.value());
        } else {
            currentTags_.clear();
        }
        syncCurrentMtime();  // 外部修改检测的 mtime 基准
        if (mode_ == ViewMode::Preview) syncPreviewOnEnter_ = true;
        // 新建条目的父目录跟随当前打开笔记的所在目录
        // 选中不级联：文件夹选中标记仅精确匹配，不因打开笔记而上溯选中父目录
        newItemTargetDir_ = parentDirOf(relPath);
        saveViewState();  // 选中笔记跨会话记忆
        LOG_INFO("打开笔记: " + relPath);
    }

    // 视图状态持久化

    // 恢复上次选中笔记 / 四态视图 / 收起目录；笔记已被外部删除时静默跳过。
    void loadViewState() {
        viewStore_ = std::make_unique<MarkdownViewStateStore>(::core::appDatabase());
        const MarkdownViewState st = viewStore_->load();
        if (st.mode != static_cast<int>(mode_)) {
            // 直接赋值而非 setMode：启动期无需再落一次视图状态
            mode_ = static_cast<ViewMode>(st.mode);
            if (mode_ == ViewMode::Preview) syncPreviewOnEnter_ = true;
            if (mode_ == ViewMode::Live) enterLiveState("启动恢复视图");
        }
        collapsedDirs_.insert(st.collapsedDirs.begin(), st.collapsedDirs.end());
        treePanelCollapsed_ = st.treePanelCollapsed;  // UAT2 F批：面板折叠态恢复
        // D-R3-3 c：启动即按折叠态把动画宽直接钉到目标（无起播动画，首帧即收敛）
        treePanelWidth_ = treePanelCollapsed_ ? 0.0f : kTreePanelWidth;
        // 目录已被外部删除时不在此校验，由 newItemParentDir 兜底
        selectedDir_ = st.selectedDir;
        newItemTargetDir_ = selectedDir_;  // 启动时与选中态同步，后续由 openNote / selectDir 分别更新
        if (st.lastPath.empty()) return;
        const bool exists =
            std::any_of(notes_.begin(), notes_.end(),
                        [&](const NoteMeta& n) {
                            return n.relativePath.generic_string() == st.lastPath;
                        });
        if (!exists) {
            LOG_INFO("Markdown: 上次笔记已不存在，跳过恢复: " + st.lastPath);
            return;
        }
        openNote(st.lastPath);
        LOG_INFO("Markdown: 已恢复上次笔记: " + st.lastPath);
    }

    // 视图状态写失败只记日志，不影响编辑主流程
    void saveViewState() const {
        if (!viewStore_) return;
        MarkdownViewState st;
        st.lastPath = currentPath_;
        st.mode = static_cast<int>(mode_);
        st.collapsedDirs.assign(collapsedDirs_.begin(), collapsedDirs_.end());
        st.selectedDir = selectedDir_;
        st.treePanelCollapsed = treePanelCollapsed_;
        auto r = viewStore_->save(st);
        if (r.isFail()) {
            LOG_WARN("Markdown: 视图状态落盘失败（不影响编辑）: " + r.errorMessage());
        }
    }

    // 有未落盘改动时写回文件 + 索引（每帧调用，自带防抖）；
    // 保存失败走指数退避，避免 dirty 期间每帧重试打失败 IO/日志。
    // conflictOpen_ 时不写：磁盘版本已变（外部修改 / 库导入拉回新内容），此刻落盘
    // 就是用本地旧缓冲盖掉刚 pull 下来的内容——等用户在冲突弹窗里选完再写。
    void flushEditorIfIdle() {
        if (conflictOpen_) return;
        if (!editorDirty_) return;
        const auto now = std::chrono::steady_clock::now();
        const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - lastEditAt_).count();
        if (idleMs < kAutoSaveDelayMs) return;
        const auto sinceAttemptMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        now - lastSaveAttemptAt_).count();
        if (sinceAttemptMs < saveBackoffMs_) return;
        lastSaveAttemptAt_ = now;
        saveEditor();
    }

    void saveEditor() {
        if (!service_ || currentPath_.empty()) return;
        if (editorText_ == lastSavedText_) {
            editorDirty_ = false;
            saveFailed_ = false;
            return;
        }
        auto r = service_->write(currentPath_, editorText_);
        if (r.isFail()) {
            LOG_ERROR("保存笔记失败 " + currentPath_ + ": " + r.errorMessage());
            saveFailed_ = true;
            // 500ms → 1s → 2s → … 封顶 60s
            saveBackoffMs_ = (saveBackoffMs_ == 0)
                ? 500 : std::min(saveBackoffMs_ * 2, kMaxSaveBackoffMs);
            // 失败重试期间只在无 toast 展示时弹一次
            if (!toast_.open) {
                showToast("保存失败",
                          "「" + currentPath_ + "」" + r.errorMessage() +
                              "，已自动重试",
                          true);
            }
            return;
        }
        lastSavedText_ = editorText_;
        editorDirty_ = false;
        saveFailed_ = false;
        saveBackoffMs_ = 0;
        lastSavedAt_ = std::time(nullptr);
        syncCurrentMtime();  // 自己写盘后推进 mtime 基准（避免误报外部修改）
        // 写盘成功 = 本地版本已在磁盘上 —— 若冲突 toast 还开着，它的另一半答案
        //（「重载磁盘版本」）会把刚保存的内容再丢掉，选项与现实不符：此刻等价于用户
        // 选了「保留本地」（✕ 关闭那条），立刻收掉。自动链路（flushEditorIfIdle）
        // 在 conflictOpen_ 时根本不写，所以只有显式保存路径（Ctrl+S / 切换笔记 /
        // 退出模块）会走到这里 —— 外部版本仍可从笔记 git 历史 / 远端找回。
        if (conflictOpen_) resolveConflict(false);
        core::event::bus().emit(core::event::kMdSaved);
        // 写盘成功 → 触发 git 自动提交（节流判定在 onFileSaved 内，<5 分钟不重复）
        if (noteGit_) {
            noteGit_->onFileSaved(lastSavedAt_);
        }
    }

    // 文件变化自动感知（5 秒轮询）

    // 每 5 秒一次（compose 每帧调用，steady_clock 节流）
    void pollDiskChangesIfDue() {
        if (!service_) return;
        const auto now = std::chrono::steady_clock::now();
        const auto sinceMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - lastPollAt_).count();
        if (!service_->hasWatchCredit() && sinceMs < pollIntervalMs_) return;
        lastPollAt_ = now;

        // 1) 目录级轻量比对：有差异才做全量 rescan
        auto changed = service_->pollChanged();
        if (changed.isOk() && changed.value()) {
            auto r = service_->rescan();
            if (r.isOk()) {
                listError_.clear();
                refreshList();
            } else {
                LOG_WARN("自动重扫失败: " + r.errorMessage());
            }
        }
        // 2) 当前笔记的外部修改 / 删除检测
        checkCurrentNoteOnDisk();
    }

    void checkCurrentNoteOnDisk() {
        if (currentPath_.empty() || conflictOpen_) return;
        auto m = service_->fileMtime(currentPath_);
        if (m.isFail()) {
            // 文件被外部删除：lastKnownMtime_ = -1 做已知缺失标记（防重复 toast）
            if (lastKnownMtime_ == -1) return;
            lastKnownMtime_ = -1;
            if (!editorDirty_) {
                currentPath_.clear();
                setEditorText(std::string());
                lastSavedText_.clear();
                outlineDirty_ = true;
                activeOutlineIdx_ = -1;
                saveViewState();  // 已删笔记不该在下次启动被恢复
                showToast("笔记已被删除", "当前笔记已从磁盘移除", true);
            } else {
                // 有未保存编辑：write() 会重建文件，提示一次即可
                showToast("笔记已被外部删除",
                          "保留的本地编辑会在保存时重新创建该文件", true);
            }
            return;
        }
        if (m.value() > lastKnownMtime_) {
            if (!editorDirty_) {
                // 无本地改动：静默重载
                reloadCurrentNote(true);
            } else {
                // 有未保存编辑：弹「重载 / 保留本地」选择（只弹一次）
                conflictOpen_ = true;
            }
        }
    }

    // 从磁盘重读当前笔记（不走 openNote 的「先保存旧笔记」路径）。
    // 重载后按内容身份映射旧活动块：三元组命中 = 精确实例；回退 (kind,hash)
    // 取 |start 差| 最小者；零命中钳位到旧光标对应新块。
    void reloadCurrentNote(bool silent) {
        auto r = service_->read(currentPath_);
        if (r.isFail()) {
            if (!silent) {
                showToast("重载失败", r.errorMessage(), true);
            }
            return;
        }
        // 步骤 1（setEditorText 之前）：记下活动块内容身份五元组
        struct CapturedBlock {
            bool valid = false;
            MdBlockKind kind = MdBlockKind::Paragraph;
            uint64_t hash = 0;
            int stableId = -1;
            size_t start = 0;
            size_t local = 0;   // 块内字节偏移（步骤 3 重定位要写回锚）
        } captured;
        if (mode_ == ViewMode::Live && liveCursorValid_ &&
            !blockModel_.blocks().empty()) {
            const size_t idx = mdLiveActiveBlockIndex(blockModel_, liveCursorByte_);
            const MdBlock& b = blockModel_.blocks()[idx];
            captured = {true, b.kind, b.hash, b.stableId, b.start,
                        mdLiveBlockLocalOffset(b, liveCursorByte_)};
        }
        setEditorText(r.value());
        // 步骤 3（setEditorText 之后、resetLiveUndoBaseline 之前）：两分支都
        // 显式写 liveCursorByte_，撤销基线才读到重定位后的字节。
        if (captured.valid) {
            const std::vector<MdBlock>& bs = blockModel_.blocks();
            const size_t newIdx =
                mdFindBlockByContent(bs, captured.kind, captured.hash,
                                     captured.stableId, captured.start);
            if (newIdx != std::string::npos) {
                // 命中：liveCursorByte_ = 新块起点 + 块内光标（钳进块长）。
                // liveInputStateFresh_ 与 liveFocusPending_ 成对（释放清焦点须同帧补发）。
                const MdBlock& nb = bs[newIdx];
                const size_t len = nb.end > nb.start ? nb.end - nb.start : 0;
                liveActiveBlock_ = newIdx;
                liveInputStart_ = nb.start;
                liveCursorByte_ = nb.start + std::min(captured.local, len);
                liveCursorValid_ = true;
                liveInputStateFresh_ = true;
                liveFocusPending_ = true;
            } else {
                // 零命中：内容被删改消失 / 文档变空。钳位到旧光标对应新块（保守选择）；
                // 文档变空时 blocks()[0] 越界，单列分支归零。
                if (bs.empty()) {
                    liveActiveBlock_ = 0;
                    liveInputStart_ = 0;
                    liveCursorByte_ = 0;
                } else {
                    const size_t clamped =
                        std::min(liveCursorByte_, blockModel_.text().size());
                    liveActiveBlock_ = blockModel_.blockIndexAt(clamped);
                    liveInputStart_ = bs[liveActiveBlock_].start;
                    liveCursorByte_ = clamped;
                }
                liveCursorValid_ = true;
                liveInputStateFresh_ = true;
                liveFocusPending_ = true;
            }
        }
        // 外部重载 = 撤销基线换代（不换代时 Ctrl+Z 会把外部改动整篇盖回本地旧版）
        if (mode_ == ViewMode::Live) {
            resetLiveUndoBaseline();
        } else {
            liveUndoSeeded_ = false;
        }
        lastSavedText_ = editorText_;
        editorDirty_ = false;
        outlineDirty_ = true;
        activeOutlineIdx_ = -1;
        syncCurrentMtime();
        if (mode_ == ViewMode::Preview) syncPreviewOnEnter_ = true;
        if (silent) {
            // 静默重载必须弹 toast（屏幕内容变了）；冲突路径不弹
            //（action 点击后 toast 必关，立刻再弹会被 onDismiss 连带清掉）。
            showToast("检测到外部修改，已自动重载", currentPath_, false);
        }
        LOG_INFO("重载笔记: " + currentPath_);
    }

    void resolveConflict(bool reloadFromDisk) {
        conflictOpen_ = false;
        if (reloadFromDisk) {
            // 重载本身即可见反馈（action 点击后 toast 必关，立刻再弹会被清掉）
            reloadCurrentNote(false);
        } else {
            // 保留本地：基准 mtime 推到磁盘当前值，未保存编辑走自动保存链路
            syncCurrentMtime();
            LOG_INFO("冲突处理：保留本地编辑 " + currentPath_);
        }
    }

    // lastKnownMtime_ ← 磁盘当前 mtime
    void syncCurrentMtime() {
        if (currentPath_.empty()) {
            lastKnownMtime_ = 0;
            return;
        }
        auto m = service_->fileMtime(currentPath_);
        lastKnownMtime_ = m.isOk() ? m.value() : -1;
    }

    void commitNewNote(eui::Ui& ui) {
        std::string name = newNoteName_;
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
            name.erase(name.begin());
        }
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
            name.pop_back();
        }
        // 空名提交 = 建「无标题笔记」撞名自动编号，不报错；显式命名撞名则直接打开。
        const bool defaultedName = name.empty();
        if (defaultedName) name = "无标题笔记";
        if (!service_) return;

        // 父目录 = 当前选中目录（失效时回根目录兜底）；名字可再带子路径，两者叠加。
        const std::string parent = newItemParentDir();

        // 路径安全校验（service 层有同规则兜底，这里给友好提示）
        std::string relPath = parent.empty() ? name : parent + "/" + name;
        if (!MarkdownService::isSafeNewNotePath(relPath)) {
            // 校验错误内嵌红字
            newNoteError_ =
                "笔记名不合法：不支持绝对路径、盘符（如 C:）或 ..，请用相对路径";
            return;
        }

        // 无扩展名时补 .md（与 MarkdownService::create 一致）
        relPath = withMdExtension(relPath);

        // 显式命名已存在 → 直接打开；默认名走「永远新建」编号
        if (!defaultedName && service_->read(relPath).isOk()) {
            newNoteOpen_ = false;
            refreshList();
            openNote(relPath);
            showToast("笔记已存在", "已直接打开 " + relPath, false);
            if (currentPath_ == relPath) ui.requestFocus(kEditorInputHitId);
            return;
        }
        if (defaultedName) {
            // 默认名撞名 → 「无标题笔记 N.md」自动编号；只换文件名、保留父目录
            for (int i = 2; service_->fileMtime(relPath).isOk(); ++i) {
                if (i > 999) {
                    newNoteError_ = "新建失败：同名无标题笔记过多，请手动命名";
                    return;
                }
                const std::string numbered =
                    "无标题笔记 " + std::to_string(i) + ".md";
                relPath = parent.empty() ? numbered : parent + "/" + numbered;
            }
        }

        auto r = service_->create(relPath);
        if (r.isFail()) {
            LOG_ERROR("新建笔记失败 " + relPath + ": " + r.errorMessage());
            newNoteError_ = "新建失败：" + r.errorMessage();
            return;
        }
        newNoteOpen_ = false;
        refreshList();
        openNote(relPath);
        showToast("已创建笔记", relPath, false);
        // 新建后自动 focus 编辑器（requestFocus 帧末消费）；
        // openNote 守卫失败（未保存改动阻塞切换）时不聚焦。
        if (currentPath_ == relPath) ui.requestFocus(kEditorInputHitId);
        LOG_INFO("新建笔记: " + relPath);
        core::event::bus().emit(core::event::kMdCreated);
    }

    // 新建文件夹（左栏「+ 文件夹」入口）

    // 相对路径的父目录（POSIX；无 '/' → "" 即根目录）
    static std::string parentDirOf(const std::string& relPath) {
        const size_t slash = relPath.rfind('/');
        return slash == std::string::npos ? std::string() : relPath.substr(0, slash);
    }

    // 目录是否存在于当前磁盘清单（"" = 根目录，恒存在）
    bool dirExistsOnDisk(const std::string& dir) const {
        return dir.empty() || allFolders_.count(dir) != 0;
    }

    // 新建条目的父目录 = newItemTargetDir_（由点击文件夹 / 打开笔记同步更新）；
    // 已被外部删除 → 回根目录 + WARN。
    // "" 既表「根目录」也表兜底，区别只在日志。
    std::string newItemParentDir() const {
        if (newItemTargetDir_.empty()) return std::string();          // 根目录
        if (dirExistsOnDisk(newItemTargetDir_)) return newItemTargetDir_;
        LOG_WARN("Markdown: 新建目标目录已不存在（外部删除？），回退到根目录: " +
                 newItemTargetDir_);
        return std::string();                                         // 兜底
    }

    // 选中目录变化 → 记住 + 落盘（跨会话）
    // 用户直接点文件夹时视觉选中与功能目标同步更新
    void selectDir(const std::string& dir) {
        if (selectedDir_ == dir) return;
        selectedDir_ = dir;
        newItemTargetDir_ = dir;
        saveViewState();
    }

    // 面板折叠/展开（UAT2 F批）：翻转 + 落盘（与目录折叠同口径，跨会话记忆）
    void toggleTreePanel() {
        treePanelCollapsed_ = !treePanelCollapsed_;
        saveViewState();
    }

    // 文件名规范化：以 .md 结尾（rescan 只索引 .md，否则笔记从树里消失）
    static std::string withMdExtension(std::string name) {
        if (name.size() >= 3 && name.compare(name.size() - 3, 3, ".md") == 0) {
            return name;
        }
        return name + ".md";
    }

    // 「上级目录」候选：根目录 + 磁盘上每一级目录（层级不设上限）。
    // 返回 (显示名, 相对路径)，显示名 = 相对路径（根目录显示为「根目录」）。
    std::vector<std::pair<std::string, std::string>> folderParentCandidates() const {
        std::vector<std::pair<std::string, std::string>> out;
        out.emplace_back("根目录", std::string());
        for (const auto& d : allFolders_) {
            if (d.empty()) continue;
            out.emplace_back(d, d);
        }
        return out;
    }

    // 候选里某目录的下标（找不到 → 0 = 根目录，避免「显示 A 却建到 B」）
    static int folderParentIndexFor(
        const std::vector<std::pair<std::string, std::string>>& candidates,
        const std::string& dir) {
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (candidates[i].second == dir) return static_cast<int>(i);
        }
        return 0;
    }

    void commitNewFolder() {
        std::string name = newFolderName_;
        // 去掉首尾空白与多余的尾部 '/'
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
            name.erase(name.begin());
        }
        while (!name.empty() &&
               (name.back() == ' ' || name.back() == '\t' || name.back() == '/')) {
            name.pop_back();
        }
        if (name.empty()) {
            // 校验错误内嵌红字
            newFolderError_ = "文件夹名不能为空";
            return;
        }
        if (!service_) return;

        // 上级目录 = 存下的路径（非下标）+ 名字；路径不在清单 → 落回根目录并出声
        const auto candidates = folderParentCandidates();
        const std::string parent =
            candidates[static_cast<size_t>(folderParentIndexFor(candidates,
                                                               newFolderParentDir_))]
                .second;
        if (parent != newFolderParentDir_) {
            LOG_WARN("Markdown: 上级目录已不存在（外部删除？），改在根目录创建: " +
                     newFolderParentDir_);
        }
        const std::string relPath = parent.empty() ? name : parent + "/" + name;

        auto r = service_->createFolder(relPath);
        if (r.isFail()) {
            LOG_ERROR("新建文件夹失败 " + relPath + ": " + r.errorMessage());
            newFolderError_ = "新建失败：" + r.errorMessage();
            return;
        }
        newFolderOpen_ = false;
        newFolderParentOpen_ = false;
        refreshList();  // listAllFolders 磁盘枚举 → 树里立即出现空文件夹行
        // 新建的目录 = 新的选中目录（接着「+ 笔记」即落在此夹）
        selectDir(relPath);
        showToast("已创建文件夹", relPath, false);
        LOG_INFO("新建文件夹: " + relPath);
    }

// 生命周期：右键菜单 / 删除 / 重命名

// 右键文件行：指针逻辑坐标经 menuAnchorForPointerInPage 换算成页面坐标
//（runtime 已做 /dpiScale；scrollOffsetY=0 = 指针即视觉位）。
    void openFileContextMenu(const std::string& relPath, const eui::PointerEvent& ev) {
        ctxMenuPath_ = relPath;
        const core::MenuAnchor anchor = menuAnchorForPointerInPage(ev);
        ctxMenuX_ = anchor.x;
        ctxMenuY_ = anchor.y;
        ctxMenuOpen_ = true;
    }

    void drawNoteContextMenu(eui::Ui& ui,
                             const eui::Screen& screen,
                             const core::theme::Tokens& t,
                             const components::theme::ThemeColorTokens& componentTheme) {
        // M-1（UAT2 一审）：右键菜单属 popup，统一 wechat 皮肤；页面暖橙
        // componentTheme 只喂页面主体。
        const auto wcTheme = core::theme::componentTokensWeChat(t);
// 无右键目标时跳过 build 并强制置关：否则组件不渲染 dismiss 层，
// 点击外部无处触发 onOpenChange(false)。
        if (ctxMenuPath_.empty()) {
            ctxMenuOpen_ = false;
            return;
        }
        components::contextMenu(ui, "md.ctx")
            .open(ctxMenuOpen_)
            .screen(screen.width, screen.height)
            .position(ctxMenuX_, ctxMenuY_)
            .autoWidth()  // 宽度按最宽项自适应
            .items({"重命名", "删除笔记"})
            .theme(wcTheme)
            .onSelect([this](int index) {
                if (index == 0) {
                    renameOpen_ = true;
                    renameTargetPath_ = ctxMenuPath_;
                    renameInput_ = ctxMenuPath_;  // 预填相对路径（含文件夹）
                    renameError_.clear();         // 打开清残留错误
                } else if (index == 1) {
                    deleteConfirmOpen_ = true;
                    deleteTargetPath_ = ctxMenuPath_;
                }
            })
            .onOpenChange([this](bool v) { ctxMenuOpen_ = v; })
            .build();
    }

// 编辑器右键「插入表格」：行→列两级级联菜单；行数含表头行。
    static std::vector<components::ContextMenuItem> buildTableMenuItems() {
        std::vector<components::ContextMenuItem> rowItems;
        for (int r = 1; r <= 6; ++r) {
            std::vector<components::ContextMenuItem> colItems;
            for (int c = 1; c <= 8; ++c) {
                colItems.emplace_back(std::to_string(c) + " 列");
            }
            rowItems.emplace_back(std::to_string(r) + " 行", std::move(colItems));
        }
        std::vector<components::ContextMenuItem> items;
        items.emplace_back("插入表格", std::move(rowItems));
        return items;
    }

    void drawEditorContextMenu(eui::Ui& ui,
                               const eui::Screen& screen,
                               const core::theme::Tokens& t,
                               const components::theme::ThemeColorTokens& componentTheme) {
        // M-1（UAT2 一审）：编辑器右键菜单属 popup，统一 wechat 皮肤。
        const auto wcTheme = core::theme::componentTokensWeChat(t);
        // 只在打开时构建（关闭态无覆盖层残留）
        if (!editorCtxOpen_) return;
        components::contextMenu(ui, "md.editor.ctx")
            .open(editorCtxOpen_)
            .screen(screen.width, screen.height)
            .position(editorCtxX_, editorCtxY_)
            .autoWidth()  // 宽度按最宽项自适应
            .items(buildTableMenuItems())
            .theme(wcTheme)
            .onSelectPath([this, &ui](const std::vector<int>& path) {
                // path = [0(插入表格), 行下标, 列下标]
                if (path.size() == 3 && path[0] == 0) {
                    insertTableAtCursor(ui, path[1] + 1, path[2] + 1);
                }
            })
            .onOpenChange([this](bool v) { editorCtxOpen_ = v; })
            .build();
    }

    // 光标处插表格模板（与键盘流同一写回：可 undo、走脏标记链路）
    void insertTableAtCursor(eui::Ui& ui, int rows, int cols) {
        using InputState = components::input_detail::InputModel::InputState;
        if (!service_ || currentPath_.empty()) return;
        if (ui.find(kEditorInputHitId) == nullptr) return;  // 预览态无编辑器
        auto& st = ui.state<InputState>(kEditorInputId);
        EditBuffer buf{st.text, st.cursor, st.selectionStart, st.selectionEnd};
        module::markdown::applyInsertTable(buf, rows, cols);
        commitEditBuffer(ui, buf);
        ui.requestFocus(kEditorInputHitId);
    }

    // 删除确认弹窗（不可恢复文案）
    void drawDeleteDialog(eui::Ui& ui,
                          const eui::Screen& screen,
                          const core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& componentTheme) {
        const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
        const float h = 200.0f;

        ui_kit::DialogFrame frame;
        frame.id = "md.dialog.del";
        frame.width = ui_kit::DialogWidth::Standard;
        frame.height = h;
        frame.open = deleteConfirmOpen_;
        frame.onOpenChange = [this](bool v) { deleteConfirmOpen_ = v; };

        ui_kit::drawDialogFrame(ui, screen, frame, core::theme::componentTokensWeChat(t), [&] {
                ui_kit::drawDialogTitle(ui, "md.dialog.del.title", w, "删除笔记",
                                        t.sizes.fontSizeLarge + 2.0f, t.colors.textPrimary);
                ui_kit::drawDialogText(
                    ui, "md.dialog.del.body", w,
                    "将删除笔记「" + deleteTargetPath_ + "」及其磁盘文件，不可恢复。",
                    t.sizes.fontSizeBody - 1.0f, t.colors.textSecondary,
                    {.y = 60.0f, .height = 40.0f, .lineHeight = 20.0f, .wrap = true});

                ui_kit::DialogButtonSpec cancel;
                cancel.label = "取消";
                cancel.fontSize = t.sizes.fontSizeBody - 1.0f;
                cancel.style = ui_kit::DialogButtonStyle::SecondaryThemed;
                cancel.onClick = [this] { deleteConfirmOpen_ = false; };
                ui_kit::DialogButtonSpec confirm;
                confirm.label = "删除";
                confirm.fontSize = t.sizes.fontSizeBody - 1.0f;
                confirm.style = ui_kit::DialogButtonStyle::DangerThemed;
                confirm.onClick = [this] { doDeleteNote(); };
                ui_kit::drawDialogButtons(ui, "md.dialog.del", w, h, cancel, confirm, t,
                                          core::theme::componentTokensWeChat(t));
        });
    }

    // 重命名对话框（预填当前相对路径；填含 / 的路径 = 同时移动）
    void drawRenameDialog(eui::Ui& ui,
                          const eui::Screen& screen,
                          const core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& componentTheme) {
        const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
        const float h = 220.0f;
        // M-1（UAT2 一审）：同类的第七处——重命名弹窗输入框此前也漏吃页面暖橙，
        // 逐处 Read 核对 popup 控件时补上（frame/buttons 本已是 wechat）。
        const auto wcTheme = core::theme::componentTokensWeChat(t);

        ui_kit::DialogFrame frame;
        frame.id = "md.dialog.rename";
        frame.width = ui_kit::DialogWidth::Standard;
        frame.height = h;
        frame.open = renameOpen_;
        frame.onOpenChange = [this](bool v) { renameOpen_ = v; };

        ui_kit::drawDialogFrame(ui, screen, frame, core::theme::componentTokensWeChat(t), [&] {
                ui_kit::drawDialogTitle(ui, "md.dialog.rename.title", w, "重命名笔记",
                                        t.sizes.fontSizeLarge, t.colors.textPrimary);
                ui_kit::drawDialogText(ui, "md.dialog.rename.hint", w,
                                       "相对笔记目录；填「工作/新名」可同时移动。"
                                       "笔记一律以 .md 结尾：不写会自动补上",
                                       t.sizes.fontSizeSmall, t.colors.textTertiary,
                                       {.y = 56.0f, .height = 18.0f, .lineHeight = 18.0f});
                components::input(ui, "md.dialog.rename.input")
                    .position(28.0f, 84.0f)
                    .size(w - 56.0f, 44.0f)
                    .value(renameInput_)
                    .placeholder("新名字或新路径")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(wcTheme)
                    .onChange([this](const std::string& v) {
                        renameInput_ = v;
                        renameError_.clear();  // 错误随下一次输入清除
                    })
                    .onEnter([this] { commitRenameNote(); })
                    // Esc=取消（关弹窗不改名）
                    .onCancel([this] { renameOpen_ = false; })
                    .build();
                // 校验/落盘失败内嵌红字，弹窗保持打开让用户改输入
                ui_kit::drawDialogText(ui, "md.dialog.rename.error", w, renameError_,
                                       12.0f, t.colors.error,
                                       {.y = 134.0f, .height = 18.0f, .lineHeight = 18.0f,
                                        .skipIfEmpty = true});
                ui_kit::DialogButtonSpec cancel;
                cancel.label = "取消";
                cancel.fontSize = t.sizes.fontSizeBody;
                cancel.style = ui_kit::DialogButtonStyle::SecondaryThemed;
                cancel.onClick = [this] { renameOpen_ = false; };
                ui_kit::DialogButtonSpec confirm;
                confirm.label = "重命名";
                confirm.fontSize = t.sizes.fontSizeBody;
                confirm.style = ui_kit::DialogButtonStyle::PrimaryThemed;
                confirm.onClick = [this] { commitRenameNote(); };
                ui_kit::drawDialogButtons(ui, "md.dialog.rename", w, h, cancel, confirm, t,
                                          core::theme::componentTokensWeChat(t));
        });
    }

    void doDeleteNote() {
        const std::string path = deleteTargetPath_;
        deleteConfirmOpen_ = false;
        deleteTargetPath_.clear();
        if (!service_ || path.empty()) return;
        auto r = service_->remove(path);
        if (r.isFail()) {
            LOG_ERROR("删除笔记失败 " + path + ": " + r.errorMessage());
            showToast("删除失败", "「" + path + "」" + r.errorMessage(), true);
            return;
        }
        // 删的是当前笔记 → 清空编辑器并清 dirty：文件已不存在，未保存编辑作废，
        // 不做「删了又写回去」的复活。
        if (currentPath_ == path) {
            currentPath_.clear();
            setEditorText(std::string());
            lastSavedText_.clear();
            editorDirty_ = false;
            saveFailed_ = false;
            saveViewState();  // 删的是当前笔记 → 视图状态同步清空
            saveBackoffMs_ = 0;
            outlineDirty_ = true;
            activeOutlineIdx_ = -1;
            currentTags_.clear();
            lastKnownMtime_ = 0;
            conflictOpen_ = false;
        }
        refreshList();
        showToast("已删除笔记", fs::path(path).filename().string(), false);
        LOG_INFO("已删除笔记: " + path);
        core::event::bus().emit(core::event::kMdDeleted);
    }

    void commitRenameNote() {
        std::string target = renameInput_;
        while (!target.empty() && (target.front() == ' ' || target.front() == '\t')) {
            target.erase(target.begin());
        }
        while (!target.empty() &&
               (target.back() == ' ' || target.back() == '\t' ||
                target.back() == '\r' || target.back() == '\n')) {
            target.pop_back();
        }
        if (target.empty()) {
            // 空名内嵌红字（统一各弹窗校验反馈），弹窗保持打开让用户重填
            renameError_ = "新名字不能为空";
            return;
        }
        if (!service_ || renameTargetPath_.empty()) {
            renameOpen_ = false;
            return;
        }
        if (!MarkdownService::isSafeNewNotePath(target)) {
            renameError_ =
                "名称不合法：不支持绝对路径、盘符（如 C:）或 ..，请用相对路径";
            return;  // 弹窗保持打开让用户改
        }
        // 扩展名只在 service 层规范化，且是追加而非替换 ——
        // 索引/树/搜索只认 .md，替换会静默丢用户敲进去的字符。
        auto r = service_->rename(renameTargetPath_, target);
        if (r.isFail()) {
            LOG_ERROR("重命名笔记失败 " + renameTargetPath_ + " → " + target +
                      ": " + r.errorMessage());
            renameError_ = "重命名失败：" + r.errorMessage();
            return;
        }
        const std::string oldPath = renameTargetPath_;
        const std::string newPath = r.value();
        renameOpen_ = false;
        renameTargetPath_.clear();
        renameInput_.clear();
        renameError_.clear();
        if (currentPath_ == oldPath) {
            // 编辑器继续跟随（未保存编辑的自动保存写到新路径）
            currentPath_ = newPath;
            syncCurrentMtime();
            saveViewState();  // 重命名后恢复新路径
        }
        refreshList();
        showToast("已重命名", newPath, false);
        LOG_INFO("重命名笔记: " + oldPath + " → " + newPath);
        core::event::bus().emit(core::event::kMdRenamed);
    }


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
    // 未创建 = 不持久化，saveViewState 静默跳过
    std::unique_ptr<MarkdownViewStateStore> viewStore_;
    std::vector<NoteMeta> notes_;      // 当前展示（全部或搜索结果）
    std::string searchQuery_;          // 搜索框内容
    std::string currentPath_;          // 打开的笔记（POSIX 相对路径）
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

// 测试缝实现（声明与理由见 markdown_module_test_seam.h）。唯一消费方是
// tests/test_modules/test_markdown.cpp，入口是 create_markdown_module()，故
// static_cast 安全 —— 该缝不对外承诺接受任意 core::Module*。
void mdSetPollIntervalMsForTest(::core::Module* mod, int64_t ms) {
    if (mod == nullptr || ms <= 0) return;
    static_cast<MarkdownModule*>(mod)->pollIntervalMs_ = ms;
}

} // namespace module::markdown

extern "C" ::core::Module* create_markdown_module() {
    return new module::markdown::MarkdownModule();
}
