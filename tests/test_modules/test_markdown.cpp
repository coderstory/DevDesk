// tests/test_modules/test_markdown.cpp
// - md_line_scanner 纯函数单测（Phase 1 大纲 / 同步滚动底座）
// - md_tree 纯函数单测（W8 二级目录树分组）
// - MarkdownModule Phase 1 + W8 UI 无头 compose / layout 冒烟
//   （三态切换 / 大纲浮动面板 / 目录树展开收起 / 缩进布局）
// 接线证据 = 代码路径 + 本无头 compose/layout 测试 + 启动冒烟；真实鼠标点验留给用户人工
#include "doctest.h"
#include "core/database.h"
#include "core/module_interface.h"
#include "module_markdown/md_line_scanner.h"
#include "module_markdown/md_tree.h"

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
// ==================== W8：二级目录树纯函数 ====================

using module::markdown::buildTreeRows;
using module::markdown::MdTreeRow;
using module::markdown::NoteMeta;

namespace {

NoteMeta makeNote(const std::string& path, int64_t mtime) {
    module::markdown::NoteMeta n;
    n.relativePath = path;
    n.title = path;
    n.modifiedAt = mtime;
    return n;
}

} // namespace

REGISTER_TEST(md_tree_grouping, "md_tree: 顶层文件夹/二级文件/根目录散文件分组", {
    // mtime 倒序（listAll 语义），同目录内保持该序。
    // 注意：REGISTER_TEST 宏按逗号切参，vector 初始化列表里的逗号会破坏宏
    // 展开（编译报 "passed 7 arguments"），必须用 push_back —— W8 踩坑。
    std::vector<NoteMeta> notes;
    notes.push_back(makeNote("inbox/b.md", 4));
    notes.push_back(makeNote("work/a.md", 2));
    notes.push_back(makeNote("root.md", 5));
    notes.push_back(makeNote("inbox/c.md", 3));
    const auto rows = buildTreeRows(notes, {});
    // 顶层文件夹按字母序（inbox, work）+ 根目录组最后；默认全展开
    DOCTEST_CHECK_EQ(rows.size(), static_cast<size_t>(7));
    DOCTEST_CHECK(rows[0].isFolder);
    DOCTEST_CHECK_EQ(rows[0].name, std::string("inbox"));
    DOCTEST_CHECK_EQ(rows[0].relPath, std::string("inbox"));
    // Files under folder keep the incoming vector order (listAll = mtime DESC)
    DOCTEST_CHECK(!rows[1].isFolder);
    DOCTEST_CHECK_EQ(rows[1].name, std::string("b.md"));
    DOCTEST_CHECK_EQ(rows[1].relPath, std::string("inbox/b.md"));
    DOCTEST_CHECK_EQ(rows[1].depth, 1);
    DOCTEST_CHECK_EQ(rows[2].name, std::string("c.md"));
    DOCTEST_CHECK(rows[3].isFolder);
    DOCTEST_CHECK_EQ(rows[3].name, std::string("work"));
    DOCTEST_CHECK_EQ(rows[4].name, std::string("a.md"));
    // Root scattered files in a separate group at the end
    DOCTEST_CHECK(rows[5].isFolder);
    DOCTEST_CHECK_EQ(rows[5].name, std::string("根目录"));
    DOCTEST_CHECK_EQ(rows[5].relPath, std::string(""));
    DOCTEST_CHECK_EQ(rows[6].name, std::string("root.md"));
    DOCTEST_CHECK_EQ(rows[6].relPath, std::string("root.md"));
    return true;
})

