// tests/test_modules/test_markdown.cpp
// - md_line_scanner 纯函数单测（Phase 1 大纲 / 同步滚动底座）
// - md_tree 纯函数单测（W8 二级目录树分组）
// - MarkdownModule Phase 1 + W8 UI 无头 compose / layout 冒烟
//   （三态切换 / 大纲浮动面板 / 目录树展开收起 / 缩进布局）
// 接线证据 = 代码路径 + 本无头 compose/layout 测试 + 启动冒烟；真实鼠标点验留给用户人工
#include "doctest.h"
#include "test_support.h"
#include "core/database.h"
#include "core/module_interface.h"
#include "core/router.h"
#include "core/theme.h"
#include "module_markdown/md_block_model.h"
#include "module_markdown/md_line_scanner.h"
#include "module_markdown/md_tree.h"
#include "module_markdown/md_view_state.h"
#include "module_markdown/md_ids.h"

using module::markdown::kEditorInputId;

#include <components/input.h>
#include <core/dsl_runtime.h>
#include <eui/dsl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

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

// 闭栏规则对齐 md4c/CommonMark（Phase 3 等价测试抓出的破口回归锚）：
// 尾随文本闭栏行、短闭栏、```a`b` 非法开栏、≤3 空格缩进开/闭栏。
REGISTER_TEST(md_scanner_fence_close_strict, "md_line_scanner: 围栏开闭语义与 md4c 对齐", {
    using module::markdown::scanFences;
    // ```closer 不是闭栏 → 整段到 EOF 一个围栏
    {
        const auto fs = scanFences("```outer\ntext\n```closer\nafter\n");
        DOCTEST_REQUIRE(fs.size() == 1);
        DOCTEST_CHECK(fs[0].contentEnd == std::string_view("```outer\ntext\n```closer\nafter\n").size());
    }
    // 闭栏长度 < 开栏 → 不闭
    {
        const auto fs = scanFences("````py\ncode\n```\nstill code\n````\n");
        DOCTEST_REQUIRE(fs.size() == 1);
        DOCTEST_CHECK(fs[0].contentEnd == std::string_view("````py\ncode\n```\nstill code\n").size());
    }
    // ` 围栏开栏 info 含 ` = 整行是普通文本（不是围栏）；后续 ``` 才是开栏
    {
        const std::string t = "```a`b`\ncode?\n```\n";
        const auto fs = scanFences(t);
        DOCTEST_REQUIRE(fs.size() == 1);
        DOCTEST_CHECK(fs[0].openLineStart == std::string_view("```a`b`\ncode?\n").size());
    }
    // ~~~ 闭栏行尾随文本不闭栏；纯空格尾随可闭
    {
        const auto fs = scanFences("~~~\ncode\n~~~ x\nclose now\n~~~  \n");
        DOCTEST_REQUIRE(fs.size() == 1);
        DOCTEST_CHECK(fs[0].contentEnd ==
                      std::string_view("~~~\ncode\n~~~ x\nclose now\n").size());
    }
    // ≤3 前导空格的开栏/闭栏有效；≥4 空格是缩进代码不是围栏
    {
        const auto fs = scanFences("   ```py\ncode\n    ```\n   ```\n");
        DOCTEST_REQUIRE(fs.size() == 1);
        DOCTEST_CHECK(fs[0].openLineStart == 0);
        DOCTEST_CHECK(fs[0].contentEnd ==
                      std::string_view("   ```py\ncode\n    ```\n").size());
    }
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

REGISTER_TEST(md_tree_grouping, "md_tree: 根组常显置顶 + 顶层文件夹/二级文件分组", {
    // mtime 倒序（listAll 语义），同目录内保持该序。
    // 注意：REGISTER_TEST 宏按逗号切参，vector 初始化列表里的逗号会破坏宏
    // 展开（编译报 "passed 7 arguments"），必须用 push_back —— W8 踩坑。
    std::vector<NoteMeta> notes;
    notes.push_back(makeNote("inbox/b.md", 4));
    notes.push_back(makeNote("work/a.md", 2));
    notes.push_back(makeNote("root.md", 5));
    notes.push_back(makeNote("inbox/c.md", 3));
    const auto rows = buildTreeRows(notes, {}, {});
    // W9：根组「根目录」常显且置顶，其后一级目录按字母序（inbox, work）
    DOCTEST_CHECK_EQ(rows.size(), static_cast<size_t>(7));
    DOCTEST_CHECK(rows[0].isFolder);
    DOCTEST_CHECK_EQ(rows[0].name, std::string("根目录"));
    DOCTEST_CHECK_EQ(rows[0].relPath, std::string(""));
    DOCTEST_CHECK_EQ(rows[1].name, std::string("root.md"));
    DOCTEST_CHECK_EQ(rows[1].relPath, std::string("root.md"));
    DOCTEST_CHECK_EQ(rows[1].depth, 1);
    // 文件夹组按字母序；组内保持传入顺序（listAll = mtime DESC）
    DOCTEST_CHECK(rows[2].isFolder);
    DOCTEST_CHECK_EQ(rows[2].name, std::string("inbox"));
    DOCTEST_CHECK_EQ(rows[2].relPath, std::string("inbox"));
    DOCTEST_CHECK_EQ(rows[3].name, std::string("b.md"));
    DOCTEST_CHECK_EQ(rows[3].relPath, std::string("inbox/b.md"));
    DOCTEST_CHECK_EQ(rows[3].depth, 1);
    DOCTEST_CHECK_EQ(rows[4].name, std::string("c.md"));
    DOCTEST_CHECK(rows[5].isFolder);
    DOCTEST_CHECK_EQ(rows[5].name, std::string("work"));
    DOCTEST_CHECK_EQ(rows[6].name, std::string("a.md"));
    return true;
})

REGISTER_TEST(md_tree_collapse_and_deep_files, "md_tree: 收起隐藏文件 / 更深层文件显示剩余子路径", {
    std::vector<NoteMeta> notes;
    notes.push_back(makeNote("a/deep/more/x.md", 1));  // 三级夹下的文件
    notes.push_back(makeNote("a/deep/y.md", 2));
    notes.push_back(makeNote("a/top.md", 3));
    notes.push_back(makeNote("solo.md", 4));
    // 收起一级夹 a → a 的文件行与子夹行一起隐藏；根目录组不受影响（常显置顶）
    const auto collapsed = buildTreeRows(notes, {}, {"a"});
    DOCTEST_CHECK_EQ(collapsed.size(), static_cast<size_t>(3));
    DOCTEST_CHECK(collapsed[0].isFolder);
    DOCTEST_CHECK_EQ(collapsed[0].name, std::string("根目录"));
    DOCTEST_CHECK(!collapsed[1].isFolder);
    DOCTEST_CHECK_EQ(collapsed[1].name, std::string("solo.md"));
    DOCTEST_CHECK(collapsed[2].isFolder);
    DOCTEST_CHECK_EQ(collapsed[2].name, std::string("a"));
    // W10 两级：组内「文件行在前（mtime 序）、子夹行在后」（与顶层根组→顶层夹同序）；
    // 更深层文件（a/deep/more/x.md）挂在二级夹 a/deep 下，显示剩余子路径（内容不丢）
    const auto rows = buildTreeRows(notes, {}, {});
    DOCTEST_CHECK_EQ(rows.size(), static_cast<size_t>(7));
    DOCTEST_CHECK(rows[0].isFolder);
    DOCTEST_CHECK_EQ(rows[0].name, std::string("根目录"));
    DOCTEST_CHECK_EQ(rows[1].name, std::string("solo.md"));
    DOCTEST_CHECK_EQ(rows[1].depth, 1);
    DOCTEST_CHECK(rows[2].isFolder);
    DOCTEST_CHECK_EQ(rows[2].name, std::string("a"));
    DOCTEST_CHECK_EQ(rows[2].depth, 0);
    DOCTEST_CHECK_EQ(rows[3].name, std::string("top.md"));  // a 的直接子文件
    DOCTEST_CHECK_EQ(rows[3].relPath, std::string("a/top.md"));
    DOCTEST_CHECK_EQ(rows[3].depth, 1);
    DOCTEST_CHECK(rows[4].isFolder);                        // 二级夹行
    DOCTEST_CHECK_EQ(rows[4].name, std::string("deep"));
    DOCTEST_CHECK_EQ(rows[4].relPath, std::string("a/deep"));
    DOCTEST_CHECK_EQ(rows[4].depth, 1);
    DOCTEST_CHECK_EQ(rows[5].name, std::string("more/x.md"));  // 残余子路径
    DOCTEST_CHECK_EQ(rows[5].relPath, std::string("a/deep/more/x.md"));
    DOCTEST_CHECK_EQ(rows[5].depth, 2);
    DOCTEST_CHECK_EQ(rows[6].name, std::string("y.md"));
    DOCTEST_CHECK_EQ(rows[6].depth, 2);
    // 收起二级夹 → 只剩子夹行（一级夹自己的文件行不受影响）
    const auto subCollapsed = buildTreeRows(notes, {}, {"a/deep"});
    DOCTEST_CHECK_EQ(subCollapsed.size(), static_cast<size_t>(5));
    DOCTEST_CHECK_EQ(subCollapsed[3].name, std::string("top.md"));
    DOCTEST_CHECK(subCollapsed[4].isFolder);
    DOCTEST_CHECK_EQ(subCollapsed[4].name, std::string("deep"));
    return true;
})

REGISTER_TEST(md_tree_two_level_empty_subfolders, "md_tree: W10 空子夹行（磁盘枚举）+ 三级目录不进树", {
    // 只有磁盘枚举的目录（没有任何笔记）：顶层夹与空子夹都有文件夹行、零文件行
    const std::vector<NoteMeta> none;
    const auto rows = buildTreeRows(none, {"工作", "工作/周会", "归档"}, {});
    DOCTEST_CHECK_EQ(rows.size(), static_cast<size_t>(4));
    DOCTEST_CHECK(rows[0].isFolder);
    DOCTEST_CHECK_EQ(rows[0].name, std::string("根目录"));  // 常显置顶
    DOCTEST_CHECK_EQ(rows[1].name, std::string("工作"));    // UTF-8 字节序：工 < 归
    DOCTEST_CHECK_EQ(rows[1].depth, 0);
    DOCTEST_CHECK_EQ(rows[2].name, std::string("周会"));    // 空子夹行（W10）
    DOCTEST_CHECK_EQ(rows[2].relPath, std::string("工作/周会"));
    DOCTEST_CHECK_EQ(rows[2].depth, 1);
    DOCTEST_CHECK_EQ(rows[3].name, std::string("归档"));
    DOCTEST_CHECK_EQ(rows[3].depth, 0);

    // 三级目录（磁盘枚举）不进树：两级上限下没有它的行位，两级仍有行
    const auto deep = buildTreeRows(none, {"a", "a/b", "a/b/c"}, {});
    DOCTEST_CHECK_EQ(deep.size(), static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(deep[1].name, std::string("a"));
    DOCTEST_CHECK_EQ(deep[2].name, std::string("b"));
    DOCTEST_CHECK_EQ(deep[2].relPath, std::string("a/b"));
    DOCTEST_CHECK_EQ(deep[2].depth, 1);
    return true;
})

REGISTER_TEST(md_tree_root_group_always, "md_tree: 根组标题常显（无根文件/空目录）", {
    // 只有子目录文件（无根散文件）→ 根组行仍在顶部（W9）
    std::vector<NoteMeta> notes;
    notes.push_back(makeNote("work/a.md", 1));
    const auto noRoot = buildTreeRows(notes, {}, {});
    DOCTEST_CHECK_EQ(noRoot.size(), static_cast<size_t>(3));
    DOCTEST_CHECK(noRoot[0].isFolder);
    DOCTEST_CHECK_EQ(noRoot[0].name, std::string("根目录"));
    DOCTEST_CHECK_EQ(noRoot[0].relPath, std::string(""));
    DOCTEST_CHECK_EQ(noRoot[1].name, std::string("work"));
    DOCTEST_CHECK_EQ(noRoot[2].name, std::string("a.md"));

    // 空目录 → 仅剩根组标题行（UI 层叠加空态提示）
    const std::vector<NoteMeta> none;
    const auto empty = buildTreeRows(none, {}, {});
    DOCTEST_CHECK_EQ(empty.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(empty[0].isFolder);
    DOCTEST_CHECK_EQ(empty[0].name, std::string("根目录"));

    // W9：磁盘枚举的空目录（extraFolders）→ 树里出现无子行的文件夹行
    const auto withEmpty = buildTreeRows(none, {"空文件夹"}, {});
    DOCTEST_CHECK_EQ(withEmpty.size(), static_cast<size_t>(2));
    DOCTEST_CHECK(withEmpty[0].isFolder);
    DOCTEST_CHECK_EQ(withEmpty[0].name, std::string("根目录"));
    DOCTEST_CHECK(withEmpty[1].isFolder);
    DOCTEST_CHECK_EQ(withEmpty[1].name, std::string("空文件夹"));
    DOCTEST_CHECK_EQ(withEmpty[1].relPath, std::string("空文件夹"));
    DOCTEST_CHECK_EQ(withEmpty[1].depth, 0);
    return true;
})

// ==================== Phase 1 UI 无头 compose 冒烟 ====================

namespace {
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

    explicit MdUiEnv(const char* tag) {
        dir = test_support::makeTempDir("devdesk_md_ui_", tag);
        auto r = core::Database::open(dir / "ui_test.db");
        if (r.isFail()) return;
        db = std::make_unique<core::Database>(std::move(r.unsafeValue()));
        if (db->migrate().isFail()) {
            db.reset();
            return;
        }
        core::registerAppDatabase(&db->db());
        // 视图状态文件隔离（审计 [23]）：无头用例不碰真实 %APPDATA% 状态
        test_support::setEnvVar("DEVDESK_MARKDOWN_VIEW_STATE_PATH", (dir / "view_state.json").string());
        // 笔记目录隔离：只对本用例生效
        test_support::setEnvVar("DEVDESK_NOTES_DIR", dir.string());
    }

    ~MdUiEnv() {
        test_support::setEnvVar("DEVDESK_NOTES_DIR", "");
        test_support::setEnvVar("DEVDESK_MARKDOWN_VIEW_STATE_PATH", "");
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
    DOCTEST_CHECK(ui.find(kEditorInputId) != nullptr);
    DOCTEST_CHECK(ui.find("md.preview.scroll") == nullptr);
    const auto* save = ui.find("md.status.save");
    DOCTEST_REQUIRE(save != nullptr);
    DOCTEST_CHECK(save->text.find("note.md") != std::string::npos);
    DOCTEST_CHECK(save->text.find("已保存") != std::string::npos);
    const auto* edDefault = ui.find(kEditorInputId);
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
    const auto& st = ui.state<components::input_detail::InputModel::InputState>(kEditorInputId);
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

    // Ctrl+Shift+P 四态循环：Edit（默认）→ Live → Split → Preview → Edit
    eui::KeyEvent p;
    p.key = eui::InputKey::P;
    p.modifiers.control = true;
    p.modifiers.shift = true;
    DOCTEST_CHECK(mod->onKeyEvent(p));   // → Live（M1 新增第四态）
    frame("f3c");
    DOCTEST_CHECK(ui.find("md.live.scroll") != nullptr);
    DOCTEST_CHECK(ui.find(kEditorInputId) == nullptr);
    DOCTEST_CHECK(ui.find("md.preview.scroll") == nullptr);
    DOCTEST_CHECK(mod->onKeyEvent(p));   // → Split
    frame("f4");
    DOCTEST_CHECK(ui.find(kEditorInputId) != nullptr);
    DOCTEST_CHECK(ui.find("md.preview.scroll") != nullptr);
    DOCTEST_CHECK(ui.find("md.live.scroll") == nullptr);
    DOCTEST_CHECK(mod->onKeyEvent(p));   // → Preview
    frame("f5");
    DOCTEST_CHECK(ui.find(kEditorInputId) == nullptr);
    DOCTEST_CHECK(ui.find("md.preview.scroll") != nullptr);
    DOCTEST_CHECK(mod->onKeyEvent(p));   // → Edit（回默认）
    frame("f6");
    DOCTEST_CHECK(ui.find(kEditorInputId) != nullptr);
    DOCTEST_CHECK(ui.find("md.preview.scroll") == nullptr);
    DOCTEST_CHECK(ui.find("md.live.scroll") == nullptr);

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

    // 帧1：树行序（W9 根组置顶）= [根目录▾, solo.md, inbox▾, idea.md, work▾, week.md]
    frame("t1");
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.0.name")->text, std::string("根目录"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.1.name")->text, std::string("solo.md"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.2.name")->text, std::string("inbox"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.3.name")->text, std::string("idea.md"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.4.name")->text, std::string("work"));

    // layout 级验证（W7 坑位：position 在流式子元素上静默失效——跑真实 layout
    // 后比对计算 frame，缩进丢失会在下面两条断言暴露）
    ui.layout(screen.width, screen.height);
    const auto* folderName = ui.find("md.tree.node.2.name");
    const auto* fileName = ui.find("md.tree.node.3.name");
    DOCTEST_REQUIRE(folderName != nullptr);
    DOCTEST_REQUIRE(fileName != nullptr);
    DOCTEST_CHECK(fileName->frame.x > folderName->frame.x + 20.0f);  // depth=1 缩进
    DOCTEST_CHECK(fileName->frame.y > folderName->frame.y);          // 纵向流式排布
    // 浮动面板未开时不存在
    DOCTEST_CHECK(ui.find("md.outline.panel") == nullptr);

    // 帧2：点文件夹行收起 inbox → inbox 的文件行消失（文件夹行保留，箭头变 ▸）
    const auto* folder = ui.find("md.tree.node.2.row");
    DOCTEST_REQUIRE(folder != nullptr);
    DOCTEST_REQUIRE(folder->onClick != nullptr);
    folder->onClick();
    frame("t2");
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.0.name")->text, std::string("根目录"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.1.name")->text, std::string("solo.md"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.2.name")->text, std::string("inbox"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.2.arrow")->text, std::string("▸"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.3.name")->text, std::string("work"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.4.name")->text, std::string("week.md"));

    // 帧3：再点展开 → 文件行回来
    const auto* folder2 = ui.find("md.tree.node.2.row");
    DOCTEST_REQUIRE(folder2 != nullptr);
    folder2->onClick();
    frame("t3");
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.3.name")->text, std::string("idea.md"));

    // 帧4：点文件行 → 打开笔记（真实链路）
    const auto* fileRow = ui.find("md.tree.node.3.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    fileRow->onClick();
    frame("t4");
    DOCTEST_CHECK(ui.find(kEditorInputId) != nullptr);

    mod->onStop();
    return true;
})

// ---- 2b3. W9：空目录树 = 根组标题 + 空态提示同显；搜索 placeholder 常显 ----

REGISTER_TEST(md_module_empty_tree, "markdown_module: W9 空目录根组标题+空态提示同显 / 搜索框 placeholder（headless）", {
    MdUiEnv env("empty");
    DOCTEST_REQUIRE(env.db != nullptr);  // 不写任何笔记 → 空目录

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("e1");
    mod->compose(ui, screen);
    ui.end();

    // 空目录：根组标题行 + 空态提示同显（📁 emoji 已换 fa-folder icon 元素）
    const auto* rootHeader = ui.find("md.tree.node.0.name");
    DOCTEST_REQUIRE(rootHeader != nullptr);
    DOCTEST_CHECK_EQ(rootHeader->text, std::string("根目录"));
    const auto* rootIcon = ui.find("md.tree.node.0.ficon");
    DOCTEST_REQUIRE(rootIcon != nullptr);
    DOCTEST_CHECK_EQ(rootIcon->fontFamily, std::string("Icon"));  // FA 路由
    DOCTEST_CHECK_EQ(rootIcon->text, eui::utf8(0xF07B));          // fa-folder
    const auto* hint = ui.find("md.tree.empty");
    DOCTEST_REQUIRE(hint != nullptr);
    DOCTEST_CHECK(hint->text.find("还没有笔记") != std::string::npos);

    // 搜索框 placeholder：空输入 + 未聚焦时也渲染（用户报告「空白文本框」，
    // 核查结论：EUI-NEO input 只要 text 为空就画 placeholder，与焦点无关）。
    // 🔍 emoji 已换 FA 浮层 icon（fa-magnifying-glass）
    const auto* search = ui.find("md.search.text");
    DOCTEST_REQUIRE(search != nullptr);
    DOCTEST_CHECK_EQ(search->text, std::string("搜索笔记…"));
    const auto* searchIcon = ui.find("md.search.icon");
    DOCTEST_REQUIRE(searchIcon != nullptr);
    DOCTEST_CHECK_EQ(searchIcon->fontFamily, std::string("Icon"));
    DOCTEST_CHECK_EQ(searchIcon->text, eui::utf8(0xF002));

    mod->onStop();
    return true;
})

// ---- 2b4. W9：新建文件夹入口（左栏按钮 → 弹窗 → 空文件夹行入树）----

REGISTER_TEST(md_module_new_folder, "markdown_module: W9 新建文件夹入口真实链路（headless）", {
    MdUiEnv env("newfolder");
    DOCTEST_REQUIRE(env.db != nullptr);
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

    // 帧1：左栏有「+ 文件夹」按钮；点开弹窗
    frame("nf1");
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.0.name")->text, std::string("根目录"));
    const auto* folderBtn = ui.find("md.tree.newfolder.bg");
    DOCTEST_REQUIRE(folderBtn != nullptr);
    DOCTEST_REQUIRE(folderBtn->onClick != nullptr);
    folderBtn->onClick();
    frame("nf2");
    DOCTEST_CHECK(ui.find("md.dialog.newfolder.input") != nullptr);

    // 输入文件夹名（真实 onTextInput 链路 → onChange → newFolderName_）
    const auto* input = ui.find("md.dialog.newfolder.input.hit");
    DOCTEST_REQUIRE(input != nullptr);
    DOCTEST_REQUIRE(input->onTextInput != nullptr);
    core::TextInputEvent tie;
    tie.text = "工作";
    input->onTextInput(tie);
    frame("nf3");

    // 点「创建」→ 磁盘建目录 + 树里出现空文件夹行（node.2，无子行）
    const auto* confirm = ui.find("md.dialog.newfolder.confirm.bg");
    DOCTEST_REQUIRE(confirm != nullptr);
    DOCTEST_REQUIRE(confirm->onClick != nullptr);
    confirm->onClick();
    frame("nf4");
    // 弹窗已关（dialog 节点常驻，靠 opacity 显隐）
    const auto* dialogPanel = ui.find("md.dialog.newfolder.panel");
    DOCTEST_REQUIRE(dialogPanel != nullptr);
    DOCTEST_CHECK_EQ(dialogPanel->opacity, 0.0f);
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.0.name")->text, std::string("根目录"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.1.name")->text, std::string("solo.md"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.2.name")->text, std::string("工作"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.2.arrow")->text, std::string("▾"));
    DOCTEST_CHECK(ui.find("md.tree.node.3.name") == nullptr);  // 空文件夹无子行
    // 磁盘上目录真实存在
    DOCTEST_CHECK(fs::is_directory(env.dir / "工作"));

    // 新建笔记走子路径（W9：placeholder 承诺「可用 / 分文件夹」——create 建中间目录）
    const auto* noteBtn = ui.find("md.tree.new.bg");
    DOCTEST_REQUIRE(noteBtn != nullptr);
    noteBtn->onClick();
    frame("nf5");
    const auto* noteInput = ui.find("md.dialog.new.input.hit");
    DOCTEST_REQUIRE(noteInput != nullptr);
    core::TextInputEvent tie2;
    tie2.text = "工作/周会";
    noteInput->onTextInput(tie2);
    frame("nf6");
    const auto* noteConfirm = ui.find("md.dialog.new.confirm.bg");
    DOCTEST_REQUIRE(noteConfirm != nullptr);
    noteConfirm->onClick();
    frame("nf7");
    // 文件真实落盘在子目录，树里 工作 组下出现 周会.md
    DOCTEST_CHECK(fs::is_regular_file(env.dir / "工作" / "周会.md"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.2.name")->text, std::string("工作"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.3.name")->text, std::string("周会.md"));

    mod->onStop();
    return true;
})

// ---- 2b4b. W10：两级目录——父目录下拉建子夹 + 空子夹行 + 两级上限拦截 ----

REGISTER_TEST(md_module_new_folder_parent,
              "markdown_module: W10 新建子夹（父目录下拉）/ 空子夹行 / 两级上限（headless）", {
    MdUiEnv env("newfolder2");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "工作" / "周报.md", "# 周报\n"));

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

    // 帧1：树 = [根目录, 工作, 周报.md]；打开新建文件夹弹窗
    frame("np1");
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.0.name")->text, std::string("根目录"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.1.name")->text, std::string("工作"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.2.name")->text, std::string("周报.md"));
    const auto* folderBtn = ui.find("md.tree.newfolder.bg");
    DOCTEST_REQUIRE(folderBtn != nullptr);
    folderBtn->onClick();
    frame("np2");

    // 父目录下拉默认第 0 项（根目录）；点开选「工作」（真实 onClick 链路）
    const auto* ddLabel = ui.find("md.dialog.newfolder.parent.dd.label");
    DOCTEST_REQUIRE(ddLabel != nullptr);
    DOCTEST_CHECK_EQ(ddLabel->text, std::string("根目录"));
    const auto* ddField = ui.find("md.dialog.newfolder.parent.dd.field");
    DOCTEST_REQUIRE(ddField != nullptr);
    DOCTEST_REQUIRE(ddField->onClick != nullptr);
    ddField->onClick();
    frame("np3");
    const auto* parentItem1 = ui.find("md.dialog.newfolder.parent.dd.item.1");
    DOCTEST_REQUIRE(parentItem1 != nullptr);
    DOCTEST_REQUIRE(parentItem1->onClick != nullptr);
    parentItem1->onClick();
    frame("np4");
    DOCTEST_CHECK_EQ(ui.find("md.dialog.newfolder.parent.dd.label")->text,
                     std::string("工作"));

    // 输入子夹名 + 创建 → 父目录 工作 ⇒ 工作/周会（磁盘真实建出）
    const auto* input = ui.find("md.dialog.newfolder.input.hit");
    DOCTEST_REQUIRE(input != nullptr);
    DOCTEST_REQUIRE(input->onTextInput != nullptr);
    core::TextInputEvent tie;
    tie.text = "周会";
    input->onTextInput(tie);
    frame("np5");
    const auto* confirm = ui.find("md.dialog.newfolder.confirm.bg");
    DOCTEST_REQUIRE(confirm != nullptr);
    DOCTEST_REQUIRE(confirm->onClick != nullptr);
    confirm->onClick();
    frame("np6");
    DOCTEST_CHECK(fs::is_directory(env.dir / "工作" / "周会"));
    // 空子夹行入树并嵌在「工作」组下（工作 的文件行在前、子夹行在后）
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.0.name")->text, std::string("根目录"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.1.name")->text, std::string("工作"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.2.name")->text, std::string("周报.md"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.3.name")->text, std::string("周会"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.3.arrow")->text, std::string("▾"));
    DOCTEST_CHECK(ui.find("md.tree.node.4.name") == nullptr);  // 空子夹无子行

    // layout 级：子夹行比顶层夹行更靠右（depth=1 缩进真实生效）
    ui.layout(screen.width, screen.height);
    const auto* topName = ui.find("md.tree.node.1.name");
    const auto* subName = ui.find("md.tree.node.3.name");
    DOCTEST_REQUIRE(topName != nullptr);
    DOCTEST_REQUIRE(subName != nullptr);
    DOCTEST_CHECK(subName->frame.x > topName->frame.x + 8.0f);

    // 两级上限：父目录=根目录 + 名字 "a/b/c"（三级）→ 就地红字，不建目录
    const auto* folderBtn2 = ui.find("md.tree.newfolder.bg");
    DOCTEST_REQUIRE(folderBtn2 != nullptr);
    folderBtn2->onClick();
    frame("np7");
    const auto* input2 = ui.find("md.dialog.newfolder.input.hit");
    DOCTEST_REQUIRE(input2 != nullptr);
    core::TextInputEvent tie2;
    tie2.text = "a/b/c";
    input2->onTextInput(tie2);
    frame("np8");
    const auto* confirm2 = ui.find("md.dialog.newfolder.confirm.bg");
    DOCTEST_REQUIRE(confirm2 != nullptr);
    confirm2->onClick();
    frame("np9");
    const auto* err = ui.find("md.dialog.newfolder.error");
    DOCTEST_REQUIRE(err != nullptr);
    DOCTEST_CHECK_EQ(err->text, std::string("最多支持两级目录（顶层 / 子目录）"));
    DOCTEST_CHECK(!fs::exists(env.dir / "a"));

    // 关掉再开（按钮 onClick 清名字/错误）→ 改成两级名字 "a/b" 建成功
    const auto* cancelBtn = ui.find("md.dialog.newfolder.cancel.bg");
    DOCTEST_REQUIRE(cancelBtn != nullptr);
    cancelBtn->onClick();
    frame("np10");
    const auto* folderBtn3 = ui.find("md.tree.newfolder.bg");
    DOCTEST_REQUIRE(folderBtn3 != nullptr);
    folderBtn3->onClick();
    frame("np11");
    const auto* input3 = ui.find("md.dialog.newfolder.input.hit");
    DOCTEST_REQUIRE(input3 != nullptr);
    core::TextInputEvent tie3;
    tie3.text = "a/b";
    input3->onTextInput(tie3);
    frame("np12");
    const auto* confirm3 = ui.find("md.dialog.newfolder.confirm.bg");
    DOCTEST_REQUIRE(confirm3 != nullptr);
    confirm3->onClick();
    frame("np13");
    DOCTEST_CHECK(fs::is_directory(env.dir / "a" / "b"));
    // 树 = [根目录, a, a/b, 工作, 周报.md, 周会]（顶层夹按字节序：a < 工作）
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.1.name")->text, std::string("a"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.2.name")->text, std::string("b"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.3.name")->text, std::string("工作"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.5.name")->text, std::string("周会"));

    mod->onStop();
    return true;
})

