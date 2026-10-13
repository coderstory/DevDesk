// tests/test_modules/test_editor_font.cpp
// markdown 编辑器字号消费端无头验证：core::theme::editorFontSize() 写入后，
// 编辑器渲染文本行的 fontSize 随通道变化（不是编译期常量）。
// 跨文件前提：
//   - 设置值不在 SettingsConfig 本体，而在 core::theme 下沉通道（module_settings 与
//     module_markdown 之间不可互相 include），与 fontScale 同型。
//   - 字号是进程级全局量，进程内用例之间会串味，用例首尾都要写通道。
#include "doctest.h"
#include "test_support.h"

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

} // namespace

REGISTER_TEST(markdown_editor_uses_font_channel,
              "markdown: 编辑器字号读 core::theme 通道（spec缩水 B 消费端 headless）", {
    // 隔离笔记目录 + 临时 DB（与 test_theme_hierarchy 的 md 用例同款夹具）
    const auto dir = test_temp::tempPath("devdesk_efont_md");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    {
        std::ofstream f(dir / "note.md", std::ios::trunc);
        f << "# 标题\n正文\n";
    }
    test_support::setEnvVar("DEVDESK_NOTES_DIR", dir.string());

    const auto dbPath = test_temp::tempPath("devdesk_efont_md.db");
    std::filesystem::remove(dbPath, ec);
    auto r = core::Database::open(dbPath);
    DOCTEST_REQUIRE(r.isOk());
    auto db = std::make_unique<core::Database>(std::move(r.value()));
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

    // 必须点树里的 note.md 选中笔记，编辑器才渲染出输入区
    frame("ef1");
    const auto* fileRow = ui.find("md.tree.node.1.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    DOCTEST_REQUIRE(fileRow->onClick != nullptr);
    fileRow->onClick();
    frame("ef1b");
    DOCTEST_REQUIRE(ui.find(kEditorInputId) != nullptr);

    core::theme::setEditorFontSize(21.0f);
    frame("ef2");
    const auto* line0 = ui.find("md.editor.input.text.0");
    DOCTEST_REQUIRE(line0 != nullptr);
    DOCTEST_CHECK_EQ(line0->fontSize, 21.0f);

    // 第二个值证明取值跟通道走，不是写死的常量
    core::theme::setEditorFontSize(13.0f);
    frame("ef3");
    const auto* line0b = ui.find("md.editor.input.text.0");
    DOCTEST_REQUIRE(line0b != nullptr);
    DOCTEST_CHECK_EQ(line0b->fontSize, 13.0f);

    // 复位不可省：通道是进程级全局量，不复位会污染同进程后续用例
    core::theme::setEditorFontSize(15.0f);
    mod->onStop();
    core::registerAppDatabase(nullptr);
    test_support::setEnvVar("DEVDESK_NOTES_DIR", "");
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(dbPath, ec);
    return true;
})