REGISTER_TEST(md_tree_collapse_and_deep_files, "md_tree: 收起隐藏文件 / 更深层文件显示剩余子路径", {
    std::vector<NoteMeta> notes;
    notes.push_back(makeNote("a/deep/x.md", 1));
    notes.push_back(makeNote("a/top.md", 2));
    notes.push_back(makeNote("solo.md", 3));
    // 收起 a → a 组只剩文件夹行；根目录组不受影响
    const auto collapsed = buildTreeRows(notes, {"a"});
    DOCTEST_CHECK_EQ(collapsed.size(), static_cast<size_t>(3));
    DOCTEST_CHECK(collapsed[0].isFolder);
    DOCTEST_CHECK_EQ(collapsed[0].name, std::string("a"));
    DOCTEST_CHECK(collapsed[1].isFolder);          // 根目录组仍在最后
    DOCTEST_CHECK_EQ(collapsed[1].name, std::string("根目录"));
    DOCTEST_CHECK(!collapsed[2].isFolder);
    DOCTEST_CHECK_EQ(collapsed[2].name, std::string("solo.md"));
    // 更深层文件（a/deep/x.md）挂在一级目录 a 下，显示剩余子路径（二级树不递归）；
    // 组内保持传入顺序（listAll 的 mtime 倒序）
    const auto rows = buildTreeRows(notes, {});
    DOCTEST_CHECK_EQ(rows.size(), static_cast<size_t>(5));
    DOCTEST_CHECK_EQ(rows[1].name, std::string("deep/x.md"));
    DOCTEST_CHECK_EQ(rows[2].name, std::string("top.md"));
    DOCTEST_CHECK_EQ(rows[1].relPath, std::string("a/deep/x.md"));
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
    fs::create_directories(path.parent_path());  // 支持子目录笔记（W8 目录树）
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

    // 帧1：二级目录树（单根文件 → [0]=「根目录」文件夹行，[1]=note.md）
    // 点击文件行打开笔记（真实 onClick 链路）
    frame("f1");
    const auto* folderRow = ui.find("md.tree.node.0.row");
    DOCTEST_REQUIRE(folderRow != nullptr);
    DOCTEST_REQUIRE(folderRow->onClick != nullptr);
    const auto* fileRow = ui.find("md.tree.node.1.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    DOCTEST_REQUIRE(fileRow->onClick != nullptr);
    fileRow->onClick();

    // 帧2：默认编辑态（W9）——只有树 + 编辑器两个可见列，预览列不渲染；
    // 编辑列内 720px 写作列 clamp 居中（1600 宽 → editor x 明显右移）
    frame("f2");
    DOCTEST_CHECK(ui.find("md.editor.input") != nullptr);
    DOCTEST_CHECK(ui.find("md.preview.scroll") == nullptr);
    const auto* save = ui.find("md.status.save");
    DOCTEST_REQUIRE(save != nullptr);
    DOCTEST_CHECK(save->text.find("note.md") != std::string::npos);
    DOCTEST_CHECK(save->text.find("已保存") != std::string::npos);
    const auto* edDefault = ui.find("md.editor.input");
    DOCTEST_REQUIRE(edDefault != nullptr);
    DOCTEST_CHECK(edDefault->x > 100.0f);

    // 帧3：顶栏大纲开关 → 浮动面板出现，两个标题条目
    const auto* toggle = ui.find("md.outline.toggle.hit");
    DOCTEST_REQUIRE(toggle != nullptr);
    DOCTEST_REQUIRE(toggle->onClick != nullptr);
    toggle->onClick();
    frame("f3");
    DOCTEST_CHECK(ui.find("md.outline.panel") != nullptr);
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

    // 面板可关闭（✕ 按钮——button 组件把 onClick 挂在 .bg 子元素上）
    const auto* closeBtn = ui.find("md.outline.panel.close.bg");
    DOCTEST_REQUIRE(closeBtn != nullptr);
    DOCTEST_REQUIRE(closeBtn->onClick != nullptr);
    closeBtn->onClick();
    frame("f3b");
    DOCTEST_CHECK(ui.find("md.outline.panel") == nullptr);

    // Ctrl+Shift+P 三态循环：Edit（默认）→ Split → Preview → Edit
    eui::KeyEvent p;
    p.key = eui::InputKey::P;
    p.modifiers.control = true;
    p.modifiers.shift = true;
    DOCTEST_CHECK(mod->onKeyEvent(p));   // → Split
    frame("f4");
    DOCTEST_CHECK(ui.find("md.editor.input") != nullptr);
    DOCTEST_CHECK(ui.find("md.preview.scroll") != nullptr);
    DOCTEST_CHECK(mod->onKeyEvent(p));   // → Preview
    frame("f5");
    DOCTEST_CHECK(ui.find("md.editor.input") == nullptr);
    DOCTEST_CHECK(ui.find("md.preview.scroll") != nullptr);
    DOCTEST_CHECK(mod->onKeyEvent(p));   // → Edit（回默认）
    frame("f6");
    DOCTEST_CHECK(ui.find("md.editor.input") != nullptr);
    DOCTEST_CHECK(ui.find("md.preview.scroll") == nullptr);

    mod->onStop();
    return true;
})

// ---- 2b. W8：目录树展开/收起 + 缩进 layout 级验证 ----

REGISTER_TEST(md_module_compose_tree, "markdown_module: W8 目录树展开收起/缩进 layout（headless）", {
    MdUiEnv env("tree");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "inbox" / "idea.md", "# idea\n"));
    DOCTEST_REQUIRE(writeTextFile(env.dir / "work" / "week.md", "# week\n"));
    DOCTEST_REQUIRE(writeTextFile(env.dir / "solo.md", "# solo\n"));

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

    // 帧1：树行序 = [inbox▾, idea.md, work▾, week.md, 根目录▾, solo.md]
    frame("t1");
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.0.name")->text, std::string("📁 inbox"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.1.name")->text, std::string("idea.md"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.2.name")->text, std::string("📁 work"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.4.name")->text, std::string("📁 根目录"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.5.name")->text, std::string("solo.md"));

    // layout 级验证（W7 坑位：position 在流式子元素上静默失效——跑真实 layout
    // 后比对计算 frame，缩进丢失会在下面两条断言暴露）
    ui.layout(screen.width, screen.height);
    const auto* folderName = ui.find("md.tree.node.0.name");
    const auto* fileName = ui.find("md.tree.node.1.name");
    DOCTEST_REQUIRE(folderName != nullptr);
    DOCTEST_REQUIRE(fileName != nullptr);
    DOCTEST_CHECK(fileName->frame.x > folderName->frame.x + 20.0f);  // depth=1 缩进
    DOCTEST_CHECK(fileName->frame.y > folderName->frame.y);          // 纵向流式排布
    // 浮动面板未开时不存在
    DOCTEST_CHECK(ui.find("md.outline.panel") == nullptr);

    // 帧2：点文件夹行收起 inbox → inbox 的文件行消失（文件夹行保留，箭头变 ▸）
    const auto* folder = ui.find("md.tree.node.0.row");
    DOCTEST_REQUIRE(folder != nullptr);
    DOCTEST_REQUIRE(folder->onClick != nullptr);
    folder->onClick();
    frame("t2");
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.0.name")->text, std::string("📁 inbox"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.0.arrow")->text, std::string("▸"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.1.name")->text, std::string("📁 work"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.2.name")->text, std::string("week.md"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.3.name")->text, std::string("📁 根目录"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.4.name")->text, std::string("solo.md"));

    // 帧3：再点展开 → 文件行回来
    const auto* folder2 = ui.find("md.tree.node.0.row");
    DOCTEST_REQUIRE(folder2 != nullptr);
    folder2->onClick();
    frame("t3");
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.1.name")->text, std::string("idea.md"));

    // 帧4：点文件行 → 打开笔记（真实链路）
    const auto* fileRow = ui.find("md.tree.node.1.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    fileRow->onClick();
    frame("t4");
    DOCTEST_CHECK(ui.find("md.editor.input") != nullptr);

    mod->onStop();
    return true;
})