// ---- 2b4c. W10：只有空文件夹（零笔记）→ 树照样出文件夹行，不叠空态提示 ----

REGISTER_TEST(md_module_tree_empty_folders_only,
              "markdown_module: W10 只有空文件夹（零笔记）→ 顶层夹/子夹行都在，空态提示不覆盖（headless）", {
    MdUiEnv env("folders0");
    DOCTEST_REQUIRE(env.db != nullptr);
    // 磁盘上只有空目录（含一级子目录），一个 .md 都没有
    std::error_code ec;
    fs::create_directories(env.dir / "工作" / "周会", ec);
    DOCTEST_REQUIRE(!ec);

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("e1");
    mod->compose(ui, screen);
    ui.end();

    DOCTEST_CHECK_EQ(ui.find("md.tree.node.0.name")->text, std::string("根目录"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.1.name")->text, std::string("工作"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.2.name")->text, std::string("周会"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.2.arrow")->text, std::string("▾"));
    // 空态提示不再叠在文件夹行上（「还没有笔记」只在连文件夹都没有时出）
    DOCTEST_CHECK(ui.find("md.tree.empty") == nullptr);

    mod->onStop();
    return true;
})

// ---- 2b5. 审计 [30]：新建笔记/文件夹弹窗错误 → 内嵌红字 formError + 输入清除 ----

REGISTER_TEST(md_module_dialog_form_error,
              "markdown_module: 弹窗创建错误内嵌红字 + 下一次输入清除（headless，审计 [30]）", {
    MdUiEnv env("formerror");
    DOCTEST_REQUIRE(env.db != nullptr);
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

    // 1) 新建文件夹空名提交 → 弹窗内红字「文件夹名不能为空」，弹窗保持打开
    //    （旧行为：showToast 隔到弹窗外；toast 单例还常被冲突提示复用）
    frame("fe1");
    const auto* folderBtn = ui.find("md.tree.newfolder.bg");
    DOCTEST_REQUIRE(folderBtn != nullptr);
    folderBtn->onClick();
    frame("fe2");
    const auto* folderConfirm = ui.find("md.dialog.newfolder.confirm.bg");
    DOCTEST_REQUIRE(folderConfirm != nullptr);
    DOCTEST_REQUIRE(folderConfirm->onClick != nullptr);
    folderConfirm->onClick();
    frame("fe3");
    const auto* folderError = ui.find("md.dialog.newfolder.error");
    DOCTEST_REQUIRE(folderError != nullptr);   // 错误行只随错误态存在
    DOCTEST_CHECK_EQ(folderError->text, std::string("文件夹名不能为空"));
    const auto* folderPanel = ui.find("md.dialog.newfolder.panel");
    DOCTEST_REQUIRE(folderPanel != nullptr);
    DOCTEST_CHECK(folderPanel->opacity > 0.0f);  // 弹窗不关（错误就地可见）

    // 2) 下一次输入（onTextInput → onChange）→ 错误行消失
    const auto* folderInput = ui.find("md.dialog.newfolder.input.hit");
    DOCTEST_REQUIRE(folderInput != nullptr);
    DOCTEST_REQUIRE(folderInput->onTextInput != nullptr);
    core::TextInputEvent tie;
    tie.text = "工作";
    folderInput->onTextInput(tie);
    frame("fe4");
    DOCTEST_CHECK(ui.find("md.dialog.newfolder.error") == nullptr);

    // 3) 新建笔记非法路径名提交 → 内嵌红字（旧行为：toast；同弹窗内与
    //    其他校验路径不一致，审计 [30] 点名）
    const auto* noteBtn = ui.find("md.tree.new.bg");
    DOCTEST_REQUIRE(noteBtn != nullptr);
    noteBtn->onClick();
    frame("fe5");
    const auto* noteInput = ui.find("md.dialog.new.input.hit");
    DOCTEST_REQUIRE(noteInput != nullptr);
    core::TextInputEvent tie2;
    tie2.text = "C:/bad";
    noteInput->onTextInput(tie2);
    frame("fe6");
    const auto* noteConfirm = ui.find("md.dialog.new.confirm.bg");
    DOCTEST_REQUIRE(noteConfirm != nullptr);
    noteConfirm->onClick();
    frame("fe7");
    const auto* noteError = ui.find("md.dialog.new.error");
    DOCTEST_REQUIRE(noteError != nullptr);
    DOCTEST_CHECK(noteError->text.find("不合法") != std::string::npos);
    const auto* notePanel = ui.find("md.dialog.new.panel");
    DOCTEST_REQUIRE(notePanel != nullptr);
    DOCTEST_CHECK(notePanel->opacity > 0.0f);  // 弹窗不关
    // （不额外断言磁盘无「C:」残留：Windows 下含盘符串的 path::exists 语义
    // 不可靠；未落盘由 isSafeNewNotePath 分支 + 弹窗保持打开共同保证）

    // 4) 重新输入 → 错误行清除（与文件夹弹窗同模式）
    //    注意：ui.begin() 每帧重建元素树，noteInput 来自 fe5 帧已失效——重 find
    const auto* noteInput2 = ui.find("md.dialog.new.input.hit");
    DOCTEST_REQUIRE(noteInput2 != nullptr);
    DOCTEST_REQUIRE(noteInput2->onTextInput != nullptr);
    core::TextInputEvent tie3;
    tie3.text = "ok";
    noteInput2->onTextInput(tie3);
    frame("fe8");
    DOCTEST_CHECK(ui.find("md.dialog.new.error") == nullptr);

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
        split = ui.find(kEditorInputId) != nullptr &&
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

// ---- 2b'. tech-debt C：单 stack 列布局语义锁（bg 铺满 / 内容坐标 / 单槽位）----

REGISTER_TEST(md_tree_column_layout_semantics,
              "markdown_module: md.tree 单 stack 列语义锁 bg 重合+绝对内容+每列一槽（tech-debt C）", {
    MdUiEnv env("tree_layout");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "a.md", "# A\nx\n"));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("tl");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);

    const float pad = 40.0f;        // spacingXXLarge
    const float sideW = 240.0f;     // 左栏树宽
    const float gap = 16.0f;
    const float contentW = screen.width - pad * 2.0f;

    const auto* tree = ui.find("md.tree");
    const auto* treeBg = ui.find("md.tree.bg");
    DOCTEST_REQUIRE(tree != nullptr);
    DOCTEST_REQUIRE(treeBg != nullptr);
    // 耦合点核心：bg 不带 position，靠 stack 语义"居中 == 铺满"——成立前提
    // 是 bg 尺寸恒等于 stack 尺寸。任一侧改动使两框不重合 → 本测试变红
    DOCTEST_CHECK_EQ(treeBg->frame.x, tree->frame.x);
    DOCTEST_CHECK_EQ(treeBg->frame.y, tree->frame.y);
    DOCTEST_CHECK_EQ(treeBg->frame.width, tree->frame.width);
    DOCTEST_CHECK_EQ(treeBg->frame.height, tree->frame.height);

    // 绝对定位内容 = 相对 stack 内容区坐标（搜索框 .position(12,12)）
    const auto* search = ui.find("md.search");
    DOCTEST_REQUIRE(search != nullptr);
    DOCTEST_CHECK_EQ(search->frame.x, tree->frame.x + 12.0f);
    DOCTEST_CHECK_EQ(search->frame.y, tree->frame.y + 12.0f);

    // 编辑态（默认）：编辑器列只占一个流式槽位（W9 修复前 overlay 兄弟双
    // 占位 → 整列右移一个 sideW、右缘溢出）。右缘必须精确贴 contentW 右边界
    const auto* editor = ui.find("md.editor");
    DOCTEST_REQUIRE(editor != nullptr);
    DOCTEST_CHECK_EQ(editor->frame.x, tree->frame.x + sideW + gap);
    DOCTEST_CHECK_EQ(editor->frame.width, contentW - sideW - gap);
    DOCTEST_CHECK_EQ(editor->frame.x + editor->frame.width, pad + contentW);
    DOCTEST_CHECK(ui.find("md.preview") == nullptr);  // 编辑态无预览列

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
    const auto* ed = ui.find(kEditorInputId);
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
    // 页面不再自铺全屏底色（半透/磨砂档主体效果的载体是主窗口 clearColor，
    // 自铺不透明底会把 alpha 恒置 1 盖掉效果——回归守卫）
    DOCTEST_CHECK(ui.find("md.pagebg") == nullptr);
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
    DOCTEST_CHECK(ui.find(kEditorInputId) != nullptr);
    mod->onStop();
    return true;
})
// ---- 2d. 标签系统（spec §8.1）：编辑器标签行 + 左栏标签筛选（headless）----

REGISTER_TEST(md_module_tags_ui, "markdown_module: 标签 chip 行 + 左栏标签筛选（headless）", {
    MdUiEnv env("tags");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "note.md", "# A\n"));
    DOCTEST_REQUIRE(writeTextFile(env.dir / "other.md", "# B\n"));

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
    // 扫树行找指定文件名的行（避免 mtime 同秒时根组内顺序不稳定）
    auto findFileRow = [&](const std::string& name) -> const core::dsl::Element* {
        for (int i = 0; i < 6; ++i) {
            const auto* nm = ui.find("md.tree.node." + std::to_string(i) + ".name");
            if (nm != nullptr && nm->text == name) {
                return ui.find("md.tree.node." + std::to_string(i) + ".row");
            }
        }
        return nullptr;
    };
    auto treeHasNote = [&](const std::string& name) {
        for (int i = 0; i < 6; ++i) {
            const auto* nm = ui.find("md.tree.node." + std::to_string(i) + ".name");
            if (nm != nullptr && nm->text == name) return true;
        }
        return false;
    };

    // 帧1：无标签 → 左栏无筛选区
    frame("tg1");
    DOCTEST_CHECK(ui.find("md.tags.title") == nullptr);

    // 打开 note.md → 编辑器顶部标签行出现（添加输入 + ＋按钮）
    const auto* fileRow = findFileRow("note.md");
    DOCTEST_REQUIRE(fileRow != nullptr);
    DOCTEST_REQUIRE(fileRow->onClick != nullptr);
    fileRow->onClick();
    frame("tg2");
    const auto* addInput = ui.find("md.tags.add.hit");
    DOCTEST_REQUIRE(addInput != nullptr);
    DOCTEST_REQUIRE(addInput->onTextInput != nullptr);

    // 输入标签名 → 点「＋」提交（真实 onTextInput + onClick 链路）
    core::TextInputEvent tie;
    tie.text = "工作";
    addInput->onTextInput(tie);
    frame("tg3");
    const auto* addBtn = ui.find("md.tags.addbtn.bg");
    DOCTEST_REQUIRE(addBtn != nullptr);
    DOCTEST_REQUIRE(addBtn->onClick != nullptr);
    addBtn->onClick();
    frame("tg4");

    // 编辑器 chip 出现；左栏筛选区出现同名 chip（带笔记数）
    const auto* chipName = ui.find("md.tags.chip.0.name");
    DOCTEST_REQUIRE(chipName != nullptr);
    DOCTEST_CHECK_EQ(chipName->text, std::string("工作"));
    DOCTEST_CHECK(ui.find("md.tags.title") != nullptr);
    const auto* filterName = ui.find("md.tags.filter.0.name");
    DOCTEST_REQUIRE(filterName != nullptr);
    DOCTEST_CHECK(filterName->text.find("工作") != std::string::npos);
    DOCTEST_CHECK(filterName->text.find("1") != std::string::npos);

    // 点筛选 chip → 树只显示含该标签的笔记（other.md 隐藏，note.md 保留）
    const auto* filterBg = ui.find("md.tags.filter.0.bg");
    DOCTEST_REQUIRE(filterBg != nullptr);
    DOCTEST_REQUIRE(filterBg->onClick != nullptr);
    filterBg->onClick();
    frame("tg5");
    DOCTEST_CHECK(!treeHasNote("other.md"));
    DOCTEST_CHECK(treeHasNote("note.md"));

    // 再点一次 → 取消筛选，other.md 回来
    ui.find("md.tags.filter.0.bg")->onClick();
    frame("tg6");
    DOCTEST_CHECK(treeHasNote("other.md"));
    DOCTEST_CHECK(treeHasNote("note.md"));

    // chip 的 × → 删除标签：chip 消失，标签无引用后左栏筛选区也消失
    const auto* del = ui.find("md.tags.chip.0.del");
    DOCTEST_REQUIRE(del != nullptr);
    DOCTEST_REQUIRE(del->onClick != nullptr);
    del->onClick();
    frame("tg7");
    DOCTEST_CHECK(ui.find("md.tags.chip.0.name") == nullptr);
    DOCTEST_CHECK(ui.find("md.tags.title") == nullptr);

    mod->onStop();
    return true;
})

