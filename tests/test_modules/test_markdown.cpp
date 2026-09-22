// tests/test_modules/test_markdown.cpp
// - md_line_scanner 纯函数单测（Phase 1 大纲 / 同步滚动底座）
// - MarkdownModule Phase 1 UI 无头 compose 冒烟（三态切换 / 大纲 / 状态栏）
// 接线证据 = 代码路径 + 本无头 compose 测试 + 启动冒烟；真实鼠标点验留给用户人工
#include "doctest.h"
#include "core/database.h"
#include "core/module_interface.h"
#include "module_markdown/md_line_scanner.h"

#include <components/input.h>
#include <eui/dsl.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

extern "C" ::core::Module* create_markdown_module();

namespace fs = std::filesystem;

REGISTER_TEST(module_pageid, "markdown: PageId", {
    DOCTEST_CHECK(core::PageId::Markdown != core::PageId::Todo);
    DOCTEST_CHECK(core::PageId::Markdown != core::PageId::Home);
    return true;
})

REGISTER_TEST(module_pagid_to_string, "markdown: pageIdToString", {
    DOCTEST_CHECK_EQ(core::pageIdToString(core::PageId::Home), std::string("Home"));
    DOCTEST_CHECK_EQ(core::pageIdToString(core::PageId::Markdown), std::string("Markdown"));
    DOCTEST_CHECK_EQ(core::pageIdToString(core::PageId::Todo), std::string("Todo"));
    DOCTEST_CHECK_EQ(core::pageIdToString(core::PageId::ClaudeProxy), std::string("ClaudeProxy"));
    DOCTEST_CHECK_EQ(core::pageIdToString(core::PageId::Settings), std::string("Settings"));
    return true;
})

// ==================== 行分类扫描器（Phase 1：大纲 / 同步滚动底座） ====================

using module::markdown::scanHeadings;

REGISTER_TEST(md_scanner_atx_levels, "md_line_scanner: ATX 标题层级与标题文本", {
    const auto hs = scanHeadings("# 一级\n## 二级\n### 三级\n###### 六级\n####### 七级不是标题\n");
    //####### 7 个 # 超过 6 级 → 不是标题
    DOCTEST_CHECK_EQ(hs.size(), static_cast<size_t>(4));
    DOCTEST_CHECK_EQ(hs[0].level, 1);
    DOCTEST_CHECK_EQ(hs[0].lineNo, 0);
    DOCTEST_CHECK_EQ(hs[0].title, std::string("一级"));
    DOCTEST_CHECK_EQ(hs[1].level, 2);
    DOCTEST_CHECK_EQ(hs[1].title, std::string("二级"));
    DOCTEST_CHECK_EQ(hs[2].level, 3);
    DOCTEST_CHECK_EQ(hs[3].level, 6);
    DOCTEST_CHECK_EQ(hs[3].title, std::string("六级"));
    return true;
})

