// src/module_markdown/markdown_module.cpp
// Markdown 笔记本模块 - 完整 UI 实现
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/theme.h"

#include <eui/dsl.h>

#include <string>

namespace module::markdown {

namespace {

void drawFileTreeItem(eui::Ui& ui,
                      const std::string& id,
                      float y,
                      float width,
                      const core::theme::Tokens& t,
                      const std::string& icon,
                      const std::string& name,
                      bool active) {
    ui.rect(id + ".row")
        .position(0.0f, y)
        .size(width, 32.0f)
        .color(active ? t.colors.primarySoftBg : t.colors.surface)
        .build();
    ui.text(id + ".icon")
        .position(12.0f, y)
        .size(20.0f, 32.0f)
        .text(icon)
        .fontSize(14.0f)
        .lineHeight(32.0f)
        .color(t.colors.textSecondary)
        .build();
    ui.text(id + ".name")
        .position(36.0f, y)
        .size(width - 48.0f, 32.0f)
        .text(name)
        .fontSize(13.0f)
        .lineHeight(32.0f)
        .color(active ? t.colors.primary : t.colors.textPrimary)
        .fontWeight(active ? 700 : 400)
        .build();
}

void drawTagChip(eui::Ui& ui,
                 const std::string& id,
                 float x,
                 float y,
                 const core::theme::Tokens& t,
                 const std::string& text) {
    ui.rect(id + ".bg")
        .position(x, y)
        .size(56.0f, 22.0f)
        .color(t.colors.surfaceElevated)
        .radius(t.sizes.radiusPill)
        .build();
    ui.text(id + ".text")
        .position(x, y)
        .size(56.0f, 22.0f)
        .text(text)
        .fontSize(11.0f)
        .lineHeight(22.0f)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .color(t.colors.textSecondary)
        .build();
}

} // namespace

class MarkdownModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::Markdown; }
    std::string displayName() const override { return "Markdown 笔记本"; }
    std::string iconGlyph() const override { return "📝"; }

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        const auto& t = core::theme::current();
        const float pad = t.sizes.spacingXXLarge;        // 40
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
                // ========== 顶部 Header ==========
                ui.row("md.header")
                    .size(contentW, 64.0f)
                    .alignItems(eui::Align::CENTER)
                    .content([&] {
                        ui.column("md.title")
                            .size(contentW - 280.0f, 64.0f)
                            .gap(2.0f)
                            .content([&] {
                                ui.text("md.h1")
                                    .size(contentW - 280.0f, 32.0f)
                                    .text("Markdown 笔记本")
                                    .fontSize(t.sizes.fontSizeTitle)
                                    .lineHeight(32.0f)
                                    .fontWeight(700)
                                    .color(t.colors.textPrimary)
                                    .build();
                                ui.text("md.subtitle")
                                    .size(contentW - 280.0f, 18.0f)
                                    .text("本地化的笔记系统，文件可直接用外部编辑器打开")
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .lineHeight(18.0f)
                                    .color(t.colors.textSecondary)
                                    .build();
                            })
                            .build();

                        ui.row("md.actions")
                            .size(280.0f, 40.0f)
                            .justifyContent(eui::Align::END)
                            .alignItems(eui::Align::CENTER)
                            .gap(t.sizes.spacingSmall)
                            .content([&] {
                                ui.rect("md.btn.rescan.bg")
                                    .size(72.0f, 40.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                                ui.text("md.btn.rescan.text")
                                    .size(72.0f, 40.0f)
                                    .text("↻ 重扫")
                                    .fontSize(t.sizes.fontSizeBody)
                                    .lineHeight(40.0f)
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .color(t.colors.textSecondary)
                                    .build();

                                ui.rect("md.btn.settings.bg")
                                    .size(48.0f, 40.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                                ui.text("md.btn.settings.text")
                                    .size(48.0f, 40.0f)
                                    .text("⚙")
                                    .fontSize(t.sizes.fontSizeBody)
                                    .lineHeight(40.0f)
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .color(t.colors.textSecondary)
                                    .build();

                                ui.rect("md.btn.new.bg")
                                    .size(124.0f, 40.0f)
                                    .color(t.colors.primary)
                                    .radius(t.sizes.radius)
                                    .build();
                                ui.text("md.btn.new.text")
                                    .size(124.0f, 40.0f)
                                    .text("+ 新建笔记")
                                    .fontSize(t.sizes.fontSizeBody)
                                    .lineHeight(40.0f)
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .color(eui::Color("#FFFFFF"))
                                    .build();
                            })
                            .build();
                    })
                    .build();

                // ========== 三栏主区 (row 内嵌 3 个 column) ==========
                ui.row("md.body")
                    .size(contentW, bodyH)
                    .gap(gap)
                    .content([&] {
                        // ---- 左栏：文件树 ----
                        ui.column("md.tree")
                            .size(sideW, bodyH)
                            .gap(0.0f)
                            .content([&] {
                                ui.rect("md.tree.bg")
                                    .size(sideW, bodyH)
                                    .color(t.colors.surface)
                                    .radius(t.sizes.radiusLarge)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                            })
                            .build();

                        // 文件树使用 stack 浮在上面（绝对定位）
                        ui.stack("md.tree.overlay")
                            .size(sideW, bodyH)
                            .content([&] {
                                // 搜索框
                                ui.rect("md.search.bg")
                                    .position(12.0f, 12.0f)
                                    .size(sideW - 24.0f, 36.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radiusSmall)
                                    .border(1.0f, t.colors.borderMuted)
                                    .build();
                                ui.text("md.search.placeholder")
                                    .position(20.0f, 12.0f)
                                    .size(sideW - 40.0f, 36.0f)
                                    .text("🔍 搜索")
                                    .fontSize(13.0f)
                                    .lineHeight(36.0f)
                                    .color(t.colors.textTertiary)
                                    .build();

                                // 文件列表
                                const float listY = 60.0f;
                                drawFileTreeItem(ui, "md.tree.1", listY, sideW, t, "📄", "设计稿v2.md", true);
                                drawFileTreeItem(ui, "md.tree.2", listY + 32.0f, sideW, t, "📄", "周会纪要.md", false);
                                drawFileTreeItem(ui, "md.tree.3", listY + 64.0f, sideW, t, "📄", "bug 分析.md", false);
                                drawFileTreeItem(ui, "md.tree.dir", listY + 96.0f, sideW, t, "▾ 📁", "inbox", false);
                                drawFileTreeItem(ui, "md.tree.4", listY + 132.0f, sideW, t, "  ", "  · ideas.md", false);
                                drawFileTreeItem(ui, "md.tree.5", listY + 164.0f, sideW, t, "  ", "  · todo.md", false);

                                // 底部「+ 新建」按钮
                                const float newBtnY = bodyH - 84.0f;
                                ui.rect("md.tree.new.bg")
                                    .position(12.0f, newBtnY)
                                    .size(sideW - 24.0f, 36.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                                ui.text("md.tree.new.text")
                                    .position(12.0f, newBtnY)
                                    .size(sideW - 24.0f, 36.0f)
                                    .text("+ 新建")
                                    .fontSize(13.0f)
                                    .lineHeight(36.0f)
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .color(t.colors.textPrimary)
                                    .build();

                                // 标签区域
                                ui.text("md.tree.tag.label")
                                    .position(16.0f, bodyH - 36.0f)
                                    .size(48.0f, 16.0f)
                                    .text("标签")
                                    .fontSize(11.0f)
                                    .lineHeight(16.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                                drawTagChip(ui, "md.tag.work", 16.0f, bodyH - 16.0f, t, "#工作");
                            })
                            .build();

                        // ---- 中栏：编辑器 ----
                        ui.column("md.editor")
                            .size(midW, bodyH)
                            .gap(0.0f)
                            .content([&] {
                                ui.rect("md.editor.bg")
                                    .size(midW, bodyH)
                                    .color(t.colors.surface)
                                    .radius(t.sizes.radiusLarge)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                            })
                            .build();

                        ui.stack("md.editor.overlay")
                            .size(midW, bodyH)
                            .content([&] {
                                // 顶部文件名
                                ui.text("md.editor.filename")
                                    .position(24.0f, 18.0f)
                                    .size(midW - 48.0f, 18.0f)
                                    .text("设计稿v2.md")
                                    .fontSize(11.0f)
                                    .lineHeight(18.0f)
                                    .color(t.colors.textTertiary)
                                    .build();

                                // 编辑器内容
                                const float edX = 24.0f;
                                const float edY = 50.0f;
                                const float edW = midW - 48.0f;

                                const char* lines[] = {
                                    "# 你好",
                                    "",
                                    "这是**Markdown**段落。",
                                    "支持 `inline code` 与代码块：",
                                    "",
                                    "## 二级标题",
                                    "",
                                    "- 列表项 1",
                                    "- 列表项 2",
                                    "",
                                    "```cpp",
                                    "#include <iostream>",
                                    "int main() { return 0; }",
                                    "```",
                                };
                                const int lineCount = sizeof(lines) / sizeof(lines[0]);
                                for (int i = 0; i < lineCount; ++i) {
                                    ui.text("md.editor.line." + std::to_string(i))
                                        .position(edX, edY + 32.0f + static_cast<float>(i) * 22.0f)
                                        .size(edW, 20.0f)
                                        .text(lines[i])
                                        .fontSize(14.0f)
                                        .lineHeight(20.0f)
                                        .color(t.colors.textPrimary)
                                        .build();
                                }
                            })
                            .build();

                        // ---- 右栏：预览 ----
                        ui.column("md.preview")
                            .size(midW, bodyH)
                            .gap(0.0f)
                            .content([&] {
                                ui.rect("md.preview.bg")
                                    .size(midW, bodyH)
                                    .color(t.colors.surface)
                                    .radius(t.sizes.radiusLarge)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                            })
                            .build();

                        ui.stack("md.preview.overlay")
                            .size(midW, bodyH)
                            .content([&] {
                                ui.text("md.preview.filename")
                                    .position(24.0f, 18.0f)
                                    .size(midW - 48.0f, 18.0f)
                                    .text("设计稿v2.md (预览)")
                                    .fontSize(11.0f)
                                    .lineHeight(18.0f)
                                    .color(t.colors.textTertiary)
                                    .build();

                                const float pvX = 24.0f;
                                const float pvY = 50.0f;
                                const float pvW = midW - 48.0f;

                                ui.text("md.preview.h1")
                                    .position(pvX, pvY)
                                    .size(pvW, 36.0f)
                                    .text("你好")
                                    .fontSize(28.0f)
                                    .lineHeight(36.0f)
                                    .fontWeight(700)
                                    .color(t.colors.textPrimary)
                                    .build();
                                ui.rect("md.preview.hr")
                                    .position(pvX, pvY + 44.0f)
                                    .size(pvW, 1.0f)
                                    .color(t.colors.borderDefault)
                                    .build();

                                ui.text("md.preview.p1")
                                    .position(pvX, pvY + 56.0f)
                                    .size(pvW, 22.0f)
                                    .text("这是 Markdown 段落。")
                                    .fontSize(15.0f)
                                    .lineHeight(22.0f)
                                    .color(t.colors.textPrimary)
                                    .build();

                                ui.text("md.preview.p2")
                                    .position(pvX, pvY + 82.0f)
                                    .size(pvW, 22.0f)
                                    .text("支持 inline code 与代码块：")
                                    .fontSize(15.0f)
                                    .lineHeight(22.0f)
                                    .color(t.colors.textPrimary)
                                    .build();

                                ui.text("md.preview.h2")
                                    .position(pvX, pvY + 114.0f)
                                    .size(pvW, 28.0f)
                                    .text("二级标题")
                                    .fontSize(20.0f)
                                    .lineHeight(28.0f)
                                    .fontWeight(600)
                                    .color(t.colors.textPrimary)
                                    .build();

                                ui.text("md.preview.li1")
                                    .position(pvX + 8.0f, pvY + 152.0f)
                                    .size(pvW, 22.0f)
                                    .text("• 列表项 1")
                                    .fontSize(15.0f)
                                    .lineHeight(22.0f)
                                    .color(t.colors.textPrimary)
                                    .build();
                                ui.text("md.preview.li2")
                                    .position(pvX + 8.0f, pvY + 176.0f)
                                    .size(pvW, 22.0f)
                                    .text("• 列表项 2")
                                    .fontSize(15.0f)
                                    .lineHeight(22.0f)
                                    .color(t.colors.textPrimary)
                                    .build();

                                ui.rect("md.preview.code.bg")
                                    .position(pvX, pvY + 210.0f)
                                    .size(pvW, 96.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radiusSmall)
                                    .border(1.0f, t.colors.borderMuted)
                                    .build();
                                ui.text("md.preview.code.l1")
                                    .position(pvX + 12.0f, pvY + 218.0f)
                                    .size(pvW - 24.0f, 18.0f)
                                    .text("#include <iostream>")
                                    .fontSize(12.0f)
                                    .lineHeight(18.0f)
                                    .color(t.colors.textPrimary)
                                    .build();
                                ui.text("md.preview.code.l2")
                                    .position(pvX + 12.0f, pvY + 238.0f)
                                    .size(pvW - 24.0f, 18.0f)
                                    .text("int main() {")
                                    .fontSize(12.0f)
                                    .lineHeight(18.0f)
                                    .color(t.colors.textPrimary)
                                    .build();
                                ui.text("md.preview.code.l3")
                                    .position(pvX + 12.0f, pvY + 258.0f)
                                    .size(pvW - 24.0f, 18.0f)
                                    .text("    return 0;")
                                    .fontSize(12.0f)
                                    .lineHeight(18.0f)
                                    .color(t.colors.textPrimary)
                                    .build();
                                ui.text("md.preview.code.l4")
                                    .position(pvX + 12.0f, pvY + 278.0f)
                                    .size(pvW - 24.0f, 18.0f)
                                    .text("}")
                                    .fontSize(12.0f)
                                    .lineHeight(18.0f)
                                    .color(t.colors.textPrimary)
                                    .build();
                            })
                            .build();
                    })
                    .build();
            })
            .build();

        LOG_DEBUG("Markdown 模块 compose 完成");
    }

    void onStart() override { LOG_INFO("Markdown 模块启动"); }
    void onStop() override { LOG_INFO("Markdown 模块停止"); }
};

} // namespace module::markdown

extern "C" ::core::Module* create_markdown_module() {
    return new module::markdown::MarkdownModule();
}