// ---- 2e. 生命周期（审计 [4][20]）：树行右键菜单 → 删除/重命名 真实链路 ----

REGISTER_TEST(md_module_delete_note_ui, "markdown_module: 右键→删除→确认弹窗（取消保留/确认落删），编辑器联动清空（headless）", {
    MdUiEnv env("delui");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "a.md", "# A\n正文A\n"));
    DOCTEST_REQUIRE(writeTextFile(env.dir / "b.md", "# B\n正文B\n"));

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
    auto findRow = [&](const std::string& name) -> core::dsl::Element* {
        for (int i = 0; i < 8; ++i) {
            const auto* nm = ui.find("md.tree.node." + std::to_string(i) + ".name");
            if (nm != nullptr && nm->text == name) {
                return ui.find("md.tree.node." + std::to_string(i) + ".row");
            }
        }
        return nullptr;
    };
    auto treeHas = [&ui](const std::string& name) {
        for (int i = 0; i < 8; ++i) {
            const auto* nm = ui.find("md.tree.node." + std::to_string(i) + ".name");
            if (nm != nullptr && nm->text == name) return true;
        }
        return false;
    };

    // 打开 a.md（编辑器活跃），右键它的树行 → 菜单出现「删除笔记」项
    frame("d1");
    auto* row1 = findRow("a.md");
    DOCTEST_REQUIRE(row1 != nullptr);
    DOCTEST_REQUIRE(row1->onClick != nullptr);
    DOCTEST_REQUIRE(row1->onContextMenu != nullptr);
    row1->onClick();
    frame("d2");
    DOCTEST_CHECK(ui.find(kEditorInputId) != nullptr);

    eui::PointerEvent pe;
    pe.x = 150.0;
    pe.y = 230.0;
    auto* row2 = findRow("a.md");
    DOCTEST_REQUIRE(row2 != nullptr);
    row2->onContextMenu(pe, eui::Rect{});
    frame("d3");
    const auto* delLabel = ui.find("md.ctx.level.0.label.1");
    DOCTEST_REQUIRE(delLabel != nullptr);
    DOCTEST_CHECK_EQ(delLabel->text, std::string("删除笔记"));

    // 取消路径：删除项 → 确认弹窗（「不可恢复」文案）→ 取消 → 文件不动
    auto* delItem = ui.find("md.ctx.level.0.item.1");
    DOCTEST_REQUIRE(delItem != nullptr && delItem->onClick != nullptr);
    delItem->onClick();
    frame("d4");
    const auto* body = ui.find("md.dialog.del.body");
    DOCTEST_REQUIRE(body != nullptr);
    DOCTEST_CHECK(body->text.find("a.md") != std::string::npos);
    DOCTEST_CHECK(body->text.find("不可恢复") != std::string::npos);
    auto* cancelBtn = ui.find("md.dialog.del.cancel.bg");
    DOCTEST_REQUIRE(cancelBtn != nullptr && cancelBtn->onClick != nullptr);
    cancelBtn->onClick();
    frame("d5");
    DOCTEST_CHECK(fs::exists(env.dir / "a.md"));
    DOCTEST_CHECK(ui.find(kEditorInputId) != nullptr);  // 编辑器未受影响

    // 确认路径：再右键 → 删除 → 确认 → 文件删除、树消失、编辑器清回空态；
    // b.md 不受牵连
    auto* row3 = findRow("a.md");
    DOCTEST_REQUIRE(row3 != nullptr);
    row3->onContextMenu(pe, eui::Rect{});
    frame("d6");
    auto* delItem2 = ui.find("md.ctx.level.0.item.1");
    DOCTEST_REQUIRE(delItem2 != nullptr && delItem2->onClick != nullptr);
    delItem2->onClick();
    frame("d7");
    auto* delBtn = ui.find("md.dialog.del.confirm.bg");
    DOCTEST_REQUIRE(delBtn != nullptr && delBtn->onClick != nullptr);
    delBtn->onClick();
    DOCTEST_CHECK_FALSE(fs::exists(env.dir / "a.md"));
    DOCTEST_CHECK(fs::exists(env.dir / "b.md"));
    frame("d8");
    DOCTEST_CHECK_FALSE(treeHas("a.md"));
    DOCTEST_CHECK(treeHas("b.md"));
    // 删的是当前打开的笔记 → 编辑器回到空态提示
    DOCTEST_CHECK(ui.find("md.editor.hint") != nullptr);

    mod->onStop();
    return true;
})

// ---- 2e2. 右键菜单坐标：窗口坐标 → 页面坐标（减页面区原点） ----
// 指针事件是窗口坐标（runtime_input 派发只做 /dpiScale），而菜单浮层构建在
// 页面子树内（app.page，原点 = 侧边栏右侧）——不换算则菜单整体右移一个
// 侧边栏宽（展开 208 / 折叠 72），跑进右侧详情区（2026-09-27 修复回归护栏）

REGISTER_TEST(md_ctx_menu_page_origin, "markdown_module: 右键菜单坐标扣页面区原点（窗口→页面坐标，headless）", {
    MdUiEnv env("ctxorigin");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "a.md", "# A\n正文A\n"));

    // 模拟 app 层：页面区原点由 app_sidebar compose 每帧写入 Router
    ::core::Router router;
    router.setPageOriginX(208.0f);  // 侧边栏展开态
    test_support::ScopedAppRouter routerGuard(&router);

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1392.0f;  // 页面区宽 = 窗口 1600 - 侧边栏 208
    screen.height = 1000.0f;
    eui::Ui ui;
    auto frame = [&](const char* page) {
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
    };
    auto findRow = [&](const std::string& name) -> core::dsl::Element* {
        for (int i = 0; i < 8; ++i) {
            const auto* nm = ui.find("md.tree.node." + std::to_string(i) + ".name");
            if (nm != nullptr && nm->text == name) {
                return ui.find("md.tree.node." + std::to_string(i) + ".row");
            }
        }
        return nullptr;
    };
    auto menuLevelX = [&]() -> float {
        const auto* level = ui.find("md.ctx.level.0");
        return level != nullptr ? level->x : -1.0f;
    };

    frame("co1");

    // 窗口坐标 x=400 → 菜单页面坐标 400 - 208 = 192（y 方向不换算）
    eui::PointerEvent pe;
    pe.x = 400.0;
    pe.y = 230.0;
    auto* row1 = findRow("a.md");
    DOCTEST_REQUIRE(row1 != nullptr);
    DOCTEST_REQUIRE(row1->onContextMenu != nullptr);
    row1->onContextMenu(pe, eui::Rect{});
    frame("co2");
    const auto* level = ui.find("md.ctx.level.0");
    DOCTEST_REQUIRE(level != nullptr);
    DOCTEST_CHECK(std::fabs(level->x - 192.0f) < 0.01f);
    DOCTEST_CHECK(std::fabs(level->y - 230.0f) < 0.01f);

    // 折叠侧边栏（可证伪预测：偏移量从 208 降到 72）→ 同一窗口坐标 = 328
    router.setPageOriginX(72.0f);
    auto* row2 = findRow("a.md");
    DOCTEST_REQUIRE(row2 != nullptr);
    row2->onContextMenu(pe, eui::Rect{});
    frame("co3");
    DOCTEST_CHECK(std::fabs(menuLevelX() - 328.0f) < 0.01f);

    // 反证：不接 Router（无页面区原点）时窗口坐标原样传出 = 修复前行为
    // （菜单落在 400 → 右移一个侧边栏宽）。此断言同时证明 level.x 就是
    // 浮层真实位置，192/328 是减原点后的结果而非被别的机制改写
    ::core::registerAppRouter(nullptr);
    auto* row3 = findRow("a.md");
    DOCTEST_REQUIRE(row3 != nullptr);
    row3->onContextMenu(pe, eui::Rect{});
    frame("co4");
    DOCTEST_CHECK(std::fabs(menuLevelX() - 400.0f) < 0.01f);
    ::core::registerAppRouter(&router);  // 复位（guard 负责最终注销）

    mod->onStop();
    return true;
})

REGISTER_TEST(md_module_rename_note_ui, "markdown_module: 右键→重命名→预填原路径→选区替换输入→落盘，编辑器跟随（headless）", {
    using InputState = components::input_detail::InputModel::InputState;

    MdUiEnv env("renui");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "old.md", "# Old\n正文\n"));

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
    auto findRow = [&](const std::string& name) -> core::dsl::Element* {
        for (int i = 0; i < 8; ++i) {
            const auto* nm = ui.find("md.tree.node." + std::to_string(i) + ".name");
            if (nm != nullptr && nm->text == name) {
                return ui.find("md.tree.node." + std::to_string(i) + ".row");
            }
        }
        return nullptr;
    };
    auto treeHas = [&ui](const std::string& name) {
        for (int i = 0; i < 8; ++i) {
            const auto* nm = ui.find("md.tree.node." + std::to_string(i) + ".name");
            if (nm != nullptr && nm->text == name) return true;
        }
        return false;
    };

    // 先打开 old.md（编辑器跟随），右键 → 重命名
    frame("r1");
    auto* row1 = findRow("old.md");
    DOCTEST_REQUIRE(row1 != nullptr);
    row1->onClick();
    frame("r2");
    eui::PointerEvent pe;
    pe.x = 120.0;
    pe.y = 210.0;
    auto* row2 = findRow("old.md");
    DOCTEST_REQUIRE(row2 != nullptr && row2->onContextMenu != nullptr);
    row2->onContextMenu(pe, eui::Rect{});
    frame("r3");
    auto* renameItem = ui.find("md.ctx.level.0.item.0");
    DOCTEST_REQUIRE(renameItem != nullptr && renameItem->onClick != nullptr);
    renameItem->onClick();
    frame("r4");

    // 弹窗输入框预填原相对路径
    const auto* prefill = ui.find("md.dialog.rename.input.text");
    DOCTEST_REQUIRE(prefill != nullptr);
    DOCTEST_CHECK_EQ(prefill->text, std::string("old.md"));

    // 全选 + 真实 onTextInput（insertAtCursor 选区替换 → onChange → renameInput_）
    auto& st = ui.state<InputState>("md.dialog.rename.input");
    st.selectionStart = 0;
    st.selectionEnd = static_cast<int>(st.text.size());
    auto* hit = ui.find("md.dialog.rename.input.hit");
    DOCTEST_REQUIRE(hit != nullptr && hit->onTextInput != nullptr);
    core::TextInputEvent tie;
    tie.text = "新名.md";
    hit->onTextInput(tie);
    frame("r5");
    auto* confirm = ui.find("md.dialog.rename.confirm.bg");
    DOCTEST_REQUIRE(confirm != nullptr && confirm->onClick != nullptr);
    confirm->onClick();
    DOCTEST_CHECK_FALSE(fs::exists(env.dir / "old.md"));
    DOCTEST_CHECK(fs::exists(env.dir / "新名.md"));
    frame("r6");
    DOCTEST_CHECK(treeHas("新名.md"));
    DOCTEST_CHECK_FALSE(treeHas("old.md"));
    // 编辑器继续跟随这篇笔记：非空态、内容保留、状态栏文件名已更新
    DOCTEST_CHECK(ui.find("md.editor.hint") == nullptr);
    const auto& est = ui.state<InputState>(kEditorInputId);
    DOCTEST_CHECK_EQ(est.text, std::string("# Old\n正文\n"));
    const auto* status = ui.find("md.status.save");
    DOCTEST_REQUIRE(status != nullptr);
    DOCTEST_CHECK(status->text.find("新名.md") != std::string::npos);

    mod->onStop();
    return true;
})