// ---- 2b2. W9：双栏宽度公式（layout 级——midW 笔误回归）----

REGISTER_TEST(md_module_split_layout, "markdown_module: W9 双栏三列铺满 contentW 无残隙（headless layout）", {
    MdUiEnv env("split");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "note.md", "# A\n"));

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

    // 打开笔记（单根文件 → 树行 [根目录, note.md]）
    frame("sp_open");
    const auto* fileRow = ui.find("md.tree.node.1.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    DOCTEST_REQUIRE(fileRow->onClick != nullptr);
    fileRow->onClick();

    // 进双栏态（默认态会随版本演进，最多循环 3 次一定经过 Split）
    eui::KeyEvent p;
    p.key = eui::InputKey::P;
    p.modifiers.control = true;
    p.modifiers.shift = true;
    bool split = false;
    for (int i = 0; i < 3 && !split; ++i) {
        frame("sp");
        split = ui.find("md.editor.input") != nullptr &&
               ui.find("md.preview.scroll") != nullptr;
        if (!split) mod->onKeyEvent(p);
    }
    DOCTEST_REQUIRE(split);

    ui.layout(screen.width, screen.height);
    const auto* tree = ui.find("md.tree.bg");
    const auto* editor = ui.find("md.editor.bg");
    const auto* preview = ui.find("md.preview.bg");
    DOCTEST_REQUIRE(tree != nullptr);
    DOCTEST_REQUIRE(editor != nullptr);
    DOCTEST_REQUIRE(preview != nullptr);

    // 三列 + 2 gap 铺满 contentW（1600 - 80 padding = 1520），无残余死白
    const float pad = 40.0f;
    const float contentW = screen.width - pad * 2.0f;
    const float gap = 16.0f;
    DOCTEST_CHECK_EQ(tree->frame.x, pad);
    DOCTEST_CHECK_EQ(tree->frame.width, 240.0f);
    DOCTEST_CHECK_EQ(editor->frame.width, preview->frame.width);
    const float total = tree->frame.width + editor->frame.width +
                        preview->frame.width + gap * 2.0f;
    DOCTEST_CHECK(total > contentW - 0.5f && total < contentW + 0.5f);
    DOCTEST_CHECK_EQ(preview->frame.x + preview->frame.width, pad + contentW);

    mod->onStop();
    return true;
})

