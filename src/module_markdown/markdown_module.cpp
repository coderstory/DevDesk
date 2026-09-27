// src/module_markdown/markdown_module.cpp
// Markdown 笔记本模块 —— 真实文件读写 + FTS5 搜索（3 栏 UI）
//
// 数据流：
//   onStart()  → 从 core::appDatabase() 拿共享连接 → MarkdownService → rescan()
//   compose()  → 左栏二级目录树（md_tree 纯函数分组）/ 中栏编辑器（真实读文件、
//                改即写盘）/ 右栏预览（components::markdown 渲染成 eui 节点）
//
// Phase 1 编辑体验（docs/superpowers/specs/2026-09-22-markdown-editor-ux-design.md）：
//   - 编辑 / 实时 / 双栏 / 预览四态切换（顶栏 segmented + Ctrl+Shift+P 循环）
//   - 双栏同步滚动（标题锚点 + 段内线性插值，编辑器单向驱动预览）
//   - 编辑器底部状态栏（保存状态 ●/✓ + 行:列 · N 字）
//
// W8 左栏重构（用户确认方案）：
//   - 左栏 = 二级目录树（顶层文件夹 → 二级文件，根目录散文件单独一组，
//     文件夹可展开/收起，搜索时展平显示命中文件）
//   - 大纲 = 顶栏开关 + 浮动面板（positioned + zIndex，不挤压编辑区）
//   - 文件变化自动感知：5 秒轮询（pollChanged 轻量比对 → rescan），
//     当前笔记被外部修改时弹「重载磁盘版本 / 保留本地」toast（action 按钮 =
//     重载；✕ 关闭 = 保留本地；不自动超时，用户必须二选一）
//
// Phase 2 键盘流（spec §5.2，通道 A = input.onKeyEvent passthrough）：
//   Tab/Shift+Tab 缩进、Enter 列表续行、Ctrl+B/I 包裹、Ctrl+K 链接骨架；
//   纯函数在 md_keyboard.{h,cpp}，本文件只做接线（handleEditorKey）
//
// Phase C 编辑器语法高亮（styled-runs，ux-design §6.2 色表）：
//   编辑器 input 挂 .lineStyles(回调) → md_highlight 行级词法扫描（围栏
//   状态与大纲共用 md_line_scanner）；主题切换 bump styleRevision 重高亮
//
// 所有 I/O 都通过 MarkdownService，失败走 core::Result（不抛异常）。
#include "core/database.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/router.h"
#include "core/theme.h"
#include "core/ui_geometry.h"
#include "core/component_theme.h"
#include "core/time.h"
#include <ui_kit/toast.h>
#include "markdown_service.h"
#include "md_block_model.h"
#include "md_block_interact.h"
#include "md_highlight.h"
#include "md_ids.h"
#include "md_keyboard.h"
#include "md_line_scanner.h"
#include "md_live_edit.h"
#include "md_preview_highlight.h"
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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace module::markdown {

namespace {

namespace fs = std::filesystem;

// 单行树条目。整行包一层 stack（W7 已知坑：position() 只在 stack 内生效，
// 行元素直接做 scrollView column 的流式子元素时其 position 会被静默忽略，
// 图标/名字会被流式排到下一行）——行是 column 的流式子元素，行内的
// 背景 rect / icon / name 才用 position 定位。
// 文件树行（icon 为 FA7 Free-Solid 码点；📄 emoji 已换 fa-file-lines）
// onContextMenu（审计 [4][20]）：右键文件行弹 重命名/删除 菜单；null = 无菜单
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
                          onContextMenu = {}) {
    ui.stack(id)
        .size(width, 32.0f)
        .content([&] {
            ui.rect(id + ".row")
                .size(width, 32.0f)
                // 行底 = 所在结构性面板底（background），非卡片白——hover/
                // active 才显色（主题层级：页面结构 vs 浮动卡片）
                .color(active ? t.colors.primarySoftBg : t.colors.background)
                .hoverColor(t.colors.hover)
                .onClick(std::move(onClick))
                .onContextMenu(std::move(onContextMenu))
                .build();
            ui.text(id + ".icon")
                .position(12.0f + indent, 0.0f)
                .size(20.0f, 32.0f)
                .icon(icon)
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(32.0f)
                .color(t.colors.textSecondary)
                .build();
            ui.text(id + ".name")
                .position(36.0f + indent, 0.0f)
                .size(std::max(0.0f, width - 48.0f - indent), 32.0f)
                .text(name)
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(32.0f)
                .color(active ? t.colors.primaryText : t.colors.textPrimary)
                .fontWeight(active ? 700 : 400)
                .build();
        })
        .build();
}

// 把 "inbox/ideas.md" 显示成文件名 + 目录提示
std::string noteDisplayName(const NoteMeta& n) {
    return n.relativePath.filename().string();
}

std::string noteDisplayPath(const NoteMeta& n) {
    return n.relativePath.generic_string();
}

// ---------- Phase 1 常量 ----------

// 四态视图（枚举值 = md_view_state.json 的持久化口径，不是 segmented 下标）：
// 0=Edit 1=Split 2=Preview 是已落盘语义，Live 只做追加（免迁移）
enum class ViewMode { Edit = 0, Split = 1, Preview = 2, Live = 3 };

// 四态视觉顺序（= Ctrl+Shift+P 循环顺序）：编辑 | 实时 | 双栏 | 预览。
// 与枚举值顺序**解耦**（枚举顺序受持久化约束不可改）——segmented 下标与
// ViewMode 的换算只经本表正反查，调用点不再 static_cast。
constexpr ViewMode kModeOrder[] = {ViewMode::Edit, ViewMode::Live,
                                   ViewMode::Split, ViewMode::Preview};
constexpr int kModeCount = 4;

// ViewMode → segmented 下标（表外值兜底 0）
int modeOrderIndex(ViewMode m) {
    for (int i = 0; i < kModeCount; ++i) {
        if (kModeOrder[i] == m) return i;
    }
    return 0;
}

// segmented 下标 → ViewMode（越界兜底 Edit）
ViewMode modeFromOrderIndex(int index) {
    return (index >= 0 && index < kModeCount) ? kModeOrder[index] : ViewMode::Edit;
}

// Ctrl+Shift+P 循环：编辑 → 实时 → 双栏 → 预览 → 编辑
ViewMode nextMode(ViewMode m) {
    return kModeOrder[(modeOrderIndex(m) + 1) % kModeCount];
}

// 编辑器字号（spec §8.5「编辑器字体大小」，spec 缩水 B）：运行时值改从
// core::theme::editorFontSize() 全局通道读（SettingsConfig 在 module_settings，
// 模块隔离不经本模块 include；通道写入方 = settings onStart/切档，默认与本
// 常量一致 15 = fontSizeBody token 基准）。行高 = 字号 × 1.2 运行时算
constexpr float kDefaultEditorFontSize = 15.0f;
constexpr float kEditTextColumnWidth = 720.0f;  // 仅编辑/实时态的居中文本列宽（Typora 写作宽度）
constexpr float kPreviewColumnWidth = 760.0f;   // 仅预览态的居中阅读列宽

// ---- Phase 3 M2：实时态单输入（活动块）----
// 单输入方案（spec §4.1 D1）：全局只有一个 input，切块时换文本 + 搬位置，焦点
// **永不迁移**（每块一个 input 会在拆块/并块的瞬间丢焦点 = 丢输入）。id 固定且
// 不得落在任何 releaseStateScope 释放的前缀下（M0 约束 C2：命中即清焦点 +
// 销毁 InputState），故本模块实时态不释放 "md.live" 前缀。
constexpr const char* kLiveInputId = "md.live.input";
constexpr const char* kLiveInputHitId = "md.live.input.hit";  // 聚焦只发 .hit（M0 C1）
constexpr float kLiveInputInset = 10.0f;   // 与编辑态 input 同款内边距
// 高度公式的测量输入必须与 input 内部同源：同字体族（InputBuilder 默认
// "Microsoft YaHei"，见 components/input.h）、同字号（全局编辑器字号）、
// 同文本宽（声明宽 − inset×2，本模块不给活动 input 开滚动条 → 无槽位扣减）。
constexpr const char* kLiveInputFontFamily = "Microsoft YaHei";

// "已保存 HH:MM"
std::string formatSaveTime(std::time_t t) {
    if (t <= 0) return {};
    std::tm tm{};
    ::core::time::localTime(t, &tm);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

// 粗略文字宽度估算（标签 chip 布局用）：ASCII 字节 ≈ 0.55×字号，
// 非 ASCII 字节（UTF-8 多字节）≈ 字号/3 —— CJK 3 字节 ≈ 1 个字宽
float estimateTextWidth(const std::string& s, float fontSize) {
    float w = 0.0f;
    for (unsigned char ch : s) {
        w += (ch < 0x80) ? fontSize * 0.55f : fontSize / 3.0f;
    }
    return w;
}

// 指针事件坐标是窗口坐标（runtime_input 派发时只做 /dpiScale，原点 = 窗口
// 客户区左上），而本模块的浮层（右键菜单）构建在页面子树内——app 布局
// row(侧边栏, app.page) 把页面区放在侧边栏右侧，其坐标原点是页面区左上，
// 故窗口坐标换算成页面坐标需减去 Router 上的页面区原点 x（app_sidebar
// compose 每帧写入，折叠/展开随侧边栏宽变化）。y 方向页面区纵跨整窗
// （原点恒 0）不换算。无 Router（无头测试等）按原点 0 处理 = 旧行为。
//
// 换成本体统一在 core::windowToCompose（审计 M4）；本模块走的是
// onContextMenu / onMove 通道，runtime 派发前**已**做 /dpiScale（见 EUI-NEO
// runtime_input updateInteraction 的 logicalEvent），故指针空间与 compose
// 空间同尺度 → dpiScale 传 1.0。
float windowToPageX(float windowX) {
    const ::core::Router* router = ::core::appRouter();
    return ::core::windowToCompose(windowX, router ? router->pageOriginX() : 0.0f,
                                   1.0f);
}

} // namespace

class MarkdownModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::Markdown; }
    std::string displayName() const override { return "Markdown 笔记本"; }
    std::string iconGlyph() const override { return eui::utf8(0xF044); }  // fa-pen-to-square

    // ==================== 生命周期 ====================

    void onStart() override {
        auto* db = ::core::appDatabase();
        if (db == nullptr) {
            LOG_WARN("Markdown 模块启动：appDatabase() 为空，跳过真实索引");
            return;
        }
        service_ = std::make_unique<MarkdownService>(*db, ::core::paths::markdownDir());
        auto r = service_->rescan();
        if (r.isFail()) {
            LOG_ERROR("Markdown rescan 失败: " + r.errorMessage());
            showToast("笔记目录加载失败", "重扫失败：" + r.errorMessage(), true);
        } else {
            LOG_INFO("已加载笔记目录：" + service_->notesDir().filename().string());
        }
        refreshList();
        loadViewState();  // 审计 [23]：恢复上次的选中笔记 / 视图模式 / 收起目录
        LOG_INFO("Markdown 模块启动，共 " + std::to_string(notes_.size()) + " 篇笔记");
    }

    void onStop() override {
        // 退出前落盘：失败不能静默丢编辑（W7 P1-2）。EUI-NEO 无退出拦截
        // API，无法阻断退出弹 dialog，做同步有限重试（瞬时文件锁可自行恢复）
        // + 响亮日志兜底；编辑器内容不再有其他持久化通道。
        for (int attempt = 0; attempt < 3 && editorDirty_; ++attempt) {
            saveEditor();
            if (!editorDirty_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (editorDirty_ && !currentPath_.empty()) {
            LOG_ERROR("退出时保存笔记失败，未落盘的编辑已丢失: " + currentPath_ +
                      "（EUI-NEO 暂无退出拦截 API，无法阻断退出）");
        }
        LOG_INFO("Markdown 模块停止");
    }

    // ==================== 渲染 ====================

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        // 自动保存：停止输入 kAutoSaveDelayMs 后把编辑器内容真正写回磁盘
        flushEditorIfIdle();

        // W8：文件变化自动感知——5 秒周期轻量 mtime 轮询（新文件/删除/外部修改）
        pollDiskChangesIfDue();

        // 大纲数据：文本变化后重扫（行扫描器是纯函数，小文档开销可忽略；
        // 大文档下 onChange 每键一次 O(n) 扫描，>5000 行如成瓶颈再增量分片）
        if (outlineDirty_) {
            outline_ = scanHeadings(editorText_);
            outlineDirty_ = false;
        }

        // 消费跨模块跳转负载（Router payload，一次性语义）
        consumeNavPayload();

        const auto& t = core::theme::current();
        const auto componentTheme =
            core::theme::componentTokens(t);

        // Phase C 语法高亮：色表随主题（§6.2）。主题名变化 = 换色表 =
        // bump InputState.styleRevision 触发全部可视行重高亮（Phase B 语义：
        // 使用方自增；首帧也走一次，保证初始色表就位）
        if (const std::string themeName = core::theme::currentThemeName();
            themeName != highlightThemeName_) {
            highlightThemeName_ = themeName;
            highlighter_.setColors(mdHighlightColors(t));
            ++ui.state<components::input_detail::InputModel::InputState>(
                      kEditorInputId)
                 .styleRevision;
        }

        const float pad = t.sizes.spacingXXLarge; // 40
        const float contentW = screen.width - pad * 2.0f;
        const float contentH = screen.height - pad * 2.0f;
        const float sideW = 240.0f;
        const float gap = 16.0f;
        // 双栏态中栏宽：md.body row 实际只有左栏一个 sideW（树）+ 编辑 + 预览
        // 两列 + 两个 gap（W9 修复：旧公式多减一个 sideW，双栏各窄 128px、
        // 右侧 240px 死白）
        const float midW = (contentW - sideW - gap * 2.0f) / 2.0f;
        const float bodyH = contentH - 64.0f - t.sizes.spacingLarge;

        // 页面底色 = 主窗口 clearColor（随主题/窗口效果档运行时跟随，
        // 磨砂 Phase B/D）——不自铺不透明底，半透/磨砂档主体才生效

        ui.column("md.root")
            .size(screen.width, screen.height)
            .padding(pad)
            .gap(t.sizes.spacingLarge)
            .content([&] {
                drawHeader(ui, contentW, contentH, bodyH, t);
                drawBody(ui, contentW, bodyH, sideW, midW, gap, t, componentTheme);
            })
            .build();

        // 大纲浮动面板（zIndex 浮在编辑/预览区上方，不参与流式布局）
        drawOutlinePanel(ui, screen, t, componentTheme);

        // 弹窗要画在最上层（dialog 自带 zIndex）
        drawNewNoteDialog(ui, screen, t, componentTheme);
        drawNewFolderDialog(ui, screen, t, componentTheme);
        // 生命周期（审计 [4][20]）：树行右键菜单 + 删除确认 + 重命名对话框。
        // contextMenu 自带全屏 dismiss 层与 zIndex 1050（盖过 dialog 默认 1000），
        // 菜单项点击先 dismiss 再开确认弹窗，二者不共存，无需提 dialog zIndex
        drawNoteContextMenu(ui, screen, t, componentTheme);
        // 审计 [12]：编辑器右键「插入表格」级联菜单
        drawEditorContextMenu(ui, screen, t, componentTheme);
        drawDeleteDialog(ui, screen, t, componentTheme);
        drawRenameDialog(ui, screen, t, componentTheme);

        // 保存失败等操作的 toast 反馈（画在弹窗之上；外部修改冲突也走 toast）
        drawToast(ui, screen, componentTheme);
    }

private:
    // 审计 M5：Esc 关最上层弹窗/浮层（顺序 = z 序倒序：重命名 > 删除确认 >
    // 编辑器右键菜单 1050 > 笔记右键菜单 1050 > 新建文件夹 > 新建笔记）。
    // 返回 true = 本键已消费；无弹窗返回 false
    bool closeTopmostPopup() {
        if (renameOpen_) { renameOpen_ = false; return true; }
        if (deleteConfirmOpen_) { deleteConfirmOpen_ = false; return true; }
        if (editorCtxOpen_) { editorCtxOpen_ = false; return true; }
        if (ctxMenuOpen_) { ctxMenuOpen_ = false; return true; }
        if (newFolderOpen_) {
            newFolderOpen_ = false;
            newFolderParentOpen_ = false;  // 关弹窗顺手收下拉（同 onOpenChange）
            return true;
        }
        if (newNoteOpen_) { newNoteOpen_ = false; return true; }
        return false;
    }

    // ---------- 顶部 Header ----------
    void drawHeader(eui::Ui& ui,
                    float contentW,
                    float contentH,
                    float bodyH,
                    const core::theme::Tokens& t) {
        (void)contentH;
        (void)bodyH;
        const auto componentTheme =
            core::theme::componentTokens(t);
        // 审计 [30]：subtitle 改静态操作说明（对齐 Todo 同类修复）——
        // 笔记数移入编辑器底部状态栏，操作结果一律走 toast，不再兼任
        // 「操作日志」把快捷键提示永久顶掉
        const std::string subtitle =
            service_
                ? "Ctrl+Shift+P 切换视图 · Ctrl+B/I/K 格式 · Tab 缩进/表格跳格 · "
                  "Enter 续列表 · 右键条目/编辑器更多操作"
                : "尚未初始化笔记目录";

        ui.row("md.header")
            .size(contentW, 64.0f)
            .alignItems(eui::Align::CENTER)
            .content([&] {
                const float segW = 264.0f;
                ui.column("md.title")
                    .size(contentW - 320.0f - segW, 64.0f)
                    .gap(2.0f)
                    .content([&] {
                        ui.text("md.h1")
                            .size(contentW - 320.0f - segW, 32.0f)
                            .text("Markdown 笔记本")
                            .fontSize(t.sizes.fontSizeTitle)
                            .lineHeight(32.0f)
                            .fontWeight(700)
                            .color(t.colors.textPrimary)
                            .build();
                        // subtitle 包 clip 栈：长快捷键提示超宽时裁掉，不溢出压到右侧 segmented
                        const float subW = contentW - 320.0f - segW;
                        ui.stack("md.subtitle.clip")
                            .size(subW, 18.0f)
                            .clip()
                            .content([&] {
                                ui.text("md.subtitle")
                                    .size(subW, 18.0f)
                                    .text(subtitle)
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .lineHeight(18.0f)
                                    .color(t.colors.textSecondary)
                                    .build();
                            })
                            .build();
                    })
                    .build();

                // 四态切换：编辑 / 实时 / 双栏 / 预览（Ctrl+Shift+P 循环）。
                // 旧文案带 ✎/⇔/👁 符号——👁 是彩色 emoji（系统 emoji 字体渲染，
                // 风格不统一），segmented 只支持纯文本，故整体去符号。
                // 视觉顺序经 kModeOrder 映射（不是枚举值顺序，也不是 static_cast）
                components::segmented(ui, "md.mode")
                    .size(segW, 36.0f)
                    .items({"编辑", "实时", "双栏", "预览"})
                    .selected(modeOrderIndex(mode_))
                    .fontSize(t.sizes.fontSizeSmall)
                    .theme(componentTheme)
                    .onChange([this](int index) {
                        setMode(modeFromOrderIndex(index));
                    })
                    .build();

                ui.row("md.actions")
                    .size(320.0f, 40.0f)
                    .justifyContent(eui::Align::END)
                    .alignItems(eui::Align::CENTER)
                    .gap(t.sizes.spacingSmall)
                    .content([&] {
                        // 大纲浮动面板开关（W8：大纲从左栏页签改为顶栏开关 +
                        // 浮动面板，不挤压编辑区）
                        components::toggleSwitch(ui, "md.outline.toggle")
                            .size(112.0f, 36.0f)
                            .checked(outlineOpen_)
                            .text("大纲")
                            .fontSize(t.sizes.fontSizeSmall)
                            .theme(componentTheme)
                            .onChange([this](bool v) { outlineOpen_ = v; })
                            .build();

                        components::button(ui, "md.btn.new")
                            .size(124.0f, 40.0f)
                            .text("+ 新建笔记")
                            .fontSize(t.sizes.fontSizeBody)
                            .theme(componentTheme, true)
                            .textColor(t.colors.onPrimary)
                            .radius(t.sizes.radius)
                            .onClick([this] { openNewNoteDialog(); })
                            .build();
                    })
                    .build();
            })
            .build();
    }

    // ---------- 主区（按视图模式裁剪：编辑/实时/双栏/预览） ----------
    void drawBody(eui::Ui& ui,
                  float contentW,
                  float bodyH,
                  float sideW,
                  float midW,
                  float gap,
                  const core::theme::Tokens& t,
                  const components::theme::ThemeColorTokens& componentTheme) {
        const float fullW = contentW - sideW - gap;  // 单栏态的全宽
        ui.row("md.body")
            .size(contentW, bodyH)
            .gap(gap)
            .content([&] {
                drawTreeColumn(ui, bodyH, sideW, t, componentTheme);
                if (mode_ == ViewMode::Live) {
                    // 第四态（M1 骨架，只读）：单栏块级渲染，替换编辑+预览两列
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

    // ---- 左栏：搜索 + 文件列表 ----
    void drawTreeColumn(eui::Ui& ui,
                        float bodyH,
                        float sideW,
                        const core::theme::Tokens& t,
                        const components::theme::ThemeColorTokens& componentTheme) {
        // W9 修复 + tech-debt C（布局语义注释）：单 stack 列 = bg rect +
        // 绝对定位内容。旧结构是 column（只铺底色）+ 兄弟 overlay stack，
        // overlay 会在 md.body row 的流式布局里占第二个同样宽的槽位
        // （三列变六槽，整体右移溢出屏）。
        // EUI-NEO stack 语义（core/layout.h layoutStack）：stack 子元素
        // 全部同位叠放、不互相挤占——带 .position() 的按栈内容区坐标绝对
        // 定位；不带 position 的居中摆放 → bg 用"尺寸恒等于 stack 尺寸 +
        // 居中"实现铺满。⚠ 耦合点：改 bg 或 stack 任一侧尺寸会静默错位
        // （居中偏移而非报错）；测试 md_tree_column_layout_semantics 锁
        // "bg 与 stack 精确重合 + 每列在 row 中只占一个槽位"。
        ui.stack("md.tree").size(sideW, bodyH).content([&] {
            ui.rect("md.tree.bg")
                .size(sideW, bodyH)
                // 左栏树面板是页面结构（非浮动卡片）→ background（米色/暖墨），
                // 不用卡片白（用户反馈浅色下一堆组件白得刺眼的同一根因）
                .color(t.colors.background)
                .radius(t.sizes.radiusLarge)
                .border(1.0f, t.colors.borderDefault)
                .build();
            // 搜索框：真实调用 FTS5。🔍 emoji 换 FA（fa-magnifying-glass，
            // 码点已对 EUI-NEO FA7 cmap 离线核对）——input 无前缀图标槽，
            // 用浮层 icon 元素 + 加大 inset 给文字让位
            components::input(ui, "md.search")
                .position(12.0f, 12.0f)
                .size(sideW - 24.0f, 36.0f)
                .value(searchQuery_)
                .placeholder("搜索笔记…")
                .fontSize(t.sizes.fontSizeSmall)
                .inset(28.0f)
                .theme(componentTheme)
                .onChange([this](const std::string& v) {
                    searchQuery_ = v;
                    refreshList();
                })
                .build();
            ui.text("md.search.icon")
                .position(24.0f, 22.0f)
                .size(16.0f, 16.0f)
                .icon(0xF002)  // fa-magnifying-glass
                .fontSize(12.0f)
                .lineHeight(16.0f)
                .color(t.colors.textTertiary)
                .build();

            // 标签筛选区（spec §8.1：全部标签 chip 带笔记数，点击过滤树 /
            // 再点取消；选中 chip 高亮 primary）。有标签才显示。
            const float tagY = 56.0f;
            float tagAreaH = 0.0f;
            if (!allTags_.empty()) {
                ui.text("md.tags.title")
                    .position(16.0f, tagY)
                    .size(sideW - 32.0f, 14.0f)
                    .text("标签")
                    .fontSize(10.0f)
                    .lineHeight(14.0f)
                    .fontWeight(700)
                    .color(t.colors.textTertiary)
                    .build();
                const float fs = 11.0f;
                const float rowH = 24.0f;
                float cx = 12.0f;
                float cy = tagY + 18.0f;
                int row = 0;
                for (size_t i = 0; i < allTags_.size(); ++i) {
                    const auto& tc = allTags_[i];
                    const std::string label =
                        tc.name + "·" + std::to_string(tc.noteCount);
                    const std::string id = "md.tags.filter." + std::to_string(i);
                    const float chipW = 16.0f + estimateTextWidth(label, fs);
                    if (cx + chipW > sideW - 12.0f) {
                        // 换行；最多 3 行，超出丢弃
                        // （ponytail: 标签很多时改 scrollView 虚拟列表）
                        cx = 12.0f;
                        cy += rowH + 4.0f;
                        if (++row >= 3) break;
                    }
                    const bool active = tagFilter_ == tc.name;
                    ui.stack(id)
                        .position(cx, cy)
                        .size(chipW, rowH)
                        .content([&] {
                            ui.rect(id + ".bg")
                                .size(chipW, rowH)
                                .color(active ? t.colors.primary : t.colors.hover)
                                .hoverColor(t.colors.primarySoftBg)
                                .radius(t.sizes.radiusPill)
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
                                .color(active ? t.colors.onPrimary
                                              : t.colors.textSecondary)
                                .build();
                        })
                        .build();
                    cx += chipW + 4.0f;
                }
                tagAreaH = 18.0f + static_cast<float>(row + 1) * (rowH + 4.0f);
            }

            // 搜索时树展平为命中文件列表；空搜索 = 二级目录树（W8）
            const float listY = 60.0f + tagAreaH;
            const float listH = std::max(0.0f, bodyH - listY - 60.0f);

            // 错误态与空态区分（W7 P1-1，仿 Home 三态）：
            // DB/列表失败 → 错误文案，不再伪装成「还没有笔记」。
            // W8：重扫按钮已移除（5 秒自动轮询），错误态等下一轮自动恢复
            if (!listError_.empty()) {
                ui.text("md.tree.error")
                    .position(16.0f, listY + 8.0f)
                    .size(sideW - 32.0f, 60.0f)
                    .text("列表加载失败：\n" + listError_ +
                          "\n每 5 秒自动重试")
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(18.0f)
                    .wrap()
                    .color(t.colors.error)
                    .build();
            } else if (notes_.empty()) {
                if (searchQuery_.empty() && tagFilter_.empty()) {
                    // 空目录：根组标题 + 空态提示同显（W9——组标题行常显，
                    // 提示挪到标题行下方，不再独占替换掉树结构）
                    drawTreeList(ui, listY, listH, sideW, t, componentTheme);
                    // W10：已有文件夹时只留树——提示行（listY+40）会压在
                    // 第一个文件夹行上，而「有空文件夹」本身就是内容不是空态
                    if (allFolders_.empty()) {
                        ui.text("md.tree.empty")
                            .position(36.0f, listY + 40.0f)
                            .size(sideW - 48.0f, 40.0f)
                            .text("还没有笔记\n点下面「+ 笔记」开始")
                            .fontSize(t.sizes.fontSizeSmall)
                            .lineHeight(18.0f)
                            .wrap()
                            .color(t.colors.textTertiary)
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
                        .color(t.colors.textTertiary)
                        .build();
                }
            } else if (!searchQuery_.empty()) {
                drawNoteList(ui, listY, listH, sideW, t, componentTheme);
            } else {
                drawTreeList(ui, listY, listH, sideW, t, componentTheme);
            }

            // 底部动作条：+ 笔记 / + 文件夹（W9：新建文件夹入口）
            const float newBtnY = bodyH - 52.0f;
            const float btnW = (sideW - 24.0f - 8.0f) / 2.0f;
            components::button(ui, "md.tree.new")
                .position(12.0f, newBtnY)
                .size(btnW, 36.0f)
                .text("+ 笔记")
                .fontSize(t.sizes.fontSizeSmall)
                .theme(componentTheme, false)
                .textColor(t.colors.textPrimary)
                .radius(t.sizes.radius)
                .border(1.0f, t.colors.borderDefault)
                .onClick([this] { openNewNoteDialog(); })
                .build();
            components::button(ui, "md.tree.newfolder")
                .position(12.0f + btnW + 8.0f, newBtnY)
                .size(btnW, 36.0f)
                .text("+ 文件夹")
                .fontSize(t.sizes.fontSizeSmall)
                .theme(componentTheme, false)
                .textColor(t.colors.textPrimary)
                .radius(t.sizes.radius)
                .border(1.0f, t.colors.borderDefault)
                .onClick([this] {
                    newFolderOpen_ = true;
                    newFolderName_.clear();
                    newFolderError_.clear();
                    newFolderParentIdx_ = 0;      // W10：父目录默认根目录
                    newFolderParentOpen_ = false;
                })
                .build();
        }).build();
    }

    void drawNoteList(eui::Ui& ui,
                      float listY,
                      float listH,
                      float sideW,
                      const core::theme::Tokens& t,
                      const components::theme::ThemeColorTokens& componentTheme) {
        const float rowW = sideW - 16.0f;
        // contentKey 反模式修复（W7）：旧 key 用 notes_.size()，同数量替换
        // （如搜索结果变化）不触发滚动内容重建、左栏显示旧列表。改为
        // 逐条路径组合 hash，内容变即 key 变。
        size_t listKey = 0;
        for (const auto& n : notes_) {
            listKey = listKey * 1315423911u +
                      std::hash<std::string>{}(noteDisplayPath(n));
        }
        components::scrollView(ui, "md.tree.scroll")
            .position(8.0f, listY)
            .size(sideW - 16.0f, listH)
            .gap(0.0f)
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
                                     });
                }
            })
            .build();
    }

    // ---- 左栏：两级目录树（W8，md_tree 纯函数分组；W10 升两级+空文件夹）----
    void drawTreeList(eui::Ui& ui,
                      float listY,
                      float listH,
                      float sideW,
                      const core::theme::Tokens& t,
                      const components::theme::ThemeColorTokens& componentTheme) {
        const std::vector<MdTreeRow> rows = buildTreeRows(notes_, allFolders_,
                                                          collapsedDirs_);
        const float rowW = sideW - 16.0f;
        // contentKey：行内容（类型+名字+路径）组合 hash，内容变即重建
        size_t treeKey = 0;
        for (const auto& r : rows) {
            treeKey = treeKey * 1315423911u +
                      std::hash<std::string>{}(r.name + "\x1f" + r.relPath) +
                      (r.isFolder ? 1u : 0u) + static_cast<size_t>(r.depth);
        }
        components::scrollView(ui, "md.tree.scroll")
            .position(8.0f, listY)
            .size(sideW - 16.0f, listH)
            .gap(0.0f)
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
                        // 文件夹行：▸/▾ 展开指示 + fa-folder，点击收起/展开
                        // （同样包 stack，position 只在 stack 内生效；📁 emoji 已换 FA）
                        // W10：缩进按 depth（顶层夹 0 / 子夹 1），每级 16px
                        const float indent = static_cast<float>(r.depth) * 16.0f;
                        const bool expanded = collapsedDirs_.count(r.relPath) == 0;
                        inner.stack(id)
                            .size(w, 32.0f)
                            .content([&] {
                                inner.rect(id + ".row")
                                    .size(w, 32.0f)
                                    // 同文件行：行底 = 面板底（background）
                                    .color(t.colors.background)
                                    .hoverColor(t.colors.hover)
                                    .onClick([this, dir = r.relPath] {
                                        if (collapsedDirs_.count(dir) != 0) {
                                            collapsedDirs_.erase(dir);
                                        } else {
                                            collapsedDirs_.insert(dir);
                                        }
                                        saveViewState();  // 审计 [23]：目录折叠跨会话记忆
                                    })
                                    .build();
                                inner.text(id + ".arrow")
                                    .position(6.0f + indent, 0.0f)
                                    .size(16.0f, 32.0f)
                                    .text(expanded ? "▾" : "▸")
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .lineHeight(32.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                                inner.text(id + ".ficon")
                                    .position(26.0f + indent, 0.0f)
                                    .size(16.0f, 32.0f)
                                    .icon(0xF07B)  // fa-folder
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .lineHeight(32.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                                inner.text(id + ".name")
                                    .position(46.0f + indent, 0.0f)
                                    .size(std::max(0.0f, w - 58.0f - indent), 32.0f)
                                    .text(r.name)
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .lineHeight(32.0f)
                                    .color(t.colors.textPrimary)
                                    .fontWeight(700)
                                    .build();
                            })
                            .build();
                    } else {
                        // 文件行：缩进 = 32 + (depth-1)*16（depth 1 = 顶层夹/根目录
                        // 下的文件，2 = 子夹下的文件；文件名 x = 36+indent，与上一级
                        // 文件夹名 x 差 22px，层级可辨）
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

    // ---- 大纲浮动面板（W8：顶栏开关触发，positioned + zIndex 浮在编辑/预览区
    //      右侧，不挤压布局。沿用 Phase 1 的 md_line_scanner + 点击跳转逻辑）----
    void drawOutlinePanel(eui::Ui& ui,
                          const eui::Screen& screen,
                          const core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& componentTheme) {
        if (!outlineOpen_) return;
        const float pad = t.sizes.spacingXXLarge; // 与 compose 同基准（40）
        const float panelW = 264.0f;
        const float panelH = screen.height - pad * 2.0f - 64.0f -
                             t.sizes.spacingLarge;
        const float x = screen.width - pad - panelW;
        const float y = pad + 64.0f + t.sizes.spacingLarge;

        ui.stack("md.outline.panel")
            .position(x, y)
            .size(panelW, panelH)
            .zIndex(10)
            .content([&] {
                ui.rect("md.outline.panel.bg")
                    .size(panelW, panelH)
                    // zIndex 浮层（浮在编辑/预览区上方）→ surfaceElevated 浮层语义
                    .color(t.colors.surfaceElevated)
                    .radius(t.sizes.radiusLarge)
                    .border(1.0f, t.colors.borderDefault)
                    .build();
                ui.text("md.outline.panel.title")
                    .position(16.0f, 10.0f)
                    .size(120.0f, 24.0f)
                    .text("大纲")
                    .fontSize(t.sizes.fontSizeBody)
                    .lineHeight(24.0f)
                    .fontWeight(700)
                    .color(t.colors.textPrimary)
                    .build();
                components::button(ui, "md.outline.panel.close")
                    .position(panelW - 44.0f, 8.0f)
                    .size(32.0f, 28.0f)
                    .text("✕")
                    .fontSize(t.sizes.fontSizeSmall)
                    .theme(componentTheme, false)
                    .textColor(t.colors.textSecondary)
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
                        .color(t.colors.textTertiary)
                        .build();
                    return;
                }

                // 当前节：光标行所在的上一个标题（仅预览态无编辑器，用上次跳转位置）
                const int active = activeOutlineIndex(ui);

                // contentKey：标题+级别组合 hash（W7 反模式修复同款）
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
                            // 当前节高亮 primarySoftBg + primary 文本；按级别缩进。
                            // 行包 stack（position 只在 stack 内生效，同左栏树行）
                            inner.stack(id)
                                .size(rowW, 30.0f)
                                .content([&] {
                                    inner.rect(id + ".row")
                                        .size(rowW, 30.0f)
                                        // 行底 = 浮层面板底（surfaceElevated）
                                        .color(current ? t.colors.primarySoftBg
                                                       : t.colors.surfaceElevated)
                                        .hoverColor(t.colors.hover)
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
                                        .color(current ? t.colors.primaryText
                                                       : t.colors.textPrimary)
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

    // ---- W2 MD-02：编辑 / 实时两态共用的列壳（单点维护）----
    // 两态的列壳本就逐行相同：面板底 + 空态提示条 + 标签 chip 行 + 文本列几何 +
    // 底部状态栏。差异只有两处——id 前缀、以及是否把文本列钳到 720px（编辑态
    // 单栏钳 720 居中 = 无干扰写作宽，双栏态与实时态占满列宽）。抽出前靠注释
    //「与编辑态同款」口头同步，任一侧调整都要改两遍且易漏。
    // 几何回传给调用方，正文在 chip 行之下、状态栏之上绘制；元素创建顺序与
    // 抽出前逐字一致（底 → 提示 / 正文 → 状态栏），命中层级不变。
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
                              bool clampColumn,
                              const core::theme::Tokens& t,
                              const components::theme::ThemeColorTokens& componentTheme,
                              Body&& body) {
        ui.stack(columnId).size(midW, bodyH).content([&] {
            ui.rect(columnId + ".bg")
                .size(midW, bodyH)
                // 编辑区面板是页面结构 → background（spec §6.5 层级：结构性
                // 面板不浮起，卡片白只给浮动元素）
                .color(t.colors.background)
                .radius(t.sizes.radiusLarge)
                .border(1.0f, t.colors.borderDefault)
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

            float colX = 16.0f;
            float colW = midW - 32.0f;
            if (clampColumn) {
                // 无干扰写作：文本列 clamp 720px 居中（Typora 写作宽度）
                colW = std::min(colW, kEditTextColumnWidth);
                colX = std::max(0.0f, (midW - colW) * 0.5f);
            }
            // 标签 chip 行（spec §8.1 标签 UI；与文本列同列对齐）
            drawTagBar(ui, colX, colW, t, componentTheme);
            const float colY = 48.0f;  // 16 顶距 + 24 chip 行 + 8 间距
            const float colH = std::max(0.0f, bodyH - colY - 22.0f - 8.0f);

            body(TextColumnGeometry{colX, colY, colW, colH});

            drawStatusBar(ui, 0.0f, bodyH - 22.0f, midW, t);
        }).build();
    }

    // ---- 中栏：编辑器（真实读 / 写文件）----
    // Phase 1：栏内小标题移到状态栏（§5.7 去密度）；仅编辑态文本列居中 720px。
    void drawEditorColumn(eui::Ui& ui,
                          float bodyH,
                          float midW,
                          const core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& componentTheme) {
        (void)componentTheme;
        // W9 修复：同左栏——column+overlay 兄弟双占位改单 stack
        // （布局语义与耦合点见 drawTreeColumn 注释，测试统一锁在
        // md_tree_column_layout_semantics）
        drawTextColumnChrome(
            ui, "md.editor", bodyH, midW, mode_ == ViewMode::Edit, t,
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
                        // 每次输入都更新内存缓冲；落盘走防抖（见 flushEditorIfIdle）
                        onEditorTextChanged(v);
                    })
                    // Phase 2 键盘流（spec §5.2 通道 A）：聚焦时先于内置处理收到
                    // 按键，返回 true = 拦截。ui 引用即 Runtime 的常驻 Ui（状态
                    // store 跨帧持久），lambda 捕获安全
                    .onKeyEvent([this, &ui](const eui::KeyEvent& event) {
                        return handleEditorKey(ui, event);
                    })
                    // Phase C 行级语法高亮（styled-runs）：回调只给行文本与行首
                    // 偏移，围栏状态需要全文——从 InputState 取当前 display 文本
                    // （IME 组合期间是 preedit 快照，与框架坐标系同源，字节偏移
                    // 不会错位）。返回全文档坐标 runs，框架裁剪平移归一化。
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
                    .build();

                // 审计 [12]：编辑器右键 → 插入表格菜单。EUI-NEO input 组件不透出
                // 右键入口，runtime 的 contextMenu 只派发给最上层交互元素（即 .hit，
                // 见 runtime_input updateInteraction），故 build 后直接把回调挂到该
                // 元素上（ui.begin() 每帧重建元素树 → 每帧 compose 内重新挂）。
                if (auto* hit = ui.find(kEditorInputHitId)) {
                    hit->onContextMenu = [this](const eui::PointerEvent& ev,
                                                const eui::Rect&) {
                        // 同树行菜单：窗口坐标 → 页面坐标（减去页面区原点 x）
                        editorCtxX_ = windowToPageX(static_cast<float>(ev.x));
                        editorCtxY_ = static_cast<float>(ev.y);
                        editorCtxOpen_ = true;
                    };
                }
            });
    }

    // ---- Phase 2 键盘流：onKeyEvent 接线（纯函数在 md_keyboard）----
    // 键位：Tab/Shift+Tab 缩进、Enter 列表续行、Ctrl+B/I 包裹、Ctrl+K 链接。
    // 文本/光标/选区变换全部走 md_keyboard 纯函数，这里只做：
    // IME 防御 → 纯函数 → 写回 InputState（undo 快照 + textRevision +
    // onChange 副作用）。无变化的拦截（如 Shift+Tab 无前导空格）只消费不动状态。
    bool handleEditorKey(eui::Ui& ui, const eui::KeyEvent& event) {
        using InputState = components::input_detail::InputModel::InputState;
        if (!event.isDown()) return false;
        auto& st = ui.state<InputState>(kEditorInputId);
        // IME 预编辑中不拦截（避免打断组字；Ctrl 类修饰键 IME 也可能用）
        if (!st.compositionText.empty()) return false;

        EditBuffer buf{st.text, st.cursor, st.selectionStart, st.selectionEnd};
        const bool shortcut = event.modifiers.shortcut();
        const bool plain = !shortcut && !event.modifiers.shift &&
                           !event.modifiers.alt && !event.modifiers.super;
        bool handled = false;
        if (event.key == eui::InputKey::Tab && !shortcut && !event.modifiers.alt) {
            // 审计 [12]：光标在表格行时优先单元格跳格，否则走缩进语义
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

    // 键盘流与右键「插入表格」共用的写回：undo 快照 + textRevision + onChange
    // 副作用。无变化只消费不动状态——不制造空 undo / 脏标记。
    void commitEditBuffer(eui::Ui& ui, EditBuffer& buf) {
        using InputState = components::input_detail::InputModel::InputState;
        auto& st = ui.state<InputState>(kEditorInputId);
        if (buf.text == st.text) {
            // 纯光标/选区移动（表格跳格落点、无变化消费）：只更新位置，不进
            // undo 栈、不 bump revision、不打脏——否则 Tab 一次「未保存」上头
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

    // onChange 与键盘流共用的副作用（内存缓冲 + 脏标记，落盘走防抖）
    void onEditorTextChanged(const std::string& v) {
        setEditorText(v);
        editorDirty_ = true;
        outlineDirty_ = true;
        lastEditAt_ = std::chrono::steady_clock::now();
    }

    // 审计 perf [14][15][39]：editorText_ 唯一写点。内容变化时推进修订号 +
    // 刷新预览 hash 缓存，让同步滚动几何表（MdSyncMap，P3-④）、预览
    // contentKey、状态栏
    // （行:列·字数）三个下游缓存都以 (rev) 为键跨帧复用，不再每个重组合
    // 帧付 O(doc) 的全文 hash / 前缀 substr / 逐字节扫描。内容不变则不动
    // rev/hash —— 与旧「每帧对当前内容重算」等价（同串同 hash 同结果）。
    void setEditorText(const std::string& v) {
        if (editorText_ == v) return;
        editorText_ = v;
        ++editorTextRev_;
        previewTextHash_ = std::hash<std::string>{}(editorText_);
        // Phase 3 块模型：唯一文本写点顺带 update（同文零重算）。编辑只脏
        // 受影响块——预览逐块渲染时未变块按内容命中上游 md4c 解析缓存。
        const MdBlockUpdate upd = blockModel_.update(editorText_);
        if (upd.changed) {
            LOG_DEBUG("Markdown 块模型: 共 " +
                      std::to_string(blockModel_.blocks().size()) + " 块, 脏区间 [" +
                      std::to_string(upd.dirtyFirst) + "," +
                      std::to_string(upd.dirtyLast) + "], 复用 " +
                      std::to_string(upd.reused) + " 块");
        }
    }

    // ---- 编辑器顶栏：当前笔记的标签 chip 行（显示 + 添加输入 + × 删除）----
    // ponytail: 标签多到溢出文本列宽时直接截断，不换行/不滚动；需要大量
    // 标签时再升级为 wrap 布局
    void drawTagBar(eui::Ui& ui,
                    float x,
                    float w,
                    const core::theme::Tokens& t,
                    const components::theme::ThemeColorTokens& componentTheme) {
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
                        .color(t.colors.primarySoftBg)
                        .radius(t.sizes.radiusPill)
                        .build();
                    ui.text(id + ".name")
                        .position(10.0f, 0.0f)
                        .size(std::max(0.0f, chipW - 30.0f), h)
                        .text(name)
                        .fontSize(fs)
                        .lineHeight(h)
                        .color(t.colors.primaryText)
                        .fontWeight(600)
                        .build();
                    // × 删除（chip 右侧独立热区，底色同 chip 不额外可见）
                    ui.rect(id + ".del")
                        .position(chipW - 24.0f, 0.0f)
                        .size(24.0f, h)
                        .color(t.colors.primarySoftBg)
                        .hoverColor(t.colors.hover)
                        .radius(t.sizes.radiusPill)
                        .onClick([this, name] { removeTag(name); })
                        .build();
                    ui.text(id + ".x")
                        .position(chipW - 24.0f, 0.0f)
                        .size(24.0f, h)
                        .text("×")
                        .fontSize(fs)
                        .lineHeight(h)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .color(t.colors.primaryText)
                        .build();
                })
                .build();
            cx += chipW + 6.0f;
        }
        // 添加输入 + 「＋」按钮（回车或点击都提交）
        const float inW = 92.0f;
        if (cx + inW + 30.0f <= x + w) {
            components::input(ui, "md.tags.add")
                .position(cx, y)
                .size(inW, h)
                .value(tagInput_)
                .placeholder("＋ 标签")
                .fontSize(fs)
                .inset(8.0f)
                .theme(componentTheme)
                .onChange([this](const std::string& v) { tagInput_ = v; })
                .onEnter([this] { commitAddTag(); })
                // 审计 [21]：Esc=丢弃半截标签草稿（不再走 onEnter 误提交）
                .onCancel([this] { tagInput_.clear(); })
                .build();
            components::button(ui, "md.tags.addbtn")
                .position(cx + inW + 4.0f, y)
                .size(26.0f, h)
                .text("＋")
                .fontSize(fs)
                .theme(componentTheme, true)
                .textColor(t.colors.onPrimary)
                .radius(t.sizes.radiusPill)
                .onClick([this] { commitAddTag(); })
                .build();
        }
    }

    // 编辑器底部状态栏：左 = 文件名 + 保存状态（● 未保存 / ✓ 已保存 HH:MM），
    // 右 = 行:列 · N 字（隐含 UX #7：把自动保存从视线外的顶栏 subtitle 升级为
    // 视线内的持续反馈）
    void drawStatusBar(eui::Ui& ui, float x, float y, float w, const core::theme::Tokens& t) {
        ui.rect("md.status.bg")
            .position(x, y)
            .size(w, 22.0f)
            // 工具条底 = 编辑面板底（background），随面板融为一块
            .color(t.colors.background)
            .build();

        std::string left = "未选择笔记";
        eui::Color leftColor = t.colors.textTertiary;
        if (!currentPath_.empty()) {
            const std::string name = fs::path(currentPath_).filename().string();
            if (saveFailed_) {
                left = name + "  ✕ 保存失败，自动重试中";
                leftColor = t.colors.error;
            } else if (editorDirty_) {
                left = name + "  ● 未保存";
                leftColor = t.colors.warning;
            } else {
                left = name + "  ✓ 已保存 " + formatSaveTime(lastSavedAt_);
                leftColor = t.colors.success;
            }
        }
        // 左框右缘 = 右串左缘（x+w-244）留 24px 间距：旧宽 w*0.6 在双栏态
        // （midW≈520）需 w≥670 才不相撞，长文件名/保存状态压到右侧行列字数上
        const float statusLeftW = w - 24.0f - 220.0f - 24.0f;
        ui.text("md.status.save")
            .position(x + 24.0f, y)
            .size(statusLeftW, 22.0f)
            .clip()  // 超长文件名裁掉，不越过右缘压右侧文本
            .text(left)
            .fontSize(11.0f)
            .lineHeight(22.0f)
            .color(leftColor)
            .build();

        // 行:列 · N 字 · N 篇笔记（列/字数按 UTF-8 码点计；笔记数从顶栏
        // subtitle 移来——审计 [30]，subtitle 已改静态操作说明）
        std::string right;
        if (!currentPath_.empty()) {
            // 审计 perf [39]：(行, 列, 字数) 按 (文本修订, 光标) 缓存，文本与
            // 光标都没变的帧直接复用；变化帧才付一次扫描（旧行为每帧都付）
            const auto& st = ui.state<components::input_detail::InputModel::InputState>(kEditorInputId);
            const size_t cursor = static_cast<size_t>(st.cursor);
            if (statusCacheRev_ != editorTextRev_ || statusCacheCursor_ != cursor) {
                const int line = byteOffsetToLine(st.text, cursor);
                const size_t lineStart = lineStartOffset(st.text, line);
                const int col = static_cast<int>(utf8Length(
                    st.text.substr(lineStart, cursor - lineStart))) + 1;
                statusCacheLine_ = line;
                statusCacheCol_ = col;
                statusCacheChars_ = utf8Length(st.text);
                statusCacheRev_ = editorTextRev_;
                statusCacheCursor_ = cursor;
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
            .color(t.colors.textTertiary)
            .build();
    }

    // ---- 右栏：预览（components::markdown 渲染成 eui 节点树）----
    // 双栏态：编辑器单向驱动预览滚动（块模型精确锚，§5.3 / P3-④）；
    // 仅预览态：手动滚动（previewOffset_ 记忆），进入时对齐编辑器位置。
    void drawPreviewColumn(eui::Ui& ui,
                           float bodyH,
                           float midW,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& componentTheme,
                           float gap) {
        (void)gap;
        // W9 修复：同左栏——column+overlay 兄弟双占位改单 stack
        // （布局语义与耦合点见 drawTreeColumn 注释，测试统一锁在
        // md_tree_column_layout_semantics）
        ui.stack("md.preview").size(midW, bodyH).content([&] {
            ui.rect("md.preview.bg")
                .size(midW, bodyH)
                // 预览列同编辑列：结构性面板 → background（阅读底与页面同层）
                .color(t.colors.background)
                .radius(t.sizes.radiusLarge)
                .border(1.0f, t.colors.borderDefault)
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
                    .color(t.colors.textTertiary)
                    .build();
                return;
            }

            const components::MarkdownStyle style(componentTheme);

            // 滚动位置来源：
            //   Split  = 同步滚动目标（编辑器驱动，每帧覆盖用户滚动）
            //   Preview= previewOffset_（手动滚动记忆；刚切入时对齐编辑器位置）
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
                .gap(8.0f)
                .step(56.0f)
                .theme(componentTheme)
                .offset(scrollOffset)
                // 审计 perf [15]：contentKey 用缓存 hash（setEditorText 维护），
                // 与旧 std::hash(editorText_) 同值，但不再每帧全文重算
                .contentKey("md.preview.scroll." + currentPath_ + "." +
                            std::to_string(previewTextHash_));
            if (mode_ == ViewMode::Preview) {
                // 仅预览态允许手动滚动（双向：滚轮写回 previewOffset_）
                scroll.onChange([this](float v) { previewOffset_ = v; });
            }
            scroll.content([&](eui::Ui& inner, float contentWidth, float viewportHeight) {
                    (void)viewportHeight;
                    lastPvContentWidth_ = contentWidth;  // 供同步滚动 estimateHeight 用
                    // 逐块渲染 + 几何：与第四态「实时」共用同一条通路
                    // （见 drawBlockFlow；观感一致是 spec §3.2 的硬要求）。
                    // 元素建在 inner（测量 pass 会给一次性 Ui）；点击定位要写的是
                    // 模块 Ui 的编辑器状态 → 回调里闭包捕获外层 ui。
                    // 第 7 参 = 块流注入点（MD-03）：预览 / 双栏态没有块级特判
                    // → 传空实现（三回调全空 = 全部走默认路径）。
                    drawBlockFlow(inner, contentWidth, t, style, "md.preview",
                                  [this, &ui](size_t k, float y) {
                                      handlePreviewBlockClick(ui, k, y);
                                  },
                                  {});
                })
                .build();
        }).build();
    }

    // ---- 块流注入点（MD-03）----
    // drawBlockFlow 只做「几何表重建 + 逐块包裹 + 渲染通路」这三件与视图态无关的
    // 事；视图态对【单个块】的额外特判（活动块 = 源码态 input 取代渲染内容等）
    // 一律经本注入点传入 —— 新增视图态只传自己的 hooks，不必回头改 drawBlockFlow
    // 的分支 / 加参数（旧形态：加一个视图态就给 drawBlockFlow 加一个参数 + 一串
    // isActive 分支）。
    // 三个回调都可空（空 = 该维度走默认行为），预览态传的就是三空实现：
    //   wrapHeight      空 = 包裹高取几何表 H_k（P3-⑤ 的既有包裹高口径）
    //   drawContent     空 = 该块走上游渲染通路（CodeFence 仍走 DevDesk 自绘）
    //   afterHitOverlay 空 = 覆盖层建完不做额外处理
    // 回调参数里的 k = 块下标、b = 块、wrapId = 该块包裹 stack 的元素 id。
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

    // 实时态自己的块级特判上下文（活动块锚 + input 主题）。drawBlockFlow 不认识
    // 它 —— 特判经上面的 hooks 注入（MD-03）；本结构只在实时态内部传递。
    struct LiveBlockCtx {
        size_t activeBlock = static_cast<size_t>(-1);  // 活动块下标（其余值不命中）
        const components::theme::ThemeColorTokens* theme = nullptr;  // input 主题
    };

    // ---- 逐块渲染（预览 / 第四态「实时」共用同一条通路）----
    // Phase 3 增量块渲染：按块模型逐块 components::markdown（每块 = 一段自足源码，
    // 切分器保证与整篇渲染等价——逐块 md4c 解析结果与整篇逐块一致，由
    // md_block_model_render_equivalence 回归锁定）。收益构成：
    //   - ≤8 块文档：未变块命中上游 cachedMarkdownDocument（LRU 8 槽）→ 编辑只
    //     重析脏块；
    //   - 大文档：解析缓存 8 槽不够会抖动，但昂贵的行内布局（cachedInlineLayout，
    //     256 槽、按块 runs 键控）与整篇模式同等命中 → 成本与整篇模式同量级。
    // 上游能力边界登记 KNOWN_LIMITATIONS #9（markdown 无块级增量 API，增量全靠
    // 内容键控缓存）。块间隙用 column.gap = MarkdownStyle.blockGap，与单实例内部
    // gap 同源，观感一致。排版全在上游组件内完成（DevDesk 零字形级布局）；带重音
    // 符（combining accent，U+0300–036F）字形轻微跨行属上游 TextPrimitive 行框
    // 问题，根因与升级路径见 KNOWN_LIMITATIONS #8，DevDesk 侧无可修点。
    //
    // 预览与实时共用本函数（spec §3.2：两者观感必须一致，不复制第二份）。
    // prefix 决定键控 id：预览 "md.preview" → md.preview.blk.<stableId> /
    // 实时 "md.live" → md.live.blk.<stableId>；onBlockTap 为空 = 块只 hover 不可点
    //（M1 实时态无活动块可定位，点击定位接线留 M3）。
    //
    // ⚠ ui 必是 scrollView content 回调交给我们的那个 Ui：该回调同时用于真实
    // compose 与「测量 pass」（scrollview.h measureContentHeight 用一次性
    // measureUi 调 content_），元素必须建在传入的 Ui 上——建到外层 Ui 会让测量
    // pass 量到空内容（可滚高度恒 0、预览不滚）。点击回调要写的那个 Ui 由调用方
    // 在自己的 onBlockTap 里闭包捕获（测量 pass 不会触发回调）。
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
        // P3-③ 色表注入（每帧）：主题切换时缓存旧色条目自动作废
        previewCodeCache_.setColors(mdHighlightColors(t));
        // P3-⑤ 逐块 hit/hover 几何表：与 ④ 同一张表。Split 态 computeSyncOffset
        // 已重建（(rev,width,gap,块数) 全等 → 本调用零成本）；Preview / Live 态这里
        // 是唯一重建点（表本就常驻成员，无额外状态）。
        rebuildSyncMap(contentWidth, style);
        const auto& blocks = blockModel_.blocks();
        std::vector<int> activeCodeIds;  // 本帧 CodeFence 块，供缓存回收
        // hover 高亮底色（token 随浅/深主题，同树行 hover 语义）
        const eui::Color hoverTint = t.colors.hover;
        ui.column(prefix + ".blocks")
            .width(contentWidth)
            .gap(style.blockGap)
            .content([&] {
                for (size_t k = 0; k < blocks.size(); ++k) {
                    const MdBlock& b = blocks[k];
                    const bool hovered = previewHover_.hovered(b.stableId);
                    const float hk = syncMap_.blockHeight(k);
                    // 包裹高默认取几何表 H_k（P3-⑤ / ④ 同源的既有口径）；注入点可
                    // 覆盖 —— 实时态活动块改取 input 内容贴合高（不由渲染块高 H_k
                    // 决定：源码行数与渲染高度天然不等，用 H_k 会切成输入框内部滚动
                    // = 吃滚轮）。代价：本块之后各块的列内累计偏移与几何表 H_k 不再
                    // 逐块恒等（spec §2.2 V5 已登记：只影响覆盖层/命中矩形落点，
                    // M6 处理）。
                    const float wrapH =
                        hooks.wrapHeight ? hooks.wrapHeight(k, b, hk, contentWidth)
                                         : hk;
                    // ---- P3-⑤ 逐块交互包裹 ----
                    // 上游 components::markdown 无逐块命中/指针事件（纯展示，
                    // link 亦不可点——KNOWN_LIMITATIONS #9 补 ⑤），DevDesk 侧给
                    // 每个块（含上游块，不只 CodeFence）包一层固定高 stack：hover
                    // 底衬垫最下、块内容居中、透明 MouseArea 覆盖层在最上。
                    // 包裹高 = H_k 取自几何表（④ 同源 = 实际布局，列内累计与旧
                    // 逐块直挂完全一致 → 内容总高不变、不回归 ②③④）。滚轮路由走
                    // hitTestScrollable（只匹配 scrollStateId/onScroll 元素），透明
                    // mouseArea 不劫持滚动。
                    const std::string wrapId =
                        prefix + ".blk." + std::to_string(b.stableId);
                    ui.stack(wrapId)
                        .width(contentWidth)
                        .height(wrapH)
                        .content([&] {
                            // hover 底衬（上游块文字底透明 → 直接透出成背景高亮；
                            // CodeFence 自带不透明 bg 盖在其下 → 走混色参数由块自身
                            // 呈现）
                            ui.rect(wrapId + ".hl")
                                .size(contentWidth, wrapH)
                                .radius(style.radius)
                                .color(hovered
                                           ? hoverTint
                                           : eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                                .build();
                            // 注入点：视图态可自绘本块内容（返回 true = 已画，
                            // 跳过下方渲染通路）。预览态三空 → 恒 false，逐块渲染
                            // 与引入注入点之前逐项一致。
                            bool contentDrawn =
                                hooks.drawContent &&
                                hooks.drawContent(ui, k, b, wrapId, contentWidth,
                                                  wrapH);
                            if (!contentDrawn &&
                                b.kind == MdBlockKind::CodeFence) {
                                // CodeFence 块走 DevDesk 侧自绘：md_highlight 色表
                                // 产出着色 runs 喂给带 runs 的 text 节点（上游
                                // markdown 代码块单色且不暴露预解析注入——
                                // KNOWN_LIMITATIONS #9，本块是唯一不能走上游通路的
                                // 类型）。未变块命中 (stableId, hash) 缓存零重算。
                                // 解析不出单围栏（防御）→ 落到下方上游通路兜底。
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
                            // 透明 hit 覆盖层（默认手型光标 = 点击定位可点性提示；
                            // onTap 的 y 为区域局部坐标，经反查表映射回编辑器行）
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
                            // 注入点收尾：覆盖层建完后交给视图态处理（实时态活动块
                            // 在此关命中 —— 覆盖层建在 input 之后，命中测试对兄弟走
                            // 反向遍历取最上层（runtime_input.hitTest），不关则透明
                            // 覆盖层会抢走 input 的点击与焦点「打字点不进去」）。
                            // 预览态三空 → 无动作。
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

    // ---- 第四态：实时（块级实时渲染；M1 只读骨架 + M2 活动块输入）----
    // 单栏 + 720px 写作列（与编辑态同宽），块级渲染走 drawBlockFlow —— 与预览
    // 同一条通路、同一张 MdSyncMap 几何表（spec §3.2：两者观感必须一致）。
    // M2：活动块位置改放单输入 md.live.input（内容贴合高度），其余块仍是只读
    // 渲染（spec §4.4：活动块只出 input、不出渲染内容，避免重复渲染与命中歧义）；
    // 上行链路 onChange → syncDocumentFromLiveInput → setEditorText。
    void drawLiveColumn(eui::Ui& ui,
                        float bodyH,
                        float midW,
                        const core::theme::Tokens& t,
                        const components::theme::ThemeColorTokens& componentTheme) {
        // 列壳（面板底 / 标签 chip 行 / 720px 写作列 / 状态栏）与编辑态共用同一
        // 实现（W2 MD-02）；本态恒钳 720 居中的写作列（spec §3.2）
        drawTextColumnChrome(
            ui, "md.live", bodyH, midW, true, t, componentTheme,
            [&](const TextColumnGeometry& g) {
                const components::MarkdownStyle style(componentTheme);
                // ---- M3 / M0 C2：输入框状态换代（释放点）----
                // enterLiveState（切实时态 / 换笔记 / 启动恢复）置位，本帧先释放
                // 再建 input：新的一代从空状态开始（无上一篇的块文本、无在飞的
                // 组字、framework undo 栈清零）。释放会命中前缀清掉焦点
                //（runtime_lifecycle 帧末消费 releaseStateScopes），故【必须】与
                // liveFocusPending_ 的同帧补发配对 —— 本帧先释放，帧末按 pending
                // 聚焦（M0 V2c 已验证的"先释放、后补发"顺序）。
                // 单输入方案下【切块】不释放：id 一旦落在被释放的前缀下，每次
                // 切块都会清焦点 + 清 InputState（M0 C2 硬约束 / spec D1）。
                if (liveInputStateFresh_) {
                    liveInputStateFresh_ = false;
                    ui.releaseStateScope(kLiveInputId);
                    LOG_DEBUG("[live] release scope=md.live.input");
                }
                // ---- M2：活动块锚（文档字节偏移，spec D3）----
                // 进实时态首次：以编辑器的文档光标为初始锚（此刻编辑器列已不在帧树，
                // 但 InputState 按 id 跨帧存活，见 M0 V1）；之后换块/跳转由 M3 的
                // setLiveCursor 写入（M2 只在切块帧反向写入块局部光标）。
                // 此后【每帧】锚都跟着活动 input 的真实光标推进（MD-01 修复）。
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
                        // 本帧 input 还未与本块同步（它的块内光标属于上一次会话），
                        // 基准只能取块起点；真实落点由换文本帧反向写入块局部光标
                        liveInputStart_ = bs[liveActiveBlock_].start;
                    } else if (liveCaretPending_) {
                        // 本帧锚由 setLiveCursor（点击渲染块 / 大纲跳转）显式定下：
                        // 此刻输入框的光标仍是【上一块】的局部坐标（换文本发生在
                        // 本帧 build 里），拿它反推会得到「新块起点 + 旧块光标」的
                        // 错位落点 —— 锚即权威，本帧跳过重推（下一帧输入框光标已
                        // 按锚写对，重推与锚同值）。
                    } else if (!mdLiveCompositionFrozen(lst.compositionText)) {
                        // 组字在飞（compositionText 非空）→ 走上面的 if 落空分支：
                        // 锚 / 活动块 / 输入框文本一律保持上一帧（不重推、不换文本），
                        // 组字结束后下一帧一次性重算（spec §3.4 / R3）。
                        // MD-01：锚必须跟着真实光标走，不能冻结在进态时的旧值上。
                        // 活动 input 的 InputState 持有块内光标（用户每次按键由组件
                        // 推进），【文档字节 = 输入框文本起点 + 块内光标】即用户此刻
                        // 的真实光标；据此重定位活动块后，分块器因回车空行 / 行首
                        // `# ` `- ` 重切出的【新块】才是活动块。否则锚停在切块前的
                        // 旧值（仍落在前一块区间内）→ 下一帧 input 被整段换成前一块
                        // 文本、光标跳块首，用户刚敲的内容从输入框消失，此后每次按
                        // 键都写进前块。
                        // 基准取上一帧定下的 liveInputStart_，不用本帧重算出的块起点：
                        // 重切后 input 文本可从旧块起点一路盖进新块，拿新块起点解释
                        // 同一块内光标会把光标推到错位置。
                        const size_t local =
                            lst.cursor > 0 ? static_cast<size_t>(lst.cursor) : 0;
                        liveCursorByte_ = liveInputStart_ + local;
                        liveActiveBlock_ =
                            mdLiveActiveBlockIndex(blockModel_, liveCursorByte_);
                        liveInputStart_ = bs[liveActiveBlock_].start;
                    }
                    live.activeBlock = liveActiveBlock_;
                    hasActive = true;
                }
                // ---- MD-03：实时态的块级特判经注入点传入（drawBlockFlow 自身已
                // 不含活动块分支）----
                // 活动块 = 源码态 input，与渲染内容互斥（spec §4.4），故三处决策：
                //   wrapHeight      → 包裹高改 input 内容贴合高（换块后列内累计随之
                //                     变化，V5 已登记）
                //   drawContent     → 本块内容改由 input 呈现（跳过渲染通路）
                //   afterHitOverlay → 关掉本块覆盖层的命中（它建在 input 之后，不关
                //                     会抢走点击与焦点）
                // 非活动块一律 false / 默认值 = 与预览态逐块渲染完全同一条默认路。
                BlockFlowHooks hooks;
                hooks.wrapHeight = [this, &live](size_t k, const MdBlock& b,
                                                 float geometryHeight,
                                                 float contentWidth) {
                    if (k != live.activeBlock) return geometryHeight;
                    return liveFitHeight(b, liveBlockText(b), contentWidth);
                };
                hooks.drawContent = [this, &live](eui::Ui& inner, size_t k,
                                                  const MdBlock& b,
                                                  const std::string& wrapId,
                                                  float contentWidth,
                                                  float wrapHeight) {
                    (void)wrapId;
                    if (k != live.activeBlock) return false;
                    drawLiveInputBlock(inner, b, liveBlockText(b), contentWidth,
                                       wrapHeight, live);
                    return true;  // 已画：跳过下方渲染通路
                };
                hooks.afterHitOverlay = [this, &live](eui::Ui& inner, size_t k,
                                                      const std::string& hitId) {
                    if (k != live.activeBlock) return;
                    if (auto* hitEl = inner.find(hitId)) {
                        hitEl->hitTestMode = core::dsl::HitTestMode::None;
                    }
                };
                // 单一滚动权威 = 本 scrollView（M1 手动滚动；光标跟随留 M6）
                components::scrollView(ui, "md.live.scroll")
                    .position(g.x, g.y)
                    .size(g.w, g.h)
                    .gap(8.0f)
                    .step(56.0f)
                    .theme(componentTheme)
                    .offset(liveOffset_)
                    // contentKey 用内容 hash（setEditorText 维护；照抄预览 perf [15]
                    // 做法，不回退到按 text.size() 键控）、宽度由 MeasureCache 的
                    // entry 自带 —— 同长度换字符时高度缓存必须失效
                    .contentKey("md.live.scroll." + currentPath_ + "." +
                                std::to_string(previewTextHash_))
                    .onChange([this](float v) { liveOffset_ = v; })
                    .content([&](eui::Ui& inner, float contentWidth, float viewportHeight) {
                        (void)viewportHeight;
                        // 活动块 input 由 drawBlockFlow 建在 inner（content 回调交给的
                        // Ui）上——测量 pass 也走本回调，元素必须挂在同一层
                        drawBlockFlow(inner, contentWidth, t, style, "md.live",
                                      [this, &ui](size_t k, float y) {
                                          handleLiveBlockClick(ui, k, y);
                                      },
                                      hooks);
                    })
                    .build();
                // 本帧的「写光标」一次性请求已由 drawLiveInputBlock 消费（切块帧
                // 走换文本路径，同块点击走 liveCaretPending_）—— 帧末统一清位，
                // 避免下一帧再写一次（下一帧锚由真实光标重推，写了也是同值）。
                liveCaretPending_ = false;

                // M0 C1/C5：进实时态首帧同帧补发 requestFocus（目标必须已是当帧树中
                // 的元素，早发/事后补都被丢弃），且只发 <input id>.hit —— 组件聚焦
                // 判定用 ui.isFocused(id + ".hit")，发基 id 会不画 caret、按键落空。
                // 空文档（无块 → 无 input）不聚焦。
                if (liveFocusPending_) {
                    liveFocusPending_ = false;
                    if (hasActive) ui.requestFocus(kLiveInputHitId);
                }
            });
    }

    // P3-③ CodeFence 块预览自绘（上游 markdown 代码块单色 + 无预解析注入，
    // KNOWN_LIMITATIONS #9）。几何参数逐项对齐上游 renderCodeBlock
    // （bg 圆角块 + codePadding + codeSize+6 行高 + estimateMarkdownTextHeight
    // 高度公式 + wrap）——唯一差异是 text 节点带 md_highlight 色表的着色 runs
    //（上游 styled-runs：颜色只改顶点色不动 layout/测量）。runs 未覆盖段
    //（理论不存在，防御性）兜底 style.codeText 基色 = 旧单色渲染，不回归。
    // P3-⑤ hover：本块 bg 不透明且垫在 hover 底衬之上（底衬透不出来）→
    // hovered 时把 codeBackground 混向 hover 色呈现块背景高亮（纯函数
    // mdHoverMix，与上游块同一观感强度）。
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

    // ================= Phase 3 M2：实时态活动块输入 =================

    // 块源码文本（块模型与 editorText_ 同源，块区间连续覆盖文档）
    std::string liveBlockText(const MdBlock& b) const {
        return blockModel_.text().substr(b.start, b.end - b.start);
    }

    // 活动块内容贴合高度（spec D2 / M0 约束 C4）：测量函数注入
    // InputModel::measureLines —— 与 input 内部排版同源（同字体族/字号/文本宽），
    // 故公式值与框架 contentHeight 严格同口径（+1px 余量 ⇒ maxVerticalScroll==0）。
    // 备忘键 (stableId, hash, 文本宽, 字号)：整块 shaping 不逐帧重付（M9 帧率前提）。
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

    // 上行链路（spec §4.2 D4 单向写）：活动块局部文本 → 拼回整篇 → 既有唯一文档
    // 写点 onEditorTextChanged（= setEditorText + 脏标记 + 400ms 防抖时间戳）。
    // 分块器保证块区间连续覆盖 [0,size)：前段 + 局部 + 后段 ≡ 在编辑态对同一处
    // 做的同一编辑（逐字节），故保存语义/冲突检测/索引全部零改动。
    void syncDocumentFromLiveInput(const std::string& blockText) {
        const std::vector<MdBlock>& blocks = blockModel_.blocks();
        if (liveActiveBlock_ >= blocks.size()) return;   // 防御：无块不写
        const size_t start = blocks[liveActiveBlock_].start;
        const size_t end = blocks[liveActiveBlock_].end;
        const std::string& doc = blockModel_.text();
        if (start > end || end > doc.size()) return;
        // 拼装必须在 onEditorTextChanged 之前完成：它进 setEditorText →
        // blockModel_.update，blocks/doc 两个引用随即失效（故不能先拷贝整篇）
        std::string next;
        next.reserve(doc.size() + blockText.size());
        next.append(doc, 0, start);
        next.append(blockText);
        next.append(doc, end, std::string::npos);
        onEditorTextChanged(next);
    }

    // 活动块源码态 input（M2 单输入）：建在块包裹 stack 内（0,0），尺寸 =
    // 列宽 × 贴合高。调用方保证：
    //   · 元素建在 scrollView content 回调交给的 Ui 上（M1 开发期回归教训：
    //     建到外层 Ui 会让测量 pass 量到空内容）
    //   · 同一帧只建一个 input（活动块互斥，spec §4.4）
    void drawLiveInputBlock(eui::Ui& ui,
                            const MdBlock& b,
                            const std::string& blockText,
                            float contentWidth,
                            float fitHeight,
                            const LiveBlockCtx& live) {
        using InputState = components::input_detail::InputModel::InputState;
        InputState& st = ui.state<InputState>(kLiveInputId);
        // 换文本帧（切块 / 换笔记 / M5 撤销跳转）：input 内部走"外部改值"路径，
        // 聚焦时把光标搬到新文本末尾（components/input.h）——必须在 build 之后
        // 反向写入正确落点（文档锚 → 块局部偏移），早写会被 build 内的重载覆盖；
        // 同时显式清掉上一块的滚动残余（M0 约束 C3；贴合高度下残余本就被钳 0，
        // 这里是兜底）。
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
            // M3 失焦提交（语义见 commitLiveInputOnBlur）：焦点离开活动 input
            // （点渲染块 / 左树 / 切态 / 弹窗）时收口。捕获 &ui 与编辑态
            // onKeyEvent 同款 —— Runtime 常驻 Ui，state store 跨帧持久。
            .onFocus([this, &ui](bool focused) {
                if (!focused) commitLiveInputOnBlur(ui);
            })
            .build();
        // 写光标的两条来路（都必须在 build 之后：聚焦时框架走"外部改值"把光标
        // 搬到新文本末尾，早写会被 build 内的重载覆盖）：
        //   swapped           = 换文本帧（切块 / 换笔记 / 撤销跳转）
        //   liveCaretPending_ = 同块定位帧（点本块 / 大纲跳转：文本没变，框架不
        //                       会动光标，只能由 setLiveCursor 请求写）
        if (swapped || liveCaretPending_) {
            st.cursor =
                static_cast<int>(mdLiveBlockLocalOffset(b, liveCursorByte_));
            st.selectionStart = st.cursor;
            st.selectionEnd = st.cursor;
            st.dragAnchor = st.cursor;
            st.hasPreferredCursorX = false;
            st.followCaret = true;     // 落点即目标（贴合高度下框内无滚动）
            st.verticalScroll = 0.0f;  // M0 C3：切块/定位帧清上一块的滚动残余
            st.horizontalScroll = 0.0f;
        }
        // 失焦提交的安全判据（见 commitLiveInputOnBlur）：输入框文本 == 当前块源
        // 即「输入框已与文档对齐到本修订号」——外部重载/撤销把文档换掉、输入框
        // 还持旧块文本的那一帧不满足该条，故失焦不会把旧文本灌回新文档。
        if (st.text == blockText) liveSyncedRev_ = editorTextRev_;
        // spec §5 #4 记录式校验：活动 input 必须内容贴合（maxVerticalScroll == 0
        // ⇒ 不吃外层滚轮、无内部滚动条）。框架不暴露 maxVerticalScroll，用组件
        // "是否注册滚轮处理器"做探针（components/input.h：仅 maxVerticalScroll > 0
        // 才挂 onScroll；M0 V3a ③ 同款判据）——故 N 是 0/1 探针而非像素值
        //（像素超出量由高度公式保证恒 0，打它只是同义反复）。
        const auto* hit = ui.find(kLiveInputHitId);
        const int over = (hit != nullptr && hit->onScroll) ? 1 : 0;
        if (over > 0) {
            LOG_WARN("[live] input over=1 活动块高度未贴合（应 0）: stableId=" +
                     std::to_string(b.stableId) +
                     " 声明高=" + std::to_string(fitHeight));
        } else {
            LOG_DEBUG("[live] input over=0");
        }
    }

    // ---- Phase 3 M3：光标写点 / 块点击定位 / 失焦提交 ----

    // 实时态唯一光标写点（spec §4.2 数据流第二支）：文档字节偏移 = 活动块身份锚
    //（D3）。一次做完三件事：
    //   ① 锚 = docByte（钳进 [0, size]）
    //   ② 活动块 / 输入框文本起点按锚重算（本帧之后的第一次 compose 里，输入框
    //      光标还是上一块的局部坐标 —— 由 liveCaretPending_ 让那一帧跳过重推，
    //      锚即权威；从再下一帧起输入框光标已按锚写对，重推与锚同值）
    //   ③ 记账两个一次性请求 —— liveCaretPending_（文本未变时框架不会动光标，
    //      必须显式写）与 liveFocusPending_（指针按下已把焦点从 input 上摘掉，
    //      不补发就"点完不能直接打字"）；两者都在下一次 compose 内同帧消费
    //      （M0 C1/C5：requestFocus 的目标必须与请求同帧存在）。
    void setLiveCursor(size_t docByte, const char* reason) {
        const size_t size = editorText_.size();
        liveCursorByte_ = docByte < size ? docByte : size;
        liveCursorValid_ = true;
        const std::vector<MdBlock>& bs = blockModel_.blocks();
        if (!bs.empty()) {
            liveActiveBlock_ = mdLiveActiveBlockIndex(blockModel_, liveCursorByte_);
            liveInputStart_ = bs[liveActiveBlock_].start;
        }
        liveCaretPending_ = true;
        liveFocusPending_ = true;
        LOG_DEBUG(std::string("[live] setLiveCursor(") + reason + ") byte=" +
                  std::to_string(liveCursorByte_) + " block=" +
                  std::to_string(liveActiveBlock_));
    }

    // 点击渲染块 → 该块变源码态 + 光标落到点击行（复用 ⑤ 预览点击的同一套反查：
    // 块内局部 y → 编辑器行 → 行首字节；spec §1.1 目标 3 / §3.3）。活动块的覆盖
    // 层已关命中（点击落进 input 自身，指针→caret 由框架做），故本函数只服务非
    // 活动块。组字在飞时【拒绝切块】（R3）：不换块不换文本，并补发聚焦把组字
    // 会话留在原块（焦点已被框架的指针按下摘掉）。
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

    // 失焦提交（spec §7 M3）——焦点离开活动 input 时的两条收口：
    //   ① 清在飞的组字：焦点已走 = IME 会话结束，而框架不会自己清
    //      compositionText（input.h 只在组字事件里改它）。不清的两个后果：幽灵
    //      预编辑继续显示；冻结谓词（组字非空）恒真 → 切块被永久拒绝。
    //   ② 上行未提交的块局部文本：只在「输入框与文档对齐到同一修订号」时做
    //      （liveSyncedRev_ == editorTextRev_，drawLiveInputBlock 每帧核对）——
    //      否则就是在文档已被外部重载/撤销换代、输入框还持旧块文本的那一帧，
    //      把旧文本灌回新文档 = 数据丢失。而键盘流下每键都已上行，值相同 →
    //      setEditorText 同串守卫直接返回，零成本。
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
        // 状态被换代（释放后重建）时 text 为空、修订号也对不上 —— 两道门都挡住
        if (st.text.empty() || liveSyncedRev_ != editorTextRev_) return;
        if (st.text == liveBlockText(bs[liveActiveBlock_])) return;   // 无未提交内容
        const std::string pending = st.text;   // 取副本：上行会换掉 blockModel_ 内部文本
        LOG_DEBUG("[live] 失焦提交未上行文本 len=" + std::to_string(pending.size()));
        syncDocumentFromLiveInput(pending);
    }

    // 双栏同步滚动（§5.3，Phase 3 ④ 块模型精确锚）：编辑器视口顶行 →
    // 块模型定位所在块 → 预览侧块几何（MdSyncMap）。旧版锚点 = 标题 +
    // 节内按行线性插值（原注释点名「块无源码 offset，全块扫描器是
    // Phase 3 工作」——块模型现已就绪，兑现该升级）：
    //   - 锚点从「仅标题」细化到「每个块」：预览块 k 顶 y =
    //     Σ_{i<k}(H_i + blockGap)，H_i = estimateHeight(块 i)，与预览逐块
    //     渲染路径完全同源（上游 estimateBlockHeight 与 renderBlock 共用
    //     cachedInlineLayout / estimateMarkdownTextHeight 测量 ⇒ 块边界
    //     误差 0，不再随节长累积标题锚点间的摊分误差）。
    //   - 块内按编辑器行线性插值：短行（不折行）段落逐行精确，异高
    //     极端情形误差有界（≤ 块高，恒 ≤ 旧版整节摊分）。
    //   - 反向映射（预览 y → 编辑器行，editorLineForPreviewY）是同一函数
    //     的反函数，块边界精确可逆——P3-⑤ 块级交互（点预览块定位编辑器）
    //     的底座；本轮不接线实时 UI（双栏仍是编辑器单向驱动，避免
    //     滚动回环）。
    // 缓存从旧 [14] 的 yA/yB 单锚对（换锚即重算）升级为 MdSyncMap 全表
    // （键 = 修订 + 宽度 + blockGap + 块数；块高度按 stableId 备忘 ⇒ 编辑
    // 只重测脏块）。留 20% 视口余量（旧行为保持）。
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

    // 几何表按需重建（(修订, 宽度, blockGap, 块数) 未变 = 查表零成本；
    // 变了也只对 memo 未命中的块付 estimateHeight）。
    void rebuildSyncMap(float width, const components::MarkdownStyle& style) {
        if (blockModel_.text() != editorText_) {
            blockModel_.update(editorText_);  // 防御：理论不可达（同预览路径）
        }
        syncMap_.rebuild(
            editorText_, blockModel_.blocks(), editorTextRev_, width,
            style.blockGap,
            [this, width, &style](const MdBlock& b) {
                // 与预览逐块渲染同一份块源码 + 同一宽度 + 同一 style ⇒
                // 测量结果与预览实际布局同源（块边界误差 0 的依据）。
                // CodeFence 块的自绘几何（P3-③）与本公式逐项对齐。
                return components::MarkdownBuilder::estimateHeight(
                    editorText_.substr(b.start, b.end - b.start), width, style);
            });
    }

    // P3-⑤ 预览块点击 → 编辑器光标定位 + 聚焦（ux-design §7 Phase 3 ⑤）。
    // 坐标链路全程与 ④ 同源：本块 hit 区域局部 y → 预览 content y →
    // editorLineForPreviewY 反查（分段线性函数，块边界行号精确）→ floor +
    // 块内行区间钳制 → 行首字节（mdResolvePreviewJump 纯函数，单测锁语义）。
    // 行内列不映射：预览折行/宽度（阅读态 clamp 760）与编辑器布局不同源，
    // 行首是唯一无歧义落点。cursor 写法照抄 jumpToHeading（不改文本 = 不动
    // undo 栈；followCaret=true 让编辑器自动滚到光标，双栏态预览随之回稳）。
    // 仅预览态无编辑器可定位（块本就在预览里可见）→ 忽略点击。
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

    // ---- 新建笔记弹窗 ----
    void drawNewNoteDialog(eui::Ui& ui,
                           const eui::Screen& screen,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& componentTheme) {
        const float w = 460.0f;
        const float h = 220.0f;   // 错误行走输入框(y84+44=128)与按钮(y160)间 32px 空档，不撑高

        components::dialog(ui, "md.dialog.new")
            .open(newNoteOpen_)
            .screen(screen.width, screen.height)
            .size(w, h)
            .theme(componentTheme)
            .onOpenChange([this](bool v) { newNoteOpen_ = v; })
            .content([&] {
                ui.text("md.dialog.new.title")
                    .position(28.0f, 24.0f)
                    .size(w - 56.0f, 28.0f)
                    .text("新建笔记")
                    .fontSize(t.sizes.fontSizeLarge)
                    .lineHeight(28.0f)
                    .fontWeight(700)
                    .color(t.colors.textPrimary)
                    .build();

                ui.text("md.dialog.new.hint")
                    .position(28.0f, 56.0f)
                    .size(w - 56.0f, 18.0f)
                    .text("支持子目录，例如 工作/周会.md")
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(18.0f)
                    .color(t.colors.textTertiary)
                    .build();

                components::input(ui, "md.dialog.new.input")
                    .position(28.0f, 84.0f)
                    .size(w - 56.0f, 44.0f)
                    .value(newNoteName_)
                    .placeholder("笔记名，可用 / 分文件夹，如 工作/周会")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(componentTheme)
                    .onChange([this](const std::string& v) {
                        newNoteName_ = v;
                        newNoteError_.clear();  // 审计 [30]：错误随下一次输入清除
                    })
                    .onEnter([this, &ui] { commitNewNote(ui); })
                    // 审计 [21]：Esc=取消（关弹窗不落盘，同「取消」按钮）
                    .onCancel([this] { newNoteOpen_ = false; })
                    .build();

                // 审计 [30]：内嵌红字错误行（claude formError_ 模式）——弹窗里
                // 的校验/创建失败就地可见，不再隔 toast 指到弹窗外
                if (!newNoteError_.empty()) {
                    ui.text("md.dialog.new.error")
                        .position(28.0f, 134.0f)
                        .size(w - 56.0f, 18.0f)
                        .text(newNoteError_)
                        .fontSize(12.0f)
                        .lineHeight(18.0f)
                        .color(t.colors.error)
                        .build();
                }

                components::button(ui, "md.dialog.new.cancel")
                    .position(28.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("取消")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(componentTheme, false)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] { newNoteOpen_ = false; })
                    .build();

                components::button(ui, "md.dialog.new.confirm")
                    .position(28.0f + (w - 68.0f) * 0.5f + 12.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("创建")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(componentTheme, true)
                    .textColor(t.colors.onPrimary)
                    .radius(t.sizes.radius)
                    .onClick([this, &ui] { commitNewNote(ui); })
                    .build();
            })
            .build();
    }

    // ---- 新建文件夹弹窗（W9：左栏「+ 文件夹」入口；W10：加父目录下拉）----
    void drawNewFolderDialog(eui::Ui& ui,
                             const eui::Screen& screen,
                             const core::theme::Tokens& t,
                             const components::theme::ThemeColorTokens& componentTheme) {
        const float w = 460.0f;
        const float h = 320.0f;

        // 父目录候选：根目录占位 + 顶层夹（两级上限下，父目录只能是根或顶层夹，
        // 所以二级夹不进候选——选中它建出来的三级夹在树里没有行位可挂）
        std::vector<std::string> parents;
        parents.push_back("根目录");
        for (const auto& d : topLevelFolders()) parents.push_back(d);
        const int parentCount = static_cast<int>(parents.size());
        const int parentIdx = std::clamp(newFolderParentIdx_, 0, parentCount - 1);

        components::dialog(ui, "md.dialog.newfolder")
            .open(newFolderOpen_)
            .screen(screen.width, screen.height)
            .size(w, h)
            .theme(componentTheme)
            .onOpenChange([this](bool v) {
                newFolderOpen_ = v;
                if (!v) newFolderParentOpen_ = false;  // 关弹窗顺手收下拉
            })
            .content([&] {
                ui.text("md.dialog.newfolder.title")
                    .position(28.0f, 24.0f)
                    .size(w - 56.0f, 28.0f)
                    .text("新建文件夹")
                    .fontSize(t.sizes.fontSizeLarge)
                    .lineHeight(28.0f)
                    .fontWeight(700)
                    .color(t.colors.textPrimary)
                    .build();

                ui.text("md.dialog.newfolder.hint")
                    .position(28.0f, 56.0f)
                    .size(w - 56.0f, 18.0f)
                    .text("最多两级（顶层 / 子目录）；空文件夹也会显示在树里")
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(18.0f)
                    .color(t.colors.textTertiary)
                    .build();

                ui.text("md.dialog.newfolder.parent.label")
                    .position(28.0f, 84.0f)
                    .size(w - 56.0f, 16.0f)
                    .text("父目录")
                    .fontSize(12.0f)
                    .lineHeight(16.0f)
                    .color(t.colors.textTertiary)
                    .build();

                // 下拉弹层要盖住下方输入框/按钮：dropdown 自带 zIndex=20，经
                // dsl 的 subtreeMaxZIndex 把整个子树抬到后续兄弟之上（与设置页
                // 同一机制），故这里只需包一层定位 stack
                ui.stack("md.dialog.newfolder.parent")
                    .position(28.0f, 102.0f)
                    .size(w - 56.0f, 40.0f)
                    .content([&] {
                        components::dropdown(ui, "md.dialog.newfolder.parent.dd")
                            .size(w - 56.0f, 40.0f)
                            .items(parents)
                            .selected(parentIdx)
                            .placeholder("根目录")
                            .theme(componentTheme)
                            .open(newFolderParentOpen_)
                            .onOpenChange(
                                [this](bool v) { newFolderParentOpen_ = v; })
                            .onChange([this](int idx) { newFolderParentIdx_ = idx; })
                            .build();
                    })
                    .build();

                ui.text("md.dialog.newfolder.name.label")
                    .position(28.0f, 156.0f)
                    .size(w - 56.0f, 16.0f)
                    .text("文件夹名")
                    .fontSize(12.0f)
                    .lineHeight(16.0f)
                    .color(t.colors.textTertiary)
                    .build();

                components::input(ui, "md.dialog.newfolder.input")
                    .position(28.0f, 174.0f)
                    .size(w - 56.0f, 44.0f)
                    .value(newFolderName_)
                    .placeholder("文件夹名，如 工作")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(componentTheme)
                    .onChange([this](const std::string& v) {
                        newFolderName_ = v;
                        newFolderError_.clear();  // 审计 [30]：错误随下一次输入清除
                    })
                    .onEnter([this] { commitNewFolder(); })
                    // 审计 [21]：Esc=取消（关弹窗不建目录）
                    .onCancel([this] { newFolderOpen_ = false; })
                    .build();

                // 审计 [30]：内嵌红字错误行，同新建笔记弹窗
                if (!newFolderError_.empty()) {
                    ui.text("md.dialog.newfolder.error")
                        .position(28.0f, 228.0f)
                        .size(w - 56.0f, 18.0f)
                        .text(newFolderError_)
                        .fontSize(12.0f)
                        .lineHeight(18.0f)
                        .color(t.colors.error)
                        .build();
                }

                components::button(ui, "md.dialog.newfolder.cancel")
                    .position(28.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("取消")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(componentTheme, false)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] { newFolderOpen_ = false; })
                    .build();

                components::button(ui, "md.dialog.newfolder.confirm")
                    .position(28.0f + (w - 68.0f) * 0.5f + 12.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("创建")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(componentTheme, true)
                    .textColor(t.colors.onPrimary)
                    .radius(t.sizes.radius)
                    .onClick([this] { commitNewFolder(); })
                    .build();
            })
            .build();
    }

    // ---- 外部修改冲突提示（W8 → 本批改版）：dialog → toast action ----
    // EUI-NEO toast 已支持 action 按钮（单按钮 + 右上 ✕）：
    //   action「重载磁盘版本」= 从磁盘重读；✕ 关闭 = 保留本地编辑。
    // duration(0) 不自动超时——二选一必须显式决定，超时默认替用户做选择不可取。
    // ponytail: toast 单实例 + 单 action 按钮，冲突提示与普通状态提示共用
    // 一个实例（冲突期间优先展示冲突内容），普通 toast 排队语义等上游支持再做。

    // ==================== 业务动作 ====================

    // ---- Toast 反馈（仿 claude_module；⚠/✓ 前缀已换 icon 码点）----
    // 普通状态提示走 ui_kit（D1 去重）；外部修改冲突是 markdown 专用形态
    //（带 action 按钮 + 不自动超时，见上），就地绘制、不进 ui_kit——二者共用
    // md.toast 单实例且互斥，故每帧仍只调用一次 builder。
    void drawToast(eui::Ui& ui, const eui::Screen& screen,
                   const components::theme::ThemeColorTokens& themeTokens) {
        if (!conflictOpen_) {
            ui_kit::drawToast(ui, screen, "md.toast", toast_, themeTokens);
            return;
        }
        components::toast(ui, "md.toast")
            .theme(themeTokens)
            .screen(screen.width, screen.height)
            .visible(true)  // = 原 `conflict || toastOpen_`：本分支 conflict 必真
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

    // ---- Phase 1：四态切换 / 大纲 ----

    // W2 MD-05：实时态状态位群（文档锚 / 首帧聚焦 / 滚动位置）的单一复位点。
    // 三个位语义上是「一份状态」：锚失效（下次 compose 用编辑器光标重取）、
    // 首帧同帧补发 requestFocus（M0 C1/C5）、滚动位置归零。原先散在各路径手写
    // ——换笔记那条漏了滚动复位（换笔记后仍停在上篇的滚动位置），且新增入口
    // 容易再漏一个。故收成一处，进入实时态的三条路径（setMode 切态 / openNote
    // 换笔记 / loadViewState 启动恢复）统一调用。
    void enterLiveState(const char* reason) {
        liveCursorValid_ = false;
        liveFocusPending_ = true;
        liveOffset_ = 0.0f;
        // M3 / M0 C2：输入框状态也换代（下一帧 compose 里 release md.live.input）。
        // 换笔记 / 切实时态 / 启动恢复三条路径都经本函数 —— 只复位锚不复位状态
        // 时，上一篇的块文本、在飞的组字（compositionText）、framework 内置 undo
        // 栈会跟着稳定 id「复活」（M0 V2a 重挂载复活 / R7 状态累积）。
        liveInputStateFresh_ = true;
        LOG_DEBUG(std::string("Markdown 实时态状态复位: ") + reason);
    }

    void setMode(ViewMode m) {
        if (m == mode_) return;
        // Phase 3 M2：进实时态 → 状态位群复位（锚 / 聚焦 / 滚动，见 enterLiveState）
        if (m == ViewMode::Live) enterLiveState("切换视图");
        mode_ = m;
        // 进预览态时把滚动位置对齐编辑器当前位置（compose 里延迟计算，
        // 因为需要预览列宽和主题样式）
        if (m == ViewMode::Preview) syncPreviewOnEnter_ = true;
        saveViewState();  // 审计 [23]：四态视图跨会话记忆
        LOG_INFO("Markdown 视图切换: " + std::to_string(static_cast<int>(m)));
    }

    // Ctrl+Shift+P 循环切换四态（编辑 → 实时 → 双栏 → 预览 → 编辑）/ Ctrl+N
    // 新建笔记。通道 = app 级 onKeyEvent → Router 派发到当前页模块（EUI-NEO
    // InputBuilder 无逐组件按键 passthrough，那是 Phase 2 上游 PR；本快捷键不涉及
    // 编辑器文本变换，走全局通道无 undo 问题）。焦点在编辑器/搜索框/弹窗输入时
    // runtime_input 把内置未消费的键回调出来，Ctrl+N 不在 EUI-NEO input 内置键位表
    //（只吃 A/C/X/V/Z/Y），能到达这里
    bool onKeyEvent(const eui::KeyEvent& key) override {
        // 审计 M5：Esc 关最上层弹窗/浮层。键走得到本通道 = 焦点元素没消费
        //（新建笔记/新建文件夹/重命名三个弹窗的输入框均挂 onCancel，聚焦时
        // Esc 已被 input 收走 → 天然让位不重复）
        if (key.isDown() && key.key == eui::InputKey::Escape) {
            return closeTopmostPopup();
        }
        if (key.isDown() && key.modifiers.shortcut() && key.modifiers.shift &&
            key.key == eui::InputKey::P) {
            setMode(nextMode(mode_));
            return true;
        }
        // ux-design §4 #6 P1：Ctrl+N 打开新建笔记弹窗（与「+ 新建笔记」按钮/
        // 跨模块负载同一入口 openNewNoteDialog，秒建走弹窗内空名回车=默认名，
        // 行为与既有链路完全一致）。弹窗已开时不重复触发——否则会清掉用户
        // 正在输入的草稿名（返回 false 让按键落空，比误清代价低）。
        if (key.isDown() && key.modifiers.shortcut() && !key.modifiers.shift &&
            !key.modifiers.alt && key.key == eui::InputKey::N) {
            // 任一弹窗开着就落空：避免键盘叠弹窗 / 清掉正在输入的草稿
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

    // 新建笔记弹窗统一入口（顶栏「+ 新建笔记」/ 左栏「+ 笔记」/ Ctrl+N /
    // 跨模块负载共用）：打开前重置草稿名与内嵌错误行（审计 [19] 去重）
    void openNewNoteDialog() {
        newNoteOpen_ = true;
        newNoteName_.clear();
        newNoteError_.clear();
    }

    // 大纲当前节：光标行（InputState）所在的上一个标题；仅预览态无编辑器
    // 渲染时沿用上次跳转/光标位置（activeOutlineIdx_）
    int activeOutlineIndex(eui::Ui& ui) {
        if (outline_.empty()) return -1;
        if (mode_ == ViewMode::Preview) return activeOutlineIdx_;
        const auto& st = ui.state<components::input_detail::InputModel::InputState>(kEditorInputId);
        const int line = byteOffsetToLine(st.text, static_cast<size_t>(st.cursor));
        int idx = -1;
        for (size_t i = 0; i < outline_.size(); ++i) {
            if (outline_[i].lineNo <= line) idx = static_cast<int>(i);
            else break;
        }
        return idx;
    }

    // 大纲点击跳转：编辑/双栏态写 InputState.cursor 到标题行行首 + followCaret
    // （编辑器自动滚到光标，预览经同步滚动链路跟随）；仅预览态直接滚预览到该节。
    // 只改 cursor 不改文本 → 不动 undo 栈（§9.3 的 editText 规约不适用）。
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
        auto& st = ui.state<components::input_detail::InputModel::InputState>(kEditorInputId);
        st.cursor = static_cast<int>(h.byteOffset);
        st.selectionStart = st.cursor;
        st.selectionEnd = st.cursor;
        st.dragAnchor = st.cursor;
        st.hasPreferredCursorX = false;
        st.followCaret = true;
    }

    // 消费挂在 Markdown 页上的跳转负载（每帧调用；consumePendingPayload
    // 取出即清空，compose 逐帧重复调用不会重复触发）：
    //   "action:new_note"     → 打开新建笔记弹窗（等价页内「+ 新建笔记」）
    //   "open:<relativePath>" → 打开指定笔记进编辑器（POSIX 相对路径）
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

    void refreshList() {
        if (!service_) {
            notes_.clear();
            allFolders_.clear();
            allTags_.clear();
            return;
        }
        // W9/W10：磁盘全层目录实时枚举（含空目录），目录树据此显示
        // 空文件夹行（顶层夹 + 子夹）
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
        // 标签筛选：树只显示含当前筛选标签的笔记
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

    // 全部标签（左栏筛选区数据源）+ 筛选有效性检查
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
        // 筛选标签已无任何笔记引用（如最后一个用它的标签被删）→ 自动取消筛选
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

    // ---- 标签动作（spec §8.1 手动标签；数据全在 DB）----

    // 左栏标签 chip：点击过滤 / 再点取消
    void toggleTagFilter(const std::string& name) {
        tagFilter_ = (tagFilter_ == name) ? std::string() : name;
        refreshList();
        LOG_INFO("标签筛选: " + (tagFilter_.empty() ? "(无)" : tagFilter_));
    }

    // 编辑器标签行的「＋」/回车：给当前笔记加一个标签
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
        auto r = service_->setTags(currentPath_, currentTags_);
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

    // 编辑器标签 chip 的 ×：从当前笔记移除一个标签
    void removeTag(const std::string& name) {
        if (!service_ || currentPath_.empty()) return;
        std::vector<std::string> next;
        for (const auto& tg : currentTags_) {
            if (tg != name) next.push_back(tg);
        }
        if (next.size() == currentTags_.size()) return;
        auto r = service_->setTags(currentPath_, next);
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
        // 切换前先把上一篇的改动落盘
        saveEditor();
        // 审计 2026-09-23 [3]：saveEditor 失败（文件锁/磁盘满/只读，正是
        // W7 P1-2 退避机制针对的场景）时 editorDirty_ 保持 true，继续往下
        // 会无条件 editorText_ = 新内容 + 清 dirty——未落盘的编辑被静默丢
        // 弃。保数据优先于便利：还有未落盘改动就阻止切换并提示。每次点击
        // 都会经 saveEditor 立即重试一次保存，故障恢复后自然放行。
        // （onStop 对同一失败做 3 次重试 + 响亮日志；这里 5 个 openNote
        // 调用点——树点击/跳转/新建笔记——共用这一个守卫。）
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
        previewHover_.reset();  // P3-⑤：换笔记 = 块身份换代，旧 hover 作废
        // M2：换笔记 = 实时态状态群换代（锚 / 聚焦 / 滚动，见 enterLiveState；
        // W2 MD-05 前这里只复位锚，滚动位置残留在上一篇的偏移上）
        enterLiveState("换笔记");
        tagInput_.clear();  // 终验复审：标签输入框残留上一篇的半截输入
        // 标签行随笔记切换
        if (auto tr = service_->tagsForNote(relPath); tr.isOk()) {
            currentTags_ = std::move(tr.value());
        } else {
            currentTags_.clear();
        }
        syncCurrentMtime();  // W8：外部修改检测的 mtime 基准
        if (mode_ == ViewMode::Preview) syncPreviewOnEnter_ = true;
        saveViewState();  // 审计 [23]：选中笔记跨会话记忆
        showToast("已打开", relPath, false);
        LOG_INFO("打开笔记: " + relPath);
    }

    // ==================== 视图状态持久化（审计 [23]） ====================

    // onStart 尾部调用（refreshList 之后）：恢复上次选中笔记 / 四态视图 /
    // 收起目录。笔记可能已被外部删除——只在当前列表里能找到才重开，找不到
    // 静默跳过（下次成功 openNote 会覆盖旧值）。
    void loadViewState() {
        viewStore_ = std::make_unique<MarkdownViewStateStore>(markdownViewStateFile());
        const MarkdownViewState st = viewStore_->load();
        if (st.mode != static_cast<int>(mode_)) {
            // 直接赋值而非 setMode：恢复发生在启动期，setMode 的
            // saveViewState 会冗余落一次同样的值
            mode_ = static_cast<ViewMode>(st.mode);
            if (mode_ == ViewMode::Preview) syncPreviewOnEnter_ = true;
            // W2 MD-05：恢复进实时态与切实时态同语义（状态位群复位）
            if (mode_ == ViewMode::Live) enterLiveState("启动恢复视图");
        }
        collapsedDirs_.insert(st.collapsedDirs.begin(), st.collapsedDirs.end());
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

    // 尽力而为：视图状态写失败只记日志，绝不影响编辑主流程
    void saveViewState() const {
        if (!viewStore_) return;
        MarkdownViewState st;
        st.lastPath = currentPath_;
        st.mode = static_cast<int>(mode_);
        st.collapsedDirs.assign(collapsedDirs_.begin(), collapsedDirs_.end());
        auto r = viewStore_->save(st);
        if (r.isFail()) {
            LOG_WARN("Markdown: 视图状态落盘失败（不影响编辑）: " + r.errorMessage());
        }
    }

    // 编辑器里还有未落盘的改动时写回文件 + 索引（每帧调用，自带防抖）。
    // 保存失败后指数退避（W7 P1-2）：旧代码 dirty 期间每帧重试，
    // 60fps 下每秒打 60 次失败 IO/日志。
    void flushEditorIfIdle() {
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
            // toast 单实例：失败重试风暴期间只在无 toast 展示时弹一次
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
        syncCurrentMtime();  // W8：自己写盘后推进 mtime 基准（避免误报外部修改）
    }

    // ---- W8：文件变化自动感知（spec §8.1 承诺的 5 秒轮询）----

    // 每 5 秒一次（compose 每帧调用，内部用 steady_clock 节流）
    void pollDiskChangesIfDue() {
        if (!service_) return;
        const auto now = std::chrono::steady_clock::now();
        const auto sinceMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - lastPollAt_).count();
        if (!service_->hasWatchCredit() && sinceMs < kPollIntervalMs) return;
        lastPollAt_ = now;

        // 1) 目录级轻量比对：有差异（新文件/删除/修改）才做全量 rescan
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
        // 2) 当前打开笔记的外部修改 / 删除检测
        checkCurrentNoteOnDisk();
    }

    void checkCurrentNoteOnDisk() {
        if (currentPath_.empty() || conflictOpen_) return;
        auto m = service_->fileMtime(currentPath_);
        if (m.isFail()) {
            // 文件被外部删除：lastKnownMtime_ = -1 做已知缺失标记（防 5 秒一次
            // 的重复 toast；EUI-NEO toast 单实例限制下的节流手段）
            if (lastKnownMtime_ == -1) return;
            lastKnownMtime_ = -1;
            if (!editorDirty_) {
                currentPath_.clear();
                setEditorText(std::string());
                lastSavedText_.clear();
                outlineDirty_ = true;
                activeOutlineIdx_ = -1;
                saveViewState();  // 审计 [23]：已删笔记不该在下次启动被恢复
                showToast("笔记已被删除", "当前笔记已从磁盘移除", true);
            } else {
                // 本地还有未保存编辑：write() 会重建文件，提示一次即可
                showToast("笔记已被外部删除",
                          "保留的本地编辑会在保存时重新创建该文件", true);
            }
            return;
        }
        if (m.value() > lastKnownMtime_) {
            if (!editorDirty_) {
                // 无本地改动：静默重载（用户无感知的正确行为）
                reloadCurrentNote(true);
            } else {
                // 有未保存编辑：弹「重载 / 保留本地」选择（只弹一次，选择时
                // 才更新 lastKnownMtime_，避免轮询期间重复弹窗）
                conflictOpen_ = true;
            }
        }
    }

    // 从磁盘重读当前笔记（不经过 openNote 的「先保存旧笔记」路径）
    //
    // ⚠ 已知缺口（MD-01 残留入口，M3 未覆盖，见 open-questions-review-B 第 1 条）：
    // 静默重载 / 冲突重载把文档整篇换成磁盘版本后，实时态的活动块锚仍是【旧文档
    // 的字节偏移】，被新文档重新解释 → 活动块落到"覆盖该字节的新块"上（实测：
    // 前插一段后活动块从旧文档的块 3 跳到新文档里含该字节的块）。当帧即收敛、
    // 不丢字（输入框文本恒等于新文档某块原文，外部改值不触发 onChange），但
    // 「跟内容走」不成立——需要块身份映射（旧块起点 / stableId → 新文档），属
    // spec D3「块身份锚 = 文档字节偏移」的拍板项：本批不自行发明映射方案。
    void reloadCurrentNote(bool silent) {
        auto r = service_->read(currentPath_);
        if (r.isFail()) {
            if (!silent) {
                showToast("重载失败", r.errorMessage(), true);
            }
            return;
        }
        setEditorText(r.value());
        lastSavedText_ = editorText_;
        editorDirty_ = false;
        outlineDirty_ = true;
        activeOutlineIdx_ = -1;
        syncCurrentMtime();
        if (mode_ == ViewMode::Preview) syncPreviewOnEnter_ = true;
        if (silent) {
            // 审计 [30]：原 subtitle「已重新加载」改走 toast（5s 轮询自动
            // 重载换了屏幕内容必须出声）；冲突处理路径（silent=false）不弹
            // ——action 点击后 toast 必关，立刻再弹会被 onDismiss 连带清掉
            showToast("检测到外部修改，已自动重载", currentPath_, false);
        }
        LOG_INFO("重载笔记: " + currentPath_);
    }

    void resolveConflict(bool reloadFromDisk) {
        conflictOpen_ = false;
        if (reloadFromDisk) {
            // 重载本身就是可见反馈（编辑器内容即刻变化）；
            // 不再补 toast——action 点击后 toast 必关，立刻再弹会被 onDismiss
            // 连带清掉（单实例限制，见 drawToast 注释）
            reloadCurrentNote(false);
        } else {
            // 保留本地：把基准 mtime 推到磁盘当前值，未保存编辑走原有
            // 自动保存链路（400ms 防抖后覆盖写回）
            syncCurrentMtime();
            LOG_INFO("冲突处理：保留本地编辑 " + currentPath_);
        }
    }

    // lastKnownMtime_ ← 磁盘当前 mtime（打开/保存/重载后调用）
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
        // 去掉首尾空白
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
            name.erase(name.begin());
        }
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
            name.pop_back();
        }
        // ux-design §4 #6「默认名快速创建」（审计 [13][31]）：空名提交 = 建
        // 「无标题笔记」撞名自动编号，而非报错——显式命名撞名保持原语义
        // （已存在 → 直接打开）
        const bool defaultedName = name.empty();
        if (defaultedName) name = "无标题笔记";
        if (!service_) return;

        // W8 路径安全校验（目录树依赖 relativePath 落在 notesDir 内；
        // MarkdownService::create 有同规则兜底，这里给出更友好的提示）
        if (!MarkdownService::isSafeNewNotePath(name)) {
            // 审计 [30]：弹窗内校验错误改内嵌红字（原走 toast 隔到弹窗外）
            newNoteError_ =
                "笔记名不合法：不支持绝对路径、盘符（如 C:）或 ..，请用相对路径";
            return;
        }

        // 规范化路径：无扩展名时补 .md（与 MarkdownService::create 一致）
        std::string relPath = name;
        if (relPath.size() < 3 ||
            relPath.compare(relPath.size() - 3, 3, ".md") != 0) {
            relPath += ".md";
        }

        // 已存在 → 直接打开，不报错（仅显式命名；默认名走「永远新建」编号）
        if (!defaultedName && service_->read(relPath).isOk()) {
            newNoteOpen_ = false;
            refreshList();
            openNote(relPath);
            showToast("笔记已存在", "已直接打开 " + relPath, false);
            if (currentPath_ == relPath) ui.requestFocus(kEditorInputHitId);
            return;
        }
        if (defaultedName) {
            // 默认名撞名 → 「无标题笔记 N.md」自动编号（fileMtime 失败 = 不存在）
            for (int i = 2; service_->fileMtime(relPath).isOk(); ++i) {
                if (i > 999) {
                    newNoteError_ = "新建失败：同名无标题笔记过多，请手动命名";
                    return;
                }
                relPath = "无标题笔记 " + std::to_string(i) + ".md";
            }
        }

        auto r = service_->create(relPath);
        if (r.isFail()) {
            LOG_ERROR("新建笔记失败 " + relPath + ": " + r.errorMessage());
            // 审计 [30]：service 创建失败（目录不可写等）就地红字，弹窗不关
            newNoteError_ = "新建失败：" + r.errorMessage();
            return;
        }
        newNoteOpen_ = false;
        refreshList();
        openNote(relPath);
        showToast("已创建笔记", relPath, false);
        // 「新建后自动 focus 编辑器」（ux-design §4 #6，审计 [13][31]）：
        // EUI-NEO Ui::requestFocus 为上游小增量（Runtime 帧末消费），回车即
        // 可打字；openNote 守卫失败（未保存改动阻塞切换）时不聚焦
        if (currentPath_ == relPath) ui.requestFocus(kEditorInputHitId);
        LOG_INFO("新建笔记: " + relPath);
    }

    // ---- W9/W10：新建文件夹（左栏「+ 文件夹」入口）----

    // W10：父目录候选 = 顶层夹（两级上限下，二级夹不能再当父目录）
    std::vector<std::string> topLevelFolders() const {
        std::vector<std::string> out;
        for (const auto& d : allFolders_) {
            if (d.find('/') == std::string::npos) out.push_back(d);
        }
        return out;
    }

    // 父目录下拉下标 → 相对路径（0 = 根目录 → ""；越界/夹已消失兜底为根目录）
    std::string parentFolderForIndex(int idx) const {
        if (idx <= 0) return std::string();
        const std::vector<std::string> tops = topLevelFolders();
        const size_t i = static_cast<size_t>(idx) - 1;
        return i < tops.size() ? tops[i] : std::string();
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
            // 审计 [30]：弹窗内校验错误改内嵌红字
            newFolderError_ = "文件夹名不能为空";
            return;
        }
        if (!service_) return;

        // W10：父目录下拉（默认根目录）+ 名字 → 完整相对路径
        const std::string parent = parentFolderForIndex(newFolderParentIdx_);
        const std::string relPath = parent.empty() ? name : parent + "/" + name;
        // 树只渲染两级（顶层夹 + 一层子夹）：'/' ≥ 2 即三级夹，建出来在树里
        // 没有文件夹行可挂（用户看不见）→ 就地红字拦下。service 层仍支持更深
        // （createFolder 走 create_directories），这里只是 UI 侧的层级约束
        int slashes = 0;
        for (char c : relPath) {
            if (c == '/') ++slashes;
        }
        if (slashes >= 2) {
            newFolderError_ = "最多支持两级目录（顶层 / 子目录）";
            return;
        }

        auto r = service_->createFolder(relPath);
        if (r.isFail()) {
            LOG_ERROR("新建文件夹失败 " + relPath + ": " + r.errorMessage());
            // 审计 [30]：service 创建失败就地红字，弹窗不关
            newFolderError_ = "新建失败：" + r.errorMessage();
            return;
        }
        newFolderOpen_ = false;
        newFolderParentOpen_ = false;
        refreshList();  // listAllFolders 磁盘枚举 → 树里立即出现空文件夹行
        showToast("已创建文件夹", relPath, false);
        LOG_INFO("新建文件夹: " + relPath);
    }

    // ---- 生命周期：右键菜单 / 删除 / 重命名（审计 [4][20]：service remove
    //      早已就绪且被单测覆盖，本段补 UI 入口）----

    // 右键文件行 → 菜单弹出位置用指针事件的逻辑坐标（runtime 派发时已做
    // /dpiScale，见 runtime_input updateInteraction），标准右键菜单语义。
    // 指针坐标是窗口坐标、菜单浮层在页面子树内 → x 经 windowToPageX 减去
    // 页面区原点（y 方向原点恒 0 不换算）
    void openFileContextMenu(const std::string& relPath, const eui::PointerEvent& ev) {
        ctxMenuPath_ = relPath;
        ctxMenuX_ = windowToPageX(static_cast<float>(ev.x));
        ctxMenuY_ = static_cast<float>(ev.y);
        ctxMenuOpen_ = true;
    }

    void drawNoteContextMenu(eui::Ui& ui,
                             const eui::Screen& screen,
                             const core::theme::Tokens& t,
                             const components::theme::ThemeColorTokens& componentTheme) {
        (void)t;
        // 未右键过时无目标；常驻构建（关闭态由组件内部 opacity/disabled 收起）
        if (ctxMenuPath_.empty()) return;
        components::contextMenu(ui, "md.ctx")
            .open(ctxMenuOpen_)
            .screen(screen.width, screen.height)
            .position(ctxMenuX_, ctxMenuY_)
            .size(160.0f, 36.0f)
            .items({"重命名", "删除笔记"})
            .theme(componentTheme)
            .onSelect([this](int index) {
                if (index == 0) {
                    renameOpen_ = true;
                    renameTargetPath_ = ctxMenuPath_;
                    renameInput_ = ctxMenuPath_;  // 预填相对路径（含文件夹）
                    renameError_.clear();         // 审计 [19]：打开清残留错误
                } else if (index == 1) {
                    deleteConfirmOpen_ = true;
                    deleteTargetPath_ = ctxMenuPath_;
                }
            })
            .onOpenChange([this](bool v) { ctxMenuOpen_ = v; })
            .build();
    }

    // ---- 审计 [12]：编辑器右键「插入表格」（ux-design §5.5 MVP）----
    // spec 例举自绘网格 hover 选区；EUI-NEO contextMenu 原生支持子菜单级联
    // （ContextMenuItem children + onSelectPath），行→列两级即可选尺寸，网格化
    // 留 V1 增强。行数含表头行（GFM 表头 + 分隔行自动生成）。
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
        (void)t;
        // 只在打开时构建（树形菜单同款惰性；关闭态无覆盖层残留）
        if (!editorCtxOpen_) return;
        components::contextMenu(ui, "md.editor.ctx")
            .open(editorCtxOpen_)
            .screen(screen.width, screen.height)
            .position(editorCtxX_, editorCtxY_)
            .size(170.0f, 36.0f)
            .items(buildTableMenuItems())
            .theme(componentTheme)
            .onSelectPath([this, &ui](const std::vector<int>& path) {
                // path = [0(插入表格), 行下标, 列下标]（三级叶子的层级索引）
                if (path.size() == 3 && path[0] == 0) {
                    insertTableAtCursor(ui, path[1] + 1, path[2] + 1);
                }
            })
            .onOpenChange([this](bool v) { editorCtxOpen_ = v; })
            .build();
    }

    // 「插入表格」→ 光标处插模板（与键盘流同一写回：可 undo、走脏标记链路）
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

    // 删除确认弹窗（与 todo 删任务 / claude 删 Profile 同款「不可恢复」文案）
    void drawDeleteDialog(eui::Ui& ui,
                          const eui::Screen& screen,
                          const core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& componentTheme) {
        const float w = 460.0f;
        const float h = 200.0f;
        components::dialog(ui, "md.dialog.del")
            .open(deleteConfirmOpen_)
            .screen(screen.width, screen.height)
            .size(w, h)
            .theme(componentTheme)
            .onOpenChange([this](bool v) { deleteConfirmOpen_ = v; })
            .content([&] {
                ui.text("md.dialog.del.title")
                    .position(28.0f, 24.0f)
                    .size(w - 56.0f, 28.0f)
                    .text("删除笔记")
                    .fontSize(t.sizes.fontSizeLarge + 2.0f)
                    .lineHeight(28.0f)
                    .fontWeight(700)
                    .color(t.colors.textPrimary)
                    .build();
                ui.text("md.dialog.del.body")
                    .position(28.0f, 60.0f)
                    .size(w - 56.0f, 40.0f)
                    .text("将删除笔记「" + deleteTargetPath_ + "」及其磁盘文件，不可恢复。")
                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                    .lineHeight(20.0f)
                    .wrap()
                    .color(t.colors.textSecondary)
                    .build();
                components::button(ui, "md.dialog.del.cancel")
                    .position(28.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("取消")
                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                    .theme(componentTheme, false)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] { deleteConfirmOpen_ = false; })
                    .build();
                components::button(ui, "md.dialog.del.confirm")
                    .position(28.0f + (w - 68.0f) * 0.5f + 12.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("删除")
                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                    .theme(componentTheme, false)
                    .colors(t.colors.error, t.colors.error, t.colors.error)
                    .textColor(t.colors.onPrimary)
                    .radius(t.sizes.radius)
                    .onClick([this] { doDeleteNote(); })
                    .build();
            })
            .build();
    }

    // 重命名对话框（预填当前相对路径；改名 = 换名，填含 / 的路径 = 移动文件）
    void drawRenameDialog(eui::Ui& ui,
                          const eui::Screen& screen,
                          const core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& componentTheme) {
        const float w = 460.0f;
        const float h = 220.0f;
        components::dialog(ui, "md.dialog.rename")
            .open(renameOpen_)
            .screen(screen.width, screen.height)
            .size(w, h)
            .theme(componentTheme)
            .onOpenChange([this](bool v) { renameOpen_ = v; })
            .content([&] {
                ui.text("md.dialog.rename.title")
                    .position(28.0f, 24.0f)
                    .size(w - 56.0f, 28.0f)
                    .text("重命名笔记")
                    .fontSize(t.sizes.fontSizeLarge)
                    .lineHeight(28.0f)
                    .fontWeight(700)
                    .color(t.colors.textPrimary)
                    .build();
                ui.text("md.dialog.rename.hint")
                    .position(28.0f, 56.0f)
                    .size(w - 56.0f, 18.0f)
                    .text("相对笔记目录；填「工作/新名」可同时移动，无 .md 自动补全")
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(18.0f)
                    .color(t.colors.textTertiary)
                    .build();
                components::input(ui, "md.dialog.rename.input")
                    .position(28.0f, 84.0f)
                    .size(w - 56.0f, 44.0f)
                    .value(renameInput_)
                    .placeholder("新名字或新路径")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(componentTheme)
                    .onChange([this](const std::string& v) {
                        renameInput_ = v;
                        renameError_.clear();  // 审计 [19] 收尾：错误随下一次输入清除
                    })
                    .onEnter([this] { commitRenameNote(); })
                    // 审计 [21]：Esc=取消（关弹窗不改名）
                    .onCancel([this] { renameOpen_ = false; })
                    .build();
                // 审计 [19] 收尾：重命名弹窗校验/落盘失败内嵌红字（原走 toast
                // 隔到弹窗外，与 [30] 的新建笔记/文件夹弹窗不一致）；弹窗保持
                // 打开让用户改输入
                if (!renameError_.empty()) {
                    ui.text("md.dialog.rename.error")
                        .position(28.0f, 134.0f)
                        .size(w - 56.0f, 18.0f)
                        .text(renameError_)
                        .fontSize(12.0f)
                        .lineHeight(18.0f)
                        .color(t.colors.error)
                        .build();
                }
                components::button(ui, "md.dialog.rename.cancel")
                    .position(28.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("取消")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(componentTheme, false)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] { renameOpen_ = false; })
                    .build();
                components::button(ui, "md.dialog.rename.confirm")
                    .position(28.0f + (w - 68.0f) * 0.5f + 12.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("重命名")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(componentTheme, true)
                    .textColor(t.colors.onPrimary)
                    .radius(t.sizes.radius)
                    .onClick([this] { commitRenameNote(); })
                    .build();
            })
            .build();
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
        // 删的是当前打开的笔记 → 编辑器直接清空：文件已不存在，未保存的
        // 编辑随之作废（不做「删了又写回去」的复活）；清 dirty 防自动保存
        // 把文件重建出来
        if (currentPath_ == path) {
            currentPath_.clear();
            setEditorText(std::string());
            lastSavedText_.clear();
            editorDirty_ = false;
            saveFailed_ = false;
            saveViewState();  // 审计 [23]：删的是当前笔记 → 视图状态同步清空
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
            // 审计 [19] 收尾：原「清空预填回车 = 关弹窗视为反悔」与新建笔记/
            // 文件夹弹窗的校验反馈不一致（且静默关闭容易被当成没生效），
            // 统一改内嵌红字，弹窗保持打开让用户重填
            renameError_ = "新名字不能为空";
            return;
        }
        if (!service_ || renameTargetPath_.empty()) {
            renameOpen_ = false;
            return;
        }
        if (!MarkdownService::isSafeNewNotePath(target)) {
            // 审计 [19] 收尾：校验失败内嵌红字（原走 toast 隔到弹窗外）
            renameError_ =
                "名称不合法：不支持绝对路径、盘符（如 C:）或 ..，请用相对路径";
            return;  // 弹窗保持打开让用户改
        }
        auto r = service_->rename(renameTargetPath_, target);
        if (r.isFail()) {
            LOG_ERROR("重命名笔记失败 " + renameTargetPath_ + " → " + target +
                      ": " + r.errorMessage());
            // 审计 [19] 收尾：service 失败（目标已存在/撞名等）内嵌红字，
            // 原走 toast；弹窗保持打开可改输入重试
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
            // 编辑器继续跟随这篇笔记（内容未变，只换 path；若有未保存编辑，
            // 自动保存将写到新路径——正确语义）
            currentPath_ = newPath;
            syncCurrentMtime();
            saveViewState();  // 审计 [23]：重命名后恢复新路径
        }
        refreshList();
        showToast("已重命名", newPath, false);
        LOG_INFO("重命名笔记: " + oldPath + " → " + newPath);
    }

    // ==================== 状态 ====================

    static constexpr int64_t kAutoSaveDelayMs = 400;
    static constexpr int64_t kMaxSaveBackoffMs = 60000;  // 保存失败退避封顶 60s
    static constexpr int64_t kPollIntervalMs = 5000;     // W8：文件变化轮询周期

    std::unique_ptr<MarkdownService> service_;
    // 审计 [23]：视图状态 store（loadViewState 创建；未创建 = 不持久化，saveViewState 静默跳过）
    std::unique_ptr<MarkdownViewStateStore> viewStore_;
    std::vector<NoteMeta> notes_;      // 当前展示（全部或搜索结果）
    std::string searchQuery_;          // 搜索框内容
    std::string currentPath_;          // 打开的笔记（POSIX 相对路径）
    std::string editorText_;           // 编辑器缓冲
    // ---- 审计 perf [14][15][39]：以 editorText_ 内容修订为键的跨帧缓存 ----
    // 写点唯一（setEditorText）。初值 hash 与旧「首帧 std::hash(空串)」一致，
    // 保证任何帧 contentKey 取值与优化前逐位相等。
    uint64_t editorTextRev_ = 0;
    uint64_t previewTextHash_ = std::hash<std::string>{}(std::string());
    // [14] → P3-④：computeSyncOffset 块模型精确锚几何表（键 = 修订 + 宽度 +
    // blockGap + 块数；块高度按 stableId 备忘，编辑只重测脏块）。取代旧
    // yA/yB 单锚缓存（标题锚 + 换锚即重算）。
    MdSyncMap syncMap_;
    // P3-⑤：预览逐块 hover 状态（当前 hovered 的 stableId，乱序 leave 保护
    // 在 MdPreviewHoverState 内）；点击定位的纯数学在 mdResolvePreviewJump。
    MdPreviewHoverState previewHover_;
    // [39] 状态栏 (行, 列, 码点数) 缓存（键 = 修订 + 光标字节偏移）
    uint64_t statusCacheRev_ = ~0ull;
    size_t statusCacheCursor_ = ~0ull;
    int statusCacheLine_ = 0;
    int statusCacheCol_ = 1;
    size_t statusCacheChars_ = 0;
    std::string lastSavedText_;        // 上次落盘内容（用于跳过冗余写）
    bool editorDirty_ = false;
    bool saveFailed_ = false;          // 上次保存尝试失败（状态栏 ✕ 提示）
    std::chrono::steady_clock::time_point lastEditAt_{};
    std::chrono::steady_clock::time_point lastSaveAttemptAt_{};  // 退避计时基准
    int64_t saveBackoffMs_ = 0;        // 0 = 无退避；失败后指数增长
    std::string listError_;            // 左栏错误态（空 = 正常；W7 P1-1）
    bool newNoteOpen_ = false;
    std::string newNoteName_;
    // 审计 [30]：弹窗内嵌红字错误行（照抄 claude_module formError_ 模式）——
    // 校验/创建失败不再走 toast 隔空提示，错误随下一次输入清除
    std::string newNoteError_;
    bool newFolderOpen_ = false;       // W9：新建文件夹弹窗
    std::string newFolderName_;
    std::string newFolderError_;       // 审计 [30]：同款内嵌错误行
    // W10：父目录下拉（0 = 根目录，1..n = 顶层夹下标；只列顶层夹——
    // 两级上限下，二级夹不能再当父目录）
    int newFolderParentIdx_ = 0;
    bool newFolderParentOpen_ = false; // 下拉展开态（组件自带 popup 显隐）
    // W10：磁盘全层目录（POSIX 相对路径，含空目录；树渲染 + 父目录下拉用）
    std::set<std::string> allFolders_;

    // ---- 生命周期（审计 [4][20]）：树行右键菜单 + 删除确认 + 重命名 ----
    bool ctxMenuOpen_ = false;
    float ctxMenuX_ = 0.0f;
    float ctxMenuY_ = 0.0f;
    std::string ctxMenuPath_;        // 右键目标（POSIX 相对路径）
    // 审计 [12]：编辑器右键「插入表格」级联菜单
    bool editorCtxOpen_ = false;
    float editorCtxX_ = 0.0f;
    float editorCtxY_ = 0.0f;
    bool deleteConfirmOpen_ = false;
    std::string deleteTargetPath_;   // 待删笔记（确认弹窗展示 + 删除目标）
    bool renameOpen_ = false;
    std::string renameTargetPath_;   // 重命名原路径
    std::string renameInput_;        // 重命名输入框内容
    std::string renameError_;        // 审计 [19] 收尾：弹窗内嵌红字错误行（照 [30] newNoteError_ 模式）

    // ---- Toast（EUI-NEO 单实例，复用一个状态位；状态位 + 绘制接线走 ui_kit）----
    ui_kit::ToastState toast_;

    // ---- Phase 1 状态 ----
    // 四态视图（W9：默认编辑态 = 左树 + 右编辑两栏；实时/双栏/预览经顶栏
    // segmented 或 Ctrl+Shift+P 切换——用户确认期望「左侧目录树、右侧编辑区」）
    ViewMode mode_ = ViewMode::Edit;
    std::vector<MdHeading> outline_;   // 大纲（标题扫描结果）
    bool outlineDirty_ = true;
    int activeOutlineIdx_ = -1;        // 仅预览态用（无编辑器光标可读）
    float previewOffset_ = 0.0f;       // 仅预览态的手动滚动位置
    float liveOffset_ = 0.0f;          // 实时态的手动滚动位置（M1；M6 接光标跟随；enterLiveState 归零）
    // ---- Phase 3 M2：实时态活动块（单输入跟随 + 内容贴合高度）----
    size_t liveCursorByte_ = 0;        // 文档字节偏移 = 活动块身份锚（spec D3）
    size_t liveActiveBlock_ = 0;       // 本帧活动块下标（每帧由锚重算）
    size_t liveInputStart_ = 0;        // 活动 input 文本在文档中的起点（= 其所属块 start，MD-01 锚推导基准）
    bool liveCursorValid_ = false;     // 锚是否已按当前文档初始化（enterLiveState 复位）
    bool liveFocusPending_ = false;    // 进实时态首帧同帧补发 requestFocus（M0 C1；enterLiveState 置位）
    // ---- Phase 3 M3：焦点/切块状态机 ----
    bool liveInputStateFresh_ = false; // 输入框状态待换代：enterLiveState 置位，
                                       // 下一帧 compose 释放 md.live.input（M0 C2）
    bool liveCaretPending_ = false;    // 待写块局部光标（setLiveCursor 置位；文本未变
                                       // 的同块定位帧也要写，帧末统一清位）
    uint64_t liveSyncedRev_ = 0;       // 输入框文本与文档对齐的修订号（失焦提交安全判据）
    // 贴合高度备忘（键 = stableId + 内容 hash + 文本宽 + 字号）：活动块整块
    // shaping 不逐帧重付；内容/宽度/字号任一变化即失效重测
    struct LiveHeightMemo {
        int stableId = -1;
        uint64_t hash = 0;
        float width = -1.0f;
        float fontSize = 0.0f;
        float height = 0.0f;
    };
    LiveHeightMemo liveHeight_;
    bool syncPreviewOnEnter_ = false;  // 刚切入预览态 → 对齐编辑器位置
    float lastPvContentWidth_ = 0.0f;  // 预览内容宽（estimateHeight 用，滞后一帧）
    std::time_t lastSavedAt_ = 0;      // 上次落盘时刻（状态栏 "✓ 已保存 HH:MM"）

    // ---- W8 状态 ----
    std::set<std::string> collapsedDirs_;             // 已收起的一级目录（内存态）
    bool outlineOpen_ = false;                        // 大纲浮动面板开关
    int64_t lastKnownMtime_ = 0;                      // 当前笔记已知磁盘 mtime（-1=已删除）
    bool conflictOpen_ = false;                       // 外部修改冲突 toast（action 二选一）
    std::chrono::steady_clock::time_point lastPollAt_{};  // 轮询节流基准

    // ---- 标签状态（spec §8.1）----
    std::vector<std::string> currentTags_;  // 当前打开笔记的标签
    std::vector<TagCount> allTags_;         // 左栏筛选区的全部标签（含笔记数）
    std::string tagFilter_;                 // 生效中的标签筛选（空 = 无）
    std::string tagInput_;                  // 添加标签输入框内容

    // ---- Phase C 语法高亮状态 ----
    MdHighlighter highlighter_;        // 行级高亮器（围栏状态随 display 文本重建）
    std::string highlightThemeName_;   // 上次注入色表的主题名（变化 = 重高亮）

    // ---- Phase 3 块级元素模型（预览逐块增量渲染的键控源）----
    MdBlockModel blockModel_;

    // ---- Phase 3 ③ 预览代码块高亮（CodeFence 块自绘的脏块缓存）----
    MdPreviewCodeCache previewCodeCache_;
};

} // namespace module::markdown

extern "C" ::core::Module* create_markdown_module() {
    return new module::markdown::MarkdownModule();
}