// ---- 2f. 新建笔记即时性（审计 [13][31]）：默认名快速创建 + 编辑器聚焦接线 ----

REGISTER_TEST(md_module_default_name_create, "markdown_module: 空名提交=「无标题笔记」秒建+撞名编号，编辑器随开（headless）", {
    MdUiEnv env("defname");
    DOCTEST_REQUIRE(env.db != nullptr);

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

    // 弹窗直接回车/确认（不输入名字）→ 旧行为是「笔记名不能为空」拒办，
    // 现在建「无标题笔记.md」并打开（commitNewNote 成功后经
    // Ui::requestFocus 聚焦编辑器——headless 无 Runtime 消费端，聚焦行为
    // 由上游单测/人工冒烟覆盖，这里断言到「已打开」为止）
    frame("dn1");
    auto* newBtn = ui.find("md.tree.new.bg");
    DOCTEST_REQUIRE(newBtn != nullptr && newBtn->onClick != nullptr);
    newBtn->onClick();
    frame("dn2");
    auto* confirm = ui.find("md.dialog.new.confirm.bg");
    DOCTEST_REQUIRE(confirm != nullptr && confirm->onClick != nullptr);
    confirm->onClick();
    DOCTEST_CHECK(fs::exists(env.dir / "无标题笔记.md"));
    frame("dn3");
    DOCTEST_CHECK(ui.find("md.editor.hint") == nullptr);  // 编辑器已打开这篇

    // 再建一篇：默认名撞名自动编号，不覆盖第一篇
    ui.find("md.tree.new.bg")->onClick();
    frame("dn4");
    ui.find("md.dialog.new.confirm.bg")->onClick();
    DOCTEST_CHECK(fs::exists(env.dir / "无标题笔记 2.md"));
    frame("dn5");
    DOCTEST_CHECK(fs::exists(env.dir / "无标题笔记.md"));  // 第一篇原样

    mod->onStop();
    return true;
})

// ---- 审计 [30]：subtitle 静态操作说明（对齐 Todo 同类修复）----
// 旧行为：subtitle = 「N 篇笔记 · 最后一次操作结果」，操作日志顶掉快捷键
// 提示且永久驻留。新行为：subtitle 恒为静态说明；操作结果走 toast（status_
// 成员已删）；笔记数进编辑器底部状态栏右侧
REGISTER_TEST(md_module_subtitle_static, "markdown_module: subtitle 静态快捷键说明 + 笔记数在状态栏（审计[30]，headless）", {
    MdUiEnv env("subtitle");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "note.md", "# 标题\n正文\n"));

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
    frame("f1");

    // subtitle 含快捷键提示（Phase 2 键盘流 + Ctrl+Shift+P 三态都是真实能力），
    // 不再携带笔记数/操作结果
    const auto* sub = ui.find("md.subtitle");
    DOCTEST_REQUIRE(sub != nullptr);
    DOCTEST_CHECK(sub->text.find("Ctrl+Shift+P") != std::string::npos);
    DOCTEST_CHECK(sub->text.find("Ctrl+B/I/K") != std::string::npos);
    DOCTEST_CHECK(sub->text.find("Tab") != std::string::npos);
    DOCTEST_CHECK(sub->text.find("Enter") != std::string::npos);
    DOCTEST_CHECK(sub->text.find("篇笔记") == std::string::npos);
    const std::string staticSub = sub->text;

    // 打开笔记（旧行为会把「已打开 note.md」写进 subtitle 顶掉提示）
    const auto* fileRow = ui.find("md.tree.node.1.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    DOCTEST_REQUIRE(fileRow->onClick != nullptr);
    fileRow->onClick();
    frame("f2");

    // subtitle 逐字不变 = 静态
    const auto* sub2 = ui.find("md.subtitle");
    DOCTEST_REQUIRE(sub2 != nullptr);
    DOCTEST_CHECK_EQ(sub2->text, staticSub);
    // 操作反馈改走 toast（title=「已打开」）
    const auto* toastTitle = ui.find("md.toast.title");
    DOCTEST_REQUIRE(toastTitle != nullptr);
    DOCTEST_CHECK_EQ(toastTitle->text, std::string("已打开"));
    // 笔记数落点：编辑器底部状态栏右侧
    const auto* pos = ui.find("md.status.pos");
    DOCTEST_REQUIRE(pos != nullptr);
    DOCTEST_CHECK(pos->text.find("篇笔记") != std::string::npos);

    mod->onStop();
    return true;
})

// ---- 审计 [21]：创建/编辑弹窗与标签输入框 Esc=取消（onCancel 取代旧 Esc→提交）----

REGISTER_TEST(md_module_esc_cancel_dialogs,
              "markdown_module: 新建笔记/文件夹/重命名 Esc=取消不落盘；标签 Esc=丢草稿（headless，审计 [21]）", {
    using InputState = components::input_detail::InputModel::InputState;

    MdUiEnv env("esccancel");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "old2.md", "# Old2\n"));

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
    auto pressEsc = [](eui::Ui& u, const std::string& inputId) -> bool {
        auto* hit = u.find(inputId + ".hit");
        DOCTEST_REQUIRE(hit != nullptr);
        DOCTEST_REQUIRE(hit->onKeyEvent != nullptr);
        eui::KeyEvent esc;
        esc.key = eui::InputKey::Escape;
        esc.action = eui::KeyAction::Press;
        return hit->onKeyEvent(esc);   // 应被 onCancel 消费
    };
    auto findFileRow = [&](const std::string& name) -> const core::dsl::Element* {
        for (int i = 0; i < 8; ++i) {
            const auto* nm = ui.find("md.tree.node." + std::to_string(i) + ".name");
            if (nm != nullptr && nm->text == name) {
                return ui.find("md.tree.node." + std::to_string(i) + ".row");
            }
        }
        return nullptr;
    };

    // 1) 新建笔记：输入名字 → Esc → 弹窗关、磁盘不落盘
    frame("ec1");
    const auto* noteBtn = ui.find("md.tree.new.bg");
    DOCTEST_REQUIRE(noteBtn != nullptr && noteBtn->onClick != nullptr);
    noteBtn->onClick();
    frame("ec2");
    auto* nhit = ui.find("md.dialog.new.input.hit");
    DOCTEST_REQUIRE(nhit != nullptr && nhit->onTextInput != nullptr);
    core::TextInputEvent nt;
    nt.text = "esc笔记";
    nhit->onTextInput(nt);
    frame("ec3");
    DOCTEST_CHECK(pressEsc(ui, "md.dialog.new.input"));
    frame("ec4");
    DOCTEST_CHECK_EQ(ui.find("md.dialog.new.panel")->opacity, 0.0f);
    DOCTEST_CHECK_FALSE(fs::exists(env.dir / "esc笔记.md"));   // 旧行为：Esc=提交会建出来

    // 2) 新建文件夹：输入名字 → Esc → 弹窗关、目录不创建
    const auto* folderBtn = ui.find("md.tree.newfolder.bg");
    DOCTEST_REQUIRE(folderBtn != nullptr && folderBtn->onClick != nullptr);
    folderBtn->onClick();
    frame("ec5");
    auto* fhIT = ui.find("md.dialog.newfolder.input.hit");
    DOCTEST_REQUIRE(fhIT != nullptr && fhIT->onTextInput != nullptr);
    core::TextInputEvent ft;
    ft.text = "esc文件夹";
    fhIT->onTextInput(ft);
    frame("ec6");
    DOCTEST_CHECK(pressEsc(ui, "md.dialog.newfolder.input"));
    frame("ec7");
    DOCTEST_CHECK_EQ(ui.find("md.dialog.newfolder.panel")->opacity, 0.0f);
    DOCTEST_CHECK_FALSE(fs::exists(env.dir / "esc文件夹"));

    // 3) 重命名：打开笔记 → 右键 → 重命名 → 替换输入 → Esc → 旧文件原样在
    const auto* oldRow = findFileRow("old2.md");
    DOCTEST_REQUIRE(oldRow != nullptr && oldRow->onClick != nullptr);
    oldRow->onClick();
    frame("ec8");
    eui::PointerEvent pe;
    pe.x = 120.0;
    pe.y = 210.0;
    const auto* oldRow2 = findFileRow("old2.md");
    DOCTEST_REQUIRE(oldRow2 != nullptr && oldRow2->onContextMenu != nullptr);
    oldRow2->onContextMenu(pe, eui::Rect{});
    frame("ec9");
    auto* renameItem = ui.find("md.ctx.level.0.item.0");
    DOCTEST_REQUIRE(renameItem != nullptr && renameItem->onClick != nullptr);
    renameItem->onClick();
    frame("ec10");
    auto& rst = ui.state<InputState>("md.dialog.rename.input");
    rst.selectionStart = 0;
    rst.selectionEnd = static_cast<int>(rst.text.size());
    auto* rihit = ui.find("md.dialog.rename.input.hit");
    DOCTEST_REQUIRE(rihit != nullptr && rihit->onTextInput != nullptr);
    core::TextInputEvent rt;
    rt.text = "esc新名.md";
    rihit->onTextInput(rt);
    frame("ec11");
    DOCTEST_CHECK(pressEsc(ui, "md.dialog.rename.input"));
    frame("ec12");
    DOCTEST_CHECK_EQ(ui.find("md.dialog.rename.panel")->opacity, 0.0f);
    DOCTEST_CHECK(fs::exists(env.dir / "old2.md"));            // 未改名
    DOCTEST_CHECK_FALSE(fs::exists(env.dir / "esc新名.md"));    // 旧行为：Esc=提交会改名

    // 4) 标签输入框：编辑器开着笔记 → 打半截标签 → Esc → 草稿丢弃、chip 不出现
    const auto* chipAdd = ui.find("md.tags.add.hit");
    DOCTEST_REQUIRE(chipAdd != nullptr && chipAdd->onTextInput != nullptr);
    core::TextInputEvent tt;
    tt.text = "半截";
    chipAdd->onTextInput(tt);
    frame("ec13");
    DOCTEST_CHECK_EQ(ui.find("md.tags.add.text")->text, std::string("半截"));
    DOCTEST_CHECK(pressEsc(ui, "md.tags.add"));
    frame("ec14");
    // 草稿清空 → 输入框回 placeholder；标签未被提交（旧行为：Esc=commitAddTag）
    DOCTEST_CHECK_EQ(ui.find("md.tags.add.text")->text, std::string("＋ 标签"));
    DOCTEST_CHECK(ui.find("md.tags.chip.0.name") == nullptr);

    mod->onStop();
    return true;
})

// ---- ux-design §4 #6 P1：Ctrl+N 新建笔记快捷键（app 级通道，同弹窗入口）----

REGISTER_TEST(md_module_ctrl_n_new_note,
              "markdown_module: Ctrl+N 开新建笔记弹窗；弹窗开着再按不重置草稿；完整创建链路（headless）", {
    using InputState = components::input_detail::InputModel::InputState;

    MdUiEnv env("ctrln");
    DOCTEST_REQUIRE(env.db != nullptr);

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

    // 1) Ctrl+N（isDown + control，无 shift/alt）→ 模块消费、弹窗打开
    frame("cn1");
    eui::KeyEvent cn;
    cn.key = eui::InputKey::N;
    cn.modifiers.control = true;
    DOCTEST_CHECK(mod->onKeyEvent(cn));
    frame("cn2");
    const auto* panel = ui.find("md.dialog.new.panel");
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK(panel->opacity > 0.0f);
    DOCTEST_CHECK(ui.find("md.dialog.new.input") != nullptr);

    // 2) 输入草稿名后（弹窗开着）再按 Ctrl+N → 不消费、不清草稿（防误重开）
    const auto* hit = ui.find("md.dialog.new.input.hit");
    DOCTEST_REQUIRE(hit != nullptr && hit->onTextInput != nullptr);
    core::TextInputEvent tie;
    tie.text = "草稿";
    hit->onTextInput(tie);
    frame("cn3");
    DOCTEST_CHECK_FALSE(mod->onKeyEvent(cn));
    frame("cn4");
    DOCTEST_CHECK_EQ(ui.state<InputState>("md.dialog.new.input").text,
                     std::string("草稿"));

    // 3) 确认 → 走既有 commitNewNote 链路：落盘 + 弹窗关（快捷键与按钮同源）
    const auto* confirm = ui.find("md.dialog.new.confirm.bg");
    DOCTEST_REQUIRE(confirm != nullptr && confirm->onClick != nullptr);
    confirm->onClick();
    DOCTEST_CHECK(fs::exists(env.dir / "草稿.md"));
    frame("cn5");
    DOCTEST_CHECK_EQ(ui.find("md.dialog.new.panel")->opacity, 0.0f);

    // 4) Ctrl+Shift+N 不触发（显式排除 shift 组合，留给「无痕新窗口」类语义）
    eui::KeyEvent csn;
    csn.key = eui::InputKey::N;
    csn.modifiers.control = true;
    csn.modifiers.shift = true;
    DOCTEST_CHECK_FALSE(mod->onKeyEvent(csn));
    frame("cn6");
    DOCTEST_CHECK_EQ(ui.find("md.dialog.new.panel")->opacity, 0.0f);

    // 5) 新建文件夹弹窗开着时 Ctrl+N 落空（不叠弹窗）
    ui.find("md.tree.newfolder.bg")->onClick();
    frame("cn7");
    DOCTEST_CHECK_FALSE(mod->onKeyEvent(cn));

    mod->onStop();
    return true;
})
// ---- 审计 [19] 收尾：重命名弹窗校验错误改内嵌红字 formError（原走 toast）----

REGISTER_TEST(md_module_rename_form_error,
              "markdown_module: 重命名空名/撞名/非法路径 → 内嵌红字 + 弹窗不关 + 输入清除 + 打开清残留（headless）", {
    using InputState = components::input_detail::InputModel::InputState;

    MdUiEnv env("rferr");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "a.md", "# A\n"));
    DOCTEST_REQUIRE(writeTextFile(env.dir / "b.md", "# B\n"));

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
    auto findRow = [&](const std::string& name) -> core::dsl::Element* {
        for (int i = 0; i < 8; ++i) {
            const auto* nm = ui.find("md.tree.node." + std::to_string(i) + ".name");
            if (nm != nullptr && nm->text == name) {
                return ui.find("md.tree.node." + std::to_string(i) + ".row");
            }
        }
        return nullptr;
    };
    // （框架宏失败 = return false，lambda 必须返回 bool，调用处 DOCTEST_CHECK）
    auto openRename = [&](const std::string& name) -> bool {
        auto* row = findRow(name);
        DOCTEST_REQUIRE(row != nullptr);
        eui::PointerEvent pe;
        pe.x = 120.0;
        pe.y = 210.0;
        DOCTEST_REQUIRE(row->onContextMenu != nullptr);
        row->onContextMenu(pe, eui::Rect{});
        frame("rfx1");
        auto* item = ui.find("md.ctx.level.0.item.0");
        DOCTEST_REQUIRE(item != nullptr && item->onClick != nullptr);
        item->onClick();
        return true;
    };
    // 全选 + Backspace：走 input 内置选区删除 → onChange 真实链路
    auto clearInput = [&]() -> bool {
        auto& st = ui.state<InputState>("md.dialog.rename.input");
        st.selectionStart = 0;
        st.selectionEnd = static_cast<int>(st.text.size());
        auto* hit2 = ui.find("md.dialog.rename.input.hit");
        DOCTEST_REQUIRE(hit2 != nullptr && hit2->onKeyEvent != nullptr);
        eui::KeyEvent bs;
        bs.key = eui::InputKey::Backspace;
        hit2->onKeyEvent(bs);
        return true;
    };
    auto typeOver = [&](const std::string& text) -> bool {
        auto& st = ui.state<InputState>("md.dialog.rename.input");
        st.selectionStart = 0;
        st.selectionEnd = static_cast<int>(st.text.size());
        auto* hit2 = ui.find("md.dialog.rename.input.hit");
        DOCTEST_REQUIRE(hit2 != nullptr && hit2->onTextInput != nullptr);
        core::TextInputEvent t2;
        t2.text = text;
        hit2->onTextInput(t2);
        return true;
    };

    // 打开 a.md 的重命名弹窗（预填原路径）
    frame("rf1");
    DOCTEST_CHECK(openRename("a.md"));
    frame("rf2");
    DOCTEST_CHECK(ui.find("md.dialog.rename.panel")->opacity > 0.0f);
    DOCTEST_CHECK_EQ(ui.find("md.dialog.rename.input.text")->text,
                     std::string("a.md"));

    // 1) 空名提交 → 红字「新名字不能为空」，弹窗不关（旧行为：静默关弹窗）
    DOCTEST_CHECK(clearInput());
    frame("rf3");
    auto* confirm = ui.find("md.dialog.rename.confirm.bg");
    DOCTEST_REQUIRE(confirm != nullptr && confirm->onClick != nullptr);
    confirm->onClick();
    frame("rf4");
    const auto* err = ui.find("md.dialog.rename.error");
    DOCTEST_REQUIRE(err != nullptr);
    DOCTEST_CHECK_EQ(err->text, std::string("新名字不能为空"));
    DOCTEST_CHECK(ui.find("md.dialog.rename.panel")->opacity > 0.0f);

    // 2) 下一次输入清错误；撞名提交 → 红字「重命名失败：…已存在…」，文件未动
    DOCTEST_CHECK(typeOver("b.md"));
    frame("rf5");
    DOCTEST_CHECK(ui.find("md.dialog.rename.error") == nullptr);
    ui.find("md.dialog.rename.confirm.bg")->onClick();
    frame("rf6");
    err = ui.find("md.dialog.rename.error");
    DOCTEST_REQUIRE(err != nullptr);
    DOCTEST_CHECK(err->text.find("重命名失败") != std::string::npos);
    DOCTEST_CHECK(err->text.find("已存在") != std::string::npos);
    DOCTEST_CHECK(ui.find("md.dialog.rename.panel")->opacity > 0.0f);
    DOCTEST_CHECK(fs::exists(env.dir / "a.md"));  // 失败不落盘
    DOCTEST_CHECK(fs::exists(env.dir / "b.md"));

    // 3) 非法路径提交 → 红字「不合法」，弹窗不关
    DOCTEST_CHECK(typeOver("C:/bad"));
    frame("rf7");
    ui.find("md.dialog.rename.confirm.bg")->onClick();
    frame("rf8");
    err = ui.find("md.dialog.rename.error");
    DOCTEST_REQUIRE(err != nullptr);
    DOCTEST_CHECK(err->text.find("不合法") != std::string::npos);
    DOCTEST_CHECK(ui.find("md.dialog.rename.panel")->opacity > 0.0f);

    // 4) 取消后重开 → 残留错误已清（打开清；错误行只随错误态存在）
    ui.find("md.dialog.rename.cancel.bg")->onClick();
    frame("rf9");
    DOCTEST_CHECK_EQ(ui.find("md.dialog.rename.panel")->opacity, 0.0f);
    DOCTEST_CHECK(openRename("a.md"));
    frame("rf10");
    DOCTEST_CHECK(ui.find("md.dialog.rename.error") == nullptr);
    DOCTEST_CHECK_EQ(ui.find("md.dialog.rename.input.text")->text,
                     std::string("a.md"));

    mod->onStop();
    return true;
})

