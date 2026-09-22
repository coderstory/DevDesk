// src/module_markdown/markdown_module.cpp
// Markdown 笔记本模块 —— 真实文件读写 + FTS5 搜索（3 栏 UI）
//
// 数据流：
//   onStart()  → 从 core::appDatabase() 拿共享连接 → MarkdownService → rescan()
//   compose()  → 左栏二级目录树（md_tree 纯函数分组）/ 中栏编辑器（真实读文件、
//                改即写盘）/ 右栏预览（components::markdown 渲染成 eui 节点）
//
// Phase 1 编辑体验（docs/superpowers/specs/2026-09-22-markdown-editor-ux-design.md）：
//   - 编辑 / 双栏 / 预览三态切换（顶栏 segmented + Ctrl+Shift+P 循环）
//   - 双栏同步滚动（标题锚点 + 段内线性插值，编辑器单向驱动预览）
//   - 编辑器底部状态栏（保存状态 ●/✓ + 行:列 · N 字）
//
// W8 左栏重构（用户确认方案）：
//   - 左栏 = 二级目录树（顶层文件夹 → 二级文件，根目录散文件单独一组，
//     文件夹可展开/收起，搜索时展平显示命中文件）
//   - 大纲 = 顶栏开关 + 浮动面板（positioned + zIndex，不挤压编辑区）
//   - 文件变化自动感知：5 秒轮询（pollChanged 轻量比对 → rescan），
//     当前笔记被外部修改时弹「重载 / 保留本地」选择（dialog；EUI-NEO toast
//     无按钮能力，选择类交互用 dialog）
//
// 所有 I/O 都通过 MarkdownService，失败走 core::Result（不抛异常）。
#include "core/database.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/router.h"
#include "core/theme.h"
#include "core/time.h"
#include "markdown_service.h"
#include "md_line_scanner.h"
#include "md_tree.h"

#include <eui/dsl.h>