REGISTER_TEST(md_scanner_no_space_after_hashes, "md_line_scanner: # 后无空格不是标题", {
    const auto hs = scanHeadings("#没有空格\n#teardown\n# 有空格\n");
    DOCTEST_CHECK_EQ(hs.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(hs[0].title, std::string("有空格"));
    DOCTEST_CHECK_EQ(hs[0].lineNo, 2);
    return true;
})

REGISTER_TEST(md_scanner_code_fence, "md_line_scanner: 围栏代码块内不算标题", {
    const std::string text = "# 真标题\n```cpp\n# include 不是标题\nvoid foo();\n```\n## 围栏后恢复\n";
    const auto hs = scanHeadings(text);
    DOCTEST_CHECK_EQ(hs.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(hs[0].title, std::string("真标题"));
    DOCTEST_CHECK_EQ(hs[1].title, std::string("围栏后恢复"));
    DOCTEST_CHECK_EQ(hs[1].lineNo, 5);
    return true;
})

REGISTER_TEST(md_scanner_tilde_fence_pairs, "md_line_scanner: ~~~ 与 ``` 各自配对", {
    const std::string text = "~~~\n# 波浪围栏内\n```\n# 反引号在波浪围栏内仍不算标题\n~~~\n# 出来了\n";
    const auto hs = scanHeadings(text);
    DOCTEST_CHECK_EQ(hs.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(hs[0].title, std::string("出来了"));
    return true;
})

REGISTER_TEST(md_scanner_crlf_and_utf8, "md_line_scanner: CRLF 与中文标题", {
    const std::string text = "# 项目周报\r\n正文\r\n## 本周进展\r\n";
    const auto hs = scanHeadings(text);
    DOCTEST_CHECK_EQ(hs.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(hs[0].title, std::string("项目周报"));
    DOCTEST_CHECK_EQ(hs[0].byteOffset, static_cast<size_t>(0));
    DOCTEST_CHECK_EQ(hs[1].title, std::string("本周进展"));
    // 行首偏移："# 项目周报\r\n"(16) + "正文\r\n"(8) = 24
    DOCTEST_CHECK_EQ(hs[1].byteOffset, static_cast<size_t>(24));
    return true;
})

REGISTER_TEST(md_scanner_empty_and_bare_hash, "md_line_scanner: 空文本 / 裸 # / 文尾无换行", {
    DOCTEST_CHECK_EQ(scanHeadings("").size(), static_cast<size_t>(0));
    // 裸 # = 空标题（CommonMark 语义）
    const auto bare = scanHeadings("#");
    DOCTEST_CHECK_EQ(bare.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(bare[0].level, 1);
    DOCTEST_CHECK_EQ(bare[0].title, std::string(""));
    // 文尾无换行的最后一行也要扫到
    const auto tail = scanHeadings("正文\n## 尾行标题");
    DOCTEST_CHECK_EQ(tail.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(tail[0].lineNo, 1);
    return true;
})

REGISTER_TEST(md_scanner_line_utils, "md_line_scanner: 行号/偏移换算 + UTF-8 计数", {
    const std::string text = "aa\nbb\ncc";  // 2 行有换行 + 尾行
    DOCTEST_CHECK_EQ(module::markdown::countLines(text), 3);
    DOCTEST_CHECK_EQ(module::markdown::countLines(""), 1);
    DOCTEST_CHECK_EQ(module::markdown::byteOffsetToLine(text, 0), 0);
    DOCTEST_CHECK_EQ(module::markdown::byteOffsetToLine(text, 3), 1);   // 'b' 行
    DOCTEST_CHECK_EQ(module::markdown::byteOffsetToLine(text, 6), 2);   // 'c' 行
    DOCTEST_CHECK_EQ(module::markdown::byteOffsetToLine(text, 99), 2);  // 越界钳制
    DOCTEST_CHECK_EQ(module::markdown::lineStartOffset(text, 0), static_cast<size_t>(0));
    DOCTEST_CHECK_EQ(module::markdown::lineStartOffset(text, 1), static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(module::markdown::lineStartOffset(text, 2), static_cast<size_t>(6));
    DOCTEST_CHECK_EQ(module::markdown::lineStartOffset(text, 99), text.size());
    // 中文：每字 3 字节
    DOCTEST_CHECK_EQ(module::markdown::utf8Length("abc"), static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(module::markdown::utf8Length("项目周报"), static_cast<size_t>(4));
    DOCTEST_CHECK_EQ(module::markdown::utf8Length(""), static_cast<size_t>(0));
    return true;
})
// ==================== Phase 1 UI 无头 compose 冒烟 ====================

namespace {

// 无头测试用临时笔记目录（经 DEVDESK_NOTES_DIR 环境变量隔离真实目录）
fs::path makeTempNotesDir(const char* tag) {
    static int counter = 0;
    ++counter;
    const fs::path dir = fs::temp_directory_path() /
        ("devdesk_md_ui_" + std::string(tag) + "_" + std::to_string(counter));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

bool writeTextFile(const fs::path& path, const std::string& content) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    return ofs.good();
}

// RAII：临时笔记目录 + 临时 DB 注册为 appDatabase（析构注销，防污染其他用例）
struct MdUiEnv {
    fs::path dir;
    std::unique_ptr<core::Database> db;

    static void setEnvVar(const std::string& value) {
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

    explicit MdUiEnv(const char* tag) {
        dir = makeTempNotesDir(tag);
        auto r = core::Database::open(dir / "ui_test.db");
        if (r.isFail()) return;
        db = std::make_unique<core::Database>(std::move(r.unsafeValue()));
        if (db->migrate().isFail()) {
            db.reset();
            return;
        }
        core::registerAppDatabase(&db->db());
        // 笔记目录隔离：只对本用例生效
        setEnvVar(dir.string());
    }

    ~MdUiEnv() {
        setEnvVar("");
        core::registerAppDatabase(nullptr);
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

} // namespace

// ---- 1. 无服务（appDatabase 为 null）时 compose 不崩 ----

REGISTER_TEST(md_module_compose_no_service, "markdown_module: 无服务 compose 不崩（headless）", {
    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();   // appDatabase 为 null → 跳过服务初始化

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("md_null");
    mod->compose(ui, screen);
    ui.end();

    DOCTEST_CHECK(ui.find("md.root") != nullptr);
    DOCTEST_CHECK(ui.find("md.mode") != nullptr);        // 三态切换 segmented
    DOCTEST_CHECK(ui.find("md.tree.bg") != nullptr);
    DOCTEST_CHECK(ui.find("md.editor.hint") != nullptr);
    DOCTEST_CHECK(ui.find("md.status.bg") != nullptr);   // 状态栏常驻

    // 无修饰按键不消费（不能拦截正常打字）
    eui::KeyEvent plain;
    plain.key = eui::InputKey::A;
    DOCTEST_CHECK(!mod->onKeyEvent(plain));

    mod->onStop();
    return true;
})

// ---- 2. 有数据时：文件打开→大纲→跳转→三态循环 全链路（headless） ----

REGISTER_TEST(md_module_compose_phase1, "markdown_module: Phase 1 三态/大纲/状态栏 compose（headless）", {
    const std::string text = "# 标题一\n正文一\n\n## 标题二\n正文二\n";
    MdUiEnv env("phase1");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "note.md", text));

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

    // 帧1：文件列表 → 点击文件项打开笔记（真实 onClick 链路）
    frame("f1");
    const auto* row = ui.find("md.tree.item.0.row");
    DOCTEST_REQUIRE(row != nullptr);
    DOCTEST_REQUIRE(row->onClick != nullptr);
    row->onClick();

    // 帧2：默认双栏态 —— 编辑器 + 预览 + 状态栏三件套
    frame("f2");
    DOCTEST_CHECK(ui.find("md.editor.input") != nullptr);
    DOCTEST_CHECK(ui.find("md.preview.scroll") != nullptr);
    const auto* save = ui.find("md.status.save");
    DOCTEST_REQUIRE(save != nullptr);
    DOCTEST_CHECK(save->text.find("note.md") != std::string::npos);
    DOCTEST_CHECK(save->text.find("已保存") != std::string::npos);

    // 帧3：切到大纲页签 → 两个标题条目
    const auto* tab = ui.find("md.tree.tab.hit.1");
    DOCTEST_REQUIRE(tab != nullptr);
    DOCTEST_REQUIRE(tab->onClick != nullptr);
    tab->onClick();
    frame("f3");
    DOCTEST_CHECK(ui.find("md.outline.scroll") != nullptr);
    const auto* h1 = ui.find("md.outline.item.0.label");
    const auto* h2 = ui.find("md.outline.item.1.label");
    DOCTEST_REQUIRE(h1 != nullptr);
    DOCTEST_REQUIRE(h2 != nullptr);
    DOCTEST_CHECK_EQ(h1->text, std::string("标题一"));
    DOCTEST_CHECK_EQ(h2->text, std::string("标题二"));

    // 大纲跳转：点第二个标题 → InputState.cursor 写到该标题行首（byteOffset）
    const auto* out2 = ui.find("md.outline.item.1.row");
    DOCTEST_REQUIRE(out2 != nullptr);
    DOCTEST_REQUIRE(out2->onClick != nullptr);
    out2->onClick();
    const auto& st = ui.state<components::input_detail::InputModel::InputState>("md.editor.input");
    const auto headings = scanHeadings(text);
    DOCTEST_REQUIRE(headings.size() == 2);
    DOCTEST_CHECK_EQ(st.cursor, static_cast<int>(headings[1].byteOffset));

    // Ctrl+Shift+P 三态循环：Split → Preview → Edit → Split
    eui::KeyEvent p;
    p.key = eui::InputKey::P;
    p.modifiers.control = true;
    p.modifiers.shift = true;
    DOCTEST_CHECK(mod->onKeyEvent(p));   // → Preview
    frame("f4");
    DOCTEST_CHECK(ui.find("md.editor.input") == nullptr);
    DOCTEST_CHECK(ui.find("md.preview.scroll") != nullptr);
    DOCTEST_CHECK(mod->onKeyEvent(p));   // → Edit
    frame("f5");
    DOCTEST_CHECK(ui.find("md.editor.input") != nullptr);
    DOCTEST_CHECK(ui.find("md.preview.scroll") == nullptr);
    // 仅编辑态：文本列 clamp 720px 居中（1600 宽 → editor x 明显右移）
    const auto* ed = ui.find("md.editor.input");
    DOCTEST_REQUIRE(ed != nullptr);
    DOCTEST_CHECK(ed->x > 100.0f);
    DOCTEST_CHECK(mod->onKeyEvent(p));   // → Split（回默认）
    frame("f6");
    DOCTEST_CHECK(ui.find("md.editor.input") != nullptr);
    DOCTEST_CHECK(ui.find("md.preview.scroll") != nullptr);

    mod->onStop();
    return true;
})