// ---- 审计 perf [39]：状态栏（行:列 · N 字）缓存等价 ----
// 旧实现每次 compose 对全文 O(n) 扫（byteOffsetToLine + utf8Length）；新实现按
// (文本修订, 光标) 缓存。等价锁 = 三种帧的结果都与「独立复算」（同公式、纯函数
// 源自 md_line_scanner）逐字一致：①未变帧（缓存命中）②仅光标移动帧（大纲跳转
// 改 cursor 不改文本）③编辑帧（onChange 改文本）。
REGISTER_TEST(md_module_status_bar_cache_equivalence,
              "markdown_module: 状态栏缓存=独立复算（headless，perf [39]）", {
    const std::string text = "# 标题一\n正文一二三\n\n## 标题二\n正文二\n";
    MdUiEnv env("statuscache");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "note.md", text));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    // 全程同一 page 名：Ui 的 state store 以 pageId 为前缀，换名会让
    // InputState（cursor/verticalScroll）每帧重建成新对象，跳光标/滚动全丢
    eui::Ui ui;
    auto frame = [&](void) {
        ui.begin("sc");
        mod->compose(ui, screen);
        ui.end();
    };
    using IS = components::input_detail::InputModel::InputState;
    // 旧公式的独立复算（与优化前代码同源同参）
    auto oracle = [&](const std::string& t, int cursor) -> std::string {
        using namespace module::markdown;
        const int line = byteOffsetToLine(t, static_cast<size_t>(cursor));
        const size_t lineStart = lineStartOffset(t, line);
        const int col = static_cast<int>(
            utf8Length(t.substr(lineStart,
                               static_cast<size_t>(cursor) - lineStart))) + 1;
        return std::to_string(line + 1) + ":" + std::to_string(col) + " · " +
               std::to_string(utf8Length(t)) + " 字";
    };
    auto statusPos = [&](void) -> std::string {
        const auto* n = ui.find("md.status.pos");
        return n == nullptr ? std::string("<missing>") : n->text;
    };
    auto checkMatchesRecompute = [&](void) -> bool {
        const auto& st = ui.state<IS>(kEditorInputId);
        const std::string o = oracle(st.text, st.cursor);
        const std::string s = statusPos();
        if (s.compare(0, o.size(), o) != 0) {
            std::cerr << "status=[" << s << "] oracle=[" << o << "]"
                      << std::endl;
            return false;
        }
        return true;
    };

    frame();
    const auto* fileRow = ui.find("md.tree.node.1.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    DOCTEST_REQUIRE(fileRow->onClick != nullptr);
    fileRow->onClick();

    frame();   // 打开笔记（编辑态），光标=文末
    DOCTEST_CHECK(checkMatchesRecompute());
    const std::string s2 = statusPos();
    frame();   // 缓存命中帧：与上一帧逐字节一致
    DOCTEST_CHECK_EQ(statusPos(), s2);

    // 仅光标移动：大纲跳转「标题二」（onChange 不触发，修订号不变）
    const auto* toggle = ui.find("md.outline.toggle.hit");
    DOCTEST_REQUIRE(toggle != nullptr);
    DOCTEST_REQUIRE(toggle->onClick != nullptr);
    toggle->onClick();
    frame();
    const auto* h2 = ui.find("md.outline.item.1.row");
    DOCTEST_REQUIRE(h2 != nullptr);
    DOCTEST_REQUIRE(h2->onClick != nullptr);
    h2->onClick();
    frame();
    DOCTEST_CHECK(checkMatchesRecompute());
    DOCTEST_CHECK(statusPos() != s2);   // 行列随光标更新（缓存未冻结旧值）
    const std::string s5 = statusPos();
    frame();
    DOCTEST_CHECK_EQ(statusPos(), s5);

    // 文本编辑：光标处插一个 CJK 码点（字数 +1，行列随 UTF-8 语义变化）
    const auto* hit = ui.find("md.editor.input.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_REQUIRE(hit->onTextInput != nullptr);
    const size_t charsBefore = module::markdown::utf8Length(
        ui.state<IS>(kEditorInputId).text);
    core::TextInputEvent ev;
    ev.text = "汉";
    hit->onTextInput(ev);
    frame();
    DOCTEST_CHECK_EQ(module::markdown::utf8Length(
                         ui.state<IS>(kEditorInputId).text),
                     charsBefore + 1);
    DOCTEST_CHECK(checkMatchesRecompute());
    const std::string s7 = statusPos();
    frame();
    DOCTEST_CHECK_EQ(statusPos(), s7);

    mod->onStop();
    return true;
})

// ---- 审计 perf [14][15]：双栏同步滚动 yA/yB 缓存 + 预览 contentKey 缓存等价 ----
// [14] 旧实现每次 Split compose 两次全文前缀 substr + estimateHeight；新实现按
// (修订, 锚点, 宽度) 缓存 yA/yB。等价锁：编辑器视口滚到深锚点 → 回顶锚点 →
// 预览 scrollOffset 与第一次顶锚点读数逐位相等（缓存被中段覆盖后复算 = 旧值）。
// [15] contentKey 从每帧全文 std::hash 改为 setEditorText 维护的缓存 hash。
// 等价锁：编辑后预览可滚高度（maxOffset，contentKey 变了才会重测）增大且跨帧
// 稳定；未编辑帧读数不动。
REGISTER_TEST(md_module_split_scroll_cache_equivalence,
              "markdown_module: 双栏同步滚动/预览测量缓存等价（headless，perf [14][15]）", {
    std::string text;
    for (int s = 0; s < 8; ++s) {
        text += "# 节" + std::to_string(s) + "\n";
        for (int l = 0; l < 12; ++l) {
            text += "内容行 " + std::to_string(s) + "-" +
                    std::to_string(l) + " 一二三四五六七八九十\n";
        }
        text += "\n";
    }
    MdUiEnv env("splitcache");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "long.md", text));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    // 全程同一 page 名（state store 按 pageId 分键，换名 = 编辑器状态每帧清零）
    eui::Ui ui;
    auto frame = [&](void) {
        ui.begin("pv");
        mod->compose(ui, screen);
        ui.end();
    };

    // 打开笔记 → Ctrl+Shift+P 进 Split（两帧：首帧建宽度、次帧同步滚动稳定）
    frame();
    const auto* fileRow = ui.find("md.tree.node.1.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    DOCTEST_REQUIRE(fileRow->onClick != nullptr);
    fileRow->onClick();
    eui::KeyEvent p;
    p.key = eui::InputKey::P;
    p.modifiers.control = true;
    p.modifiers.shift = true;
    DOCTEST_CHECK(mod->onKeyEvent(p));   // → 实时（M1 第四态）
    DOCTEST_CHECK(mod->onKeyEvent(p));   // → 双栏
    frame();
    frame();
    DOCTEST_CHECK(ui.find(kEditorInputId) != nullptr);
    const auto* pvScroll = ui.find("md.preview.scroll");
    DOCTEST_REQUIRE(pvScroll != nullptr);
    DOCTEST_CHECK(pvScroll->scrollMaxOffset > 0.0f);   // 内容高于视口才可滚

    // 大纲面板常驻打开：跳转顶锚点/深锚点驱动 computeSyncOffset 换锚
    const auto* toggle = ui.find("md.outline.toggle.hit");
    DOCTEST_REQUIRE(toggle != nullptr);
    DOCTEST_REQUIRE(toggle->onClick != nullptr);
    toggle->onClick();
    frame();
    // 元素每帧重建，跨帧持指针悬垂（旧 onClick 调用 = bad_function_call）——
    // 点击一律「当前帧 find + 立即 onClick」
    auto clickOutline = [&](size_t i) -> bool {
        const auto* row = ui.find("md.outline.item." + std::to_string(i) + ".row");
        if (row == nullptr || row->onClick == nullptr) {
            std::cerr << "missing outline row " << i << std::endl;
            return false;
        }
        row->onClick();
        return true;
    };
    DOCTEST_REQUIRE(ui.find("md.outline.item.0.row") != nullptr);
    DOCTEST_REQUIRE(ui.find("md.outline.item.5.row") != nullptr);

    // 顶锚点读数（光标=文首行，视口回顶 → sync offset 归零族）
    clickOutline(0);
    frame();
    frame();
    const float oTop1 = ui.find("md.preview.scroll")->scrollOffset;
    // 缓存命中帧：读数不动
    frame();
    DOCTEST_CHECK_EQ(ui.find("md.preview.scroll")->scrollOffset, oTop1);

    // 跳深锚点（节5）：编辑视口跟随光标下移 → 同步偏移必须变大；
    // 首帧（miss 重算）与次帧（hit 缓存）读数必须逐位相等
    DOCTEST_CHECK(clickOutline(5));
    frame();
    const float oDeepMiss = ui.find("md.preview.scroll")->scrollOffset;
    frame();
    const float oDeepHit = ui.find("md.preview.scroll")->scrollOffset;
    DOCTEST_CHECK(oDeepMiss > oTop1);
    DOCTEST_CHECK_EQ(oDeepHit, oDeepMiss);

    // 回顶锚点：缓存已被中段覆盖，此帧为「重算」——必须与旧顶锚点读数逐位
    // 相等（缓存命中路径与新鲜计算等价的直接证据）
    DOCTEST_CHECK(clickOutline(0));
    frame();
    frame();
    DOCTEST_CHECK_EQ(ui.find("md.preview.scroll")->scrollOffset, oTop1);

    // [15]：编辑加长内容 → contentKey（缓存 hash）变化 → 预览重测高度，
    // maxOffset 增大；未编辑帧（命中）读数稳定
    const float maxBefore = ui.find("md.preview.scroll")->scrollMaxOffset;
    const auto* hit = ui.find("md.editor.input.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_REQUIRE(hit->onTextInput != nullptr);
    std::string big = "\n";
    for (int i = 0; i < 60; ++i) {
        big += "填充行一二三四五六七八九十\n";
    }
    core::TextInputEvent ev;
    ev.text = big;
    hit->onTextInput(ev);
    frame();
    const float maxAfter = ui.find("md.preview.scroll")->scrollMaxOffset;
    DOCTEST_CHECK(maxAfter > maxBefore);
    frame();
    DOCTEST_CHECK_EQ(ui.find("md.preview.scroll")->scrollMaxOffset, maxAfter);

    mod->onStop();
    return true;
})

// ---- P3-④ 双栏同步滚动块模型精确锚（headless）----
// 旧机制（标题锚 + 节内按行摊分）在单节文档里步长恒定；块模型锚给出
// 块级签名。全部断言从模块实时读数反推、不复制实现公式：
//   ① 块内相邻行步长 = H_k/lc_k 按块而异（4 行段 0.8u vs 2 行段 0.67u）
//      —— 全文档单一斜率被证伪
//   ② 块界步长（末行 → 下块首行）− 块内步长 = blockGap，对段落/列表/
//      围栏等异型块界给出同一常数 —— 精确累计 Σ(H_i+gap) 的几何签名，
//      旧机制此值恒为 0
//   ③ 同型块跨 rep 步长一致（累计漂移为零）
//   ④ 文首插入一个与已测 paraA 逐字同源的段落块后，深层未变块首行读数
//      位移 == 插入块高（= s_A×lc_A）+ gap（锚点跟随块几何，不按节摊分漂移）
// 编辑器行高 = editorFontSize()×1.2，与模块 kInputLineHeightFactor 同步。
// 注：REGISTER_TEST 是宏，body 内裸逗号（如 std::map<int, float> 的模板
// 逗号）会被当宏参数分隔——逗号一律藏进括号或类型别名。
namespace {
using IntFloatMap = std::map<int, float>;
} // namespace

REGISTER_TEST(md_module_split_block_anchor_precision,
              "markdown_module: 双栏同步滚动块模型精确锚（headless，Phase 3 ④）", {
    std::string text = "# Section\n\n";
    for (int r = 0; r < 6; ++r) {
        const std::string tag = std::to_string(r);
        text += "para r" + tag + " A one\npara r" + tag + " A two\npara r" + tag +
                " A three\npara r" + tag + " A four\n\n";
        text += "para r" + tag + " B one\npara r" + tag + " B two\n\n";
        text += "- r" + tag + " list one\n- r" + tag + " list two\n\n";
        text += "```\nr" + tag + " f1\nr" + tag + " f2\nr" + tag + " f3\n```\n\n";
    }
    text += "final tail line\n";
    MdUiEnv env("blockanchor");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "anchors.md", text));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    auto frame = [&](void) {
        ui.begin("pv");
        mod->compose(ui, screen);
        ui.end();
    };
    frame();
    const auto* fileRow = ui.find("md.tree.node.1.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    DOCTEST_REQUIRE(fileRow->onClick != nullptr);
    fileRow->onClick();
    eui::KeyEvent p;
    p.key = eui::InputKey::P;
    p.modifiers.control = true;
    p.modifiers.shift = true;
    DOCTEST_CHECK(mod->onKeyEvent(p));   // → 实时（M1 第四态）
    DOCTEST_CHECK(mod->onKeyEvent(p));   // → 双栏
    frame();
    frame();  // 首帧建宽度、次帧同步滚动稳定（同缓存等价用例）
    const auto* pvScroll = ui.find("md.preview.scroll");
    DOCTEST_REQUIRE(pvScroll != nullptr);
    DOCTEST_CHECK(pvScroll->scrollMaxOffset > 0.0f);

    // 块首行表（分块纯函数独立重建，与模块块模型同源）
    const auto blocks = module::markdown::segmentMarkdownBlocks(text);
    DOCTEST_REQUIRE(blocks.size() >= 20);  // 标题 + 4 块/rep × 6 + 尾
    std::vector<int> fl;
    for (const auto& b : blocks) {
        fl.push_back(module::markdown::byteOffsetToLine(text, b.start));
    }

    using IS = components::input_detail::InputModel::InputState;
    const float lineH = core::theme::editorFontSize() * 1.2f;
    IntFloatMap probeCache;
    auto atLine = [&](int line) -> float {
        auto it = probeCache.find(line);
        if (it != probeCache.end()) return it->second;
        auto& st = ui.state<IS>(kEditorInputId);
        st.followCaret = false;  // 不清则组件每帧把滚动拽回光标位（同 md_highlight 用例）
        st.verticalScroll = static_cast<float>(line) * lineH;
        frame();
        const float o = ui.find("md.preview.scroll")->scrollOffset;
        probeCache[line] = o;
        return o;
    };

    // 中段窗口探测（避开顶部 20% 视口余量钳位区与编辑器滚动上限）：
    // 从首个行号 ≥34 的块起连续 4 个块界（paraA→paraB→list→fence→paraA）
    size_t k0 = fl.size();
    for (size_t k = 0; k < fl.size(); ++k) {
        if (fl[k] >= 34) { k0 = k; break; }
    }
    DOCTEST_REQUIRE(k0 + 5 < fl.size());
    const size_t kEnd = k0 + 5;

    std::vector<float> cBoundary;  // 块界步长 − 块内步长（应恒 = blockGap）
    std::vector<float> stepsParaA;
    float stepParaB = 0.0f;
    bool haveParaB = false;
    float sA = 0.0f;      // 首个 paraA（4 内容行 + 前导空行槽）步长与行数
    int lcA = 0;
    std::string aSrc;     // 其块源（④ 逐字复制插入用）
    for (size_t k = k0; k < kEnd; ++k) {
        const int lc = fl[k + 1] - fl[k];
        DOCTEST_REQUIRE(lc >= 2);
        const float oTop = atLine(fl[k]);
        const float oSecond = atLine(fl[k] + 1);
        const float sIn = oSecond - oTop;  // = H_k/lc_k
        const float oLast = atLine(fl[k + 1] - 1);
        const float oNextTop = atLine(fl[k + 1]);
        const float bnd = oNextTop - oLast;  // = H_k/lc_k + gap
        DOCTEST_CHECK(sIn > 1.0f);
        DOCTEST_CHECK(bnd > sIn);            // 界处必跳一个 gap
        cBoundary.push_back(bnd - sIn);
        const std::string src = text.substr(blocks[k].start, blocks[k].end - blocks[k].start);
        // 注意：分块器「前导空行并入块头」→ src 可能以 \n 开头，用 find 判型
        if (lc == 5 && src.find("A one") != std::string::npos) {
            stepsParaA.push_back(sIn);
            if (aSrc.empty()) {
                sA = sIn;
                lcA = lc;
                aSrc = src;
                const size_t nb = aSrc.find_first_not_of('\n');  // 去前导空行
                aSrc.erase(0, nb == std::string::npos ? aSrc.size() : nb);
            }
        }
        if (lc == 3 && src.find("B one") != std::string::npos) {
            stepParaB = sIn;
            haveParaB = true;
        }
        DOCTEST_CHECK(oNextTop > oTop);  // 深层块界严格递增
        DOCTEST_CHECK(oTop > 0.0f);      // 未被 0 钳位
    }
    // ② 所有异型块界的「界步长 − 块内步长」同一常数（旧标题锚恒为 0）
    DOCTEST_REQUIRE(cBoundary.size() >= 5);
    const float cMin = *std::min_element(cBoundary.begin(), cBoundary.end());
    const float cMax = *std::max_element(cBoundary.begin(), cBoundary.end());
    DOCTEST_CHECK(cMin > 1.0f);
    DOCTEST_CHECK(cMax - cMin < 0.1f);
    const float cGap = 0.5f * (cMin + cMax);
    // ① 块内步长按块而异（4 行段 vs 2 行段）
    DOCTEST_REQUIRE(!stepsParaA.empty());
    DOCTEST_CHECK(haveParaB);
    DOCTEST_CHECK(std::fabs(stepsParaA.front() - stepParaB) > 0.5f);
    // ③ 同型块跨 rep 步长一致（累计漂移为零）
    for (float s : stepsParaA) DOCTEST_CHECK(std::fabs(s - stepsParaA.front()) < 0.05f);

    // ④ 文首插入一个与窗口内 paraA 逐字同源的段落块（同内容 ⇒ 同 md4c
    //    解析 ⇒ 同预览高，无需假设单行高）→ 深层目标块（CodeFence，内容
    //    未变）首行读数位移必须 == 插入块高（= s_A×lc_A）+ gap。
    //    旧标题锚机制下位移 = 全局行斜率 × 插入行数，与此值一般不等。
    size_t tgt = blocks.size();
    for (size_t k = 0; k < blocks.size(); ++k) {
        if (blocks[k].kind == module::markdown::MdBlockKind::CodeFence &&
            fl[k] >= 26 && fl[k] <= 40) {
            tgt = k;
            break;
        }
    }
    DOCTEST_REQUIRE(tgt < blocks.size());
    DOCTEST_REQUIRE(!aSrc.empty());
    const float offA = atLine(fl[tgt]);
    DOCTEST_CHECK(offA > 0.0f);
    {
        auto& st = ui.state<IS>(kEditorInputId);
        st.cursor = 0;
        st.selectionStart = 0;
        st.selectionEnd = 0;
    }
    const auto* hit = ui.find("md.editor.input.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_REQUIRE(hit->onTextInput != nullptr);
    core::TextInputEvent ev;
    ev.text = aSrc;  // "para rX A one\n…\npara rX A four\n"（ATX 标题必打断段落）
    hit->onTextInput(ev);
    probeCache.clear();
    frame();
    frame();
    const std::string text2 = aSrc + text;
    DOCTEST_CHECK(ui.state<IS>(kEditorInputId).text == text2);
    const auto blocksNew = module::markdown::segmentMarkdownBlocks(text2);
    DOCTEST_REQUIRE(blocksNew.size() == blocks.size() + 1);
    const std::string targetSrc =
        text.substr(blocks[tgt].start, blocks[tgt].end - blocks[tgt].start);
    size_t tgtNew = blocksNew.size();
    for (size_t k = 0; k < blocksNew.size(); ++k) {
        if (blocksNew[k].kind == blocks[tgt].kind &&
            text2.substr(blocksNew[k].start, blocksNew[k].end - blocksNew[k].start) == targetSrc) {
            tgtNew = k;
            break;
        }
    }
    DOCTEST_REQUIRE(tgtNew < blocksNew.size());
    const int flTgtNew = static_cast<int>(std::count(
        text2.begin(), text2.begin() + static_cast<long>(blocksNew[tgtNew].start), '\n'));
    auto& stPoke = ui.state<IS>(kEditorInputId);
    stPoke.followCaret = false;
    stPoke.verticalScroll = static_cast<float>(flTgtNew) * lineH;
    frame();
    const float offB = ui.find("md.preview.scroll")->scrollOffset;
    const float expectedShift = sA * static_cast<float>(lcA) + cGap;
    DOCTEST_CHECK(std::fabs((offB - offA) - expectedShift) < 0.3f);

    mod->onStop();
    return true;
})