#include <components/button.h>
#include <components/dialog.h>
#include <components/input.h>
#include <components/markdown.h>
#include <components/scrollview.h>
#include <components/segmented.h>
#include <components/switch.h>
#include <components/theme.h>
#include <components/toast.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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
void drawFileTreeItem(eui::Ui& ui,
                      const std::string& id,
                      float width,
                      const core::theme::Tokens& t,
                      const std::string& icon,
                      const std::string& name,
                      bool active,
                      float indent,
                      std::function<void()> onClick) {
    ui.stack(id)
        .size(width, 32.0f)
        .content([&] {
            ui.rect(id + ".row")
                .size(width, 32.0f)
                .color(active ? t.colors.primarySoftBg : t.colors.surface)
                .hoverColor(t.colors.hover)
                .onClick(std::move(onClick))
                .build();
            ui.text(id + ".icon")
                .position(12.0f + indent, 0.0f)
                .size(20.0f, 32.0f)
                .text(icon)
                .fontSize(t.sizes.fontSizeBody)
                .lineHeight(32.0f)
                .color(t.colors.textSecondary)
                .build();
            ui.text(id + ".name")
                .position(36.0f + indent, 0.0f)
                .size(std::max(0.0f, width - 48.0f - indent), 32.0f)
                .text(name)
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(32.0f)
                .color(active ? t.colors.primary : t.colors.textPrimary)
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

// 三态视图（值 = 顶栏 segmented 的选中下标）
enum class ViewMode { Edit = 0, Split = 1, Preview = 2 };

constexpr float kEditorFontSize = 15.0f;      // = fontSizeBody（视觉规范正文 15px，W7 P1-4 换 token 基准）
constexpr float kEditorLineHeight = kEditorFontSize * 1.2f;  // input 内部行高
constexpr float kEditTextColumnWidth = 720.0f;  // 仅编辑态的居中文本列宽（Typora 写作宽度）
constexpr float kPreviewColumnWidth = 760.0f;   // 仅预览态的居中阅读列宽

// "已保存 HH:MM"
std::string formatSaveTime(std::time_t t) {
    if (t <= 0) return {};
    std::tm tm{};
    ::core::time::localTime(t, &tm);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
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
            status_ = "重扫失败：" + r.errorMessage();
        } else {
            status_ = "已加载笔记目录：" + service_->notesDir().filename().string();
        }
        refreshList();
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
            t.isDark ? components::theme::dark() : components::theme::light();

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

        // 页面背景自绘（仿 settings_module：main.cpp clearColor 固定浅色，
        // 深色主题下页面必须自己铺底，否则黑底/浅底混色不可读——W7 P0-1）
        ui.rect("md.pagebg")
            .size(screen.width, screen.height)
            .color(t.colors.background)
            .build();

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
        drawConflictDialog(ui, screen, t, componentTheme);

        // 保存失败等操作的 toast 反馈（画在弹窗之上）
        drawToast(ui, screen, componentTheme);
    }

private:
    // ---------- 顶部 Header ----------
    void drawHeader(eui::Ui& ui,
                    float contentW,
                    float contentH,
                    float bodyH,
                    const core::theme::Tokens& t) {
        (void)contentH;
        (void)bodyH;
        const auto componentTheme =
            t.isDark ? components::theme::dark() : components::theme::light();
        const std::string subtitle =
            service_ ? (std::to_string(notes_.size()) + " 篇笔记 · " + status_)
                     : "尚未初始化笔记目录";

        ui.row("md.header")
            .size(contentW, 64.0f)
            .alignItems(eui::Align::CENTER)
            .content([&] {
                const float segW = 264.0f;
                ui.column("md.title")
                    .size(contentW - 320.0f - segW - t.sizes.spacingSmall, 64.0f)
                    .gap(2.0f)
                    .content([&] {
                        ui.text("md.h1")
                            .size(contentW - 320.0f - segW - t.sizes.spacingSmall, 32.0f)
                            .text("Markdown 笔记本")
                            .fontSize(t.sizes.fontSizeTitle)
                            .lineHeight(32.0f)
                            .fontWeight(700)
                            .color(t.colors.textPrimary)
                            .build();
                        ui.text("md.subtitle")
                            .size(contentW - 320.0f - segW - t.sizes.spacingSmall, 18.0f)
                            .text(subtitle)
                            .fontSize(t.sizes.fontSizeSmall)
                            .lineHeight(18.0f)
                            .color(t.colors.textSecondary)
                            .build();
                    })
                    .build();

                // 三态切换：编辑 / 双栏 / 预览（Ctrl+Shift+P 循环）
                components::segmented(ui, "md.mode")
                    .size(segW, 36.0f)
                    .items({"✎ 编辑", "⇔ 双栏", "👁 预览"})
                    .selected(static_cast<int>(mode_))
                    .fontSize(t.sizes.fontSizeSmall)
                    .theme(componentTheme)
                    .onChange([this](int index) {
                        setMode(static_cast<ViewMode>(index));
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
                            .textColor(eui::Color("#FFFFFF"))
                            .radius(t.sizes.radius)
                            .onClick([this] {
                                newNoteOpen_ = true;
                                newNoteName_.clear();
                            })
                            .build();
                    })
                    .build();
            })
            .build();
    }

    // ---------- 三栏主区（按视图模式裁剪：编辑/双栏/预览） ----------
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
        // W9 修复：旧结构 = column（只铺底色）+ 兄弟 overlay stack，overlay
        // 会在 md.body row 的流式布局里占第二个同样宽的槽位（三列整体右移
        // 溢出屏）。改为单个 stack：bg 不定位时居中铺满，内容绝对定位
        // （同 drawOutlinePanel 的成熟模式）。
        ui.stack("md.tree").size(sideW, bodyH).content([&] {
            ui.rect("md.tree.bg")
                .size(sideW, bodyH)
                .color(t.colors.surface)
                .radius(t.sizes.radiusLarge)
                .border(1.0f, t.colors.borderDefault)
                .build();
            // 搜索框：真实调用 FTS5
            components::input(ui, "md.search")
                .position(12.0f, 12.0f)
                .size(sideW - 24.0f, 36.0f)
                .value(searchQuery_)
                .placeholder("🔍 搜索笔记…")
                .fontSize(t.sizes.fontSizeSmall)
                .inset(10.0f)
                .theme(componentTheme)
                .onChange([this](const std::string& v) {
                    searchQuery_ = v;
                    refreshList();
                })
                .build();

            // 搜索时树展平为命中文件列表；空搜索 = 二级目录树（W8）
            const float listY = 60.0f;
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
                ui.text("md.tree.empty")
                    .position(16.0f, listY + 8.0f)
                    .size(sideW - 32.0f, 40.0f)
                    .text(searchQuery_.empty() ? "还没有笔记\n点下面「+ 新建」开始"
                                               : "没有匹配的笔记")
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(18.0f)
                    .wrap()
                    .color(t.colors.textTertiary)
                    .build();
            } else if (!searchQuery_.empty()) {
                drawNoteList(ui, listY, listH, sideW, t, componentTheme);
            } else {
                drawTreeList(ui, listY, listH, sideW, t, componentTheme);
            }

            // 底部「+ 新建」
            const float newBtnY = bodyH - 52.0f;
            components::button(ui, "md.tree.new")
                .position(12.0f, newBtnY)
                .size(sideW - 24.0f, 36.0f)
                .text("+ 新建")
                .fontSize(t.sizes.fontSizeSmall)
                .theme(componentTheme, false)
                .textColor(t.colors.textPrimary)
                .radius(t.sizes.radius)
                .border(1.0f, t.colors.borderDefault)
                .onClick([this] {
                    newNoteOpen_ = true;
                    newNoteName_.clear();
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
                    drawFileTreeItem(inner, id, w, t, "📄", noteDisplayName(n),
                                     path == currentPath_, 0.0f,
                                     [this, path] { openNote(path); });
                }
            })
            .build();
    }

    // ---- 左栏：二级目录树（W8，md_tree 纯函数分组）----
    void drawTreeList(eui::Ui& ui,
                      float listY,
                      float listH,
                      float sideW,
                      const core::theme::Tokens& t,
                      const components::theme::ThemeColorTokens& componentTheme) {
        const std::vector<MdTreeRow> rows = buildTreeRows(notes_, collapsedDirs_);
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
                        // 文件夹行：▸/▾ 展开指示 + 📁，点击收起/展开
                        // （同样包 stack，position 只在 stack 内生效）
                        const bool expanded = collapsedDirs_.count(r.relPath) == 0;
                        inner.stack(id)
                            .size(w, 32.0f)
                            .content([&] {
                                inner.rect(id + ".row")
                                    .size(w, 32.0f)
                                    .color(t.colors.surface)
                                    .hoverColor(t.colors.hover)
                                    .onClick([this, dir = r.relPath] {
                                        if (collapsedDirs_.count(dir) != 0) {
                                            collapsedDirs_.erase(dir);
                                        } else {
                                            collapsedDirs_.insert(dir);
                                        }
                                    })
                                    .build();
                                inner.text(id + ".arrow")
                                    .position(6.0f, 0.0f)
                                    .size(16.0f, 32.0f)
                                    .text(expanded ? "▾" : "▸")
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .lineHeight(32.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                                inner.text(id + ".name")
                                    .position(28.0f, 0.0f)
                                    .size(std::max(0.0f, w - 40.0f), 32.0f)
                                    .text("📁 " + r.name)
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .lineHeight(32.0f)
                                    .color(t.colors.textPrimary)
                                    .fontWeight(700)
                                    .build();
                            })
                            .build();
                    } else {
                        // 文件行：depth=1 缩进 20px
                        drawFileTreeItem(inner, id, w, t, "📄", r.name,
                                         r.relPath == currentPath_,
                                         20.0f,
                                         [this, path = r.relPath] { openNote(path); });
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
                    .color(t.colors.surface)
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
                                        .color(current ? t.colors.primarySoftBg
                                                       : t.colors.surface)
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
                                        .color(current ? t.colors.primary
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

    // ---- 中栏：编辑器（真实读 / 写文件）----
    // Phase 1：栏内小标题移到状态栏（§5.7 去密度）；仅编辑态文本列居中 720px。
    void drawEditorColumn(eui::Ui& ui,
                          float bodyH,
                          float midW,
                          const core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& componentTheme) {
        (void)componentTheme;
        // W9 修复：同左栏——column+overlay 兄弟双占位改单 stack
        ui.stack("md.editor").size(midW, bodyH).content([&] {
            ui.rect("md.editor.bg")
                .size(midW, bodyH)
                .color(t.colors.surface)
                .radius(t.sizes.radiusLarge)
                .border(1.0f, t.colors.borderDefault)
                .build();

            const bool hasNote = !currentPath_.empty();

            if (!hasNote) {
                ui.text("md.editor.hint")
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

            float edX = 16.0f;
            float edW = midW - 32.0f;
            if (mode_ == ViewMode::Edit) {
                // 无干扰写作：文本列 clamp 720px 居中（Typora 写作宽度）
                edW = std::min(edW, kEditTextColumnWidth);
                edX = std::max(0.0f, (midW - edW) * 0.5f);
            }
            const float edY = 16.0f;
            const float edH = std::max(0.0f, bodyH - edY - 22.0f - 8.0f);

            components::input(ui, "md.editor.input")
                .position(edX, edY)
                .size(edW, edH)
                .value(editorText_)
                .placeholder("在这里写 Markdown…")
                .multiline(true)
                .scrollbar(true)
                .fontSize(kEditorFontSize)
                .inset(10.0f)
                .theme(componentTheme)
                .onChange([this](const std::string& v) {
                    // 每次输入都更新内存缓冲；落盘走防抖（见 flushEditorIfIdle）
                    editorText_ = v;
                    editorDirty_ = true;
                    outlineDirty_ = true;
                    lastEditAt_ = std::chrono::steady_clock::now();
                })
                .build();

            drawStatusBar(ui, 0.0f, bodyH - 22.0f, midW, t);
        }).build();
    }

    // 编辑器底部状态栏：左 = 文件名 + 保存状态（● 未保存 / ✓ 已保存 HH:MM），
    // 右 = 行:列 · N 字（隐含 UX #7：把自动保存从视线外的顶栏 subtitle 升级为
    // 视线内的持续反馈）
    void drawStatusBar(eui::Ui& ui, float x, float y, float w, const core::theme::Tokens& t) {
        ui.rect("md.status.bg")
            .position(x, y)
            .size(w, 22.0f)
            .color(t.colors.surface)
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
        ui.text("md.status.save")
            .position(x + 24.0f, y)
            .size(w * 0.6f, 22.0f)
            .text(left)
            .fontSize(11.0f)
            .lineHeight(22.0f)
            .color(leftColor)
            .build();

        // 行:列 · N 字（列/字数按 UTF-8 码点计）
        std::string right;
        if (!currentPath_.empty()) {
            const auto& st = ui.state<components::input_detail::InputModel::InputState>("md.editor.input");
            const int line = byteOffsetToLine(st.text, static_cast<size_t>(st.cursor));
            const size_t lineStart = lineStartOffset(st.text, line);
            const int col = static_cast<int>(utf8Length(
                st.text.substr(lineStart, static_cast<size_t>(st.cursor) - lineStart))) + 1;
            right = std::to_string(line + 1) + ":" + std::to_string(col) + " · " +
                    std::to_string(utf8Length(st.text)) + " 字";
        }
        ui.text("md.status.pos")
            .position(x + w - 24.0f - 160.0f, y)
            .size(160.0f, 22.0f)
            .text(right)
            .fontSize(11.0f)
            .lineHeight(22.0f)
            .horizontalAlign(eui::HorizontalAlign::Right)
            .color(t.colors.textTertiary)
            .build();
    }

    // ---- 右栏：预览（components::markdown 渲染成 eui 节点树）----
    // 双栏态：编辑器单向驱动预览滚动（标题锚点 + 段内插值，§5.3）；
    // 仅预览态：手动滚动（previewOffset_ 记忆），进入时对齐编辑器位置。
    void drawPreviewColumn(eui::Ui& ui,
                           float bodyH,
                           float midW,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& componentTheme,
                           float gap) {
        (void)gap;
        // W9 修复：同左栏——column+overlay 兄弟双占位改单 stack
        ui.stack("md.preview").size(midW, bodyH).content([&] {
            ui.rect("md.preview.bg")
                .size(midW, bodyH)
                .color(t.colors.surface)
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
                .contentKey("md.preview.scroll." + currentPath_ + "." +
                            std::to_string(std::hash<std::string>{}(editorText_)));
            if (mode_ == ViewMode::Preview) {
                // 仅预览态允许手动滚动（双向：滚轮写回 previewOffset_）
                scroll.onChange([this](float v) { previewOffset_ = v; });
            }
            scroll.content([&](eui::Ui& inner, float contentWidth, float viewportHeight) {
                    (void)viewportHeight;
                    lastPvContentWidth_ = contentWidth;  // 供同步滚动 estimateHeight 用
                    components::markdown(inner, "md.preview.md")
                        .markdown(editorText_)
                        .width(contentWidth)
                        .wrapContentHeight()
                        .style(style)
                        .build();
                })
                .build();
        }).build();
    }

    // 双栏同步滚动（§5.3）：编辑器视口顶行 → 所在标题节 → 用
    // MarkdownBuilder::estimateHeight（public static，带文档级缓存）算该节
    // 在预览里的 y，段内按行数线性插值，再留 20% 视口余量。
    // 近似（ponytail）：锚点用标题而非 md4c 块（块无源码 offset，全块扫描器
    // 是 Phase 3 工作）；estimateHeight 本身是平均字宽估算，误差见设计文档 §9.2。
    float computeSyncOffset(eui::Ui& ui,
                            float pvW,
                            float pvH,
                            const components::MarkdownStyle& style) {
        if (editorText_.empty()) return 0.0f;
        const auto& st = ui.state<components::input_detail::InputModel::InputState>("md.editor.input");
        const float topLine = st.verticalScroll / kEditorLineHeight;
        const float width = lastPvContentWidth_ > 0.0f ? lastPvContentWidth_ : pvW;

        // 找 topLine 所在节：锚点 0 = 虚拟文首（line 0, y 0），其后每个标题一个锚点
        size_t a = 0;
        for (size_t i = 0; i < outline_.size(); ++i) {
            if (outline_[i].lineNo <= topLine) a = i + 1;
            else break;
        }
        const int aLine = (a == 0) ? 0 : outline_[a - 1].lineNo;
        const int bLine = (a < outline_.size())
            ? outline_[a].lineNo
            : std::max(countLines(editorText_), aLine + 1);
        const float yA = (a == 0)
            ? 0.0f
            : components::MarkdownBuilder::estimateHeight(
                  editorText_.substr(0, outline_[a - 1].byteOffset), width, style);
        const float yB = (a < outline_.size())
            ? components::MarkdownBuilder::estimateHeight(
                  editorText_.substr(0, outline_[a].byteOffset), width, style)
            : components::MarkdownBuilder::estimateHeight(editorText_, width, style);

        const float frac = (bLine > aLine) ? (topLine - aLine) / (bLine - aLine) : 0.0f;
        const float y = yA + frac * (yB - yA);
        return std::max(0.0f, y - pvH * 0.2f);
    }

    // ---- 新建笔记弹窗 ----
    void drawNewNoteDialog(eui::Ui& ui,
                           const eui::Screen& screen,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& componentTheme) {
        const float w = 460.0f;
        const float h = 220.0f;

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
                    .text("支持子目录，例如 inbox/ideas.md")
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(18.0f)
                    .color(t.colors.textTertiary)
                    .build();

                components::input(ui, "md.dialog.new.input")
                    .position(28.0f, 84.0f)
                    .size(w - 56.0f, 44.0f)
                    .value(newNoteName_)
                    .placeholder("笔记名，例如 周会纪要")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(componentTheme)
                    .onChange([this](const std::string& v) { newNoteName_ = v; })
                    .onEnter([this] { commitNewNote(); })
                    .build();

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
                    .textColor(eui::Color("#FFFFFF"))
                    .radius(t.sizes.radius)
                    .onClick([this] { commitNewNote(); })
                    .build();
            })
            .build();
    }

    // ---- 外部修改冲突弹窗（W8：当前笔记被外部程序改动时二选一）----
    // 注：EUI-NEO toast 无按钮能力，「重载/保留本地」这类选择交互用 dialog。
    void drawConflictDialog(eui::Ui& ui,
                            const eui::Screen& screen,
                            const core::theme::Tokens& t,
                            const components::theme::ThemeColorTokens& componentTheme) {
        const float w = 460.0f;
        const float h = 220.0f;

        components::dialog(ui, "md.dialog.conflict")
            .open(conflictOpen_)
            .screen(screen.width, screen.height)
            .size(w, h)
            .theme(componentTheme)
            .onOpenChange([this](bool v) { conflictOpen_ = v; })
            .content([&] {
                ui.text("md.dialog.conflict.title")
                    .position(28.0f, 24.0f)
                    .size(w - 56.0f, 28.0f)
                    .text("文件已被外部修改")
                    .fontSize(t.sizes.fontSizeLarge)
                    .lineHeight(28.0f)
                    .fontWeight(700)
                    .color(t.colors.textPrimary)
                    .build();

                ui.text("md.dialog.conflict.hint")
                    .position(28.0f, 56.0f)
                    .size(w - 56.0f, 36.0f)
                    .text("「" + currentPath_ + "」在磁盘上被其他程序修改，"
                          "与未保存的本地编辑冲突。")
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(18.0f)
                    .wrap()
                    .color(t.colors.textTertiary)
                    .build();

                components::button(ui, "md.dialog.conflict.reload")
                    .position(28.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("↻ 重载磁盘版本")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(componentTheme, true)
                    .textColor(eui::Color("#FFFFFF"))
                    .radius(t.sizes.radius)
                    .onClick([this] { resolveConflict(true); })
                    .build();

                components::button(ui, "md.dialog.conflict.keep")
                    .position(28.0f + (w - 68.0f) * 0.5f + 12.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("保留本地编辑")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(componentTheme, false)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] { resolveConflict(false); })
                    .build();
            })
            .build();
    }

    // ==================== 业务动作 ====================

    // ---- Toast 反馈（仿 claude_module）----
    void drawToast(eui::Ui& ui, const eui::Screen& screen,
                   const components::theme::ThemeColorTokens& themeTokens) {
        components::toast(ui, "md.toast")
            .theme(themeTokens)
            .screen(screen.width, screen.height)
            .visible(toastOpen_)
            .duration(3.5f)
            .title(toastTitle_)
            .message(toastMessage_)
            .onAutoDismiss([this] { toastOpen_ = false; })
            .onDismiss([this] { toastOpen_ = false; })
            .build();
    }

    void showToast(const std::string& title, const std::string& msg, bool isError) {
        toastTitle_ = isError ? ("⚠ " + title) : ("✓ " + title);
        toastMessage_ = msg;
        toastOpen_ = true;
    }

    // ---- Phase 1：三态切换 / 大纲 ----

    void setMode(ViewMode m) {
        if (m == mode_) return;
        mode_ = m;
        // 进预览态时把滚动位置对齐编辑器当前位置（compose 里延迟计算，
        // 因为需要预览列宽和主题样式）
        if (m == ViewMode::Preview) syncPreviewOnEnter_ = true;
        LOG_INFO("Markdown 视图切换: " + std::to_string(static_cast<int>(m)));
    }

    // Ctrl+Shift+P 循环切换三态。通道 = app 级 onKeyEvent → Router 派发到
    // 当前页模块（EUI-NEO InputBuilder 无逐组件按键 passthrough，那是
    // Phase 2 上游 PR；本快捷键不涉及编辑器文本变换，走全局通道无 undo 问题）
    bool onKeyEvent(const eui::KeyEvent& key) override {
        if (key.isDown() && key.modifiers.shortcut() && key.modifiers.shift &&
            key.key == eui::InputKey::P) {
            setMode(static_cast<ViewMode>((static_cast<int>(mode_) + 1) % 3));
            return true;
        }
        return false;
    }

    // 大纲当前节：光标行（InputState）所在的上一个标题；仅预览态无编辑器
    // 渲染时沿用上次跳转/光标位置（activeOutlineIdx_）
    int activeOutlineIndex(eui::Ui& ui) {
        if (outline_.empty()) return -1;
        if (mode_ == ViewMode::Preview) return activeOutlineIdx_;
        const auto& st = ui.state<components::input_detail::InputModel::InputState>("md.editor.input");
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
        auto& st = ui.state<components::input_detail::InputModel::InputState>("md.editor.input");
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
            newNoteOpen_ = true;
            newNoteName_.clear();
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
            return;
        }
        auto r = searchQuery_.empty() ? service_->listAll()
                                      : service_->search(searchQuery_);
        if (r.isFail()) {
            LOG_ERROR("刷新笔记列表失败: " + r.errorMessage());
            status_ = "列表失败：" + r.errorMessage();
            listError_ = r.errorMessage();
            notes_.clear();  // 不拿旧数据伪装成当前态
            return;
        }
        listError_.clear();
        notes_ = std::move(r.value());
    }

    void openNote(const std::string& relPath) {
        // 切换前先把上一篇的改动落盘
        saveEditor();
        if (!service_) return;

        auto r = service_->read(relPath);
        if (r.isFail()) {
            LOG_ERROR("读取笔记失败 " + relPath + ": " + r.errorMessage());
            status_ = "读取失败：" + r.errorMessage();
            showToast("读取失败", "「" + relPath + "」" + r.errorMessage(), true);
            return;
        }
        currentPath_ = relPath;
        editorText_ = r.value();
        lastSavedText_ = editorText_;
        editorDirty_ = false;
        outlineDirty_ = true;
        activeOutlineIdx_ = -1;
        syncCurrentMtime();  // W8：外部修改检测的 mtime 基准
        if (mode_ == ViewMode::Preview) syncPreviewOnEnter_ = true;
        status_ = "已打开 " + relPath;
        LOG_INFO("打开笔记: " + relPath);
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
            status_ = "保存失败：" + r.errorMessage();
            saveFailed_ = true;
            // 500ms → 1s → 2s → … 封顶 60s
            saveBackoffMs_ = (saveBackoffMs_ == 0)
                ? 500 : std::min(saveBackoffMs_ * 2, kMaxSaveBackoffMs);
            // toast 单实例：失败重试风暴期间只在无 toast 展示时弹一次
            if (!toastOpen_) {
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
        status_ = "已保存 " + currentPath_;
    }

    // ---- W8：文件变化自动感知（spec §8.1 承诺的 5 秒轮询）----

    // 每 5 秒一次（compose 每帧调用，内部用 steady_clock 节流）
    void pollDiskChangesIfDue() {
        if (!service_) return;
        const auto now = std::chrono::steady_clock::now();
        const auto sinceMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - lastPollAt_).count();
        if (sinceMs < kPollIntervalMs) return;
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
                editorText_.clear();
                lastSavedText_.clear();
                outlineDirty_ = true;
                activeOutlineIdx_ = -1;
                status_ = "笔记已被外部删除";
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
    void reloadCurrentNote(bool silent) {
        auto r = service_->read(currentPath_);
        if (r.isFail()) {
            if (!silent) {
                showToast("重载失败", r.errorMessage(), true);
            }
            return;
        }
        editorText_ = r.value();
        lastSavedText_ = editorText_;
        editorDirty_ = false;
        outlineDirty_ = true;
        activeOutlineIdx_ = -1;
        syncCurrentMtime();
        if (mode_ == ViewMode::Preview) syncPreviewOnEnter_ = true;
        status_ = "已重新加载 " + currentPath_;
        LOG_INFO("重载笔记: " + currentPath_);
    }

    void resolveConflict(bool reloadFromDisk) {
        conflictOpen_ = false;
        if (reloadFromDisk) {
            reloadCurrentNote(false);
            showToast("已重载", "已载入磁盘上的最新版本", false);
        } else {
            // 保留本地：把基准 mtime 推到磁盘当前值，未保存编辑走原有
            // 自动保存链路（400ms 防抖后覆盖写回）
            syncCurrentMtime();
            status_ = "保留本地编辑 " + currentPath_;
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

    void commitNewNote() {
        std::string name = newNoteName_;
        // 去掉首尾空白
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
            name.erase(name.begin());
        }
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
            name.pop_back();
        }
        if (name.empty()) {
            status_ = "笔记名不能为空";
            return;
        }
        if (!service_) return;

        // W8 路径安全校验（目录树依赖 relativePath 落在 notesDir 内；
        // MarkdownService::create 有同规则兜底，这里给出更友好的提示）
        if (!MarkdownService::isSafeNewNotePath(name)) {
            status_ = "笔记名不合法：不支持绝对路径、盘符或 ..";
            showToast("笔记名不合法",
                      "不允许绝对路径、盘符（如 C:）或 ..，请用相对路径，"
                      "例如 inbox/ideas.md", true);
            return;
        }

        // 规范化路径：无扩展名时补 .md（与 MarkdownService::create 一致）
        std::string relPath = name;
        if (relPath.size() < 3 ||
            relPath.compare(relPath.size() - 3, 3, ".md") != 0) {
            relPath += ".md";
        }

        // 已存在 → 直接打开，不报错
        if (service_->read(relPath).isOk()) {
            newNoteOpen_ = false;
            refreshList();
            openNote(relPath);
            status_ = "笔记已存在，已打开 " + relPath;
            return;
        }

        auto r = service_->create(relPath);
        if (r.isFail()) {
            LOG_ERROR("新建笔记失败 " + relPath + ": " + r.errorMessage());
            status_ = "新建失败：" + r.errorMessage();
            return;
        }
        newNoteOpen_ = false;
        refreshList();
        openNote(relPath);
        status_ = "已创建 " + relPath;
        LOG_INFO("新建笔记: " + relPath);
    }

    // ==================== 状态 ====================

    static constexpr int64_t kAutoSaveDelayMs = 400;
    static constexpr int64_t kMaxSaveBackoffMs = 60000;  // 保存失败退避封顶 60s
    static constexpr int64_t kPollIntervalMs = 5000;     // W8：文件变化轮询周期

    std::unique_ptr<MarkdownService> service_;
    std::vector<NoteMeta> notes_;      // 当前展示（全部或搜索结果）
    std::string searchQuery_;          // 搜索框内容
    std::string currentPath_;          // 打开的笔记（POSIX 相对路径）
    std::string editorText_;           // 编辑器缓冲
    std::string lastSavedText_;        // 上次落盘内容（用于跳过冗余写）
    bool editorDirty_ = false;
    bool saveFailed_ = false;          // 上次保存尝试失败（状态栏 ✕ 提示）
    std::chrono::steady_clock::time_point lastEditAt_{};
    std::chrono::steady_clock::time_point lastSaveAttemptAt_{};  // 退避计时基准
    int64_t saveBackoffMs_ = 0;        // 0 = 无退避；失败后指数增长
    std::string status_;               // 顶栏副标题里的状态提示
    std::string listError_;            // 左栏错误态（空 = 正常；W7 P1-1）
    bool newNoteOpen_ = false;
    std::string newNoteName_;

    // ---- Toast（EUI-NEO 单实例，复用一个状态位）----
    bool toastOpen_ = false;
    std::string toastTitle_;
    std::string toastMessage_;

    // ---- Phase 1 状态 ----
    ViewMode mode_ = ViewMode::Split;  // 三态视图（默认双栏）
    std::vector<MdHeading> outline_;   // 大纲（标题扫描结果）
    bool outlineDirty_ = true;
    int activeOutlineIdx_ = -1;        // 仅预览态用（无编辑器光标可读）
    float previewOffset_ = 0.0f;       // 仅预览态的手动滚动位置
    bool syncPreviewOnEnter_ = false;  // 刚切入预览态 → 对齐编辑器位置
    float lastPvContentWidth_ = 0.0f;  // 预览内容宽（estimateHeight 用，滞后一帧）
    std::time_t lastSavedAt_ = 0;      // 上次落盘时刻（状态栏 "✓ 已保存 HH:MM"）

    // ---- W8 状态 ----
    std::set<std::string> collapsedDirs_;             // 已收起的一级目录（内存态）
    bool outlineOpen_ = false;                        // 大纲浮动面板开关
    int64_t lastKnownMtime_ = 0;                      // 当前笔记已知磁盘 mtime（-1=已删除）
    bool conflictOpen_ = false;                       // 外部修改冲突弹窗
    std::chrono::steady_clock::time_point lastPollAt_{};  // 轮询节流基准
};

} // namespace module::markdown

extern "C" ::core::Module* create_markdown_module() {
    return new module::markdown::MarkdownModule();
}