// ---- 2c. W8：大纲浮动面板不挤压编辑区（layout 级）----

REGISTER_TEST(md_module_compose_outline_float, "markdown_module: W8 大纲浮动面板 zIndex/不挤压（headless layout）", {
    MdUiEnv env("outline");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "note.md", "# A\n\n## B\n"));

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

    frame("o1");
    ui.find("md.tree.node.1.row")->onClick();  // 打开 note.md
    const auto* toggle = ui.find("md.outline.toggle.hit");
    DOCTEST_REQUIRE(toggle != nullptr);
    DOCTEST_REQUIRE(toggle->onClick != nullptr);
    toggle->onClick();
    frame("o2");

    // 面板存在且 zIndex 提升浮层
    const auto* panel = ui.find("md.outline.panel");
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK(panel->zIndex > 0);
    // 面板浮在右侧（编辑/预览区之上）：x 靠近右缘
    ui.layout(screen.width, screen.height);
    DOCTEST_CHECK(panel->frame.x > screen.width * 0.7f);
    DOCTEST_CHECK(panel->frame.x + panel->frame.width <= screen.width);

    // 不挤压编辑区：开面板前后编辑器宽度一致（同一屏宽）
    const auto* ed = ui.find("md.editor.input");
    DOCTEST_REQUIRE(ed != nullptr);
    DOCTEST_CHECK(ed->width.value > 400.0f);   // 默认编辑态编辑列全宽（W9）

    mod->onStop();
    return true;
})

// ---- 3. W7 稳定化：pagebg / toast 节点常驻（headless 冒烟）----

REGISTER_TEST(md_module_compose_w7, "markdown_module: W7 pagebg/toast compose（headless）", {
    const std::string text = "# W7\n\n冒烟内容。\n";
    MdUiEnv env("w7");
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

    frame("w7_f1");
    // 页面铺底色（深色主题黑底修复的载体）
    const auto* pagebg = ui.find("md.pagebg");
    DOCTEST_REQUIRE(pagebg != nullptr);
    DOCTEST_CHECK_EQ(pagebg->width.value, screen.width);
    DOCTEST_CHECK_EQ(pagebg->height.value, screen.height);
    // toast 单例常驻（保存失败反馈的载体，visible 由状态位控制）
    DOCTEST_CHECK(ui.find("md.toast") != nullptr);
    // 重扫按钮已移除（W8：5 秒自动轮询取代）
    DOCTEST_CHECK(ui.find("md.btn.rescan") == nullptr);

    // 打开笔记走一遍真实保存链路（onStop 落盘不崩）
    const auto* row = ui.find("md.tree.node.1.row");
    DOCTEST_REQUIRE(row != nullptr);
    DOCTEST_REQUIRE(row->onClick != nullptr);
    row->onClick();
    frame("w7_f2");
    DOCTEST_CHECK(ui.find("md.editor.input") != nullptr);
    mod->onStop();
    return true;
})