// ---- P3-⑤ 预览块级交互（headless）----
// 直接驱动 hit 覆盖层元素（MouseArea 注册的 onPress/onRelease/onHoverChanged）
// 走完整链路：点击 → MdSyncMap 反查 → InputState.cursor 定位；hover →
// 高亮底衬 / CodeFence bg 混色。tap 语义：传入 bounds 高固定 100，hit 的
// 局部 y = event.y × (H/100)（makeMouseEvent 缩放还原），fracY∈[0,1] 即块
// 内顶→底分数位。CodeFence（自绘块）与 Heading（上游 markdown 块）都验证
// ——⑤ 的覆盖不受上游无 hit API 限制。
REGISTER_TEST(md_module_split_block_click_hover,
              "markdown_module: 预览块点击编辑器定位 + hover 高亮（headless，Phase 3 ⑤）", {
    std::string text = "# T\n\npara one\npara two\n\n```\nfence a\nfence b\nfence c\n```\n\ntail line\n";
    MdUiEnv env("interact");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "inter.md", text));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    auto frame = [&](void) {
        ui.begin("pv");
        mod->compose(ui, screen);
        ui.end();
    };
    frame();
    const auto* fileRow = ui.find("md.tree.node.1.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    DOCTEST_REQUIRE(fileRow->onClick != nullptr);
    fileRow->onClick();
    eui::KeyEvent p;
    p.key = eui::InputKey::P;
    p.modifiers.control = true;
    p.modifiers.shift = true;
    mod->onKeyEvent(p);  // Editor → 实时（M1 第四态）
    mod->onKeyEvent(p);  // → Split（④ 同款通道）
    frame();
    frame();

    // 块序列同源分块；首帧 update 按序发 stableId = 块下标
    const auto blocks = module::markdown::segmentMarkdownBlocks(text);
    DOCTEST_REQUIRE(blocks.size() >= 4);
    size_t kf = blocks.size();
    size_t kh = blocks.size();
    for (size_t k = 0; k < blocks.size(); ++k) {
        if (blocks[k].kind == module::markdown::MdBlockKind::CodeFence) kf = k;
        if (blocks[k].kind == module::markdown::MdBlockKind::Heading) kh = k;
    }
    DOCTEST_REQUIRE(kf < blocks.size());
    DOCTEST_REQUIRE(kh < blocks.size());

    auto findHit = [&](size_t idx) {
        return ui.find("md.preview.blk." + std::to_string(idx) + ".hit");
    };
    const auto* fenceHit = findHit(kf);
    const auto* headHit = findHit(kh);
    DOCTEST_REQUIRE(fenceHit != nullptr);  // 自绘块有 hit 覆盖层
    DOCTEST_REQUIRE(headHit != nullptr);   // 上游 markdown 块同样有（不只 CodeFence）
    DOCTEST_REQUIRE(fenceHit->onPress != nullptr);
    DOCTEST_REQUIRE(fenceHit->onRelease != nullptr);
    DOCTEST_REQUIRE(fenceHit->onHoverChanged != nullptr);
    DOCTEST_REQUIRE(headHit->onPress != nullptr);
    DOCTEST_REQUIRE(headHit->onRelease != nullptr);
    DOCTEST_REQUIRE(headHit->onHoverChanged != nullptr);

    using IS = components::input_detail::InputModel::InputState;
    auto tap = [&](const auto* el, float fracY) {
        // REGISTER_TEST 宏体：逗号藏进括号（C++20 聚合圆括号初始化）
        eui::Rect bounds(0.0, 0.0, 100.0, 100.0);
        eui::PointerEvent down;
        down.action = eui::PointerAction::Press;
        down.x = 50.0;
        down.y = 50.0;
        eui::PointerEvent up;
        up.action = eui::PointerAction::Release;
        up.x = 50.0;
        up.y = static_cast<double>(100.0f * fracY);
        el->onPress(down, bounds);
        el->onRelease(up, bounds);
    };

    // ① 点击 CodeFence 块顶 → 光标 = 块起始字节（``` 开栏行行首）
    tap(fenceHit, 0.0f);
    {
        const auto& st = ui.state<IS>(kEditorInputId);
        DOCTEST_CHECK_EQ(st.cursor, static_cast<int>(blocks[kf].start));
        DOCTEST_CHECK_EQ(st.selectionStart, st.cursor);
        DOCTEST_CHECK_EQ(st.selectionEnd, st.cursor);
    }
    // ② 点击 CodeFence 块底（frac=1 反函数落「下一块首行」→ 钳回本块末行）
    //    → ``` 闭栏行行首；下一帧（followCaret 已滚）光标不被改写
    const int flNext =
        module::markdown::byteOffsetToLine(text, blocks[kf + 1].start);
    const size_t closeByte =
        module::markdown::lineStartOffset(text, flNext - 1);
    DOCTEST_REQUIRE(closeByte > blocks[kf].start);
    tap(fenceHit, 1.0f);
    DOCTEST_CHECK_EQ(ui.state<IS>(kEditorInputId).cursor,
                     static_cast<int>(closeByte));
    frame();
    DOCTEST_CHECK_EQ(ui.state<IS>(kEditorInputId).cursor,
                     static_cast<int>(closeByte));

    // ③ hover：上游 Heading 块底衬 idle 透明 → enter 后 = 主题 hover 色
    const eui::Color hoverTint = core::theme::current().colors.hover;
    const std::string hlHead = "md.preview.blk." + std::to_string(kh) + ".hl";
    frame();
    DOCTEST_CHECK(ui.find(hlHead)->color.a == 0.0f);
    findHit(kh)->onHoverChanged(true);
    frame();
    {
        const auto* hl = ui.find(hlHead);
        DOCTEST_REQUIRE(hl != nullptr);
        DOCTEST_CHECK(std::fabs(hl->color.r - hoverTint.r) < 1e-6f);
        DOCTEST_CHECK(std::fabs(hl->color.g - hoverTint.g) < 1e-6f);
        DOCTEST_CHECK(std::fabs(hl->color.b - hoverTint.b) < 1e-6f);
    }

    // ④ hover CodeFence 自绘块：bg 混色变深；乱序 leave（另一块迟到）不清
    //    当前 hover；匹配 leave 后回到 idle 底色
    const std::string codeBg = "md.preview.blk." + std::to_string(kf) + ".code.bg";
    frame();
    const eui::Color bgIdle = ui.find(codeBg)->color;
    findHit(kf)->onHoverChanged(true);
    frame();
    const auto diffBg = [&](void) {
        const eui::Color c = ui.find(codeBg)->color;
        return std::fabs(c.r - bgIdle.r) + std::fabs(c.g - bgIdle.g) +
               std::fabs(c.b - bgIdle.b);
    };
    DOCTEST_CHECK(diffBg() > 1e-4f);
    findHit(kh)->onHoverChanged(false);
    frame();
    DOCTEST_CHECK(diffBg() > 1e-4f);  // 迟到 leave 不误擦（enter 覆盖语义）
    findHit(kf)->onHoverChanged(false);
    frame();
    DOCTEST_CHECK(diffBg() < 1e-6f);

    mod->onStop();
    return true;
})

// ---- 审计 M5：Esc 关最上层弹窗/菜单（模块级兜底）----
// 删除确认弹窗无输入框 → Esc 修复前无效；右键菜单是盖在弹窗之上的浮层
// （zIndex 1050），Esc 必须先关菜单本身。通道 = Module::onKeyEvent
// （app 级 onKeyEvent → Router 派发到当前页）。
REGISTER_TEST(md_esc_closes_topmost_dialog,
              "markdown_module: Esc 关最上层弹窗/菜单（删除确认无输入框，审计 M5）", {
    MdUiEnv env("escui");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "a.md", "# A\n正文A\n"));

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
    auto escPress = [] {
        eui::KeyEvent e;
        e.key = eui::InputKey::Escape;
        e.action = eui::KeyAction::Press;
        return e;
    };
    auto findRow = [&](const std::string& name) -> const core::dsl::Element* {
        for (int i = 0; i < 8; ++i) {
            const auto* nm = ui.find("md.tree.node." + std::to_string(i) + ".name");
            if (nm != nullptr && nm->text == name) {
                return ui.find("md.tree.node." + std::to_string(i) + ".row");
            }
        }
        return nullptr;
    };

    // 0) 无弹窗：不消费（兜底不越权吃键）
    DOCTEST_CHECK(!mod->onKeyEvent(escPress()));

    // 1) 右键菜单浮层：Esc 只关菜单（level.0 opacity 1 → 0），笔记不删
    frame("esc1");
    const auto* row = findRow("a.md");
    DOCTEST_REQUIRE(row != nullptr);
    DOCTEST_REQUIRE(row->onContextMenu != nullptr);
    eui::PointerEvent pe;
    pe.x = 150.0;
    pe.y = 230.0;
    row->onContextMenu(pe, eui::Rect{});
    frame("esc2");
    DOCTEST_REQUIRE(ui.find("md.ctx.level.0") != nullptr);
    DOCTEST_CHECK_EQ(ui.find("md.ctx.level.0")->opacity, 1.0f);
    DOCTEST_CHECK(mod->onKeyEvent(escPress()));
    frame("esc3");
    DOCTEST_CHECK_EQ(ui.find("md.ctx.level.0")->opacity, 0.0f);

    // 2) 删除确认弹窗（无输入框）：Esc = 取消，弹窗关、文件仍在
    const auto* row2 = findRow("a.md");
    DOCTEST_REQUIRE(row2 != nullptr);
    DOCTEST_REQUIRE(row2->onContextMenu != nullptr);
    row2->onContextMenu(pe, eui::Rect{});
    frame("esc4");
    const auto* delItem = ui.find("md.ctx.level.0.item.1");  // 1=删除笔记
    DOCTEST_REQUIRE(delItem != nullptr);
    DOCTEST_REQUIRE(delItem->onClick != nullptr);
    delItem->onClick();
    frame("esc5");
    DOCTEST_REQUIRE(ui.find("md.dialog.del.panel") != nullptr);
    DOCTEST_CHECK_EQ(ui.find("md.dialog.del.panel")->opacity, 1.0f);
    DOCTEST_CHECK(mod->onKeyEvent(escPress()));
    frame("esc6");
    DOCTEST_CHECK_EQ(ui.find("md.dialog.del.panel")->opacity, 0.0f);
    DOCTEST_CHECK(fs::exists(env.dir / "a.md"));

    mod->onStop();
    return true;
})

// ---- M0 spike V1b（C1 修复）：编程式聚焦的目标必须是 <input id>.hit ----
// input 组件的聚焦判定是 ui.isFocused(id + ".hit")（components/input.h:113），
// 发基 id 时焦点落在 stack 元素上 → 组件判定未聚焦（不画 caret、按键落回模块
// keyEventHandler_，编辑器收不到字）。本用例走真实入口「预览块点击 → 编辑器
// 定位」验证；用 Runtime 逐帧 compose 才能驱动帧末的 requestFocus 消费端
//（core/runtime/runtime_lifecycle.h:38-42），plain Ui 不会消费挂起请求。
namespace {
// 无头帧驱动（pageId 固定，焦点/状态前缀随之固定；Ui 常驻故指针跨帧有效）
struct FocusEnv {
    core::dsl::Runtime rt;
    eui::Ui* ui = nullptr;
    template <typename Fn>
    void frame(Fn&& body) {
        rt.compose("mdfocus", 1600.0f, 1000.0f,
                   [&](eui::Ui& u, const eui::Screen&) {
                       ui = &u;
                       body(u);
                   });
    }
};
} // namespace

