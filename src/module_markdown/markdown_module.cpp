// src/module_markdown/markdown_module.cpp
// Markdown 笔记本模块 —— 真实文件读写 + FTS5 搜索（3 栏 UI）
//
// 数据流：
//   onStart()  → 从 core::appDatabase() 拿共享连接 → MarkdownService → rescan()
//   compose()  → 左栏文件树（真实索引）/ 中栏编辑器（真实读文件、改即写盘）
//                / 右栏预览（components::markdown 把 Markdown 渲染成 eui 节点）
//
// 所有 I/O 都通过 MarkdownService，失败走 core::Result（不抛异常）。
#include "core/database.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/router.h"
#include "core/theme.h"
#include "markdown_service.h"

#include <eui/dsl.h>

#include <components/button.h>
#include <components/dialog.h>
#include <components/input.h>
#include <components/markdown.h>
#include <components/scrollview.h>
#include <components/theme.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace module::markdown {

namespace {

namespace fs = std::filesystem;

// 单行文件树条目（现在由 scrollView 负责纵向排布，所以不传 y）
void drawFileTreeItem(eui::Ui& ui,
                      const std::string& id,
                      float width,
                      const core::theme::Tokens& t,
                      const std::string& icon,
                      const std::string& name,
                      bool active,
                      std::function<void()> onClick) {
    ui.rect(id + ".row")
        .size(width, 32.0f)
        .color(active ? t.colors.primarySoftBg : t.colors.surface)
        .hoverColor(t.colors.hover)
        .onClick(std::move(onClick))
        .build();
    ui.text(id + ".icon")
        .position(12.0f, 0.0f)
        .size(20.0f, 32.0f)
        .text(icon)
        .fontSize(14.0f)
        .lineHeight(32.0f)
        .color(t.colors.textSecondary)
        .build();
    ui.text(id + ".name")
        .position(36.0f, 0.0f)
        .size(std::max(0.0f, width - 48.0f), 32.0f)
        .text(name)
        .fontSize(13.0f)
        .lineHeight(32.0f)
        .color(active ? t.colors.primary : t.colors.textPrimary)
        .fontWeight(active ? 700 : 400)
        .build();
}

// 把 "inbox/ideas.md" 显示成文件名 + 目录提示
std::string noteDisplayName(const NoteMeta& n) {
    return n.relativePath.filename().string();
}

std::string noteDisplayPath(const NoteMeta& n) {
    return n.relativePath.generic_string();
}

} // namespace

class MarkdownModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::Markdown; }
    std::string displayName() const override { return "Markdown 笔记本"; }
    std::string iconGlyph() const override { return "📝"; }

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
        saveEditor();
        LOG_INFO("Markdown 模块停止");
    }

    // ==================== 渲染 ====================

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        // 自动保存：停止输入 kAutoSaveDelayMs 后把编辑器内容真正写回磁盘
        flushEditorIfIdle();

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
        const float midW = (contentW - sideW * 2.0f - gap * 2.0f) / 2.0f;
        const float bodyH = contentH - 64.0f - t.sizes.spacingLarge;

        ui.column("md.root")
            .size(screen.width, screen.height)
            .padding(pad)
            .gap(t.sizes.spacingLarge)
            .content([&] {
                drawHeader(ui, contentW, contentH, bodyH, t);
                drawBody(ui, contentW, bodyH, sideW, midW, gap, t, componentTheme);
            })
            .build();

        // 弹窗要画在最上层（dialog 自带 zIndex）
        drawNewNoteDialog(ui, screen, t, componentTheme);
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
                ui.column("md.title")
                    .size(contentW - 320.0f, 64.0f)
                    .gap(2.0f)
                    .content([&] {
                        ui.text("md.h1")
                            .size(contentW - 320.0f, 32.0f)
                            .text("Markdown 笔记本")
                            .fontSize(t.sizes.fontSizeTitle)
                            .lineHeight(32.0f)
                            .fontWeight(700)
                            .color(t.colors.textPrimary)
                            .build();
                        ui.text("md.subtitle")
                            .size(contentW - 320.0f, 18.0f)
                            .text(subtitle)
                            .fontSize(t.sizes.fontSizeSmall)
                            .lineHeight(18.0f)
                            .color(t.colors.textSecondary)
                            .build();
                    })
                    .build();

                ui.row("md.actions")
                    .size(320.0f, 40.0f)
                    .justifyContent(eui::Align::END)
                    .alignItems(eui::Align::CENTER)
                    .gap(t.sizes.spacingSmall)
                    .content([&] {
                        components::button(ui, "md.btn.rescan")
                            .size(84.0f, 40.0f)
                            .text("↻ 重扫")
                            .fontSize(t.sizes.fontSizeBody)
                            .theme(componentTheme, false)
                            .textColor(t.colors.textSecondary)
                            .radius(t.sizes.radius)
                            .border(1.0f, t.colors.borderDefault)
                            .onClick([this] { doRescan(); })
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

    // ---------- 三栏主区 ----------
    void drawBody(eui::Ui& ui,
                  float contentW,
                  float bodyH,
                  float sideW,
                  float midW,
                  float gap,
                  const core::theme::Tokens& t,
                  const components::theme::ThemeColorTokens& componentTheme) {
        ui.row("md.body")
            .size(contentW, bodyH)
            .gap(gap)
            .content([&] {
                drawTreeColumn(ui, bodyH, sideW, t, componentTheme);
                drawEditorColumn(ui, bodyH, midW, t, componentTheme);
                drawPreviewColumn(ui, bodyH, midW, t, componentTheme, gap);
            })
            .build();
    }

    // ---- 左栏：搜索 + 文件列表 ----
    void drawTreeColumn(eui::Ui& ui,
                        float bodyH,
                        float sideW,
                        const core::theme::Tokens& t,
                        const components::theme::ThemeColorTokens& componentTheme) {
        ui.column("md.tree").size(sideW, bodyH).gap(0.0f).content([&] {
            ui.rect("md.tree.bg")
                .size(sideW, bodyH)
                .color(t.colors.surface)
                .radius(t.sizes.radiusLarge)
                .border(1.0f, t.colors.borderDefault)
                .build();
        }).build();

        ui.stack("md.tree.overlay").size(sideW, bodyH).content([&] {
            // 搜索框：真实调用 FTS5
            components::input(ui, "md.search")
                .position(12.0f, 12.0f)
                .size(sideW - 24.0f, 36.0f)
                .value(searchQuery_)
                .placeholder("🔍 搜索笔记…")
                .fontSize(13.0f)
                .inset(10.0f)
                .theme(componentTheme)
                .onChange([this](const std::string& v) {
                    searchQuery_ = v;
                    refreshList();
                })
                .build();

            const float listY = 60.0f;
            const float listH = std::max(0.0f, bodyH - listY - 60.0f);

            if (notes_.empty()) {
                ui.text("md.tree.empty")
                    .position(16.0f, listY + 8.0f)
                    .size(sideW - 32.0f, 40.0f)
                    .text(searchQuery_.empty() ? "还没有笔记\n点下面「+ 新建」开始"
                                               : "没有匹配的笔记")
                    .fontSize(12.0f)
                    .lineHeight(18.0f)
                    .wrap()
                    .color(t.colors.textTertiary)
                    .build();
            } else {
                drawNoteList(ui, listY, listH, sideW, t, componentTheme);
            }

            // 底部「+ 新建」
            const float newBtnY = bodyH - 52.0f;
            components::button(ui, "md.tree.new")
                .position(12.0f, newBtnY)
                .size(sideW - 24.0f, 36.0f)
                .text("+ 新建")
                .fontSize(13.0f)
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
        components::scrollView(ui, "md.tree.scroll")
            .position(8.0f, listY)
            .size(sideW - 16.0f, listH)
            .gap(0.0f)
            .step(40.0f)
            .theme(componentTheme)
            .contentKey("md.tree.scroll." + std::to_string(notes_.size()))
            .content([&](eui::Ui& inner, float contentWidth, float viewportHeight) {
                (void)viewportHeight;
                const float w = std::min(contentWidth, rowW);
                for (size_t i = 0; i < notes_.size(); ++i) {
                    const NoteMeta& n = notes_[i];
                    const std::string path = noteDisplayPath(n);
                    const std::string id = "md.tree.item." + std::to_string(i);
                    drawFileTreeItem(inner, id, w, t, "📄", noteDisplayName(n),
                                     path == currentPath_,
                                     [this, path] { openNote(path); });
                }
            })
            .build();
    }

    // ---- 中栏：编辑器（真实读 / 写文件）----
    void drawEditorColumn(eui::Ui& ui,
                          float bodyH,
                          float midW,
                          const core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& componentTheme) {
        ui.column("md.editor").size(midW, bodyH).gap(0.0f).content([&] {
            ui.rect("md.editor.bg")
                .size(midW, bodyH)
                .color(t.colors.surface)
                .radius(t.sizes.radiusLarge)
                .border(1.0f, t.colors.borderDefault)
                .build();
        }).build();

        ui.stack("md.editor.overlay").size(midW, bodyH).content([&] {
            const bool hasNote = !currentPath_.empty();
            ui.text("md.editor.filename")
                .position(24.0f, 18.0f)
                .size(midW - 48.0f, 18.0f)
                .text(hasNote ? currentPath_ + " · 编辑中（自动保存）"
                              : "未选择笔记")
                .fontSize(11.0f)
                .lineHeight(18.0f)
                .color(t.colors.textTertiary)
                .build();

            if (!hasNote) {
                ui.text("md.editor.hint")
                    .position(24.0f, 50.0f)
                    .size(midW - 48.0f, 40.0f)
                    .text("从左栏选一篇笔记开始编辑")
                    .fontSize(14.0f)
                    .lineHeight(20.0f)
                    .color(t.colors.textTertiary)
                    .build();
                return;
            }

            const float edX = 16.0f;
            const float edY = 46.0f;
            const float edW = midW - 32.0f;
            const float edH = std::max(0.0f, bodyH - edY - 16.0f);

            components::input(ui, "md.editor.input")
                .position(edX, edY)
                .size(edW, edH)
                .value(editorText_)
                .placeholder("在这里写 Markdown…")
                .multiline(true)
                .scrollbar(true)
                .fontSize(14.0f)
                .inset(10.0f)
                .theme(componentTheme)
                .onChange([this](const std::string& v) {
                    // 每次输入都更新内存缓冲；落盘走防抖（见 flushEditorIfIdle）
                    editorText_ = v;
                    editorDirty_ = true;
                    lastEditAt_ = std::chrono::steady_clock::now();
                })
                .build();
        }).build();
    }

    // ---- 右栏：预览（components::markdown 渲染成 eui 节点树）----
    void drawPreviewColumn(eui::Ui& ui,
                           float bodyH,
                           float midW,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& componentTheme,
                           float gap) {
        (void)gap;
        ui.column("md.preview").size(midW, bodyH).gap(0.0f).content([&] {
            ui.rect("md.preview.bg")
                .size(midW, bodyH)
                .color(t.colors.surface)
                .radius(t.sizes.radiusLarge)
                .border(1.0f, t.colors.borderDefault)
                .build();
        }).build();

        ui.stack("md.preview.overlay").size(midW, bodyH).content([&] {
            ui.text("md.preview.filename")
                .position(24.0f, 18.0f)
                .size(midW - 48.0f, 18.0f)
                .text(currentPath_.empty() ? "预览" : currentPath_ + " · 预览")
                .fontSize(11.0f)
                .lineHeight(18.0f)
                .color(t.colors.textTertiary)
                .build();

            const float pvX = 20.0f;
            const float pvY = 46.0f;
            const float pvW = midW - 40.0f;
            const float pvH = std::max(0.0f, bodyH - pvY - 16.0f);

            if (currentPath_.empty()) {
                ui.text("md.preview.hint")
                    .position(pvX, pvY)
                    .size(pvW, 40.0f)
                    .text("选择笔记后在这里看到渲染结果")
                    .fontSize(14.0f)
                    .lineHeight(20.0f)
                    .color(t.colors.textTertiary)
                    .build();
                return;
            }

            components::scrollView(ui, "md.preview.scroll")
                .position(pvX, pvY)
                .size(pvW, pvH)
                .gap(8.0f)
                .step(56.0f)
                .theme(componentTheme)
                .contentKey("md.preview.scroll." + currentPath_ + "." +
                            std::to_string(editorText_.size()))
                .content([&](eui::Ui& inner, float contentWidth, float viewportHeight) {
                    (void)viewportHeight;
                    components::MarkdownStyle style(componentTheme);
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
                    .fontSize(20.0f)
                    .lineHeight(28.0f)
                    .fontWeight(700)
                    .color(t.colors.textPrimary)
                    .build();

                ui.text("md.dialog.new.hint")
                    .position(28.0f, 56.0f)
                    .size(w - 56.0f, 18.0f)
                    .text("支持子目录，例如 inbox/ideas.md")
                    .fontSize(12.0f)
                    .lineHeight(18.0f)
                    .color(t.colors.textTertiary)
                    .build();

                components::input(ui, "md.dialog.new.input")
                    .position(28.0f, 84.0f)
                    .size(w - 56.0f, 44.0f)
                    .value(newNoteName_)
                    .placeholder("笔记名，例如 周会纪要")
                    .fontSize(15.0f)
                    .theme(componentTheme)
                    .onChange([this](const std::string& v) { newNoteName_ = v; })
                    .onEnter([this] { commitNewNote(); })
                    .build();

                components::button(ui, "md.dialog.new.cancel")
                    .position(28.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("取消")
                    .fontSize(14.0f)
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
                    .fontSize(14.0f)
                    .theme(componentTheme, true)
                    .textColor(eui::Color("#FFFFFF"))
                    .radius(t.sizes.radius)
                    .onClick([this] { commitNewNote(); })
                    .build();
            })
            .build();
    }

    // ==================== 业务动作 ====================

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
            return;
        }
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
            return;
        }
        currentPath_ = relPath;
        editorText_ = r.value();
        lastSavedText_ = editorText_;
        editorDirty_ = false;
        status_ = "已打开 " + relPath;
        LOG_INFO("打开笔记: " + relPath);
    }

    // 编辑器里还有未落盘的改动时写回文件 + 索引（每帧调用，自带防抖）
    void flushEditorIfIdle() {
        if (!editorDirty_) return;
        const auto now = std::chrono::steady_clock::now();
        const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - lastEditAt_).count();
        if (idleMs >= kAutoSaveDelayMs) {
            saveEditor();
        }
    }

    void saveEditor() {
        if (!service_ || currentPath_.empty()) return;
        if (editorText_ == lastSavedText_) {
            editorDirty_ = false;
            return;
        }
        auto r = service_->write(currentPath_, editorText_);
        if (r.isFail()) {
            LOG_ERROR("保存笔记失败 " + currentPath_ + ": " + r.errorMessage());
            status_ = "保存失败：" + r.errorMessage();
            return;
        }
        lastSavedText_ = editorText_;
        editorDirty_ = false;
        status_ = "已保存 " + currentPath_;
    }

    void doRescan() {
        if (!service_) return;
        auto r = service_->rescan();
        if (r.isFail()) {
            LOG_ERROR("重扫失败: " + r.errorMessage());
            status_ = "重扫失败：" + r.errorMessage();
            return;
        }
        refreshList();
        status_ = "重扫完成";
        LOG_INFO("手动重扫完成");
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

    std::unique_ptr<MarkdownService> service_;
    std::vector<NoteMeta> notes_;      // 当前展示（全部或搜索结果）
    std::string searchQuery_;          // 搜索框内容
    std::string currentPath_;          // 打开的笔记（POSIX 相对路径）
    std::string editorText_;           // 编辑器缓冲
    std::string lastSavedText_;        // 上次落盘内容（用于跳过冗余写）
    bool editorDirty_ = false;
    std::chrono::steady_clock::time_point lastEditAt_{};
    std::string status_;               // 顶栏副标题里的状态提示
    bool newNoteOpen_ = false;
    std::string newNoteName_;
};

} // namespace module::markdown

extern "C" ::core::Module* create_markdown_module() {
    return new module::markdown::MarkdownModule();
}
