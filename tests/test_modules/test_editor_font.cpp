// tests/test_modules/test_editor_font.cpp
// spec 缩水 B（编辑器字号可配，spec §8.5）：markdown 消费端无头验证——
// core::theme::editorFontSize() 全局通道（settings 写入、markdown compose
// 读取）生效后，编辑器渲染文本的 fontSize 即通道值。
// 跨模块传递口径：SettingsConfig 本体在 module_settings（模块隔离，markdown
// 不可 include），运行时值下沉 core::theme，与 fontScale 通道同型。
#include "doctest.h"

#include "core/database.h"
#include "core/module_interface.h"
#include "core/theme.h"
#include "module_markdown/md_ids.h"

using module::markdown::kEditorInputId;

#include <eui/dsl.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

extern "C" ::core::Module* create_markdown_module();

namespace {

void setNotesDirEnv(const std::string& value) {
#if defined(_WIN32)
    ::_putenv_s("DEVDESK_NOTES_DIR", value.c_str());
#else
    if (value.empty()) {
        ::unsetenv("DEVDESK_NOTES_DIR");
    } else {
        ::setenv("DEVDESK_NOTES_DIR", value.c_str(), 1);
    }
#endif
}

} // namespace

REGISTER_TEST(markdown_editor_uses_font_channel,
              "markdown: 编辑器字号读 core::theme 通道（spec缩水 B 消费端 headless）", {
    // 隔离笔记目录 + 临时 DB（仿 test_theme_hierarchy 的 md 用例）
    const auto dir = test_temp::tempPath("devdesk_efont_md");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    {
        std::ofstream f(dir / "note.md", std::ios::trunc);
        f << "# 标题\n正文\n";
    }
    setNotesDirEnv(dir.string());

    const auto dbPath = test_temp::tempPath("devdesk_efont_md.db");
    std::filesystem::remove(dbPath, ec);
    auto r = core::Database::open(dbPath);
    DOCTEST_REQUIRE(r.isOk());
    auto db = std::make_unique<core::Database>(std::move(r.unsafeValue()));
    DOCTEST_REQUIRE(db->migrate().isOk());
    core::registerAppDatabase(&db->db());

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    auto frame = [&](const char* page) {
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
    };

    // 基线帧：点树里的 note.md 打开笔记（编辑器仅在选中有笔记时渲染，
    // 仿 test_markdown phase1 的真实 onClick 链路）
    frame("ef1");
    const auto* fileRow = ui.find("md.tree.node.1.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    DOCTEST_REQUIRE(fileRow->onClick != nullptr);
    fileRow->onClick();
    frame("ef1b");
    DOCTEST_REQUIRE(ui.find(kEditorInputId) != nullptr);

    // 通道设 21 → 重新 compose，渲染文本行 fontSize == 21
    core::theme::setEditorFontSize(21.0f);
    frame("ef2");
    const auto* line0 = ui.find("md.editor.input.text.0");
    DOCTEST_REQUIRE(line0 != nullptr);
    DOCTEST_CHECK_EQ(line0->fontSize, 21.0f);

    // 通道设 13 → 再 compose，取值随通道变化（不是编译期常量）
    core::theme::setEditorFontSize(13.0f);
    frame("ef3");
    const auto* line0b = ui.find("md.editor.input.text.0");
    DOCTEST_REQUIRE(line0b != nullptr);
    DOCTEST_CHECK_EQ(line0b->fontSize, 13.0f);

    core::theme::setEditorFontSize(15.0f);  // 恢复默认，防污染其他用例
    mod->onStop();
    core::registerAppDatabase(nullptr);
    setNotesDirEnv("");
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(dbPath, ec);
    return true;
})