REGISTER_TEST(md_editor_focus_target_is_hit,
              "markdown_module: 预览块点击后聚焦 md.editor.input.hit（M0 C1，headless）", {
    const std::string text = "# T\n\npara one\npara two\n";
    MdUiEnv env("focushit");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "focus.md", text));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    FocusEnv env2;
    auto frame = [&] { env2.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    frame();
    const auto* fileRow = env2.ui->find("md.tree.node.1.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    DOCTEST_REQUIRE(fileRow->onClick != nullptr);
    fileRow->onClick();

    // 进双栏态（预览块点击是双栏/预览通路）：循环推进，不依赖「按几次」
    //（默认态与态序随版本演进，见 md_module_split_layout 同款写法）
    eui::KeyEvent p;
    p.key = eui::InputKey::P;
    p.modifiers.control = true;
    p.modifiers.shift = true;
    auto inSplit = [&] {
        return env2.ui->find(kEditorInputId) != nullptr &&
               env2.ui->find("md.preview.blk.0.hit") != nullptr;
    };
    for (int i = 0; i < 4 && !inSplit(); ++i) {
        DOCTEST_REQUIRE(mod->onKeyEvent(p));
        frame();
    }
    DOCTEST_REQUIRE(inSplit());
    const auto* hit = env2.ui->find("md.preview.blk.0.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_REQUIRE(hit->onPress != nullptr);
    DOCTEST_REQUIRE(hit->onRelease != nullptr);

    // 点渲染块（mouseArea 的 onTap = onPress + onRelease，与既有用例同款驱动）
    eui::Rect bounds(0.0, 0.0, 100.0, 100.0);
    eui::PointerEvent down;
    down.action = eui::PointerAction::Press;
    down.button = eui::PointerButton::Left;
    down.x = 50.0;
    down.y = 0.0;
    eui::PointerEvent up;
    up.action = eui::PointerAction::Release;
    up.button = eui::PointerButton::Left;
    up.x = 50.0;
    up.y = 0.0;
    hit->onPress(down, bounds);
    hit->onRelease(up, bounds);
    frame();  // 帧末消费 requestFocus → focusedId_ 本帧更新
    DOCTEST_CHECK(env2.ui->isFocused("md.editor.input.hit"));
    // 反向观测：基 id 不是聚焦目标（修复前发基 id，本行会红）
    DOCTEST_CHECK_FALSE(env2.ui->isFocused(kEditorInputId));
    // 组件按「本帧收到的 focused」画 caret/边框 → 焦点生效的观感在次帧出现
    //（M0 V1：requestFocus 帧末消费，≈16ms 延迟）
    frame();
    DOCTEST_CHECK(env2.ui->isFocused("md.editor.input.hit"));
    // 用户可见后果：组件判定聚焦 → 画 caret
    DOCTEST_CHECK(env2.ui->find("md.editor.input.cursor") != nullptr);

    mod->onStop();
    return true;
})

// ==================== M1：第四态「实时」骨架（只读） ====================

// 四态循环逐态断言可见元素集合 + segmented 视觉顺序/逆映射（与枚举值顺序解耦）
REGISTER_TEST(md_module_live_mode_skeleton,
              "markdown_module: 第四态（实时）骨架 四态循环/元素集合/分段映射（headless）", {
    const std::string text = "# T\n\npara one\n\n```\nfence\n```\n\ntail\n";
    MdUiEnv env("live");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "live.md", text));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    auto frame = [&](void) {
        ui.begin("lv");
        mod->compose(ui, screen);
        ui.end();
    };
    frame();
    const auto* fileRow = ui.find("md.tree.node.1.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    DOCTEST_REQUIRE(fileRow->onClick != nullptr);
    fileRow->onClick();
    frame();

    // 编辑态（默认）：编辑器在、实时/预览列都不在
    DOCTEST_CHECK(ui.find(kEditorInputId) != nullptr);
    DOCTEST_CHECK(ui.find("md.live.scroll") == nullptr);
    DOCTEST_CHECK(ui.find("md.preview.scroll") == nullptr);

    // segmented 视觉顺序 = 编辑 | 实时 | 双栏 | 预览（枚举值是 0/1/2/3 = 编辑/
    // 双栏/预览/实时，故顺序必须经映射表，不能按枚举值排）
    DOCTEST_REQUIRE(ui.find("md.mode.label.0") != nullptr);
    DOCTEST_CHECK_EQ(ui.find("md.mode.label.0")->text, std::string("编辑"));
    DOCTEST_CHECK_EQ(ui.find("md.mode.label.1")->text, std::string("实时"));
    DOCTEST_CHECK_EQ(ui.find("md.mode.label.2")->text, std::string("双栏"));
    DOCTEST_CHECK_EQ(ui.find("md.mode.label.3")->text, std::string("预览"));

    eui::KeyEvent p;
    p.key = eui::InputKey::P;
    p.modifiers.control = true;
    p.modifiers.shift = true;

    // → 实时（第四态）：单栏块级渲染；无 input，也不出现预览列
    DOCTEST_CHECK(mod->onKeyEvent(p));
    frame();
    DOCTEST_REQUIRE(ui.find("md.live.scroll") != nullptr);
    DOCTEST_CHECK(ui.find("md.live.bg") != nullptr);
    DOCTEST_CHECK(ui.find("md.live.blocks") != nullptr);
    DOCTEST_CHECK(ui.find("md.live.blk.0.hit") != nullptr);  // 首块渲染 + hit 覆盖层
    DOCTEST_CHECK(ui.find(kEditorInputId) == nullptr);
    DOCTEST_CHECK(ui.find("md.preview.scroll") == nullptr);
    // 左树 / 顶栏动作 / 状态栏 / segmented：四态共享（零改动）
    DOCTEST_CHECK(ui.find("md.tree.bg") != nullptr);
    DOCTEST_CHECK(ui.find("md.actions") != nullptr);
    DOCTEST_CHECK(ui.find("md.status.bg") != nullptr);
    DOCTEST_CHECK(ui.find("md.mode") != nullptr);

    // → 双栏
    DOCTEST_CHECK(mod->onKeyEvent(p));
    frame();
    DOCTEST_CHECK(ui.find(kEditorInputId) != nullptr);
    DOCTEST_CHECK(ui.find("md.preview.scroll") != nullptr);
    DOCTEST_CHECK(ui.find("md.live.scroll") == nullptr);

    // → 预览
    DOCTEST_CHECK(mod->onKeyEvent(p));
    frame();
    DOCTEST_CHECK(ui.find(kEditorInputId) == nullptr);
    DOCTEST_CHECK(ui.find("md.preview.scroll") != nullptr);
    DOCTEST_CHECK(ui.find("md.live.scroll") == nullptr);

    // → 编辑（循环回起点）
    DOCTEST_CHECK(mod->onKeyEvent(p));
    frame();
    DOCTEST_CHECK(ui.find(kEditorInputId) != nullptr);
    DOCTEST_CHECK(ui.find("md.live.scroll") == nullptr);
    DOCTEST_CHECK(ui.find("md.preview.scroll") == nullptr);

    // segmented 点击走逆映射（不是 static_cast）：第 2 段 = 实时，不是双栏
    const auto* segLive = ui.find("md.mode.hit.1");
    DOCTEST_REQUIRE(segLive != nullptr);
    DOCTEST_REQUIRE(segLive->onClick != nullptr);
    segLive->onClick();
    frame();
    DOCTEST_CHECK(ui.find("md.live.scroll") != nullptr);
    DOCTEST_CHECK(ui.find(kEditorInputId) == nullptr);

    mod->onStop();
    return true;
})

// 持久化：mode=3（实时）通过净化与存读往返；旧文件 mode 0-2 语义不变；越界回落 0
REGISTER_TEST(md_view_state_mode_live,
              "markdown: 视图状态 mode 3（实时）通过净化 + 0-2 旧语义不变（M1）", {
    using module::markdown::MarkdownViewState;
    using module::markdown::MarkdownViewStateStore;
    const fs::path file =
        test_temp::tempPath("devdesk_md_view_state_live.json");
    std::error_code ec;
    fs::remove(file, ec);

    // 旧版落盘文件（同格式 JSON）→ mode 0-2 语义完全不变
    for (int m = 0; m <= 2; ++m) {
        {
            std::ofstream f(file, std::ios::trunc);
            f << "{\"lastPath\": \"old.md\", \"mode\": " << m
              << ", \"collapsedDirs\": []}";
        }
        const MarkdownViewState loaded = MarkdownViewStateStore(file).load();
        DOCTEST_CHECK_EQ(loaded.mode, m);
        DOCTEST_CHECK_EQ(loaded.lastPath, std::string("old.md"));
    }

    // mode=3（Live）在合法域内：净化保留 + 存读往返
    MarkdownViewState live;
    live.mode = 3;
    DOCTEST_CHECK_EQ(MarkdownViewStateStore::sanitized(live).mode, 3);
    {
        MarkdownViewStateStore store(file);
        DOCTEST_CHECK(store.save(live).isOk());
    }
    DOCTEST_CHECK_EQ(MarkdownViewStateStore(file).load().mode, 3);

    // 越界回落 0（新域上界为 3）
    MarkdownViewState over;
    over.mode = 4;
    DOCTEST_CHECK_EQ(MarkdownViewStateStore::sanitized(over).mode, 0);
    MarkdownViewState neg;
    neg.mode = -1;
    DOCTEST_CHECK_EQ(MarkdownViewStateStore::sanitized(neg).mode, 0);

    fs::remove(file, ec);
    return true;
})

// ==================== M2：实时态活动块单输入 + 内容贴合高度 ====================

namespace {

using MdInputState = components::input_detail::InputModel::InputState;

// 无头驱动：Runtime 逐帧 compose（requestFocus 的帧末消费端在
// Runtime::compose，plain Ui 不消费挂起请求）。pageId 恒定 —— state / 焦点 /
// 元素 id 都以 pageId 起头做命名空间，换名等于换命名空间。
struct MdLiveEnv {
    core::dsl::Runtime rt;
    eui::Ui* ui = nullptr;

    template <typename Fn>
    void frame(Fn&& body) {
        rt.compose("mdlive", 1600.0f, 1000.0f,
                   [&](eui::Ui& u, const eui::Screen&) {
                       ui = &u;
                       body(u);
                   });
    }
};

// 本帧渲染出的块包裹 id 列表（prefix = "md.live" / "md.preview"）——
// 用于比较两种模式下「块序列」是否一致
std::vector<int> renderedBlockIds(eui::Ui& ui, const std::string& prefix) {
    std::vector<int> ids;
    for (int i = 0; i < 16; ++i) {
        if (ui.find(prefix + ".blk." + std::to_string(i)) != nullptr) {
            ids.push_back(i);
        }
    }
    return ids;
}

// 读回磁盘内容（保存链路 = 模块既有 400ms 防抖 + service_->write 整篇写）
bool readFileBytes(const fs::path& path, std::string* out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    ifs.seekg(0, std::ios::end);
    const std::streamoff n = ifs.tellg();
    if (n < 0) return false;
    out->resize(static_cast<size_t>(n));
    ifs.seekg(0, std::ios::beg);
    if (n > 0) ifs.read(&(*out)[0], n);
    return !ifs.fail();
}

// 打开左栏首个文件（单文件目录：node.0 = 根目录组，node.1 = 文件行）
bool openFirstNote(eui::Ui& ui) {
    const auto* row = ui.find("md.tree.node.1.row");
    if (row == nullptr || row->onClick == nullptr) return false;
    row->onClick();
    return true;
}

// Ctrl+Shift+P（四态循环推进一次）
bool cycleMode(::core::Module* mod) {
    eui::KeyEvent p;
    p.key = eui::InputKey::P;
    p.modifiers.control = true;
    p.modifiers.shift = true;
    return mod->onKeyEvent(p);
}

} // namespace

// M2 验收（spec §8.2 #4 + M0 C1/C4）：活动块 input 就位、内容贴合高度、
// 不吃外层滚轮、聚焦打 .hit、非活动块仍是只读渲染
REGISTER_TEST(md_module_live_active_input,
              "markdown_module: 实时态活动块 input 就位 + 内容贴合高度（M2，headless）", {
    const std::string text = "# T\n\npara one\npara two\npara three\n";
    MdUiEnv env("liveinput");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "live2.md", text));

    // 期望值口径 = 块模型（与实现同源）：把文档光标放在第二块块首
    module::markdown::MdBlockModel model;
    model.update(text);
    DOCTEST_REQUIRE(model.blocks().size() >= 2);
    const size_t cursorByte = model.blocks()[1].start;
    const size_t k = model.blockIndexAt(cursorByte);
    const std::string blockText =
        text.substr(model.blocks()[k].start,
                    model.blocks()[k].end - model.blocks()[k].start);
    DOCTEST_CHECK(k == 1);

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    MdLiveEnv e;
    auto frame = [&] { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);
    // 编辑态放入文档光标（进实时态时作为活动块锚的初始来源）
    e.ui->state<MdInputState>(kEditorInputId).cursor =
        static_cast<int>(cursorByte);

    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();
    DOCTEST_REQUIRE(e.ui->find("md.live.scroll") != nullptr);
    DOCTEST_CHECK(e.ui->find(kEditorInputId) == nullptr);
    DOCTEST_CHECK(e.ui->find("md.preview.scroll") == nullptr);

    // ① 活动块源码态 input 就位：内容 = 活动块源码（逐字节）
    const auto* input = e.ui->find("md.live.input");
    DOCTEST_REQUIRE(input != nullptr);
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text, blockText);

    // ② 内容贴合高度（spec §8.2 #4）：同一 measureLines + 同字体族/字号/文本宽
    //    （inset=10、未开滚动条 → 文本宽 = 声明宽 − 20），容差 ±1px
    const float fs = core::theme::editorFontSize();
    const float textW = std::max(0.0f, input->frame.width - 20.0f);
    const int lines = static_cast<int>(
        components::input_detail::InputModel::measureLines(
            blockText, "Microsoft YaHei", fs, textW).size());
    const float expectH = 20.0f + static_cast<float>(lines) * fs * 1.2f + 1.0f;
    DOCTEST_CHECK(std::fabs(input->frame.height - expectH) <= 1.0f);

    // ③ 真不变量（M0 C4）：贴合 ⇒ 组件不注册滚轮处理器（不吃外层滚轮、无内部
    //    滚动条）。偏矮一像素都会让本行变红 —— 这就是 §5 #4 日志校验的判据
    const auto* hit = e.ui->find("md.live.input.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_CHECK(hit->onScroll == nullptr);

    // ④ M0 C1：聚焦目标是 .hit（组件判定 = ui.isFocused(id + ".hit")）
    DOCTEST_CHECK(e.ui->isFocused("md.live.input.hit"));

    // ⑤ input 建在活动块包裹内：包裹高 = 贴合高（不是渲染块高 H_k）
    const auto* wrap = e.ui->find("md.live.blk.1");
    DOCTEST_REQUIRE(wrap != nullptr);
    DOCTEST_CHECK(std::fabs(wrap->frame.height - input->frame.height) <= 0.01f);
    DOCTEST_CHECK(input->frame.y >= wrap->frame.y - 0.5f);
    DOCTEST_CHECK(input->frame.y <= wrap->frame.y + wrap->frame.height + 0.5f);
    // 活动块的 hit 覆盖层仍建（元素集合稳定）但关掉命中 —— 否则它建在 input
    // 之后会抢走指针（打字点不进去）
    const auto* activeHit = e.ui->find("md.live.blk.1.hit");
    DOCTEST_REQUIRE(activeHit != nullptr);
    DOCTEST_CHECK(activeHit->hitTestMode == core::dsl::HitTestMode::None);
    // 非活动块的覆盖层仍是正常命中（hover / M3 点击定位）
    const auto* otherHit = e.ui->find("md.live.blk.0.hit");
    DOCTEST_REQUIRE(otherHit != nullptr);
    DOCTEST_CHECK(otherHit->hitTestMode != core::dsl::HitTestMode::None);
    // 非活动块仍是只读渲染（渲染内容在，input 只此一个）
    DOCTEST_CHECK(e.ui->find("md.live.block.0") != nullptr);

    mod->onStop();
    return true;
})

// M2 验收（spec §8.2 #3）：实时态上行回写 == 编辑态做同一编辑（逐字节），
// 且两种模式下块序列一致
REGISTER_TEST(md_module_live_input_roundtrip,
              "markdown_module: 实时态上行回写 == 编辑态同编辑（逐字节，M2）", {
    const std::string text = "# T\n\npara one\npara two\n";
    MdUiEnv env("liveround");
    DOCTEST_REQUIRE(env.db != nullptr);

    module::markdown::MdBlockModel model;
    model.update(text);
    DOCTEST_REQUIRE(model.blocks().size() >= 2);
    const size_t cursorByte = model.blocks()[1].start;
    const std::string expect =
        text.substr(0, cursorByte) + "Z" + text.substr(cursorByte);

    // 自动保存是 400ms 防抖 + 每帧 flush（停止输入后写盘）：睡过阈值再走一帧
    // 让模块 compose（flush 在 compose 内）→ 读回磁盘做逐字节比对
    auto idleFlush = [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(520));
    };

    // ---- 路径 A：实时态打字（真实 input 事件 → onChange → 拼文档 → 写盘）----
    std::string liveDoc;
    std::vector<int> liveBlocks;
    {
        DOCTEST_REQUIRE(writeTextFile(env.dir / "rt.md", text));
        std::unique_ptr<::core::Module> mod(create_markdown_module());
        mod->onStart();
        eui::Screen screen;
        screen.width = 1600.0f;
        screen.height = 1000.0f;
        MdLiveEnv e;
        auto frame = [&](void) {
            e.frame([&](eui::Ui& u) { mod->compose(u, screen); });
        };
        frame();
        DOCTEST_REQUIRE(openFirstNote(*e.ui));
        frame();
        DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);
        e.ui->state<MdInputState>(kEditorInputId).cursor =
            static_cast<int>(cursorByte);
        DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
        frame();
        const auto* hit = e.ui->find("md.live.input.hit");
        DOCTEST_REQUIRE(hit != nullptr);
        DOCTEST_REQUIRE(hit->onTextInput != nullptr);
        // 换文本帧反向写入：活动 input 的块内光标落在活动块块首
        DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").cursor, 0);
        core::TextInputEvent ev;
        ev.text = "Z";
        hit->onTextInput(ev);
        frame();
        // onChange 上行 + 分块器重算：活动 input 文本已含新字符
        DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                         std::string("Z") +
                             text.substr(model.blocks()[1].start,
                                         model.blocks()[1].end -
                                             model.blocks()[1].start));
        idleFlush();
        frame();   // 本帧 compose 内 flushEditorIfIdle → 整篇写盘
        DOCTEST_REQUIRE(readFileBytes(env.dir / "rt.md", &liveDoc));
        // 块序列（实时态渲染出的块包裹集合）
        liveBlocks = renderedBlockIds(*e.ui, "md.live");
        mod->onStop();
    }

    // ---- 路径 B：编辑态做同一编辑（同文档偏移、同插入文本）----
    std::string editDoc;
    std::vector<int> editBlocks;
    {
        DOCTEST_REQUIRE(writeTextFile(env.dir / "rt.md", text));  // 复位文档
        // 路径 A 退出时处于实时态 → 视图状态文件会把 mode=3 带到下一次启动
        //（M1 起四态都持久化）。本段要在【编辑态】做对照编辑，故清掉状态文件
        {
            std::error_code ec;
            fs::remove(env.dir / "view_state.json", ec);
        }
        std::unique_ptr<::core::Module> mod(create_markdown_module());
        mod->onStart();
        eui::Screen screen;
        screen.width = 1600.0f;
        screen.height = 1000.0f;
        MdLiveEnv e;
        auto frame = [&](void) {
            e.frame([&](eui::Ui& u) { mod->compose(u, screen); });
        };
        frame();
        DOCTEST_REQUIRE(openFirstNote(*e.ui));
        frame();
        const auto* hit = e.ui->find("md.editor.input.hit");
        DOCTEST_REQUIRE(hit != nullptr);
        DOCTEST_REQUIRE(hit->onTextInput != nullptr);
        e.ui->state<MdInputState>(kEditorInputId).cursor =
            static_cast<int>(cursorByte);
        core::TextInputEvent ev;
        ev.text = "Z";
        hit->onTextInput(ev);
        frame();
        idleFlush();
        frame();   // 本帧 compose 内 flushEditorIfIdle → 整篇写盘
        DOCTEST_REQUIRE(readFileBytes(env.dir / "rt.md", &editDoc));
        // 收块序列：切到预览态（与实时态同一套逐块渲染通路）
        DOCTEST_REQUIRE(cycleMode(mod.get()));   // 实时
        frame();
        DOCTEST_REQUIRE(cycleMode(mod.get()));   // 双栏
        frame();
        DOCTEST_REQUIRE(cycleMode(mod.get()));   // 预览
        frame();
        DOCTEST_REQUIRE(e.ui->find("md.preview.scroll") != nullptr);
        editBlocks = renderedBlockIds(*e.ui, "md.preview");
        mod->onStop();
    }

    // ① 文档逐字节相等（实时回写 == 编辑态同编辑），且等于期望文档
    DOCTEST_CHECK_EQ(liveDoc, editDoc);
    // 失败时打出两侧原文（逐字节用例的差异定位）
    DOCTEST_CHECK_MESSAGE(liveDoc == expect,
                          "live=[" + liveDoc + "] expect=[" + expect + "]");
    // ② 块序列一致（实时 vs 预览，同一文档同一分块器）
    DOCTEST_CHECK(liveBlocks == editBlocks);
    return true;
})

// ==================== MD-01：实时态活动块锚必须跟随真实光标 ====================
// （审计 2026-09-27-full-audit-markdown.json MD-01，高危）

namespace {

// 实时态下在活动块内造出【新块边界】（回车造成的块间空行 / 行首 `# ` / `- `
// 这类必然打断段落的构造）后，分块器把文档拆成两块 —— 活动块锚必须跟着真实
// 光标落到【新块】：输入框文本 = 新块文本、块内光标映射回同一文档字节、后续
// 按键落在该字节处。锚冻结在切块前的旧值上时（MD-01），光标字节仍落在【前一
// 块】区间内 → 下一帧 input 被整段换成前一块文本（用户刚敲的内容从输入框消
// 失）、光标跳回前块，之后每次按键都写进前块。
//   tag        : 临时笔记目录标记
//   text       : 初始文档（单个块）
//   cursorByte : 进实时态前的编辑器文档光标（= 活动块锚的初始来源）
//   typed      : 逐次 onTextInput 的文本（走真实输入链路，每个事件一帧）
bool liveSplitAnchorScenario(const char* tag,
                             const std::string& text,
                             size_t cursorByte,
                             const std::vector<std::string>& typed) {
    MdUiEnv env(tag);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "split.md", text));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    MdLiveEnv e;
    auto frame = [&](void) {
        e.frame([&](eui::Ui& u) { mod->compose(u, screen); });
    };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);
    e.ui->state<MdInputState>(kEditorInputId).cursor =
        static_cast<int>(cursorByte);
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();
    DOCTEST_REQUIRE(e.ui->find("md.live.input") != nullptr);

    // 逐次真实 onTextInput，每个事件走一帧：onChange → 拼回整篇 → 分块器重切
    std::string joined;
    for (const std::string& ch : typed) {
        const auto* hit = e.ui->find("md.live.input.hit");
        DOCTEST_REQUIRE(hit != nullptr);
        DOCTEST_REQUIRE(hit->onTextInput != nullptr);
        core::TextInputEvent ev;
        ev.text = ch;
        hit->onTextInput(ev);
        joined += ch;
        frame();
    }

    // 期望口径 = 分块器（与实现同源，照 M2 用例做法）：依次在光标处插入等价
    // 于把 joined 一次性插到初始光标处，光标随之推进到插入内容末尾
    const size_t caret = cursorByte + joined.size();
    const std::string doc =
        text.substr(0, cursorByte) + joined + text.substr(cursorByte);
    module::markdown::MdBlockModel model;
    model.update(doc);
    DOCTEST_REQUIRE(model.blocks().size() >= 2);   // 本场景必须真切出了新块
    const size_t k = model.blockIndexAt(caret);
    const module::markdown::MdBlock& blk = model.blocks()[k];
    const std::string blockText = doc.substr(blk.start, blk.end - blk.start);
    const size_t local = caret - blk.start;

    // ① 输入框文本 = 光标所在【新块】文本（锚冻结时这里是前一块）
    const std::string got = e.ui->state<MdInputState>("md.live.input").text;
    DOCTEST_CHECK_MESSAGE(got == blockText,
                          "活动块 " + std::to_string(k) + " 期望=[" + blockText +
                              "] 实际=[" + got + "] 光标字节=" +
                              std::to_string(caret) + " 文档=[" + doc + "]");
    // ② 块内光标 = 该文档字节对应的块内偏移（锚跟着真实光标走，不是旧值）
    DOCTEST_CHECK_EQ(
        static_cast<size_t>(
            e.ui->state<MdInputState>("md.live.input").cursor),
        local);
    // ③ 贴合高度未回归（M2「[live] input over=0」判据：不吃外层滚轮）
    const auto* hit = e.ui->find("md.live.input.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_CHECK(hit->onScroll == nullptr);

    // ④ 后续按键落在正确位置：再打一个字符，它必须插在块内光标处（= 文档字节
    //    caret 处），而不是被写进前一块。注意这一击可能再次改变分块（回车造的
    //    块间空行被该字符填成普通行 → 两块并回一块），故期望值仍按「与实现同
    //    源」的口径现算：文档 = caret 处插 X，活动块/块内光标 = 新光标字节经
    //    分块器推出
    core::TextInputEvent ev;
    ev.text = "X";
    hit->onTextInput(ev);
    frame();
    const size_t caret2 = caret + 1;
    const std::string doc2 = doc.substr(0, caret) + "X" + doc.substr(caret);
    module::markdown::MdBlockModel model2;
    model2.update(doc2);
    const size_t k2 = model2.blockIndexAt(caret2);
    const module::markdown::MdBlock& blk2 = model2.blocks()[k2];
    const std::string blockText2 =
        doc2.substr(blk2.start, blk2.end - blk2.start);
    const std::string gotAfter = e.ui->state<MdInputState>("md.live.input").text;
    DOCTEST_CHECK_MESSAGE(gotAfter == blockText2,
                          "打字后活动块 " + std::to_string(k2) + " 期望=[" +
                              blockText2 + "] 实际=[" + gotAfter + "]");
    DOCTEST_CHECK_EQ(
        blk2.start + static_cast<size_t>(
                         e.ui->state<MdInputState>("md.live.input").cursor),
        caret2);
    // ⑤ 端到端逐字节：400ms 防抖 + 每帧 flush 后读回磁盘 = 文档 caret 处插 X
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    frame();   // 本帧 compose 内 flushEditorIfIdle → 整篇写盘
    std::string saved;
    DOCTEST_REQUIRE(readFileBytes(env.dir / "split.md", &saved));
    DOCTEST_CHECK_MESSAGE(saved == doc2,
                          "落盘期望=[" + doc2 + "] 实际=[" + saved + "]");
    mod->onStop();
    return true;
}

} // namespace

// MD-01 入口 1：块内按回车 → 块间空行 → 分块器拆两块
REGISTER_TEST(md_module_live_split_anchor_enter,
              "markdown_module: 实时态回车造块边界后锚跟随光标（MD-01 回车入口）", {
    return liveSplitAnchorScenario("livesplitnl", "para one\npara two\n", 8,
                                   {"\n"});
})

// MD-01 入口 2：新行首打 `# ` → ATX 标题必打断段落
REGISTER_TEST(md_module_live_split_anchor_heading,
              "markdown_module: 实时态行首 `# ` 造块边界后锚跟随光标（MD-01）", {
    return liveSplitAnchorScenario("livesplithd", "para one\npara two\n", 9,
                                   {"#", " "});
})

// MD-01 入口 3：新行首打 `- ` → 列表标记可打断段落
REGISTER_TEST(md_module_live_split_anchor_list,
              "markdown_module: 实时态行首 `- ` 造块边界后锚跟随光标（MD-01）", {
    return liveSplitAnchorScenario("livesplitls", "para one\npara two\n", 9,
                                   {"-", " "});
})

// ==================== W2 波 2：MD-02 / MD-05 回归 ====================

namespace {

// 实时态长文档：内容高度必须大于视口高度，滚动偏移才有作用面——否则
//「换笔记后偏移为 0」可能是被 maxOffset 钳出来的假绿
std::string tallNoteDoc(const char* marker, int paragraphs) {
    std::string doc;
    for (int i = 0; i < paragraphs; ++i) {
        doc += std::string(marker) + " paragraph " + std::to_string(i) + "\n\n";
    }
    return doc;
}

// 树行（按文件名找，避免同秒 mtime 下根组内顺序不稳定）
const core::dsl::Element* findFileRowByName(eui::Ui& ui,
                                            const std::string& name) {
    for (int i = 0; i < 8; ++i) {
        const auto* nm = ui.find("md.tree.node." + std::to_string(i) + ".name");
        if (nm != nullptr && nm->text == name) {
            return ui.find("md.tree.node." + std::to_string(i) + ".row");
        }
    }
    return nullptr;
}

} // namespace

// MD-05：换笔记 = 实时态状态群换代。修复前这里只复位活动块锚，liveOffset_
// 残留上一篇的滚动位置（换笔记后仍停在上篇的偏移上、与新旧内容高度错配）
REGISTER_TEST(md_module_live_state_reset_note_switch,
              "markdown_module: 实时态换笔记复位滚动位置（W2 MD-05，headless）", {
    const std::string docA = tallNoteDoc("A", 60);
    const std::string docB = tallNoteDoc("B", 60);
    MdUiEnv env("livereset");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "a.md", docA));
    DOCTEST_REQUIRE(writeTextFile(env.dir / "b.md", docB));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    MdLiveEnv e;
    auto frame = [&](void) {
        e.frame([&](eui::Ui& u) { mod->compose(u, screen); });
    };

    frame();
    const auto* rowA = findFileRowByName(*e.ui, "a.md");
    DOCTEST_REQUIRE(rowA != nullptr);
    DOCTEST_REQUIRE(rowA->onClick != nullptr);
    rowA->onClick();
    frame();
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();

    const auto* scroll = e.ui->find("md.live.scroll");
    DOCTEST_REQUIRE(scroll != nullptr);
    DOCTEST_CHECK_EQ(scroll->scrollOffset, 0.0f);
    DOCTEST_REQUIRE(scroll->scrollMaxOffset >= 200.0f);   // 可滚（长文档前提）

    // 手动滚动：onChange 是模块写 liveOffset_ 的唯一入口
    DOCTEST_REQUIRE(scroll->onScrollOffsetChanged != nullptr);
    scroll->onScrollOffsetChanged(200.0f);
    frame();
    DOCTEST_CHECK_EQ(e.ui->find("md.live.scroll")->scrollOffset, 200.0f);

    // 换笔记 → 滚动位置随新文档归零（修复前滞留 200）
    const auto* rowB = findFileRowByName(*e.ui, "b.md");
    DOCTEST_REQUIRE(rowB != nullptr);
    DOCTEST_REQUIRE(rowB->onClick != nullptr);
    rowB->onClick();
    frame();
    const auto* scrollB = e.ui->find("md.live.scroll");
    DOCTEST_REQUIRE(scrollB != nullptr);
    DOCTEST_REQUIRE(scrollB->scrollMaxOffset >= 200.0f);  // 新文档同样可滚
    DOCTEST_CHECK_EQ(scrollB->scrollOffset, 0.0f);

    mod->onStop();
    return true;
})

// MD-05 第二条路径：切实时态与换笔记共用同一复位点（偏移归零 + 首帧聚焦）
REGISTER_TEST(md_module_live_state_reset_on_enter,
              "markdown_module: 切实时态复位状态位群 + 首帧聚焦（W2 MD-05）", {
    MdUiEnv env("liveenter");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "a.md", tallNoteDoc("E", 60)));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    MdLiveEnv e;
    auto frame = [&](void) {
        e.frame([&](eui::Ui& u) { mod->compose(u, screen); });
    };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();
    const auto* scroll = e.ui->find("md.live.scroll");
    DOCTEST_REQUIRE(scroll != nullptr);
    DOCTEST_REQUIRE(scroll->scrollMaxOffset >= 200.0f);
    DOCTEST_REQUIRE(scroll->onScrollOffsetChanged != nullptr);
    scroll->onScrollOffsetChanged(200.0f);
    frame();
    DOCTEST_CHECK_EQ(e.ui->find("md.live.scroll")->scrollOffset, 200.0f);

    // 切走（实时 → 双栏 → 预览 → 编辑）
    for (int i = 0; i < 3; ++i) {
        DOCTEST_REQUIRE(cycleMode(mod.get()));
        frame();
    }
    DOCTEST_CHECK(e.ui->find("md.live.scroll") == nullptr);

    // 切回 → 同一 enterLiveState：偏移归零 + 首帧同帧补发 requestFocus（M0 C1）
    DOCTEST_REQUIRE(cycleMode(mod.get()));
    frame();
    const auto* back = e.ui->find("md.live.scroll");
    DOCTEST_REQUIRE(back != nullptr);
    DOCTEST_CHECK_EQ(back->scrollOffset, 0.0f);
    DOCTEST_CHECK(e.ui->isFocused("md.live.input.hit"));

    mod->onStop();
    return true;
})

// MD-05 第三条路径：启动恢复进实时态（loadViewState）与切实时态同语义——
// 修复前这条路径完全不复位状态位群（首帧不聚焦活动 input）
REGISTER_TEST(md_module_live_state_reset_on_restore,
              "markdown_module: 启动恢复实时态走同一复位点（W2 MD-05）", {
    MdUiEnv env("liverestore");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "a.md", tallNoteDoc("R", 60)));
    // 预置视图状态：mode=3（实时）+ 上次笔记，onStart 尾部恢复
    {
        std::ofstream f(env.dir / "view_state.json", std::ios::trunc);
        f << "{\"lastPath\": \"a.md\", \"mode\": 3, \"collapsedDirs\": []}";
        DOCTEST_REQUIRE(f.good());
    }

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    MdLiveEnv e;
    auto frame = [&](void) {
        e.frame([&](eui::Ui& u) { mod->compose(u, screen); });
    };

    frame();
    DOCTEST_REQUIRE(e.ui->find("md.live.scroll") != nullptr);  // 恢复了实时态
    DOCTEST_CHECK(e.ui->find(kEditorInputId) == nullptr);
    DOCTEST_CHECK_EQ(e.ui->find("md.live.scroll")->scrollOffset, 0.0f);
    DOCTEST_CHECK(e.ui->isFocused("md.live.input.hit"));       // 首帧聚焦

    mod->onStop();
    return true;
})

// MD-02：编辑态与实时态的列壳是同一份实现（drawTextColumnChrome）——同屏宽下
// 两态文本列几何必须逐项相同；两个 clampColumn 分支（编辑态单栏钳 720 居中 /
// 双栏态不钳）保持原样。本用例是抽出后「几何零变化」的回归锁。
REGISTER_TEST(md_module_text_column_chrome_shared,
              "markdown_module: 编辑/实时文本列壳几何逐项相同（W2 MD-02）", {
    MdUiEnv env("textchrome");
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "a.md", "# A\n\nbody\n"));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    auto frame = [&](void) {
        ui.begin("tc");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);
    };
    const float pad = 40.0f;
    const float sideW = 240.0f;
    const float gap = 16.0f;

    // (1) 空态（未选笔记）：提示条 + 状态栏，chip 行不画
    frame();
    DOCTEST_CHECK(ui.find("md.editor.hint") != nullptr);
    DOCTEST_CHECK(ui.find("md.tags.add.hit") == nullptr);
    DOCTEST_CHECK(ui.find("md.status.bg") != nullptr);

    // (2) 打开笔记 → 编辑态（单栏：文本列钳 720 居中）
    const auto* row = findFileRowByName(ui, "a.md");
    DOCTEST_REQUIRE(row != nullptr);
    DOCTEST_REQUIRE(row->onClick != nullptr);
    row->onClick();
    frame();
    const auto* edCol = ui.find("md.editor");
    const auto* edInput = ui.find(kEditorInputId);
    DOCTEST_REQUIRE(edCol != nullptr);
    DOCTEST_REQUIRE(edInput != nullptr);
    DOCTEST_CHECK(ui.find("md.tags.add.hit") != nullptr);   // chip 行在
    const float fullW = screen.width - pad * 2.0f - sideW - gap;
    DOCTEST_CHECK_EQ(edInput->frame.width, 720.0f);
    DOCTEST_CHECK_EQ(edInput->frame.x - edCol->frame.x,
                     (fullW - 720.0f) * 0.5f);
    DOCTEST_CHECK_EQ(edInput->frame.y - edCol->frame.y, 48.0f);
    const float edX = edInput->frame.x;
    const float edY = edInput->frame.y;
    const float edW = edInput->frame.width;
    const float edH = edInput->frame.height;

    // (3) 切实时态：同屏宽下几何逐项相同（同一列壳；列槽位也不变）
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // 编辑 → 实时
    frame();
    const auto* live = ui.find("md.live.scroll");
    DOCTEST_REQUIRE(live != nullptr);
    DOCTEST_CHECK_EQ(live->frame.x, edX);
    DOCTEST_CHECK_EQ(live->frame.y, edY);
    DOCTEST_CHECK_EQ(live->frame.width, edW);
    DOCTEST_CHECK_EQ(live->frame.height, edH);
    DOCTEST_CHECK(ui.find("md.tags.add.hit") != nullptr);
    DOCTEST_CHECK(ui.find("md.status.bg") != nullptr);

    // (4) 宽窗口 2400：双栏态不钳（原条件 mode_ == ViewMode::Edit）——左半栏
    //     编辑器文本列贴左 16px 且宽于 720；回到编辑态同一列壳又钳回 720 居中
    screen.width = 2400.0f;
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // 实时 → 双栏
    frame();
    const auto* edSplit = ui.find(kEditorInputId);
    DOCTEST_REQUIRE(edSplit != nullptr);
    DOCTEST_REQUIRE(ui.find("md.preview.scroll") != nullptr);
    const auto* edColSplit = ui.find("md.editor");
    DOCTEST_REQUIRE(edColSplit != nullptr);
    DOCTEST_CHECK_EQ(edSplit->frame.x - edColSplit->frame.x, 16.0f);
    DOCTEST_CHECK(edSplit->frame.width > 720.0f);
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // 双栏 → 预览
    frame();
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // 预览 → 编辑
    frame();
    const auto* edWide = ui.find(kEditorInputId);
    const auto* edColWide = ui.find("md.editor");
    DOCTEST_REQUIRE(edWide != nullptr);
    DOCTEST_REQUIRE(edColWide != nullptr);
    const float fullWide = screen.width - pad * 2.0f - sideW - gap;
    DOCTEST_CHECK_EQ(edWide->frame.width, 720.0f);
    DOCTEST_CHECK_EQ(edWide->frame.x - edColWide->frame.x,
                     (fullWide - 720.0f) * 0.5f);

    mod->onStop();
    return true;
})
