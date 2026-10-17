// tests/test_modules/test_markdown.cpp
// md_line_scanner / md_tree 纯函数 + MarkdownModule 无头 compose / layout 冒烟。
// 跨文件前提：
//   - 视图状态与配置存 app_config 表（不是文件），经 core::appDatabase() 取连接。
//   - DB 查询在后台线程，compose 只读快照——「变更可见」最多晚 1 帧，断言可见性
//     必须走有界轮询 + onPageEnter，不能只 compose 一帧就断言。
//   - position 在流式子元素上静默失效，几何断言必须先跑 ui.layout()。
// 真实鼠标点验留给人工。
#include "doctest.h"
#include "test_support.h"
#include "../frame_env.h"   // headless 帧驱动三合一 HeadlessFrame
#include "core/app_config.h"
#include "core/database.h"
#include "core/module_interface.h"
#include "core/router.h"
#include "core/theme.h"
#include "core/component_theme.h"   // componentTokens（圆角/边框色比对口径）
#include "core/event_bus.h"
#include "module_markdown/md_block_model.h"
#include "module_markdown/md_line_scanner.h"
#include "module_markdown/md_live_edit.h"
#include "module_markdown/md_tree.h"
#include "module_markdown/md_view_state.h"
#include "module_markdown/md_ids.h"
#include "module_markdown/markdown_module_test_seam.h"   // 轮询周期注入（假红根因见用例）

using module::markdown::kEditorInputId;

#include <components/input.h>
#include <components/markdown.h>   // MarkdownStyle（块间隙/圆角/色表比对口径）
#include <components/theme.h>      // withAlpha（活动块边框色口径）
#include <core/dsl_runtime.h>
#include <eui/dsl.h>
#include <eui/types.h>   // 合成指针事件（块点击的 press→release 直驱）

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

extern "C" ::core::Module* create_markdown_module();

namespace fs = std::filesystem;

REGISTER_TEST(module_pagid_to_string, "markdown: pageIdToString", {
    DOCTEST_CHECK_EQ(core::pageIdToString(core::PageId::Home), std::string("Home"));
    DOCTEST_CHECK_EQ(core::pageIdToString(core::PageId::Markdown), std::string("Markdown"));
    DOCTEST_CHECK_EQ(core::pageIdToString(core::PageId::Todo), std::string("Todo"));
    DOCTEST_CHECK_EQ(core::pageIdToString(core::PageId::ClaudeProxy), std::string("ClaudeProxy"));
    DOCTEST_CHECK_EQ(core::pageIdToString(core::PageId::Settings), std::string("Settings"));
    return true;
})

// 行分类扫描器

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

// 闭栏规则对齐 md4c/CommonMark：尾随文本闭栏行、短闭栏、```a`b` 非法开栏、
// ≤3 空格缩进开/闭栏。
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
// 二级目录树纯函数

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
    std::vector<NoteMeta> notes = {
        makeNote("inbox/b.md", 4),
        makeNote("work/a.md", 2),
        makeNote("root.md", 5),
        makeNote("inbox/c.md", 3),
    };
    const auto rows = buildTreeRows(notes, {}, {});
    // 根组「根目录」常显且置顶，其后一级目录按字母序（inbox, work）
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
    DOCTEST_CHECK_EQ(rows[3].depth, 2);  // 顶层夹（1）的直接文件 → 2
    DOCTEST_CHECK_EQ(rows[4].name, std::string("c.md"));
    DOCTEST_CHECK(rows[5].isFolder);
    DOCTEST_CHECK_EQ(rows[5].name, std::string("work"));
    DOCTEST_CHECK_EQ(rows[6].name, std::string("a.md"));
    return true;
})

REGISTER_TEST(md_tree_collapse_and_deep_files, "md_tree: 收起隐藏整棵子树 / 任意层级目录都有行位", {
    std::vector<NoteMeta> notes = {
        makeNote("a/deep/more/x.md", 1),   // 三级夹下的文件
        makeNote("a/deep/y.md", 2),
        makeNote("a/top.md", 3),
        makeNote("solo.md", 4),
    };
    // 收起一级夹 a → a 的文件行与整棵子夹子树一起隐藏；根目录组不受影响（常显置顶）
    const auto collapsed = buildTreeRows(notes, {}, {"a"});
    DOCTEST_CHECK_EQ(collapsed.size(), static_cast<size_t>(3));
    DOCTEST_CHECK(collapsed[0].isFolder);
    DOCTEST_CHECK_EQ(collapsed[0].name, std::string("根目录"));
    DOCTEST_CHECK(!collapsed[1].isFolder);
    DOCTEST_CHECK_EQ(collapsed[1].name, std::string("solo.md"));
    DOCTEST_CHECK(collapsed[2].isFolder);
    DOCTEST_CHECK_EQ(collapsed[2].name, std::string("a"));
    // 组内「文件行在前（mtime 序）、子夹行在后」。**每一级目录都有自己的行**——
    // 目录无法被选中时，「选中目录」就没有父目录来源。
    const auto rows = buildTreeRows(notes, {}, {});
    DOCTEST_CHECK_EQ(rows.size(), static_cast<size_t>(8));
    DOCTEST_CHECK(rows[0].isFolder);
    DOCTEST_CHECK_EQ(rows[0].name, std::string("根目录"));
    DOCTEST_CHECK_EQ(rows[1].name, std::string("solo.md"));
    DOCTEST_CHECK_EQ(rows[1].depth, 1);
    DOCTEST_CHECK(rows[2].isFolder);
    DOCTEST_CHECK_EQ(rows[2].name, std::string("a"));
    DOCTEST_CHECK_EQ(rows[2].depth, 1);  // 顶层夹 = 根组行的子行
    DOCTEST_CHECK_EQ(rows[3].name, std::string("top.md"));  // a 的直接子文件
    DOCTEST_CHECK_EQ(rows[3].relPath, std::string("a/top.md"));
    DOCTEST_CHECK_EQ(rows[3].depth, 2);
    DOCTEST_CHECK(rows[4].isFolder);                        // 二级夹行
    DOCTEST_CHECK_EQ(rows[4].name, std::string("deep"));
    DOCTEST_CHECK_EQ(rows[4].relPath, std::string("a/deep"));
    DOCTEST_CHECK_EQ(rows[4].depth, 2);
    DOCTEST_CHECK_EQ(rows[5].name, std::string("y.md"));     // 二级夹的直接子文件
    DOCTEST_CHECK_EQ(rows[5].relPath, std::string("a/deep/y.md"));
    DOCTEST_CHECK_EQ(rows[5].depth, 3);
    DOCTEST_CHECK(rows[6].isFolder);                        // 三级夹行（新）
    DOCTEST_CHECK_EQ(rows[6].name, std::string("more"));
    DOCTEST_CHECK_EQ(rows[6].relPath, std::string("a/deep/more"));
    DOCTEST_CHECK_EQ(rows[6].depth, 3);
    DOCTEST_CHECK_EQ(rows[7].name, std::string("x.md"));     // 三级夹的文件：文件名
    DOCTEST_CHECK_EQ(rows[7].relPath, std::string("a/deep/more/x.md"));
    DOCTEST_CHECK_EQ(rows[7].depth, 4);
    // 收起二级夹 → 只剩子夹行（一级夹自己的文件行不受影响）
    const auto subCollapsed = buildTreeRows(notes, {}, {"a/deep"});
    DOCTEST_CHECK_EQ(subCollapsed.size(), static_cast<size_t>(5));
    DOCTEST_CHECK_EQ(subCollapsed[3].name, std::string("top.md"));
    DOCTEST_CHECK(subCollapsed[4].isFolder);
    DOCTEST_CHECK_EQ(subCollapsed[4].name, std::string("deep"));
    return true;
})

REGISTER_TEST(md_tree_two_level_empty_subfolders, "md_tree: 空目录行（磁盘枚举，各层）+ 层级不设上限", {
    // 只有磁盘枚举的目录（没有任何笔记）：顶层夹与空子夹都有文件夹行、零文件行
    const std::vector<NoteMeta> none;
    const auto rows = buildTreeRows(none, {"工作", "工作/周会", "归档"}, {});
    DOCTEST_CHECK_EQ(rows.size(), static_cast<size_t>(4));
    DOCTEST_CHECK(rows[0].isFolder);
    DOCTEST_CHECK_EQ(rows[0].name, std::string("根目录"));  // 常显置顶
    DOCTEST_CHECK_EQ(rows[1].name, std::string("工作"));    // UTF-8 字节序：工 < 归
    DOCTEST_CHECK_EQ(rows[1].depth, 1);                     // 顶层夹 = 根组的子行
    DOCTEST_CHECK_EQ(rows[2].name, std::string("周会"));    // 空子夹行（W10）
    DOCTEST_CHECK_EQ(rows[2].relPath, std::string("工作/周会"));
    DOCTEST_CHECK_EQ(rows[2].depth, 2);
    DOCTEST_CHECK_EQ(rows[3].name, std::string("归档"));
    DOCTEST_CHECK_EQ(rows[3].depth, 1);

    // 三级目录（磁盘枚举，无笔记）同样有行位：层级不设上限
    const auto deep = buildTreeRows(none, {"a", "a/b", "a/b/c"}, {});
    DOCTEST_CHECK_EQ(deep.size(), static_cast<size_t>(4));
    DOCTEST_CHECK_EQ(deep[1].name, std::string("a"));
    DOCTEST_CHECK_EQ(deep[2].name, std::string("b"));
    DOCTEST_CHECK_EQ(deep[2].relPath, std::string("a/b"));
    DOCTEST_CHECK_EQ(deep[2].depth, 2);
    DOCTEST_CHECK_EQ(deep[3].name, std::string("c"));
    DOCTEST_CHECK_EQ(deep[3].relPath, std::string("a/b/c"));
    DOCTEST_CHECK_EQ(deep[3].depth, 3);
    return true;
})

REGISTER_TEST(md_tree_root_group_always, "md_tree: 根组标题常显（无根文件/空目录）", {
    // 只有子目录文件（无根散文件）→ 根组行仍在顶部
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

    // 磁盘枚举的空目录（extraFolders）→ 树里出现无子行的文件夹行
    const auto withEmpty = buildTreeRows(none, {"空文件夹"}, {});
    DOCTEST_CHECK_EQ(withEmpty.size(), static_cast<size_t>(2));
    DOCTEST_CHECK(withEmpty[0].isFolder);
    DOCTEST_CHECK_EQ(withEmpty[0].name, std::string("根目录"));
    DOCTEST_CHECK(withEmpty[1].isFolder);
    DOCTEST_CHECK_EQ(withEmpty[1].name, std::string("空文件夹"));
    DOCTEST_CHECK_EQ(withEmpty[1].relPath, std::string("空文件夹"));
    DOCTEST_CHECK_EQ(withEmpty[1].depth, 1);  // 顶层夹 = 根组的子行
    return true;
})

// UI 无头 compose 冒烟

namespace {
// 笔记临时目录前缀 / 库名（写文件用共享的 test_support::writeTextFile）
constexpr const char* kDirPrefix = "devdesk_md_ui_";
constexpr const char* kDbName = "ui_test.db";

using test_support::writeTextFile;

// 颜色逐分量比对（树行选中态底色等断言用；定义在文件前部供各用例共用）
bool colorEq(const eui::Color& a, const eui::Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

    // 树行定位（按显示名）。不写死行下标——新建笔记按 mtime 倒序插进组内，
    // 行序会随用例步骤漂移。目录行才有 .arrow 元素，文件行没有 → 用它区分。
    // 返回行下标（可拼 "md.tree.node.N.row" / ".toggle" / ".name"），找不到 → -1
int treeRowIndex(eui::Ui& ui, const std::string& displayName, bool folder) {
    for (int i = 0; i < 40; ++i) {
        const std::string idx = std::to_string(i);
        const auto* nameEl = ui.find("md.tree.node." + idx + ".name");
        if (nameEl == nullptr) return -1;
        const bool isFolder = ui.find("md.tree.node." + idx + ".arrow") != nullptr;
        if (isFolder == folder && nameEl->text == displayName) return i;
    }
    return -1;
}

// 目录行是否处于选中态（底色 == primarySoftBg）
bool folderRowSelected(eui::Ui& ui, const std::string& displayName) {
    const int i = treeRowIndex(ui, displayName, /*folder=*/true);
    if (i < 0) return false;
    const auto* row = ui.find("md.tree.node." + std::to_string(i) + ".row");
    return row != nullptr &&
           colorEq(row->color, core::theme::current().colors.primarySoftBg);
}

} // namespace

// 1. 无服务（appDatabase 为 null）时 compose 不崩

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
    DOCTEST_CHECK(ui.find("md.mode") != nullptr);        // 模式切换 segmented
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

// 2. 有数据时：文件打开→大纲→跳转→三态循环 全链路（headless）

// GitHub Sync 拉取导入远端库（表被 DROP 重建）后必须重载笔记列表缓存，否则用户看到旧列表。
// 观测口 = 目录树行（notes_ 缓存 → buildTreeRows）。对照帧证明「不广播就不刷新」，
// 排除 compose 本来就会重查的可能。
REGISTER_TEST(md_module_db_import_reloads_list,
              "markdown_module: db.imported 后 compose 重载笔记列表（headless）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "dbimport", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "a.md", "# A\n正文\n"));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    eui::Screen screen{1600.0f, 1000.0f};
    eui::Ui ui;
    auto frame = [&](const char* page) {
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
    };
    auto treeHasNote = [&](const std::string& name) {
        for (int i = 0; i < 64; ++i) {
            const auto* n =
                ui.find("md.tree.node." + std::to_string(i) + ".name");
            if (n == nullptr) break;
            if (n->text == name) return true;
        }
        return false;
    };

    // 测试缝：轮询周期调到 1 小时 ——「本用例窗口内不发生磁盘轮询」从**墙钟假设**
    // 变成保证。原注释写「首帧建立 5s 轮询基准，此后同用例内不再轮询」：负载下
    // >5s 卡顿会让轮询闯进窗口，rescan 把下面手工插入的幽灵行剪掉 → 假红
    //（机理的确定性复现见 md_module_poll_seam_is_live）。首帧仍会消费 watcher
    // credit —— credit 路径不受周期节流（hasWatchCredit() 短路）。
    module::markdown::mdSetPollIntervalMsForTest(mod.get(), 3600 * 1000);
    frame("di1");
    DOCTEST_CHECK(treeHasNote("a.md"));

    // 模拟远端库导入：索引多出一行。刻意不动文件系统——磁盘变化会被 watcher/轮询
    // 抢先刷新（掩掉本改动），只有 db.imported 事件能驱动这次重载。轮询已调走，
    // 这条窗口里只剩事件一条路径。
    auto ins = env.db->db().exec(
        "INSERT INTO markdown_index (path, title, size_bytes, modified_at, created_at) "
        "VALUES ('imported.md', '导入笔记', 10, 9999999999, 9999999999)");
    DOCTEST_REQUIRE(ins.isOk());

    frame("di2");
    DOCTEST_CHECK(!treeHasNote("imported.md"));   // 对照：未广播 → 仍旧列表

    ::core::event::bus().emit(::core::event::kDbImported);
    frame("di3");
    DOCTEST_CHECK(treeHasNote("imported.md"));    // 广播后重载可见

    mod->onStop();
    return true;
})

// 轮询缝自证（防「缝是空操作」）：把周期压到 1ms → 手工插入的幽灵索引行
//（磁盘上无此文件）必被下一帧轮询的 rescan 剪掉 —— 这正是既有导入用例在负载下
// 假红的机理，此处确定性复现。与 md_module_db_import_reloads_list 的「周期调到
// 1 小时 → 行留着」构成**相反**期望：缝一旦失效（no-op / 节流不读该值），必有一条红。
REGISTER_TEST(md_module_poll_seam_is_live,
              "markdown_module: 轮询缝自证（1ms → 幽灵索引行被 rescan 剪掉；1 小时 → 留着）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "mdpollseam", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "a.md", "# A\n正文\n"));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen{1600.0f, 1000.0f};
    eui::Ui ui;
    auto frame = [&](const char* page) {
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
    };
    // 幽灵行计数口：直接查 markdown_index（不经过树/缓存，判据只盯 rescan 剪枝）
    auto phantomRows = [&]() -> int {
        auto prep = env.db->db().prepare(
            "SELECT COUNT(*) AS n FROM markdown_index WHERE path = 'imported.md'");
        if (prep.isFail()) return -1;
        auto rows = prep.value().fetchAll();
        if (rows.isFail() || rows.value().empty()) return -1;
        auto v = rows.value()[0].getInt64("n");
        return v.isOk() ? static_cast<int>(v.value()) : -1;
    };
    auto insertPhantom = [&]() {
        return env.db->db()
            .exec("INSERT INTO markdown_index "
                  "(path, title, size_bytes, modified_at, created_at) "
                  "VALUES ('imported.md', '幽灵', 10, 9999999999, 9999999999)")
            .isOk();
    };

    // ① 周期 1 小时：轮询不可能到期 → 幽灵行留着（= 导入用例的确定性前提）
    module::markdown::mdSetPollIntervalMsForTest(mod.get(), 3600 * 1000);
    frame("ps1");
    DOCTEST_REQUIRE(insertPhantom());
    DOCTEST_REQUIRE(phantomRows() == 1);
    frame("ps2");
    DOCTEST_CHECK_MESSAGE(phantomRows() == 1,
                          "周期 1 小时仍发生了轮询（缝没生效，导入用例的假红根因还在）");

    // ② 周期 1ms：下一帧必到期 → rescan 以磁盘为准 → 幽灵行被剪掉
    module::markdown::mdSetPollIntervalMsForTest(mod.get(), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));   // 越过 1ms 节流门
    frame("ps3");
    DOCTEST_CHECK_MESSAGE(phantomRows() == 0,
                          "周期 1ms 未触发轮询 rescan（缝没生效）");

    mod->onStop();
    return true;
})

REGISTER_TEST(md_module_compose_phase1, "markdown_module: Phase 1 三态/大纲/状态栏 compose（headless）", {
    const std::string text = "# 标题一\n正文一\n\n## 标题二\n正文二\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "phase1", kDbName);
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

    // 帧2：默认编辑态——只有树 + 编辑器两个可见列，预览列不渲染；
    // 编辑器正文列占满栏宽（x == 列内留白 8）
    frame("f2");
    DOCTEST_CHECK(ui.find(kEditorInputId) != nullptr);
    DOCTEST_CHECK(ui.find("md.preview.scroll") == nullptr);
    const auto* save = ui.find("md.status.save");
    DOCTEST_REQUIRE(save != nullptr);
    DOCTEST_CHECK(save->text.find("note.md") != std::string::npos);
    DOCTEST_CHECK(save->text.find("已保存") != std::string::npos);
    const auto* edDefault = ui.find(kEditorInputId);
    DOCTEST_REQUIRE(edDefault != nullptr);
    DOCTEST_CHECK_EQ(edDefault->x, 8.0f);   // 列内左留白 = kTextColumnInsetX

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

    // 面板可关闭（button 组件把 onClick 挂在 .bg 子元素上）
    const auto* closeBtn = ui.find("md.outline.panel.close.bg");
    DOCTEST_REQUIRE(closeBtn != nullptr);
    DOCTEST_REQUIRE(closeBtn->onClick != nullptr);
    closeBtn->onClick();
    frame("f3b");
    DOCTEST_CHECK(ui.find("md.outline.panel") == nullptr);

    // Ctrl+Shift+P 四态循环：Edit（默认）→ Live → Split → Preview → Edit
    eui::KeyEvent p = test_support::pressShortcut(eui::InputKey::P, /*shift=*/true);
    DOCTEST_CHECK(mod->onKeyEvent(p));   // → Live（第四态）
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

// 2b. 目录树展开/收起 + 缩进 layout 级验证

REGISTER_TEST(md_module_compose_tree, "markdown_module: W8 目录树展开收起/缩进 layout（headless）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "tree", kDbName);
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

    // 帧1：树行序（根组置顶）= [根目录▾, solo.md, inbox▾, idea.md, work▾, week.md]
    frame("t1");
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.0.name")->text, std::string("根目录"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.1.name")->text, std::string("solo.md"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.2.name")->text, std::string("inbox"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.3.name")->text, std::string("idea.md"));
    DOCTEST_CHECK_EQ(ui.find("md.tree.node.4.name")->text, std::string("work"));

    // layout 级验证：position 在流式子元素上静默失效——跑真实 layout 后比对
    // 计算 frame，缩进丢失会在下面两条断言暴露
    ui.layout(screen.width, screen.height);
    const auto* folderName = ui.find("md.tree.node.2.name");
    const auto* fileName = ui.find("md.tree.node.3.name");
    DOCTEST_REQUIRE(folderName != nullptr);
    DOCTEST_REQUIRE(fileName != nullptr);
    DOCTEST_CHECK(fileName->frame.x > folderName->frame.x + 20.0f);  // depth=1 缩进
    DOCTEST_CHECK(fileName->frame.y > folderName->frame.y);          // 纵向流式排布
    // 浮动面板未开时不存在
    DOCTEST_CHECK(ui.find("md.outline.panel") == nullptr);

    // 帧2：点**箭头命中区**收起 inbox → inbox 的文件行消失（文件夹行保留，箭头变 ▸）
    //。「行体点击 = 选中目录」「箭头点击 = 收起/展开」是两个命中区，行体不再收起
    const auto* folder = ui.find("md.tree.node.2.toggle");
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

    // 帧3：再点箭头展开 → 文件行回来
    const auto* folder2 = ui.find("md.tree.node.2.toggle");
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

// 2b2''. 树行几何数值锁
// 反重构警告：本组断言彼此独立，别把任一条当成冗余删掉——
//   a. 行内每个文本元素（文件行 .icon/.name，文件夹行 .arrow/.ficon/.name）的
//      frame 中心 y == 所在 item 行（.row）中心 y（±0.5）；
//   b. frame.y 无半像素；
//   c. x 方向同层级统一：同 kind + 同 depth 的所有行 x 完全相同；
//   d. 箭头字号 ∈ [fontSizeSmall×1.25, fontSizeSmall×1.4] 且 < 行高（不溢出）；
//   e. 每个行内文本都挂 VerticalAlign::Center——渲染锚点用 frame 中线
//      （runtime_render.h），没有它「frame 中心 == 行中心」也换不来视觉居中。
REGISTER_TEST(md_tree_row_geometry,
              "markdown: 树行文本 frame 中心==行中心 / x 同层级统一 / 箭头字号（数值锁）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "rowgeom", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    // 三层结构：根组文件 + 顶层夹（含文件 + 子夹，子夹内再一文件）
    DOCTEST_REQUIRE(writeTextFile(env.dir / "solo.md", "# solo\n"));
    DOCTEST_REQUIRE(writeTextFile(env.dir / "工作" / "周报.md", "# 周报\n"));
    DOCTEST_REQUIRE(writeTextFile(env.dir / "工作" / "周会" / "纪要.md", "# 纪要\n"));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("treegeom");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);

    // 行序（根组置顶 + 组内文件在前/子夹在后）：
    //   [0] 根目录(夹,d0) [1] solo.md(文件,d1) [2] 工作(夹,d1)
    //   [3] 周报.md(文件,d2) [4] 周会(夹,d2) [5] 纪要.md(文件,d3)
    // 根组行 = 整棵树的根：顶层夹与其直接文件同层 d1
    struct RowSpec { const char* id; bool folder; };
    const RowSpec rows[] = {{"md.tree.node.0", true},  {"md.tree.node.1", false},
                            {"md.tree.node.2", true},  {"md.tree.node.3", false},
                            {"md.tree.node.4", true},  {"md.tree.node.5", false}};
    const char* kFolderParts[] = {".arrow", ".ficon", ".name"};
    const char* kFileParts[] = {".icon", ".name"};
    const float kRowH = 32.0f;  // kTreeRowHeight（markdown_module.cpp 同名常量）

    for (const RowSpec& spec : rows) {
        const std::string base = spec.id;
        const auto* row = ui.find(base + ".row");
        DOCTEST_REQUIRE_MESSAGE(row != nullptr, base + ".row 不存在");
        DOCTEST_CHECK_MESSAGE(row->frame.height == kRowH, base + ".row 高度 != 32");
        const float rowCenterY = row->frame.y + row->frame.height * 0.5f;
        const char* const* parts = spec.folder ? kFolderParts : kFileParts;
        const size_t partCount = spec.folder ? 3u : 2u;
        for (size_t p = 0; p < partCount; ++p) {
            const std::string partId = base + parts[p];
            const auto* el = ui.find(partId);
            DOCTEST_REQUIRE_MESSAGE(el != nullptr, partId + " 不存在");
            const float centerY = el->frame.y + el->frame.height * 0.5f;
            // a. 文本元素 frame 中心 == 行中心（±0.5）
            DOCTEST_CHECK_MESSAGE(std::fabs(centerY - rowCenterY) <= 0.5f,
                                  partId + " frame 中心 y=" + std::to_string(centerY) +
                                      " ≠ 行中心 y=" + std::to_string(rowCenterY));
            // b. 无半像素
            DOCTEST_CHECK_MESSAGE(std::fabs(el->frame.y - std::round(el->frame.y)) < 0.001f,
                                  partId + " frame.y=" + std::to_string(el->frame.y) +
                                      " 半像素错位");
            // e. 渲染锚点 = frame 中线（垂直居中的真正落点）
            DOCTEST_CHECK_MESSAGE(el->verticalAlign == eui::VerticalAlign::Center,
                                  partId + " 未挂 VerticalAlign::Center");
        }
    }

    // c. x 方向同层级统一（同 kind + 同 depth → 同 x，整数不差半像素）
    const auto xOf = [&](const char* id) { return ui.find(id)->frame.x; };
    // 顶层夹 = 根组行的**子**行：node.2 各列 = node.0 + 一级缩进 16
    //（反重构：改成与根行同 x 就等于把顶层夹渲染成根行的兄弟）
    DOCTEST_CHECK_EQ(xOf("md.tree.node.2.arrow"), xOf("md.tree.node.0.arrow") + 16.0f);
    DOCTEST_CHECK_EQ(xOf("md.tree.node.2.ficon"), xOf("md.tree.node.0.ficon") + 16.0f);
    DOCTEST_CHECK_EQ(xOf("md.tree.node.2.name"), xOf("md.tree.node.0.name") + 16.0f);
    // 子夹（d2）= 顶层夹（d1）+ 一级缩进 16
    DOCTEST_CHECK_EQ(xOf("md.tree.node.4.name"), xOf("md.tree.node.2.name") + 16.0f);
    // 文件行：d1（solo.md）→ d2（工作/周报.md，= d1 + 16）→ d3（纪要.md，= d1 + 32）
    DOCTEST_CHECK_EQ(xOf("md.tree.node.3.name"), xOf("md.tree.node.1.name") + 16.0f);
    DOCTEST_CHECK_EQ(xOf("md.tree.node.5.name"), xOf("md.tree.node.1.name") + 32.0f);
    // 文件行内 icon → name 间距恒定 24（12 → 36）；且箭头列不与文件夹图标列重叠
    DOCTEST_CHECK_EQ(xOf("md.tree.node.1.name") - xOf("md.tree.node.1.icon"), 24.0f);
    const auto* arrow0 = ui.find("md.tree.node.2.arrow");
    const auto* ficon0 = ui.find("md.tree.node.2.ficon");
    DOCTEST_REQUIRE(arrow0 != nullptr);
    DOCTEST_REQUIRE(ficon0 != nullptr);
    DOCTEST_CHECK(arrow0->frame.x + arrow0->frame.width <= ficon0->frame.x);

    // d. 箭头字号：一档放大（fontSizeSmall 13 × 1.385 ≈ 18，落在 1.25~1.4 倍区间），
    //    且 < 行高不溢出
    const auto* arrow = ui.find("md.tree.node.2.arrow");
    const auto* label = ui.find("md.tree.node.2.name");
    DOCTEST_REQUIRE(arrow != nullptr);
    DOCTEST_REQUIRE(label != nullptr);
    const float arrowFontSize = arrow->fontSize;  // 跨帧读值：重开帧后旧 Element* 失效
    DOCTEST_CHECK_EQ(arrowFontSize, 18.0f);       // 锁定的常量值
    DOCTEST_CHECK_MESSAGE(arrowFontSize >= 13.0f * 1.25f && arrowFontSize <= 13.0f * 1.4f,
                          "箭头 fontSize=" + std::to_string(arrowFontSize) +
                              " 不在 [16.25, 18.2]");
    DOCTEST_CHECK(arrowFontSize > label->fontSize);  // 确实比行内正文大
    DOCTEST_CHECK(arrowFontSize < kRowH);            // 不溢出 item 行高
    DOCTEST_CHECK_EQ(arrowFontSize, ui.find("md.tree.node.4.arrow")->fontSize);
    // 折叠态（▸）与展开态（▾）同一字号口径：点根组行的**箭头命中区**收起 →
    // 箭头文本变 ▸、字号不变。行体点击 = 选中后，收起只剩箭头命中区这一个入口。
    const auto* rootRow = ui.find("md.tree.node.0.row");
    DOCTEST_REQUIRE(rootRow != nullptr);
    DOCTEST_REQUIRE(rootRow->onClick != nullptr);
    const auto* rootToggle = ui.find("md.tree.node.0.toggle");
    DOCTEST_REQUIRE(rootToggle != nullptr);
    DOCTEST_REQUIRE(rootToggle->onClick != nullptr);
    // 箭头命中区落在行体之上（命中测试取最后声明的匹配兄弟 → 点箭头不会选中）
    DOCTEST_CHECK(rootToggle->frame.x < rootRow->frame.x + rootRow->frame.width);
    const eui::Color rowColorBefore = rootRow->color;
    rootToggle->onClick();
    ui.begin("treegeom2");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);
    const auto* collapsedArrow = ui.find("md.tree.node.0.arrow");
    DOCTEST_REQUIRE(collapsedArrow != nullptr);
    DOCTEST_CHECK_EQ(collapsedArrow->text, std::string("▸"));
    DOCTEST_CHECK_EQ(collapsedArrow->fontSize, arrowFontSize);
    // 点箭头只收起、不改选中：根组行底色与收起前一致
    const auto* rootRowAfter = ui.find("md.tree.node.0.row");
    DOCTEST_REQUIRE(rootRowAfter != nullptr);
    DOCTEST_CHECK(colorEq(rootRowAfter->color, rowColorBefore));

    mod->onStop();
    return true;
})

// 2b3. 空目录树 = 根组标题 + 空态提示同显；搜索 placeholder 常显

REGISTER_TEST(md_module_empty_tree, "markdown_module: W9 空目录根组标题+空态提示同显 / 搜索框 placeholder（headless）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "empty", kDbName);
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

    // 空目录：根组标题行 + 空态提示同显（📁 用 fa-folder icon 元素）
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

    // 搜索框 placeholder：空输入 + 未聚焦时也渲染（EUI-NEO input 只要 text 为空
    // 就画 placeholder，与焦点无关）。🔍 用 FA 浮层 icon（fa-magnifying-glass）
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

// 2b4. 新建文件夹入口（左栏按钮 → 弹窗 → 空文件夹行入树）

REGISTER_TEST(md_module_new_folder, "markdown_module: W9 新建文件夹入口真实链路（headless）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "newfolder", kDbName);
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

    // 新建笔记走子路径（placeholder 承诺「可用 / 分文件夹」——create 建中间目录）。
    // 注意：新建文件夹会把**选中目录**挪到刚建的那个夹（与 selectDir 口径一致）。
    // 这里先点根目录行把选中态放回根，再验证「名字带
    // 子路径」这条语义——顺带覆盖「点根目录行 = 选中根」。
    const auto* rootRowSel = ui.find("md.tree.node.0.row");
    DOCTEST_REQUIRE(rootRowSel != nullptr);
    DOCTEST_REQUIRE(rootRowSel->onClick != nullptr);
    rootRowSel->onClick();
    frame("nf4b");
    // 选中根目录后，新建笔记弹窗提示行必须写明落点 = 根目录（不是子夹）
    const auto* nfHint = ui.find("md.dialog.new.hint");
    DOCTEST_REQUIRE(nfHint != nullptr);
    DOCTEST_CHECK(nfHint->text.find("根目录") != std::string::npos);
    // 选中态可视：根目录行底色 = primarySoftBg（选中色），刚建好的「工作」行回底色
    const auto* rootRowNow = ui.find("md.tree.node.0.row");
    const auto* workRowNow = ui.find("md.tree.node.2.row");
    DOCTEST_REQUIRE(rootRowNow != nullptr);
    DOCTEST_REQUIRE(workRowNow != nullptr);
    const auto& treeColors = core::theme::current().colors;
    DOCTEST_CHECK(colorEq(rootRowNow->color, treeColors.primarySoftBg));
    DOCTEST_CHECK(colorEq(rootRowNow->color, treeColors.primarySoftBg) &&
                  !colorEq(workRowNow->color, treeColors.primarySoftBg));
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

// 2b4b. 两级目录——父目录下拉建子夹 + 空子夹行

// 2b4b. 上级目录下拉：默认 = 选中目录 / 路径不随候选漂移 / 逐层映射
// 反重构警告，三条都要靠：
//   ① 默认上级恒为「根目录」（硬编码下标 0）→ 树上选了目录也没用；
//   ② 存候选列表**下标**、提交时用同一下标重新推导目录 → 列表中途一变（外部建
//      目录 + 5 秒轮询）就落到另一个目录；
//   ③ 候选只到一级夹 → 二级夹无法当上级。
REGISTER_TEST(md_module_new_folder_parent,
              "markdown_module: 新建文件夹「上级目录」默认=选中目录 / 路径不随下标漂移 / 逐层映射（headless）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "newfolder2", kDbName);
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

    // 父目录下拉默认第 0 项（根目录）；点开选「工作」
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

    // 上级目录候选：每一级都在 + 默认 = 当前选中目录
    // 按显示名点下拉项（候选按字节序，用例不写死下标）
    const auto selectParent = [&](const std::string& label) {
        for (int i = 0; i < 8; ++i) {
            const std::string idx = std::to_string(i);
            const auto* item =
                ui.find("md.dialog.newfolder.parent.dd.item.label." + idx);
            if (item == nullptr) break;
            if (item->text != label) continue;
            const auto* hit = ui.find("md.dialog.newfolder.parent.dd.item." + idx);
            if (hit != nullptr && hit->onClick != nullptr) {
                hit->onClick();
                return true;
            }
        }
        return false;
    };
    const auto folderRowShown = [&](const std::string& displayName) {
        for (int i = 0; i < 40; ++i) {
            const std::string idx = std::to_string(i);
            const auto* nameEl = ui.find("md.tree.node." + idx + ".name");
            if (nameEl == nullptr) return false;
            // 目录行才有箭头元素（文件行没有）
            if (ui.find("md.tree.node." + idx + ".arrow") == nullptr) continue;
            if (nameEl->text == displayName) return true;
        }
        return false;
    };

    // 弹窗默认上级 = **当前选中目录**（刚建好的 工作/周会），不是硬编码的根目录，
    // 也不是「一级夹拿不到就只能给根」——二级目录同样能当上级
    const auto* folderBtn2 = ui.find("md.tree.newfolder.bg");
    DOCTEST_REQUIRE(folderBtn2 != nullptr);
    folderBtn2->onClick();
    frame("np7");
    DOCTEST_CHECK_EQ(ui.find("md.dialog.newfolder.parent.dd.label")->text,
                     std::string("工作/周会"));
    const auto* ddField2 = ui.find("md.dialog.newfolder.parent.dd.field");
    DOCTEST_REQUIRE(ddField2 != nullptr);
    DOCTEST_REQUIRE(ddField2->onClick != nullptr);
    ddField2->onClick();  // 展开下拉（真实 onClick 链路）
    frame("np8");
    DOCTEST_CHECK(selectParent("工作"));  // 每一级都在候选里（根 / 一级 / 二级）
    frame("np9");
    DOCTEST_CHECK_EQ(ui.find("md.dialog.newfolder.parent.dd.label")->text,
                     std::string("工作"));

    // 上级目录 = 路径，不是候选列表下标
    // 存下标、提交时用同一下标重新推导目录时，弹窗开着期间目录清单一变（外部新建
    // 目录 / 5 秒轮询刷新），同一��标就落到另一个目录。这里复刻真实机制：保持弹窗
    // 打开，磁盘上出现一个排序在「工作」之前的目录，并让 5 秒轮询把清单刷进来
    //（空目录不在 pollChanged 的比对范围内，所以另写一个 .md 文件作为轮询信号——
    // 这也是外部改动的真实形态），然后提交 —— 落点必须仍是用户选的「工作」。
    {
        std::error_code dec;
        fs::create_directories(env.dir / "aaa", dec);
        DOCTEST_REQUIRE(!dec);
        DOCTEST_REQUIRE(writeTextFile(env.dir / "poll_signal.md", "# s\n"));
        std::this_thread::sleep_for(std::chrono::milliseconds(5200));
        frame("np10");  // 本帧 pollDiskChangesIfDue → rescan → refreshList（候选列表变了）
    }
    const auto* input2 = ui.find("md.dialog.newfolder.input.hit");
    DOCTEST_REQUIRE(input2 != nullptr);
    core::TextInputEvent tie2;
    tie2.text = "漂移夹";
    input2->onTextInput(tie2);
    frame("np11");
    const auto* confirm2 = ui.find("md.dialog.newfolder.confirm.bg");
    DOCTEST_REQUIRE(confirm2 != nullptr);
    confirm2->onClick();
    frame("np12");
    DOCTEST_CHECK(fs::is_directory(env.dir / "工作" / "漂移夹"));   // 落在用户选的目录
    DOCTEST_CHECK(!fs::exists(env.dir / "aaa" / "漂移夹"));         // 没有跟着下标漂走
    DOCTEST_CHECK(folderRowShown("漂移夹"));

    // 上级显式选「根目录」→ 必须建在根目录
    const auto* folderBtn3 = ui.find("md.tree.newfolder.bg");
    DOCTEST_REQUIRE(folderBtn3 != nullptr);
    folderBtn3->onClick();
    frame("np13");
    const auto* ddField3 = ui.find("md.dialog.newfolder.parent.dd.field");
    DOCTEST_REQUIRE(ddField3 != nullptr);
    DOCTEST_REQUIRE(ddField3->onClick != nullptr);
    ddField3->onClick();  // 展开下拉（真实 onClick 链路）
    frame("np14");
    DOCTEST_CHECK(selectParent("根目录"));
    frame("np15");
    DOCTEST_CHECK_EQ(ui.find("md.dialog.newfolder.parent.dd.label")->text,
                     std::string("根目录"));
    const auto* input3 = ui.find("md.dialog.newfolder.input.hit");
    DOCTEST_REQUIRE(input3 != nullptr);
    core::TextInputEvent tie3;
    tie3.text = "顶层甲";
    input3->onTextInput(tie3);
    frame("np16");
    const auto* confirm3 = ui.find("md.dialog.newfolder.confirm.bg");
    DOCTEST_REQUIRE(confirm3 != nullptr);
    confirm3->onClick();
    frame("np17");
    DOCTEST_CHECK(fs::is_directory(env.dir / "顶层甲"));            // 根目录下
    DOCTEST_CHECK(!fs::exists(env.dir / "工作" / "顶层甲"));        // 不是选中目录下

    // 上级选**二级目录**→ 建在二级目录下（层级不设上限）
    const auto* folderBtn4 = ui.find("md.tree.newfolder.bg");
    DOCTEST_REQUIRE(folderBtn4 != nullptr);
    folderBtn4->onClick();
    frame("np18");
    const auto* ddField4 = ui.find("md.dialog.newfolder.parent.dd.field");
    DOCTEST_REQUIRE(ddField4 != nullptr);
    DOCTEST_REQUIRE(ddField4->onClick != nullptr);
    ddField4->onClick();  // 展开下拉（真实 onClick 链路）
    frame("np19");
    DOCTEST_CHECK(selectParent("工作/周会"));
    frame("np20");
    DOCTEST_CHECK_EQ(ui.find("md.dialog.newfolder.parent.dd.label")->text,
                     std::string("工作/周会"));
    const auto* input4 = ui.find("md.dialog.newfolder.input.hit");
    DOCTEST_REQUIRE(input4 != nullptr);
    core::TextInputEvent tie4;
    tie4.text = "月会";
    input4->onTextInput(tie4);
    frame("np21");
    const auto* confirm4 = ui.find("md.dialog.newfolder.confirm.bg");
    DOCTEST_REQUIRE(confirm4 != nullptr);
    confirm4->onClick();
    frame("np22");
    DOCTEST_CHECK(fs::is_directory(env.dir / "工作" / "周会" / "月会"));  // 三级夹
    DOCTEST_CHECK(fs::is_directory(env.dir / "工作" / "周会"));           // 上级原样在
    DOCTEST_CHECK(!fs::exists(env.dir / "工作" / "月会"));                 // 没有落错层

    mod->onStop();
    return true;
})

// 2b4b''. 目录树选中态 → 新建笔记落在选中目录
// 逐档钉死（不只测根——「永远落在根目录」正是报障点）：
//   ① 初始选中 = 根目录行（默认值），行底色 = primarySoftBg；
//   ② 点**二级目录**行 → 选中态 == 工作/周会 → 新建笔记落盘 工作/周会/xxx.md
//      （不是根、也不是父目录 工作）；
//   ③ 点**一级目录**行 → 新建笔记落 工作/xxx.md；
//   ④ 点**根目录**行 → 新建笔记落 根/xxx.md；
//   ⑤ 点**笔记行** → 文件夹不级联选中，但功能目标（newItemTargetDir_）跟到该笔记所在
//      目录；新建弹窗「保存到」验证功能落地在 parent dir；
//   ⑥ 点箭头命中区只收起/展开、不改选中态（两个命中区分工）。
REGISTER_TEST(md_module_tree_select_dir_new_note,
              "markdown_module: 目录树选中态（→ 二级/一级/根逐档）+ 新建笔记落选中目录（headless）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "treesel", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "根笔记.md", "# 根笔记\n"));
    DOCTEST_REQUIRE(writeTextFile(env.dir / "工作" / "周报.md", "# 周报\n"));
    DOCTEST_REQUIRE(writeTextFile(env.dir / "工作" / "周会" / "纪要.md", "# 纪要\n"));

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
    // 命中链路辅助（断言只能写在用例体里 → 辅助 lambda 返回 bool，由用例断言）
    const auto clickId = [&](const std::string& id) -> bool {
        const auto* el = ui.find(id);
        if (el == nullptr || el->onClick == nullptr) return false;
        el->onClick();
        return true;
    };
    const auto typeId = [&](const std::string& id, const std::string& text) -> bool {
        const auto* hit = ui.find(id + ".hit");
        if (hit == nullptr || hit->onTextInput == nullptr) return false;
        core::TextInputEvent ev;
        ev.text = text;
        hit->onTextInput(ev);
        return true;
    };

    // 帧1：三层结构 = [根目录, 根笔记.md, 工作, 周报.md, 周会, 纪要.md]
    frame("sel1");
    DOCTEST_CHECK_EQ(treeRowIndex(ui, "根目录", true), 0);
    DOCTEST_CHECK_EQ(treeRowIndex(ui, "根笔记.md", false), 1);
    DOCTEST_CHECK_EQ(treeRowIndex(ui, "工作", true), 2);
    DOCTEST_CHECK_EQ(treeRowIndex(ui, "周报.md", false), 3);
    DOCTEST_CHECK_EQ(treeRowIndex(ui, "周会", true), 4);
    DOCTEST_CHECK_EQ(treeRowIndex(ui, "纪要.md", false), 5);
    // ① 初始选中 = 根目录（兜底默认：根目录行就是"被选中的那一层"，不是"无选中"）
    DOCTEST_CHECK(folderRowSelected(ui, "根目录"));

    // ② 点二级目录「周会」行 → 选中 + 新建笔记落该目录
    const int weekMeeting = treeRowIndex(ui, "周会", true);
    DOCTEST_REQUIRE(clickId("md.tree.node." + std::to_string(weekMeeting) + ".row"));
    frame("sel2");
    DOCTEST_CHECK(folderRowSelected(ui, "周会"));
    DOCTEST_CHECK(!folderRowSelected(ui, "根目录"));   // 上一选中行回落
    DOCTEST_CHECK(!folderRowSelected(ui, "工作"));
    // 弹窗提示行 = 选中目录（模块状态 → 界面文案的同源证据）
    DOCTEST_REQUIRE(clickId("md.tree.new.bg"));
    frame("sel3");
    DOCTEST_CHECK_EQ(ui.find("md.dialog.new.hint")->text,
                     std::string("保存到：工作/周会 · 名字可带子目录，如 工作/周会.md"));
    DOCTEST_REQUIRE(typeId("md.dialog.new.input", "二级选中笔记"));
    frame("sel4");
    DOCTEST_REQUIRE(clickId("md.dialog.new.confirm.bg"));
    frame("sel5");
    DOCTEST_CHECK(fs::is_regular_file(env.dir / "工作" / "周会" / "二级选中笔记.md"));
    DOCTEST_CHECK(!fs::exists(env.dir / "二级选中笔记.md"));            // 不是根
    DOCTEST_CHECK(!fs::exists(env.dir / "工作" / "二级选中笔记.md"));   // 也不是父目录
    // 打开这篇笔记后，选中态仍是它所在目录（选中态 = 当前所在目录）
    DOCTEST_CHECK(folderRowSelected(ui, "周会"));

    // ③ 点一级目录「工作」行 → 新建笔记落 工作/
    const int work = treeRowIndex(ui, "工作", true);
    DOCTEST_REQUIRE(clickId("md.tree.node." + std::to_string(work) + ".row"));
    frame("sel6");
    DOCTEST_CHECK(folderRowSelected(ui, "工作"));
    DOCTEST_CHECK(!folderRowSelected(ui, "周会"));
    DOCTEST_REQUIRE(clickId("md.tree.new.bg"));
    frame("sel7");
    DOCTEST_REQUIRE(typeId("md.dialog.new.input", "一级选中笔记"));
    frame("sel8");
    DOCTEST_REQUIRE(clickId("md.dialog.new.confirm.bg"));
    frame("sel9");
    DOCTEST_CHECK(fs::is_regular_file(env.dir / "工作" / "一级选中笔记.md"));
    DOCTEST_CHECK(!fs::exists(env.dir / "一级选中笔记.md"));

    // ④ 点根目录行 → 新建笔记落根
    DOCTEST_REQUIRE(clickId("md.tree.node.0.row"));
    frame("sel10");
    DOCTEST_CHECK(folderRowSelected(ui, "根目录"));
    DOCTEST_REQUIRE(clickId("md.tree.new.bg"));
    frame("sel11");
    DOCTEST_REQUIRE(typeId("md.dialog.new.input", "根选中笔记"));
    frame("sel12");
    DOCTEST_REQUIRE(clickId("md.dialog.new.confirm.bg"));
    frame("sel13");
    DOCTEST_CHECK(fs::is_regular_file(env.dir / "根选中笔记.md"));
    DOCTEST_CHECK(!fs::exists(env.dir / "工作" / "根选中笔记.md"));

    // ⑤ 点**笔记行**（纪要.md）→ 文件夹不级联选中，但功能目标跟到 parent dir
    const int noteRow = treeRowIndex(ui, "纪要.md", false);
    DOCTEST_REQUIRE(noteRow >= 0);
    DOCTEST_REQUIRE(clickId("md.tree.node." + std::to_string(noteRow) + ".row"));
    frame("sel14");
    // 选中不级联：父文件夹「周会」不入选
    DOCTEST_CHECK(!folderRowSelected(ui, "周会"));
    DOCTEST_CHECK(folderRowSelected(ui, "根目录"));  // 上一选中不变
    // 功能目标已跟到笔记所在目录（保存到提示行验证）
    DOCTEST_REQUIRE(clickId("md.tree.new.bg"));
    frame("sel14a");
    DOCTEST_CHECK_EQ(ui.find("md.dialog.new.hint")->text,
                     std::string("保存到：工作/周会 · 名字可带子目录，如 工作/周会.md"));
    DOCTEST_REQUIRE(clickId("md.dialog.new.cancel.bg"));  // 关弹窗，不创建

    // ⑥ 箭头命中区只收起/展开，不改选中态（⑤ 之后选中仍是「根目录」，
    // 打开笔记不级联、不覆盖文件夹选中态）
    const int weekMeeting2 = treeRowIndex(ui, "周会", true);
    DOCTEST_REQUIRE(clickId("md.tree.node." + std::to_string(weekMeeting2) + ".toggle"));
    frame("sel15");
    // 收起：纪要不在了（该目录整棵子树隐藏），但选中态不变
    DOCTEST_CHECK_EQ(treeRowIndex(ui, "纪要.md", false), -1);
    DOCTEST_CHECK(folderRowSelected(ui, "根目录"));
    DOCTEST_REQUIRE(clickId("md.tree.node." + std::to_string(weekMeeting2) + ".toggle"));
    frame("sel16");
    DOCTEST_CHECK_EQ(treeRowIndex(ui, "纪要.md", false) >= 0, true);  // 展开回来

    mod->onStop();
    return true;
})

// 根行选中 → 新建文件夹：落点 = 笔记根（逻辑）+ 树里在根行之下（几何）
// 两个面分开钉。落点派生本就正确（根行 relPath "" → newItemParentDir 返回 "" =
// 笔记根，service 端 isSafeNewNotePath 还挡死 ".."/绝对路径），错的是 md_tree 把
// **顶层夹**与根组行画成同一 depth（都 0）→ 建出来的顶层夹在树里与根行同级，读成
// 「根的兄弟 / 笔记根之外」。本用例同时锁两头：
//   a. 逻辑：根行选中新建 → 父路径 == 笔记根（env.dir/x）；不是笔记根之外
//      （env.dir.parent()/x）、也不是先选中过又切走的 工作/x；弹窗「创建在：」
//      显示 = 根目录（模块知道落点，显示与落盘一致）；
//   b. 几何：新夹行 name x == 根行 name x + 16（根的**子**行，与根的直接文件行
//      同层），且 == 其它顶层夹（工作）的 x（同层对齐）。
// 反重构：把顶层夹画成与根行同 depth（兄弟位）→ b 组断言红。
REGISTER_TEST(md_module_root_row_new_folder_inside,
              "markdown_module: 根行选中新建文件夹落笔记根 + 树里呈现为根的子行（headless）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "rootnewfolder", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "solo.md", "# solo\n"));
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
        ui.layout(screen.width, screen.height);  // 几何断言前必须 layout 出 frame.x
    };
    const auto clickId = [&](const std::string& id) -> bool {
        const auto* el = ui.find(id);
        if (el == nullptr || el->onClick == nullptr) return false;
        el->onClick();
        return true;
    };
    const auto typeId = [&](const std::string& id, const std::string& text) -> bool {
        const auto* hit = ui.find(id + ".hit");
        if (hit == nullptr || hit->onTextInput == nullptr) return false;
        core::TextInputEvent ev;
        ev.text = text;
        hit->onTextInput(ev);
        return true;
    };
    const auto xOfName = [&](const std::string& displayName) -> float {
        const int i = treeRowIndex(ui, displayName, /*folder=*/true);
        if (i < 0) return -1.0f;
        const auto* el = ui.find("md.tree.node." + std::to_string(i) + ".name");
        return el != nullptr ? el->frame.x : -1.0f;
    };

    // 帧1：树 = [根目录, solo.md, 工作, 周报.md]
    frame("rf1");
    DOCTEST_CHECK_EQ(treeRowIndex(ui, "根目录", true), 0);
    DOCTEST_CHECK_EQ(treeRowIndex(ui, "solo.md", false), 1);
    DOCTEST_CHECK_EQ(treeRowIndex(ui, "工作", true), 2);
    DOCTEST_CHECK_EQ(treeRowIndex(ui, "周报.md", false), 3);

    // 先选「工作」再点根目录行（复现用户路径：选中态从子目录切回根）
    const int work0 = treeRowIndex(ui, "工作", true);
    DOCTEST_REQUIRE(clickId("md.tree.node." + std::to_string(work0) + ".row"));
    frame("rf2");
    DOCTEST_CHECK(folderRowSelected(ui, "工作"));
    DOCTEST_REQUIRE(clickId("md.tree.node.0.row"));
    frame("rf3");
    DOCTEST_CHECK(folderRowSelected(ui, "根目录"));
    DOCTEST_CHECK(!folderRowSelected(ui, "工作"));

    // + 文件夹：弹窗「创建在」显示 = 根目录（落点提示与实际落点同源）
    DOCTEST_REQUIRE(clickId("md.tree.newfolder.bg"));
    frame("rf4");
    DOCTEST_CHECK_EQ(ui.find("md.dialog.newfolder.parent.dd.label")->text,
                     std::string("根目录"));
    DOCTEST_REQUIRE(typeId("md.dialog.newfolder.input", "新目录"));
    frame("rf5");
    DOCTEST_REQUIRE(clickId("md.dialog.newfolder.confirm.bg"));
    frame("rf6");

    // a. 逻辑：落点 = 笔记根（不是笔记根之外、不是之前选中过的 工作）
    DOCTEST_CHECK(fs::is_directory(env.dir / "新目录"));
    DOCTEST_CHECK(!fs::exists(env.dir.parent_path() / "新目录"));
    DOCTEST_CHECK(!fs::exists(env.dir / "工作" / "新目录"));
    // 建完选中态跟到新夹（与「新建的目录 = 新的选中目录」既有口径一致）
    DOCTEST_CHECK(folderRowSelected(ui, "新目录"));

    // b. 几何：新顶层夹行 = 根行的**子**行（x = 根行 + 16），与其它顶层夹同层；
    // b. 几何：新顶层夹行 = 根行的**子**行（x = 根行 + 16），与其它顶层夹同层；
    //    画成与根行同 x（兄弟位）→ 本组断言红
    const float rootX = xOfName("根目录");
    const float newX = xOfName("新目录");
    const float workX = xOfName("工作");
    DOCTEST_CHECK_MESSAGE(newX == rootX + 16.0f,
                          "新顶层夹 name x=" + std::to_string(newX) +
                              " ≠ 根行 x=" + std::to_string(rootX) + " + 16（应为其子行）");
    DOCTEST_CHECK_MESSAGE(workX == rootX + 16.0f,
                          "顶层夹 工作 name x=" + std::to_string(workX) +
                              " ≠ 根行 x=" + std::to_string(rootX) + " + 16");
    DOCTEST_CHECK_MESSAGE(newX == workX,
                          "同为顶层夹，新目录与 工作 x 不一致: " + std::to_string(newX) +
                              " vs " + std::to_string(workX));

    // 对照组：选中普通目录新建 = 正确落在其子下，且呈现在其行之下
    const int work1 = treeRowIndex(ui, "工作", true);
    DOCTEST_REQUIRE(clickId("md.tree.node." + std::to_string(work1) + ".row"));
    frame("rf7");
    DOCTEST_CHECK(folderRowSelected(ui, "工作"));
    DOCTEST_REQUIRE(clickId("md.tree.newfolder.bg"));
    frame("rf8");
    DOCTEST_CHECK_EQ(ui.find("md.dialog.newfolder.parent.dd.label")->text,
                     std::string("工作"));
    DOCTEST_REQUIRE(typeId("md.dialog.newfolder.input", "子夹"));
    frame("rf9");
    DOCTEST_REQUIRE(clickId("md.dialog.newfolder.confirm.bg"));
    frame("rf10");
    DOCTEST_CHECK(fs::is_directory(env.dir / "工作" / "子夹"));
    const float subX = xOfName("子夹");
    DOCTEST_CHECK_MESSAGE(subX == workX + 16.0f,
                          "子夹 name x=" + std::to_string(subX) +
                              " ≠ 工作 x=" + std::to_string(workX) + " + 16");

    mod->onStop();
    return true;
})

// 2b4b'''. ④ 笔记文件名显示 .md 扩展名（树 + 重命名弹窗）+ 改名不丢扩展名
//
// 用户要求：树里笔记条目显示**完整文件名（含 .md）**、重命名弹窗输入框里也是完整
// 文件名；文件夹条目不加扩展名。用户能改到扩展名 → 必须保证改完仍是 .md
// 反重构警告：树条目与重命名弹窗预填都用 **stem**（去扩展名），落盘由 service
// 补 .md。三处各自独立：任一处改掉，对应断言红——尤其「改名不带 .md 仍落 .md」
// 那条（否则笔记会从树里消失）。
REGISTER_TEST(md_module_note_filename_and_rename_md,
              "markdown_module: ④ 树/重命名弹窗显示完整文件名（含 .md）+ 改名后仍是 .md（headless）", {
    using InputState = components::input_detail::InputModel::InputState;
    test_support::MdHeadlessEnv env(kDirPrefix, "mdname", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "根笔记.md", "# 根笔记\n"));
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
    const auto clickId = [&](const std::string& id) -> bool {
        const auto* el = ui.find(id);
        if (el == nullptr || el->onClick == nullptr) return false;
        el->onClick();
        return true;
    };
    const auto typeId = [&](const std::string& id, const std::string& text) -> bool {
        const auto* hit = ui.find(id + ".hit");
        if (hit == nullptr || hit->onTextInput == nullptr) return false;
        core::TextInputEvent ev;
        ev.text = text;
        hit->onTextInput(ev);
        return true;
    };
    const auto replaceInput = [&](const std::string& id, const std::string& text) -> bool {
        const std::string stateId = id;
        auto& st = ui.state<InputState>(stateId);
        st.selectionStart = 0;
        st.selectionEnd = static_cast<int>(st.text.size());
        return typeId(id, text);
    };

    // ① 树条目显示完整文件名（含 .md）；文件夹条目不带扩展名
    frame("md1");
    DOCTEST_CHECK_EQ(treeRowIndex(ui, "根笔记.md", false), 1);
    DOCTEST_CHECK_EQ(treeRowIndex(ui, "工作", true), 2);
    DOCTEST_CHECK_EQ(treeRowIndex(ui, "周报.md", false), 3);
    const auto* folderName = ui.find("md.tree.node.2.name");
    DOCTEST_REQUIRE(folderName != nullptr);
    DOCTEST_CHECK_EQ(folderName->text, std::string("工作"));           // 文件夹不加 .md
    const auto* fileName = ui.find("md.tree.node.3.name");
    DOCTEST_REQUIRE(fileName != nullptr);
    DOCTEST_CHECK_EQ(fileName->text, std::string("周报.md"));           // 文件带 .md

    // ② 重命名弹窗预填 = 完整相对路径（含 .md）
    const int weekReport = treeRowIndex(ui, "周报.md", false);
    eui::PointerEvent pe;
    pe.x = 120.0;
    pe.y = 210.0;
    const auto* rowEl = ui.find("md.tree.node." + std::to_string(weekReport) + ".row");
    DOCTEST_REQUIRE(rowEl != nullptr);
    DOCTEST_REQUIRE(rowEl->onContextMenu != nullptr);
    rowEl->onContextMenu(pe, eui::Rect{});
    frame("md2");
    DOCTEST_REQUIRE(clickId("md.ctx.level.0.item.0"));  // 右键菜单 → 重命名
    frame("md3");
    const auto* prefill = ui.find("md.dialog.rename.input.text");
    DOCTEST_REQUIRE(prefill != nullptr);
    DOCTEST_CHECK_EQ(prefill->text, std::string("工作/周报.md"));

    // ③ 输入**不含 .md** 的名字提交 → 落盘文件名仍以 .md 结尾（关键档）
    // 注意口径：输入框内容 = 相对笔记目录的路径，所以「周报改名」= 移到根目录下
    // 改名（与弹窗提示「填「工作/新名」可同时移动」一致）
    DOCTEST_CHECK(ui.find("md.dialog.rename.panel")->opacity > 0.0f);
    DOCTEST_REQUIRE(replaceInput("md.dialog.rename.input", "周报改名"));
    frame("md4");
    DOCTEST_REQUIRE(clickId("md.dialog.rename.confirm.bg"));
    frame("md5");
    DOCTEST_CHECK(fs::is_regular_file(env.dir / "周报改名.md"));
    DOCTEST_CHECK(!fs::exists(env.dir / "周报改名"));            // 不带 .md 的裸文件
    DOCTEST_CHECK(!fs::exists(env.dir / "工作" / "周报改名.md")); // 相对路径语义：移到根
    DOCTEST_CHECK_EQ(treeRowIndex(ui, "周报改名.md", false) >= 0, true);  // 仍在树里

    // ③' 换成别的扩展名 + 写回子目录 → 按「追加不替换」规则：仍以 .md 结尾、仍在树里
    const int renamed = treeRowIndex(ui, "周报改名.md", false);
    DOCTEST_REQUIRE(renamed >= 0);
    const auto* row2 = ui.find("md.tree.node." + std::to_string(renamed) + ".row");
    DOCTEST_REQUIRE(row2 != nullptr);
    DOCTEST_REQUIRE(row2->onContextMenu != nullptr);
    row2->onContextMenu(pe, eui::Rect{});
    frame("md6");
    DOCTEST_REQUIRE(clickId("md.ctx.level.0.item.0"));
    frame("md7");
    DOCTEST_CHECK_EQ(ui.find("md.dialog.rename.input.text")->text,
                     std::string("周报改名.md"));
    DOCTEST_REQUIRE(replaceInput("md.dialog.rename.input", "工作/周报改名.txt"));
    frame("md8");
    DOCTEST_REQUIRE(clickId("md.dialog.rename.confirm.bg"));
    frame("md9");
    DOCTEST_CHECK(fs::is_regular_file(env.dir / "工作" / "周报改名.txt.md"));
    DOCTEST_CHECK(!fs::exists(env.dir / "工作" / "周报改名.txt"));      // 没有丢掉 .md
    DOCTEST_CHECK_EQ(treeRowIndex(ui, "周报改名.txt.md", false) >= 0, true);
    DOCTEST_CHECK_EQ(treeRowIndex(ui, "工作", true) >= 0, true);        // 子目录组还在

    mod->onStop();
    return true;
})

// 2b4c. 只有空文件夹（零笔记）→ 树照样出文件夹行，不叠空态提示

REGISTER_TEST(md_module_tree_empty_folders_only,
              "markdown_module: W10 只有空文件夹（零笔记）→ 顶层夹/子夹行都在，空态提示不覆盖（headless）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "folders0", kDbName);
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

// 2b5. 新建笔记/文件夹弹窗错误 → 内嵌红字 formError + 输入清除

REGISTER_TEST(md_module_dialog_form_error,
              "markdown_module: 弹窗创建错误内嵌红字 + 下一次输入清除（headless）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "formerror", kDbName);
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

    // 3) 新建笔记非法路径名提交 → 内嵌红字（须与其他校验路径同口径）
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

// 2b2. 双栏宽度公式（layout 级）

REGISTER_TEST(md_module_split_layout, "markdown_module: W9 双栏三列铺满 contentW 无残隙（headless layout）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "split", kDbName);
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
    eui::KeyEvent p = test_support::pressShortcut(eui::InputKey::P, /*shift=*/true);
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
        // 列壳 .bg 已去（编辑列为 input 单层）——几何断言
    // 改挂列壳 stack（md.editor），宽度语义不变
    const auto* editor = ui.find("md.editor");
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

// 2b'. tech-debt C：单 stack 列布局语义锁（bg 铺满 / 内容坐标 / 单槽位）

REGISTER_TEST(md_tree_column_layout_semantics,
              "markdown_module: md.tree 单 stack 列语义锁 bg 重合+绝对内容+每列一槽（tech-debt C）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "tree_layout", kDbName);
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

        // 编辑态（默认）：编辑器列只占一个流式槽位
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

// 2c. 大纲浮动面板不挤压编辑区（layout 级）

REGISTER_TEST(md_module_compose_outline_float, "markdown_module: W8 大纲浮动面板 zIndex/不挤压（headless layout）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "outline", kDbName);
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

// 3. pagebg / toast 节点常驻（headless 冒烟）

REGISTER_TEST(md_module_compose_w7, "markdown_module: W7 pagebg/toast compose（headless）", {
    const std::string text = "# W7\n\n冒烟内容。\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "w7", kDbName);
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
        // 自铺不透明底会把 alpha 恒置 1 盖掉效果
    DOCTEST_CHECK(ui.find("md.pagebg") == nullptr);
    // toast 单例常驻（保存失败反馈的载体，visible 由状态位控制）
    DOCTEST_CHECK(ui.find("md.toast") != nullptr);
        // 5 秒自动轮询取代手动重扫
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
// 2d. 标签系统：编辑器标签行 + 左栏标签筛选（headless）

REGISTER_TEST(md_module_tags_ui, "markdown_module: 标签 chip 行 + 左栏标签筛选（headless）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "tags", kDbName);
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

// 2d2. 文本垂直居中根治：标签 chip + 大纲行
// runtime_render.h 只在 verticalAlign == Center 时把 ink 锚到 frame 中线，否则从
// 行框顶沿起排 → 视觉偏上。判据与 md_tree_row_geometry 同款：
//   a. 文本节点挂 VerticalAlign::Center（渲染锚点 = frame 中线）；
//   b. 文本盒在底框内。
// 删任一 .verticalAlign(Center) → 该站点 a 断言红（frame 数值不变，只有渲染锚点
// 变了）；把文本盒挪出底框 → b 断言红。
REGISTER_TEST(md_chip_and_outline_text_centered,
              "markdown: 标签 chip / 大纲行文本垂直居中（数值锁）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "chipcenter", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "note.md", "# A\n正文\n"));

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
        ui.layout(screen.width, screen.height);  // 几何断言前必须 layout 出 frame
    };

        // 判据 helper：空串 = 通过（断言宏展开含 return，只能写在用例体里，
    // helper 回传结论、由用例断）。textId 的 frame 须与底框（chip 底 / 行底
    // rect）中心一致，且挂 VerticalAlign::Center。
    const auto centeredProblem = [&](const std::string& textId,
                                     const std::string& bgId) -> std::string {
        const auto* txt = ui.find(textId);
        const auto* bg = ui.find(bgId);
        if (txt == nullptr) return "缺文本 " + textId;
        if (bg == nullptr) return "缺底框 " + bgId;
        const std::string box =
            textId + "{cy=" +
            std::to_string(txt->frame.y + txt->frame.height * 0.5f) +
            ",h=" + std::to_string(txt->frame.height) + ",lh=" +
            std::to_string(txt->lineHeight) + "} bg{cy=" +
            std::to_string(bg->frame.y + bg->frame.height * 0.5f) +
            ",h=" + std::to_string(bg->frame.height) + "}";
        if (txt->verticalAlign != eui::VerticalAlign::Center) {
            return textId + " 未挂 VerticalAlign::Center（ink 从行框顶沿起排）";
        }
        if (std::fabs((txt->frame.y + txt->frame.height * 0.5f) -
                      (bg->frame.y + bg->frame.height * 0.5f)) > 0.5f) {
            return textId + " frame 中心 y != 底框中心 y " + box;
        }
        return {};
    };

    // 帧1：打开 note.md（真实树行 onClick 链路）
    frame("cc1");
    const core::dsl::Element* fileRow = nullptr;
    for (int i = 0; i < 6 && fileRow == nullptr; ++i) {
        const auto* nm = ui.find("md.tree.node." + std::to_string(i) + ".name");
        if (nm != nullptr && nm->text == "note.md") {
            fileRow = ui.find("md.tree.node." + std::to_string(i) + ".row");
        }
    }
    DOCTEST_REQUIRE(fileRow != nullptr);
    DOCTEST_REQUIRE(fileRow->onClick != nullptr);
    fileRow->onClick();

    // 帧2：编辑器标签行出现 → 输入「工作」并点「＋」提交
    frame("cc2");
    const auto* addHit = ui.find("md.tags.add.hit");
    DOCTEST_REQUIRE(addHit != nullptr);
    DOCTEST_REQUIRE(addHit->onTextInput != nullptr);
    core::TextInputEvent tie;
    tie.text = "工作";
    addHit->onTextInput(tie);
    frame("cc3");
    const auto* addBtn = ui.find("md.tags.addbtn.bg");
    DOCTEST_REQUIRE(addBtn != nullptr);
    DOCTEST_REQUIRE(addBtn->onClick != nullptr);
    addBtn->onClick();
    frame("cc4");

    // 编辑器顶栏 chip：名称 + ×（两处底框：chip 底 / 删除热区）
    DOCTEST_REQUIRE(ui.find("md.tags.chip.0.name") != nullptr);
    // 判定纪律：本框架 fail-fast（第一个失败即 return），六处站点累积成一条
    // 结论再断言——一次运行就能看全哪些站点没居中，而不是只看到第一个。
    std::string problems;
    const auto collect = [&](const std::string& textId, const std::string& bgId) {
        const std::string p = centeredProblem(textId, bgId);
        if (!p.empty()) problems += (problems.empty() ? "" : " | ") + p;
    };
    collect("md.tags.chip.0.name", "md.tags.chip.0.bg");
    collect("md.tags.chip.0.x", "md.tags.chip.0.del");
    // 左栏标签筛选 chip（同标签带笔记数）
    DOCTEST_REQUIRE(ui.find("md.tags.filter.0.name") != nullptr);
    collect("md.tags.filter.0.name", "md.tags.filter.0.bg");

    // 帧5：顶栏大纲开关 → 浮动面板 + 大纲行（底 = 行 rect，含 hover/选中高亮）
    const auto* toggle = ui.find("md.outline.toggle.hit");
    DOCTEST_REQUIRE(toggle != nullptr);
    DOCTEST_REQUIRE(toggle->onClick != nullptr);
    toggle->onClick();
    frame("cc5");
    DOCTEST_REQUIRE(ui.find("md.outline.item.0.label") != nullptr);
    collect("md.outline.item.0.label", "md.outline.item.0.row");
    // 状态栏左右两段（B1 评审补漏：文本盒 == 状态栏底 rect 高 22、同 y）
    DOCTEST_REQUIRE(ui.find("md.status.bg") != nullptr);
    collect("md.status.save", "md.status.bg");
    collect("md.status.pos", "md.status.bg");

    DOCTEST_CHECK_MESSAGE(problems.empty(), problems);

    mod->onStop();
    return true;
})

// 2e. 生命周期：树行右键菜单 → 删除/重命名 真实链路

REGISTER_TEST(md_module_delete_note_ui, "markdown_module: 右键→删除→确认弹窗（取消保留/确认落删），编辑器联动清空（headless）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "delui", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "a.md", "# A\n正文A\n"));
    DOCTEST_REQUIRE(writeTextFile(env.dir / "b.md", "# B\n正文B\n"));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    // 删笔记是 GitHub Sync 的 dirty 触发源之一：只有确认删除成功才发 md.deleted
    int gotDeleted = 0;
    auto delSub = core::event::bus().subscribe(
        core::event::kMdDeleted, [&](const std::string&) { ++gotDeleted; });

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
    DOCTEST_CHECK_EQ(gotDeleted, 0);                    // 取消路径：没删成，不发事件

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
    DOCTEST_CHECK_EQ(gotDeleted, 1);   // 确认删除成功 → md.deleted 恰一次
    frame("d8");
    DOCTEST_CHECK_FALSE(treeHas("a.md"));
    DOCTEST_CHECK(treeHas("b.md"));
    // 删的是当前打开的笔记 → 编辑器回到空态提示
    DOCTEST_CHECK(ui.find("md.editor.hint") != nullptr);

    mod->onStop();
    return true;
})

// 2e2. 右键菜单坐标：窗口坐标 → 页面坐标（减页面区原点）
// 指针事件是窗口坐标（runtime_input 派发只做 /dpiScale），而菜单浮层构建在
// 页面子树内（app.page，原点 = 侧边栏右侧）——不换算则菜单整体右移一个
// 侧边栏宽（展开 208 / 折叠 72）不能跑进右侧详情区

REGISTER_TEST(md_ctx_menu_page_origin, "markdown_module: 右键菜单坐标扣页面区原点（窗口→页面坐标，headless）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "ctxorigin", kDbName);
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

        // 反证：不接 Router（无页面区原点）时窗口坐标原样传出
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

    test_support::MdHeadlessEnv env(kDirPrefix, "renui", kDbName);
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

// 2f. 新建笔记即时性：默认名快速创建 + 编辑器聚焦接线

REGISTER_TEST(md_module_default_name_create, "markdown_module: 空名提交=「无标题笔记」秒建+撞名编号，编辑器随开（headless）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "defname", kDbName);
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
        // 建「无标题笔记.md」并打开
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

// subtitle 静态操作说明（对齐 Todo 同类口径）
// 旧行为：subtitle = 「N 篇笔记 · 最后一次操作结果」，操作日志顶掉快捷键
// 提示且永久驻留。新行为：subtitle 恒为静态说明；操作结果走 toast（status_
// 成员已删）；笔记数进编辑器底部状态栏右侧
REGISTER_TEST(md_module_subtitle_static, "markdown_module: subtitle 静态快捷键说明 + 笔记数在状态栏（headless）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "subtitle", kDbName);
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

        // subtitle 含快捷键提示（键盘流 + Ctrl+Shift+P 四态都是真实能力），
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
    // 「已打开」toast 已删（用户 2026-10-14）：打开笔记不再弹提示，subtitle 恒静态。
    // toast 元素常驻 UI 树（ui_kit::drawToast 无条件 compose，visible 只控显隐），
    // find 不到是结构性不可能；判据 = 标题文本为空（组件默认占位串被空串覆盖）→
    // 本次打开没有触发任何 toast。
    const auto* toastTitle = ui.find("md.toast.title");
    DOCTEST_CHECK(toastTitle == nullptr || toastTitle->text.empty());
    // 笔记数落点：编辑器底部状态栏右侧
    const auto* pos = ui.find("md.status.pos");
    DOCTEST_REQUIRE(pos != nullptr);
    DOCTEST_CHECK(pos->text.find("篇笔记") != std::string::npos);

    mod->onStop();
    return true;
})

// 创建/编辑弹窗与标签输入框 Esc=取消（onCancel 取代 Esc→提交）

REGISTER_TEST(md_module_esc_cancel_dialogs,
              "markdown_module: 新建笔记/文件夹/重命名 Esc=取消不落盘；标签 Esc=丢草稿（headless）", {
    using InputState = components::input_detail::InputModel::InputState;

    test_support::MdHeadlessEnv env(kDirPrefix, "esccancel", kDbName);
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
        // 草稿清空 → 输入框回 placeholder；标签未被提交
    DOCTEST_CHECK_EQ(ui.find("md.tags.add.text")->text, std::string("＋ 标签"));
    DOCTEST_CHECK(ui.find("md.tags.chip.0.name") == nullptr);

    mod->onStop();
    return true;
})

// Ctrl+N 新建笔记快捷键（app 级通道，同弹窗入口）

REGISTER_TEST(md_module_ctrl_n_new_note,
              "markdown_module: Ctrl+N 开新建笔记弹窗；弹窗开着再按不重置草稿；完整创建链路（headless）", {
    using InputState = components::input_detail::InputModel::InputState;

    test_support::MdHeadlessEnv env(kDirPrefix, "ctrln", kDbName);
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

    // 1) Ctrl+N（isDown + 快捷键修饰位，无 shift/alt）→ 模块消费、弹窗打开
    frame("cn1");
    eui::KeyEvent cn;
    cn.key = eui::InputKey::N;
    test_support::setShortcutModifier(cn.modifiers);
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

        // 3) 确认 → 走既有 commitNewNote 链路：落盘 + 弹窗关（与按钮同源）
    const auto* confirm = ui.find("md.dialog.new.confirm.bg");
    DOCTEST_REQUIRE(confirm != nullptr && confirm->onClick != nullptr);
    confirm->onClick();
    DOCTEST_CHECK(fs::exists(env.dir / "草稿.md"));
    frame("cn5");
    DOCTEST_CHECK_EQ(ui.find("md.dialog.new.panel")->opacity, 0.0f);

    // 4) Ctrl+Shift+N 不触发（显式排除 shift 组合，留给「无痕新窗口」类语义）
    eui::KeyEvent csn;
    csn.key = eui::InputKey::N;
    test_support::setShortcutModifier(csn.modifiers);
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
// 重命名弹窗校验错误改内嵌红字 formError

REGISTER_TEST(md_module_rename_form_error,
              "markdown_module: 重命名空名/撞名/非法路径 → 内嵌红字 + 弹窗不关 + 输入清除 + 打开清残留（headless）", {
    using InputState = components::input_detail::InputModel::InputState;

    test_support::MdHeadlessEnv env(kDirPrefix, "rferr", kDbName);
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

        // 1) 空名提交 → 红字「新名字不能为空」，弹窗不关
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

// 状态栏（行:列 · N 字）缓存等价
// 全文 O(n) 扫（byteOffsetToLine + utf8Length）改为按需重算。契约：脏标记帧与
// (文本修订, 光标) 缓存。等价锁 = 三种帧的结果都与「独立复算」（同公式、纯函数
// 源自 md_line_scanner）逐字一致：①未变帧（缓存命中）②仅光标移动帧（大纲跳转
// 改 cursor 不改文本）③编辑帧（onChange 改文本）。
REGISTER_TEST(md_module_status_bar_cache_equivalence,
              "markdown_module: 状态栏缓存=独立复算（headless，perf [39]）", {
    const std::string text = "# 标题一\n正文一二三\n\n## 标题二\n正文二\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "statuscache", kDbName);
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

// 双栏同步滚动 yA/yB 缓存 + 预览 contentKey 缓存等价
// Split 每次 compose 两次全文前缀 substr + estimateHeight 改为按
// (修订, 锚点, 宽度) 缓存。等价锁：编辑器视口滚到深锚点 → 回顶锚点 →
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
    test_support::MdHeadlessEnv env(kDirPrefix, "splitcache", kDbName);
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
    eui::KeyEvent p = test_support::pressShortcut(eui::InputKey::P, /*shift=*/true);
    DOCTEST_CHECK(mod->onKeyEvent(p));   // → 实时（第四态）
    DOCTEST_CHECK(mod->onKeyEvent(p));   // → 双栏
    frame();
    frame();
    DOCTEST_CHECK(ui.find(kEditorInputId) != nullptr);
    const auto* pvScroll = ui.find("md.preview.scroll");
    DOCTEST_REQUIRE(pvScroll != nullptr);
    DOCTEST_CHECK(pvScroll->scrollMaxOffset > 0.0f);   // 内容高于视口才可滚

    // 大纲面板常驻打开：跳转顶锚点 / 深锚点驱动 computeSyncOffset 换锚
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

    // 跳深锚点：编辑视口跟随光标下移 → 同步偏移必须变大；
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

// 双栏同步滚动块模型精确锚（headless）
// 标题锚 + 节内按行摊分在单节文档里步长恒定；块模型锚给出
// 块级签名。全部断言从模块实时读数反推、不复制实现公式：
//   ① 块内相邻行步长 = H_k/lc_k 按块而异（4 行段 0.8u vs 2 行段 0.67u）
//      —— 全文档单一斜率被证伪
//   ② 块界步长（末行 → 下块首行）− 块内步长 = blockGap，对段落/列表/
//      围栏等异型块界给出同一常数 —— 精确累计 Σ(H_i+gap) 的几何签名，
//      旧机制此值恒为 0
//   ③ 同型块跨 rep 步长一致（累计漂移为零）
//   ④ 文首插入一个与已测 paraA 逐字同源的段落块后，深层未变块首行读数
//      位移 == 插入块高（= s_A×lc_A）+ gap（锚点跟随块几何）
// 编辑器行高 = editorFontSize()×1.2，与模块 kInputLineHeightFactor 同步。
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
    test_support::MdHeadlessEnv env(kDirPrefix, "blockanchor", kDbName);
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
    eui::KeyEvent p = test_support::pressShortcut(eui::InputKey::P, /*shift=*/true);
    DOCTEST_CHECK(mod->onKeyEvent(p));   // → 实时（第四态）
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
    std::map<int, float> probeCache;
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
    // ② 所有异型块界的「界步长 − 块内步长」同一常数
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
        // 标题锚机制下位移 = 全局行斜率 × 插入行数，与此值一般不等。
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

// P3-⑤ 预览块级交互（headless）
// 直接驱动 hit 覆盖层元素（MouseArea 注册的 onPress/onRelease/onHoverChanged）
// 走完整链路：点击 → MdSyncMap 反查 → InputState.cursor 定位；hover →
// 高亮底衬 / CodeFence bg 混色。tap 语义：传入 bounds 高固定 100，hit 的
// 局部 y = event.y × (H/100)（makeMouseEvent 缩放还原），fracY∈[0,1] 即块
// 内顶→底分数位。CodeFence（自绘块）与 Heading（上游 markdown 块）都验证
// ——⑤ 的覆盖不受上游无 hit API 限制。
REGISTER_TEST(md_module_split_block_click_hover,
              "markdown_module: 预览块点击编辑器定位 + hover 高亮（headless，Phase 3 ⑤）", {
    std::string text = "# T\n\npara one\npara two\n\n```\nfence a\nfence b\nfence c\n```\n\ntail line\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "interact", kDbName);
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
    eui::KeyEvent p = test_support::pressShortcut(eui::InputKey::P, /*shift=*/true);
    mod->onKeyEvent(p);  // Editor → 实时（第四态）
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
        eui::Rect bounds{0.0, 0.0, 100.0, 100.0};
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

// Esc 关最上层弹窗/菜单（模块级兜底）
// 删除确认弹窗无输入框 → Esc 无效；右键菜单是盖在弹窗之上的浮层
// （zIndex 1050），Esc 必须先关菜单本身。通道 = Module::onKeyEvent
// （app 级 onKeyEvent → Router 派发到当前页）。
REGISTER_TEST(md_esc_closes_topmost_dialog,
              "markdown_module: Esc 关最上层弹窗/菜单（删除确认无输入框）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "escui", kDbName);
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

// 编程式聚焦的目标必须是 <input id>.hit
// input 组件的聚焦判定是 ui.isFocused(id + ".hit")（components/input.h:113），
// 发基 id 时焦点落在 stack 元素上 → 组件判定未聚焦（不画 caret、按键落回模块
// keyEventHandler_，编辑器收不到字）。本用例走真实入口「预览块点击 → 编辑器
// 定位」验证；用 Runtime 逐帧 compose 才能驱动帧末的 requestFocus 消费端
//（core/runtime/runtime_lifecycle.h:38-42），plain Ui 不会消费挂起请求。
REGISTER_TEST(md_editor_focus_target_is_hit,
              "markdown_module: 预览块点击后聚焦 md.editor.input.hit（headless）", {
    const std::string text = "# T\n\npara one\npara two\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "focushit", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "focus.md", text));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame env2("mdfocus");
    auto frame = [&] { env2.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    frame();
    const auto* fileRow = env2.ui->find("md.tree.node.1.row");
    DOCTEST_REQUIRE(fileRow != nullptr);
    DOCTEST_REQUIRE(fileRow->onClick != nullptr);
    fileRow->onClick();

    // 进双栏态（预览块点击是双栏/预览通路）：循环推进，不依赖「按几次」
    //（默认态与态序随版本演进，见 md_module_split_layout 同款写法）
    eui::KeyEvent p = test_support::pressShortcut(eui::InputKey::P, /*shift=*/true);
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
    eui::Rect bounds{0.0, 0.0, 100.0, 100.0};
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
        // 反向观测：基 id 不是聚焦目标（发了基 id，本行会红）
    DOCTEST_CHECK_FALSE(env2.ui->isFocused(kEditorInputId));
    // 组件按「本帧收到的 focused」画 caret/边框 → 焦点生效的观感在次帧出现
    //（requestFocus 帧末消费，≈16ms 延迟）
    frame();
    DOCTEST_CHECK(env2.ui->isFocused("md.editor.input.hit"));
    // 用户可见后果：组件判定聚焦 → 画 caret
    DOCTEST_CHECK(env2.ui->find("md.editor.input.cursor") != nullptr);

    mod->onStop();
    return true;
})

// 第四态「实时」骨架（只读）

// 四态循环逐态断言可见元素集合 + segmented 视觉顺序/逆映射（与枚举值顺序解耦）
REGISTER_TEST(md_module_live_mode_skeleton,
              "markdown_module: 第四态（实时）骨架 四态循环/元素集合/分段映射（headless）", {
    const std::string text = "# T\n\npara one\n\n```\nfence\n```\n\ntail\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "live", kDbName);
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

    eui::KeyEvent p = test_support::pressShortcut(eui::InputKey::P, /*shift=*/true);

    // → 实时（第四态）：单栏块级渲染；无 input，也不出现预览列
    DOCTEST_CHECK(mod->onKeyEvent(p));
    frame();
    DOCTEST_REQUIRE(ui.find("md.live.scroll") != nullptr);
    // 列壳 .bg 已去（实时列为 input 单层）——改挂列壳 stack
    DOCTEST_CHECK(ui.find("md.live") != nullptr);
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

// 持久化：mode=3（实时）通过净化与存读往返；旧值 0-2 语义不变；越界回落 0
REGISTER_TEST(md_view_state_mode_live,
              "markdown: 视图状态 mode 3（实时）通过净化 + 0-2 旧语义不变", {
    using module::markdown::MarkdownViewState;
    using module::markdown::MarkdownViewStateStore;
    // 视图状态落 app_config 表的一行，故本用例持一份已 migrate 的临时库。
    test_support::TestDb env("devdesk_test_md_live_mode_", "mode");
    core::data::SqliteDb& db = env.handle();

    // 既有数据（同格式 JSON）→ mode 0-2 语义完全不变
    for (int m = 0; m <= 2; ++m) {
        test_support::writeRawConfigRow(
            &db, module::markdown::kMarkdownViewStateKey,
            "{\"lastPath\": \"old.md\", \"mode\": " + std::to_string(m) +
                ", \"collapsedDirs\": []}");
        const MarkdownViewState loaded = MarkdownViewStateStore(&db).load();
        DOCTEST_CHECK_EQ(loaded.mode, m);
        DOCTEST_CHECK_EQ(loaded.lastPath, std::string("old.md"));
    }

    // mode=3（Live）在合法域内：净化保留 + 存读往返
    MarkdownViewState live;
    live.mode = 3;
    DOCTEST_CHECK_EQ(MarkdownViewStateStore::sanitized(live).mode, 3);
    {
        MarkdownViewStateStore store(&db);
        DOCTEST_CHECK(store.save(live).isOk());
    }
    DOCTEST_CHECK_EQ(MarkdownViewStateStore(&db).load().mode, 3);

    // 越界回落 0（新域上界为 3）
    MarkdownViewState over;
    over.mode = 4;
    DOCTEST_CHECK_EQ(MarkdownViewStateStore::sanitized(over).mode, 0);
    MarkdownViewState neg;
    neg.mode = -1;
    DOCTEST_CHECK_EQ(MarkdownViewStateStore::sanitized(neg).mode, 0);
    return true;
})

// 实时态活动块单输入 + 内容贴合高度

namespace {

using MdInputState = components::input_detail::InputModel::InputState;

// 本帧渲染出的块包裹 id 列表（prefix = "md.live" / "md.preview"），
// 用于比较两种模式下的「块序列」是否一致
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
    eui::KeyEvent p = test_support::pressShortcut(eui::InputKey::P, /*shift=*/true);
    return mod->onKeyEvent(p);
}

} // namespace

// 活动块 input 就位、内容贴合高度、不吃外层滚轮、聚焦打 .hit、非活动块仍是只读渲染
REGISTER_TEST(md_module_live_active_input,
              "markdown_module: 实时态活动块 input 就位 + 内容贴合高度（headless）", {
    const std::string text = "# T\n\npara one\npara two\npara three\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "liveinput", kDbName);
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
    test_support::HeadlessFrame e("mdlive");
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

    // ② 内容贴合高度：同一 measureLines + 同字体族/字号/文本宽
    //    （inset=10、未开滚动条 → 文本宽 = 声明宽 − 20），容差 ±1px
    const float fs = core::theme::editorFontSize();
    const float textW = std::max(0.0f, input->frame.width - 20.0f);
    const int lines = static_cast<int>(
        components::input_detail::InputModel::measureLines(
            blockText, "Microsoft YaHei", fs, textW).size());
    const float expectH = 20.0f + static_cast<float>(lines) * fs * 1.2f + 1.0f;
    DOCTEST_CHECK(std::fabs(input->frame.height - expectH) <= 1.0f);

    // ③ 真不变量：贴合 ⇒ 组件不注册滚轮处理器（不吃外层滚轮、无内部滚动条）。
    // 偏矮一像素都会让本行变红
    const auto* hit = e.ui->find("md.live.input.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_CHECK(hit->onScroll == nullptr);

    // ④ 聚焦目标是 .hit（组件判定 = ui.isFocused(id + ".hit")）
    DOCTEST_CHECK(e.ui->isFocused("md.live.input.hit"));

    // ⑤ input 建在活动块包裹内：包裹高 = 贴合高（不是渲染块高 H_k）
    const auto* wrap = e.ui->find("md.live.blk.1");
    DOCTEST_REQUIRE(wrap != nullptr);
    DOCTEST_CHECK(std::fabs(wrap->frame.height - input->frame.height) <= 0.01f);
    DOCTEST_CHECK(input->frame.y >= wrap->frame.y - 0.5f);
    DOCTEST_CHECK(input->frame.y <= wrap->frame.y + wrap->frame.height + 0.5f);
    // 活动块的 hit 覆盖层仍建（元素集合稳定）但关掉命中——否则它建在 input
    // 之后会抢走指针（打字点不进去）
    const auto* activeHit = e.ui->find("md.live.blk.1.hit");
    DOCTEST_REQUIRE(activeHit != nullptr);
    DOCTEST_CHECK(activeHit->hitTestMode == core::dsl::HitTestMode::None);
    // 非活动块的覆盖层仍是正常命中（hover / 点击定位）
    const auto* otherHit = e.ui->find("md.live.blk.0.hit");
    DOCTEST_REQUIRE(otherHit != nullptr);
    DOCTEST_CHECK(otherHit->hitTestMode != core::dsl::HitTestMode::None);
    // 非活动块仍是只读渲染（渲染内容在，input 只此一个）
    DOCTEST_CHECK(e.ui->find("md.live.block.0") != nullptr);

    mod->onStop();
    return true;
})

// 实时态上行回写 == 编辑态做同一编辑（逐字节），且两种模式下块序列一致
REGISTER_TEST(md_module_live_input_roundtrip,
              "markdown_module: 实时态上行回写 == 编辑态同编辑（逐字节）", {
    const std::string text = "# T\n\npara one\npara two\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "liveround", kDbName);
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

    // 路径 A：实时态打字（真实 input 事件 → onChange → 拼文档 → 写盘）
    std::string liveDoc;
    std::vector<int> liveBlocks;
    {
        DOCTEST_REQUIRE(writeTextFile(env.dir / "rt.md", text));
        std::unique_ptr<::core::Module> mod(create_markdown_module());
        mod->onStart();
        eui::Screen screen;
        screen.width = 1600.0f;
        screen.height = 1000.0f;
        test_support::HeadlessFrame e("mdlive");
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

    // 路径 B：编辑态做同一编辑（同文档偏移、同插入文本）
    std::string editDoc;
    std::vector<int> editBlocks;
    {
        DOCTEST_REQUIRE(writeTextFile(env.dir / "rt.md", text));  // 复位文档
        // 路径 A 退出时处于实时态 → 视图状态会把 mode=3 带到下一次启动。
        // 本段要在【编辑态】做对照编辑，故清掉视图状态：状态存 app_config 表的
        // markdown_view_state 行，删文件已经不生效，必须删那一行（否则本段会以
        // 实时态启动，后面的编辑态断言全找不到控件）。
        // 必须删那一行（否则本段会以实时态启动，后面的编辑态断言全找不到控件）。
        DOCTEST_REQUIRE(core::appconfig::erase(
                            *::core::appDatabase(),
                            module::markdown::kMarkdownViewStateKey)
                            .isOk());
        std::unique_ptr<::core::Module> mod(create_markdown_module());
        mod->onStart();
        eui::Screen screen;
        screen.width = 1600.0f;
        screen.height = 1000.0f;
        test_support::HeadlessFrame e("mdlive");
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

// 实时态活动块锚必须跟随真实光标

namespace {

    // 实时态下在活动块内造出【新块边界】（回车造成的块间空行 / 行首 `# ` / `- `
    // 这类必然打断段落的构造）后，分块器把文档拆成两块 —— 活动块锚必须跟着真实
    // 光标落到【新块】：输入框文本 = 新块文本、块内光标映射回同一文档字节、后续
    // 按键落在该字节处。锚冻结在切块前的旧值上时，光标字节仍落在【前一块】区间内
    // → 下一帧 input 被整段换成前一块文本（用户刚敲的内容从输入框消失）、光标跳回
    // 前块，之后每次按键都写进前块。
//   tag        : 临时笔记目录标记
//   text       : 初始文档（单个块）
//   cursorByte : 进实时态前的编辑器文档光标（= 活动块锚的初始来源）
//   typed      : 逐次 onTextInput 的文本（走真实输入链路，每个事件一帧）
bool liveSplitAnchorScenario(const char* tag,
                             const std::string& text,
                             size_t cursorByte,
                             const std::vector<std::string>& typed) {
    test_support::MdHeadlessEnv env(kDirPrefix, tag, kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "split.md", text));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
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

    // 期望口径 = 分块器：依次在光标处插入等价于把 joined 一次性插到初始光标处，
    // 光标随之推进到插入内容末尾
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
    // ③ 贴合高度未回归（「[live] input over=0」判据：不吃外层滚轮）
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
    // ④ 端到端逐字节：400ms 防抖 + 每帧 flush 后读回磁盘
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

// 状态复位：换笔记 / 启动恢复

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

// 换笔记 = 实时态状态群换代。只复位活动块锚的话，liveOffset_ 残留上一篇的滚动
// 位置（换笔记后仍停在上篇的偏移上、与新旧内容高度错配）
REGISTER_TEST(md_module_live_state_reset_note_switch,
              "markdown_module: 实时态换笔记复位滚动位置（W2 MD-05，headless）", {
    const std::string docA = tallNoteDoc("A", 60);
    const std::string docB = tallNoteDoc("B", 60);
    test_support::MdHeadlessEnv env(kDirPrefix, "livereset", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "a.md", docA));
    DOCTEST_REQUIRE(writeTextFile(env.dir / "b.md", docB));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
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

    // 换笔记 → 滚动位置随新文档归零（滞留 200 即红）
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
    test_support::MdHeadlessEnv env(kDirPrefix, "liveenter", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "a.md", tallNoteDoc("E", 60)));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
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

    // 切回 → 同一 enterLiveState：偏移归零 + 首帧同帧补发 requestFocus
    DOCTEST_REQUIRE(cycleMode(mod.get()));
    frame();
    const auto* back = e.ui->find("md.live.scroll");
    DOCTEST_REQUIRE(back != nullptr);
    DOCTEST_CHECK_EQ(back->scrollOffset, 0.0f);
    DOCTEST_CHECK(e.ui->isFocused("md.live.input.hit"));

    mod->onStop();
    return true;
})

// 启动恢复进实时态（loadViewState）与切实时态同语义——这条路径同样必须复位
// 整个状态位群（否则首帧不聚焦活动 input）
REGISTER_TEST(md_module_live_state_reset_on_restore,
              "markdown_module: 启动恢复实时态走同一复位点（W2 MD-05）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "liverestore", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "a.md", tallNoteDoc("R", 60)));
    // 预置视图状态：mode=3（实时）+ 上次笔记，onStart 尾部恢复
    {
        // 视图状态存 app_config 表的一行（键 kMarkdownViewStateKey，整份一行），
        // 这里直接写那一行。MdHeadlessEnv 已把它注册为进程级 appDatabase。
        DOCTEST_REQUIRE(test_support::writeRawConfigRow(
            ::core::appDatabase(), module::markdown::kMarkdownViewStateKey,
            "{\"lastPath\": \"a.md\", \"mode\": 3, \"collapsedDirs\": []}"));
    }

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
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

// 编辑态与实时态的列壳是同一份实现（drawTextColumnChrome）——同屏宽下两态文本列
// 几何必须逐项相同。两态都不钳列宽：正文列 = 栏宽 − 2×8px 留白。
// 反重构，三条各自独立：
//   ① 把 kTextColumnInsetX 改回 16 → `x 偏移 == 8` 与 `宽 == 栏宽−16` 两条红；
//   ② 把「钳 720 居中」逻辑加回来 → 宽 == 720 而非 1264-16 → 宽断言红；
//   ③ 两态任一改动不留神（如只给编辑态改）→ 实时态几何逐项相同的老断言红。
REGISTER_TEST(md_module_text_column_chrome_shared,
              "markdown_module: 编辑/实时文本列壳几何逐项相同 + ⑤ 编辑区留白最小化（W2 MD-02）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "textchrome", kDbName);
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

    // (2) 打开笔记 → 编辑态（单栏：正文列 = 栏宽 − 2×8px 留白，不再钳 720 居中）
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
    // 判据：可用宽 == 栏宽 − 2×留白；留白 8；且不再有 720 居中
    const float fullW = screen.width - pad * 2.0f - sideW - gap;
    const float kInsetNow = 8.0f;         // = markdown_module.cpp kTextColumnInsetX
    const float kInsetBefore = 16.0f;     // 参照值
    DOCTEST_CHECK_EQ(edInput->frame.width, fullW - kInsetNow * 2.0f);
    DOCTEST_CHECK_EQ(edInput->frame.x - edCol->frame.x, kInsetNow);
    DOCTEST_CHECK(kInsetNow < kInsetBefore);          // 8 < 16
    DOCTEST_CHECK(edInput->frame.width > 720.0f);     // 写作列钳已去掉
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
    //     编辑器文本列贴左 8px 且宽于 720；回到编辑态列壳不变（两态同宽口径）
    screen.width = 2400.0f;
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // 实时 → 双栏
    frame();
    const auto* edSplit = ui.find(kEditorInputId);
    DOCTEST_REQUIRE(edSplit != nullptr);
    DOCTEST_REQUIRE(ui.find("md.preview.scroll") != nullptr);
    const auto* edColSplit = ui.find("md.editor");
    DOCTEST_REQUIRE(edColSplit != nullptr);
    const float midWide = (screen.width - pad * 2.0f - sideW - gap * 2.0f) * 0.5f;
    DOCTEST_CHECK_EQ(edSplit->frame.x - edColSplit->frame.x, 8.0f);
    DOCTEST_CHECK_EQ(edSplit->frame.width, midWide - 8.0f * 2.0f);
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
    DOCTEST_CHECK_EQ(edWide->frame.width, fullWide - 8.0f * 2.0f);
    DOCTEST_CHECK_EQ(edWide->frame.x - edColWide->frame.x, 8.0f);

    mod->onStop();
    return true;
})

// MD-03：块流注入点（BlockFlowHooks）验收
// drawBlockFlow 是四合一路径（几何表重建 + 逐块渲染 + 解析缓存 + hover + 活动块
// 特判）：多加一个视图态就要多加参数 + 多加一串 isActive 分支。活动块特判收成一个
// 注入点（BlockFlowHooks）——预览 / 双栏态传空实现、实时态传自己的特判。
// 硬约束 = 零行为变化，故下面两条用例分头钉：
//   ① 行为：同一文档同宽下，预览态「无块被特判」与实时态「恰活动块一个被特判」
//      各自的元素 / 几何逐项钉死，且非活动块的包裹几何跨两态逐项相同（默认路径
//      只有一条）；
//   ② 形态：注入点在位 + 两条路径分别传入 +
//   ② 形态（md_module_blockflow_hook_source_guard）：注入点在位 + 两条路径分别传入 +
//      drawBlockFlow 本体不再含活动块特判符号。
REGISTER_TEST(md_module_blockflow_hook_contrast,
              "markdown_module: 块流注入点对照（md_live 活动块特判 / 预览态空实现，MD-03）", {
    const std::string text = "# T\n\npara one\npara two\npara three\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "bflowhook", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "hook.md", text));

    // 期望值口径 = 块模型（与实现同源）：文档光标放第二块块首 → 活动块 = 第二块
    module::markdown::MdBlockModel model;
    model.update(text);
    const auto& bs = model.blocks();
    DOCTEST_REQUIRE(bs.size() >= 2);
    const size_t cursorByte = bs[1].start;
    const size_t kA = module::markdown::mdLiveActiveBlockIndex(model, cursorByte);
    DOCTEST_CHECK_EQ(kA, static_cast<size_t>(1));
    const std::string activeText =
        text.substr(bs[kA].start, bs[kA].end - bs[kA].start);

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) {
        e.frame([&](eui::Ui& u) { mod->compose(u, screen); });
    };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);

    // ① 预览态：注入点是空实现 → 零特判（逐块都走默认路径）
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // 编辑 → 实时
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // 实时 → 双栏
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // 双栏 → 预览
    frame();
    DOCTEST_REQUIRE(e.ui->find("md.preview.scroll") != nullptr);
    DOCTEST_CHECK(e.ui->find(kEditorInputId) == nullptr);   // 确在预览（双栏还有编辑器）
    std::vector<float> previewWrapH;
    for (size_t k = 0; k < bs.size(); ++k) {
        const std::string base =
            "md.preview.blk." + std::to_string(bs[k].stableId);
        const auto* wrap = e.ui->find(base);
        DOCTEST_REQUIRE(wrap != nullptr);
        const auto* hit = e.ui->find(base + ".hit");
        DOCTEST_REQUIRE(hit != nullptr);
        DOCTEST_CHECK(hit->hitTestMode != core::dsl::HitTestMode::None);
        // 无块被取代：逐块渲染内容都在（空 drawContent ⇒ 恒走渲染通路）
        DOCTEST_CHECK(e.ui->find("md.preview.block." +
                                 std::to_string(bs[k].stableId)) != nullptr);
        previewWrapH.push_back(wrap->frame.height);
    }
    DOCTEST_CHECK(e.ui->find("md.preview.input") == nullptr);   // 也无活动 input

    // ② 实时态：注入点是实时态自己的特判 → 恰活动块一个被特判
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // 预览 → 编辑
    frame();
    DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);
    e.ui->state<MdInputState>(kEditorInputId).cursor =
        static_cast<int>(cursorByte);
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // 编辑 → 实时
    frame();
    DOCTEST_REQUIRE(e.ui->find("md.live.scroll") != nullptr);
    const auto* input = e.ui->find("md.live.input");
    DOCTEST_REQUIRE(input != nullptr);
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text, activeText);
    size_t decorated = 0;
    for (size_t k = 0; k < bs.size(); ++k) {
        const std::string base =
            "md.live.blk." + std::to_string(bs[k].stableId);
        const auto* wrap = e.ui->find(base);
        DOCTEST_REQUIRE(wrap != nullptr);
        const auto* hit = e.ui->find(base + ".hit");
        DOCTEST_REQUIRE(hit != nullptr);
        const std::string rendered =
            "md.live.block." + std::to_string(bs[k].stableId);
        if (k == kA) {
            ++decorated;
            DOCTEST_CHECK(hit->hitTestMode == core::dsl::HitTestMode::None);
            DOCTEST_CHECK(e.ui->find(rendered) == nullptr);   // input 取代渲染内容
            DOCTEST_CHECK(std::fabs(wrap->frame.height - input->frame.height) <=
                          0.01f);   // 包裹高 = 贴合高
        } else {
            DOCTEST_CHECK(hit->hitTestMode != core::dsl::HitTestMode::None);
            DOCTEST_CHECK(e.ui->find(rendered) != nullptr);   // 仍是只读渲染
            // 默认路径只有一条：同文档同宽下非活动块包裹几何与预览态逐项相同
            DOCTEST_CHECK(std::fabs(wrap->frame.height - previewWrapH[k]) <= 0.01f);
        }
    }
    DOCTEST_CHECK_EQ(decorated, static_cast<size_t>(1));   // 特判只落活动块

    mod->onStop();
    return true;
})

// 源码守卫（只读断言）
// 与 tests/test_core/test_result.cpp 的源码护栏同款：靠 cwd 上溯定位仓库根，**不**用
// __FILE__（ccache 会把绝对路径相对化）；定位 / 取体失败一律大声失败，不静默绿。
namespace {

// 仓库根：cwd 或它的祖先里同时有 src/module_markdown/markdown_module.cpp 与
// tests/CMakeLists.txt
fs::path mdRepoRoot() {
    std::error_code ec;
    fs::path dir = fs::current_path(ec);
    if (ec) return {};
    for (int up = 0; up < 8; ++up) {
        if (fs::exists(dir / "src" / "module_markdown" / "markdown_module.cpp",
                       ec) &&
            fs::exists(dir / "tests" / "CMakeLists.txt", ec)) {
            return dir;
        }
        const fs::path parent = dir.parent_path();
        if (parent == dir) break;   // 已到根
        dir = parent;
    }
    return {};
}

std::string mdReadWholeFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

// 从 src[start] 的开括号起做配对（跳过字符串 / 字符字面量与注释），返回配对段文本
// （含两端括号）；不配对 → 空串（调用方必须判空，不许静默通过）
std::string mdBalanced(const std::string& src, size_t start, char openCh,
                       char closeCh) {
    if (start >= src.size() || src[start] != openCh) return {};
    int depth = 0;
    for (size_t i = start; i < src.size(); ++i) {
        const char c = src[i];
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') ++i;   // 行注释
            continue;
        }
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) ++i;
            ++i;   // 跨过 '*/'
            continue;
        }
        if (c == '"' || c == '\'') {
            const char quote = c;
            ++i;
            while (i < src.size() && src[i] != quote) {
                if (src[i] == '\\') ++i;
                ++i;
            }
            continue;
        }
        if (c == openCh) {
            ++depth;
        } else if (c == closeCh) {
            --depth;
            if (depth == 0) return src.substr(start, i - start + 1);
        }
    }
    return {};
}

// 成员函数体：`signature` 之后的首个 '{' 起配对
std::string mdFunctionBody(const std::string& src, const std::string& signature) {
    const size_t sig = src.find(signature);
    if (sig == std::string::npos) return {};
    const size_t open = src.find('{', sig);
    if (open == std::string::npos) return {};
    return mdBalanced(src, open, '{', '}');
}

} // namespace

REGISTER_TEST(md_module_blockflow_hook_source_guard,
              "markdown_module: 块流注入点形态守卫 —— drawBlockFlow 无活动块特判分支（MD-03）", {
    const fs::path root = mdRepoRoot();
    DOCTEST_CHECK_MESSAGE(!root.empty(),
                          "守卫定位不到仓库根（cwd 上溯，不用 __FILE__：ccache 会"
                          "把绝对路径相对化）");
    const std::string src = mdReadWholeFile(
        root / "src" / "module_markdown" / "markdown_module.cpp");
    DOCTEST_REQUIRE(!src.empty());

    // (1) 注入点在位：结构体 + drawBlockFlow 末参收它
    DOCTEST_CHECK(src.find("struct BlockFlowHooks") != std::string::npos);
    DOCTEST_CHECK(src.find("const BlockFlowHooks& hooks)") != std::string::npos);

    // (2) drawBlockFlow 本体：视图态特判的符号一个都不许在（分支长回来即变红）
    const std::string body = mdFunctionBody(src, "void drawBlockFlow(");
    DOCTEST_CHECK_MESSAGE(!body.empty(),
                          "取不到 drawBlockFlow 函数体（签名或括号被改？）");
    for (const char* bad :
         {"isActive", "activeBlock", "LiveBlockCtx", "live->", "liveBlockText",
          "liveFitHeight", "drawLiveInputBlock", "codeDrawn"}) {
        DOCTEST_CHECK_MESSAGE(body.find(bad) == std::string::npos,
                              std::string("drawBlockFlow 内又出现视图态特判：") + bad);
    }
    // 三处决策都在本体里走 hooks（缺一 = 注入点没接线）
    for (const char* wired : {"hooks.wrapHeight", "hooks.drawContent",
                              "hooks.afterHitOverlay"}) {
        DOCTEST_CHECK_MESSAGE(body.find(wired) != std::string::npos,
                              std::string("注入点未接线：") + wired);
    }
    // 默认路径仍在：包裹高缺省 = 几何表 H_k、渲染通路仍是上游 markdown
    DOCTEST_CHECK(body.find("syncMap_.blockHeight(k)") != std::string::npos);
    DOCTEST_CHECK(body.find("components::markdown(") != std::string::npos);

    // (3) 两条调用路径分别传入：预览态传空实现、实时态传自己的特判
    constexpr size_t kNameLen = sizeof("drawBlockFlow") - 1;
    std::vector<std::string> calls;
    for (size_t at = src.find("drawBlockFlow("); at != std::string::npos;
         at = src.find("drawBlockFlow(", at + 1)) {
        if (at >= 5 && src.compare(at - 5, 5, "void ") == 0) continue;  // 声明非调用
        const std::string call = mdBalanced(src, at + kNameLen, '(', ')');
        DOCTEST_REQUIRE(!call.empty());   // 括号不配对 = 守卫自身失效，不许静默
        calls.push_back(call);
    }
    DOCTEST_CHECK_EQ(calls.size(), static_cast<size_t>(2));   // 只有两条视图态路径
    bool sawPreview = false;
    bool sawLive = false;
    for (const std::string& call : calls) {
        if (call.find("\"md.preview\"") != std::string::npos) {
            sawPreview = true;
            DOCTEST_CHECK_MESSAGE(call.find("hooks") == std::string::npos,
                                  "预览态必须传空实现（不得带视图态特判）");
            DOCTEST_CHECK_MESSAGE(call.find("{}") != std::string::npos,
                                  "预览态的注入点是空实现 {}");
        } else if (call.find("\"md.live\"") != std::string::npos) {
            sawLive = true;
            DOCTEST_CHECK_MESSAGE(call.find("hooks") != std::string::npos,
                                  "实时态必须传自己的特判（hooks）");
        } else {
            DOCTEST_CHECK_MESSAGE(
                false,
                std::string("drawBlockFlow 出现未登记的第三条调用路径（请一并钉死）：") +
                    call);
        }
    }
    DOCTEST_CHECK(sawPreview);
    DOCTEST_CHECK(sawLive);
    return true;
})

// 焦点 / 切块状态机 + IME 冻结
// 硬约束：
//   · 聚焦只发 .hit，不发基 id；
//   · 从编辑态聚焦到实时态活动 input 需要真窗口，这里直驱框架 mousearea 的
//     press→release 与 clearFocus，靠同帧补发回来；
//   · 其余用例的 isFocused 是防回归的不变量。
//
// 下面一组用例覆盖：连续点击切块不丢字、组字中切块被拒、失焦提交、切态换代。
// 「点完直接打字不用二次点击」的焦点部分在 release 用例里是**真断言**（释放会
// 清焦点，靠同帧补发回来）；其余用例的 isFocused 是防回归的不变量。

namespace {

// 合成一次块点击：mousearea 的 onTap 只在 release 且 inside 时触发
//（components/mousearea.h:148-158）。localY = 点击在块 hit 区域内的 y。
bool clickLiveBlock(eui::Ui& ui, int stableId, float localY) {
    const std::string hitId = "md.live.blk." + std::to_string(stableId) + ".hit";
    const auto* hit = ui.find(hitId);
    if (hit == nullptr || !hit->onPress || !hit->onRelease) return false;
    const core::Rect bounds{0.0f, 0.0f, hit->frame.width, hit->frame.height};
    eui::PointerEvent ev;
    ev.action = eui::PointerAction::Press;
    ev.button = eui::PointerButton::Left;
    ev.x = bounds.width * 0.5f;
    ev.y = localY;
    hit->onPress(ev, bounds);
    ev.action = eui::PointerAction::Release;
    hit->onRelease(ev, bounds);
    return true;
}

// 打一个字符（走真实 input 事件路径：onTextInput → onChange → 拼文档）
bool typeIntoLiveInput(eui::Ui& ui, const std::string& text) {
    const auto* hit = ui.find("md.live.input.hit");
    if (hit == nullptr || !hit->onTextInput) return false;
    core::TextInputEvent ev;
    ev.text = text;
    hit->onTextInput(ev);
    return true;
}

// 焦点离开活动 input（框架 setFocusedId 对旧元素做的事：onFocusChanged(false)）
bool blurLiveInput(eui::Ui& ui) {
    const auto* hit = ui.find("md.live.input.hit");
    if (hit == nullptr || !hit->onFocusChanged) return false;
    hit->onFocusChanged(false);
    return true;
}

// 本帧块包裹的 stableId，按垂直位置排序（块序 = 自上而下的布局顺序）。
// 不用用例侧现算模型去猜 stableId——增量重切后模块内的 stableId 与「对当前文档
// 重建一个全新模型」得到的编号可能不同（stableId 由模型跨次更新继承）。
std::vector<int> liveBlockSidsByY(eui::Ui& ui) {
    std::vector<std::pair<float, int>> found;
    for (int sid = 0; sid < 64; ++sid) {
        const auto* w = ui.find("md.live.blk." + std::to_string(sid));
        if (w != nullptr) found.push_back({w->frame.y, sid});
    }
    std::sort(found.begin(), found.end());
    std::vector<int> ids;
    ids.reserve(found.size());
    for (const auto& entry : found) ids.push_back(entry.second);
    return ids;
}

} // namespace

// 连续点击切块 20 次不丢字。每轮：点非活动块 → 该块变活动块（输入框
// 文本 = 该块源码、光标落在点击行）+ 焦点在 input 上 → 直接打一个字符 → 字符必须
// 落在点击行行首（逐字节，与分块器现算的期望值比）。
REGISTER_TEST(md_module_live_click_switch_20x,
              "markdown_module: 实时态连续点击切块 20 次不丢字（headless）", {
    const std::string text =
        "# T\n\npara one\n\npara two\n\npara three\n\npara four\n\npara five\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "liveclick", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "click.md", text));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);
    e.ui->state<MdInputState>(kEditorInputId).cursor = 0;   // 初始锚 = 文档头
    DOCTEST_REQUIRE(cycleMode(mod.get()));                  // → 实时
    frame();
    DOCTEST_REQUIRE(e.ui->find("md.live.input") != nullptr);

    std::string doc = text;
    size_t activeIdx = static_cast<size_t>(-1);   // 上轮活动块下标（下轮换一个块点）
    for (int i = 0; i < 20; ++i) {
        module::markdown::MdBlockModel m;
        m.update(doc);
        const size_t n = m.blocks().size();
        DOCTEST_REQUIRE_MESSAGE(n >= 2, "第 " + std::to_string(i) + " 轮：文档只剩 " +
                                            std::to_string(n) + " 块 [" + doc + "]");
        const std::vector<int> sids = liveBlockSidsByY(*e.ui);
        DOCTEST_REQUIRE_MESSAGE(sids.size() == n,
                                "渲染块数 " + std::to_string(sids.size()) +
                                    " != 模型块数 " + std::to_string(n));
        // 点【块的第 2 行】：块首行是块间空行——分块器把空行并入下一块头部，
        // 点在空行上打字会把空行吃掉 → 并块。点第 2 行既不动分块结构，又顺带
        // 验了「点击按行落点」：2 行块的第 2 行落在 blockTopY + [0.5,1)H 区间
        //（frac=0.75 → line = first + 1.5 → floor = 第 2 行）；1 行块（标题）
        // 恒落该行。
        const size_t k = (activeIdx + 1) % n;
        const module::markdown::MdBlock& b = m.blocks()[k];
        // 点【块的第 2 行】（块首行是块间空行 —— 分块器把空行并入下一块头部；
        // 点在空行上打字会把空行吃掉 → 并块。点第 2 行既不动分块结构，又顺带
        // 验了「点击按行落点」）：2 行块的第 2 行落在 blockTopY + [0.5,1)H 区间
        //（frac=0.75 → line = first + 1.5 → floor = 第 2 行）；1 行块（标题）
        // 恒落该行。
        size_t lineStart = b.start;
        const size_t nl = doc.find('\n', b.start);
        if (nl != std::string::npos && nl + 1 < b.end) lineStart = nl + 1;
        const std::string blockText = doc.substr(b.start, b.end - b.start);
        const auto* wrapEl =
            e.ui->find("md.live.blk." + std::to_string(sids[k]));
        DOCTEST_REQUIRE(wrapEl != nullptr);
        DOCTEST_REQUIRE_MESSAGE(
            clickLiveBlock(*e.ui, sids[k], 0.75f * wrapEl->frame.height),
            "第 " + std::to_string(i) + " 轮：块 hit 不可点（press/release 未接线？）");
        frame();
        // ① 点中的块变活动块：输入框文本 = 该块源码（逐字节）
        const std::string got = e.ui->state<MdInputState>("md.live.input").text;
        DOCTEST_CHECK_MESSAGE(got == blockText,
                              "第 " + std::to_string(i) + " 轮：活动块文本 [" + got +
                                  "] 期望 [" + blockText + "]");
        // ② 光标落在点击行行首（块内偏移）
        DOCTEST_CHECK_EQ(
            static_cast<size_t>(e.ui->state<MdInputState>("md.live.input").cursor),
            lineStart - b.start);
        // ③ 焦点在 input 上（真实窗口下指针按下会摘焦点，靠 setLiveCursor 的
        //    同帧补发回来；本断言是防回归的不变量）
        DOCTEST_CHECK(e.ui->isFocused("md.live.input.hit"));
        // ④ 直接打字：字符必须落在点击行行首（不丢字、不串块）
        DOCTEST_REQUIRE(typeIntoLiveInput(*e.ui, "X"));
        frame();
        doc = doc.substr(0, lineStart) + "X" + doc.substr(lineStart);
        module::markdown::MdBlockModel m2;
        m2.update(doc);
        const size_t c2 = lineStart + 1;
        const size_t k2 = m2.blockIndexAt(c2);
        const module::markdown::MdBlock& b2 = m2.blocks()[k2];
        const std::string want2 = doc.substr(b2.start, b2.end - b2.start);
        const std::string got2 = e.ui->state<MdInputState>("md.live.input").text;
        DOCTEST_CHECK_MESSAGE(got2 == want2,
                              "第 " + std::to_string(i) + " 轮打字后：活动块 [" + got2 +
                                  "] 期望 [" + want2 + "]");
        DOCTEST_CHECK_EQ(
            b2.start + static_cast<size_t>(
                           e.ui->state<MdInputState>("md.live.input").cursor),
            c2);
        activeIdx = k2;
    }

    // ⑤ 端到端：20 次点击 + 20 个字符全部落盘（逐字节）
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    frame();   // 本帧 compose 内 flushEditorIfIdle → 整篇写盘
    std::string saved;
    DOCTEST_REQUIRE(readFileBytes(env.dir / "click.md", &saved));
    DOCTEST_CHECK_MESSAGE(saved == doc, "落盘 [" + saved + "] 期望 [" + doc + "]");
    DOCTEST_CHECK_EQ(std::count(doc.begin(), doc.end(), 'X'), 20);
    mod->onStop();
    return true;
})

// 组字中切块被拒 + 组字中冻结块推导。
// 冻结的可观测口径：把块内光标推到块尾 —— 不冻结时「锚 = 输入框文本起点 + 块内
// 光标」会算出块边界字节（块边界归后一块）→ 下一帧就换块换文本。
REGISTER_TEST(md_module_live_ime_freeze,
              "markdown_module: 实时态组字冻结推导 + 组字中切块被拒（headless）", {
    const std::string text = "# T\n\npara one\n\npara two\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "liveime", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "ime.md", text));

    module::markdown::MdBlockModel model;
    model.update(text);
    DOCTEST_REQUIRE(model.blocks().size() >= 3);
    const std::string block0 =
        text.substr(model.blocks()[0].start,
                    model.blocks()[0].end - model.blocks()[0].start);
    const std::string block1 =
        text.substr(model.blocks()[1].start,
                    model.blocks()[1].end - model.blocks()[1].start);
    const std::string block2 =
        text.substr(model.blocks()[2].start,
                    model.blocks()[2].end - model.blocks()[2].start);

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);
    e.ui->state<MdInputState>(kEditorInputId).cursor =
        static_cast<int>(model.blocks()[1].start);   // 活动块 = 块 1
    DOCTEST_REQUIRE(cycleMode(mod.get()));           // → 实时
    frame();
    DOCTEST_REQUIRE(e.ui->find("md.live.input") != nullptr);
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text, block1);
    const std::vector<int> sids = liveBlockSidsByY(*e.ui);
    DOCTEST_REQUIRE(sids.size() == model.blocks().size());   // 渲染块 == 模型块
    const int sid0 = sids[0];                                 // 块 0（标题）

    // 组字在飞：块内光标推到块尾（= 块边界字节，不冻结就会换块）
    {
        MdInputState& st = e.ui->state<MdInputState>("md.live.input");
        st.cursor = static_cast<int>(block1.size());
        st.compositionText = "ni";
    }
    frame();
    // ① 冻结：活动块 / 输入框文本不变（未冻结时这里会变成块 2 的文本）
    const std::string frozen = e.ui->state<MdInputState>("md.live.input").text;
    DOCTEST_CHECK_MESSAGE(frozen == block1,
                          "组字中块推导未冻结：活动块文本 [" + frozen + "] 期望 [" +
                              block1 + "]");

    // ② 组字中点击另一块 → 拒绝（不换块不换文本）
    DOCTEST_REQUIRE(clickLiveBlock(*e.ui, sid0, 0.0f));
    frame();
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text, block1);
    DOCTEST_CHECK(e.ui->isFocused("md.live.input.hit"));  // 组字会话留在原块

    // ③ 组字结束 → 下一帧一次性重算：锚 = 块 1 起点 + 块内光标(=块长) = 块 2 起点
    e.ui->state<MdInputState>("md.live.input").compositionText.clear();
    frame();
    DOCTEST_CHECK_MESSAGE(e.ui->state<MdInputState>("md.live.input").text == block2,
                          "组字结束后未重算：[" +
                              e.ui->state<MdInputState>("md.live.input").text +
                              "] 期望 [" + block2 + "]");

    // ④ 放行对照：组字结束后点击恢复生效（证明拒绝不是"点了也没用"）
    DOCTEST_REQUIRE(clickLiveBlock(*e.ui, sid0, 0.0f));
    frame();
    DOCTEST_CHECK_MESSAGE(e.ui->state<MdInputState>("md.live.input").text == block0,
                          "组字结束后点击未生效：[" +
                              e.ui->state<MdInputState>("md.live.input").text +
                              "] 期望 [" + block0 + "]");
    mod->onStop();
    return true;
})

// 失焦提交——① 清在飞组字（焦点已走 = IME 会话结束；
// 不清则冻结谓词恒真 → 切块被永久拒绝、幽灵预编辑留在输入框里）；② 上行未提交
// 文本。同时钉住安全判据：文档已换代而输入框还没对齐（修订号不符）时【不许】
// 提交，否则旧块文本会被灌回新文档。
REGISTER_TEST(md_module_live_blur_commit,
              "markdown_module: 实时态失焦提交（清组字 + 上行未提交文本 + 修订号守门）", {
    const std::string text = "# T\n\npara one\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "liveblur", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "blur.md", text));

    module::markdown::MdBlockModel model;
    model.update(text);
    DOCTEST_REQUIRE(model.blocks().size() >= 2);
    const module::markdown::MdBlock& last = model.blocks().back();
    const std::string prefix = text.substr(0, last.start);
    const std::string blockText = text.substr(last.start, last.end - last.start);

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);
    e.ui->state<MdInputState>(kEditorInputId).cursor =
        static_cast<int>(last.start);                  // 活动块 = 末块
    DOCTEST_REQUIRE(cycleMode(mod.get()));             // → 实时
    frame();
    DOCTEST_REQUIRE(e.ui->find("md.live.input") != nullptr);
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text, blockText);

    // ① 幽灵组字：失焦必须清掉（否则冻结谓词恒真 → 切块永久被拒）
    {
        MdInputState& st = e.ui->state<MdInputState>("md.live.input");
        st.compositionText = "ni";
        const unsigned long long rev = st.compositionRevision;
        DOCTEST_REQUIRE(blurLiveInput(*e.ui));
        DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").compositionText,
                         std::string());
        DOCTEST_CHECK(e.ui->state<MdInputState>("md.live.input").compositionRevision >
                      rev);
    }

    // ② 未提交文本：失焦上行（下一帧起文档/输入框都含它，且落盘）
    {
        MdInputState& st = e.ui->state<MdInputState>("md.live.input");
        st.text += "Z";   // 模拟「输入框已有内容但没走 onChange」的那一帧
        st.cursor = static_cast<int>(st.text.size());
        DOCTEST_REQUIRE(blurLiveInput(*e.ui));
    }
    frame();
    DOCTEST_CHECK_MESSAGE(
        e.ui->state<MdInputState>("md.live.input").text == blockText + "Z",
        "失焦未上行未提交文本：[" + e.ui->state<MdInputState>("md.live.input").text +
            "]");
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    frame();   // 落盘
    std::string saved;
    DOCTEST_REQUIRE(readFileBytes(env.dir / "blur.md", &saved));
    DOCTEST_CHECK_MESSAGE(saved == prefix + blockText + "Z", "落盘 [" + saved + "]");

    // ③ 修订号守门：文档已换代（用真实输入让 rev 前进）、输入框还没对齐
    //      → 失焦【不许】把旧文本灌回文档 ----
    DOCTEST_REQUIRE(typeIntoLiveInput(*e.ui, "W"));   // 文档 + rev 前进、输入框已同步
    e.ui->state<MdInputState>("md.live.input").text = "STALE";  // 造出"未对齐"的那一帧
    DOCTEST_REQUIRE(blurLiveInput(*e.ui));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    frame();
    std::string saved2;
    DOCTEST_REQUIRE(readFileBytes(env.dir / "blur.md", &saved2));
    DOCTEST_CHECK_MESSAGE(saved2.find("STALE") == std::string::npos,
                          "修订号不符时仍把旧文本灌回文档：[" + saved2 + "]");
    DOCTEST_CHECK_MESSAGE(saved2 == prefix + blockText + "ZW",
                          "落盘 [" + saved2 + "] 期望 [" + prefix + blockText + "ZW]");
    mod->onStop();
    return true;
})

// 切态 / 换笔记 = 输入框状态换代（releaseStateScope）——释放
// md.live.input 前缀（组字 / 光标 / framework undo 栈不许跟着稳定 id 复活），
// 并靠 liveFocusPending_ 同帧补发聚焦（释放会清焦点；"先释放、后补发"的顺序由
// Runtime::compose 保证：consumeReleasedStateScopes → consumePendingFocusId）。
REGISTER_TEST(md_module_live_state_release_note_switch,
              "markdown_module: 实时态换笔记释放输入框状态 + 同帧补回焦点（headless）", {
    // 两篇笔记的块文本完全相同（# T / para one）——"文本没变 → 不换文本"时，
    // 唯一能证明状态被换代的证据就是组字/undo 栈被清 + 焦点被补回
    const std::string docA = "# T\n\npara one\n";
    const std::string docB = "# T\n\npara one\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "liverelease", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "a.md", docA));
    DOCTEST_REQUIRE(writeTextFile(env.dir / "b.md", docB));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    frame();
    const auto* rowA = findFileRowByName(*e.ui, "a.md");
    DOCTEST_REQUIRE(rowA != nullptr);
    DOCTEST_REQUIRE(rowA->onClick != nullptr);
    rowA->onClick();
    frame();
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();
    DOCTEST_REQUIRE(e.ui->find("md.live.input") != nullptr);

    // 造出「上一篇残留」：组字在飞 + 非零光标 + framework undo 栈有条目
    {
        MdInputState& st = e.ui->state<MdInputState>("md.live.input");
        st.compositionText = "ni";
        st.cursor = 3;
        st.undoStack.push_back(
            components::input_detail::InputModel::EditSnapshot{});
    }

    const auto* rowB = findFileRowByName(*e.ui, "b.md");
    DOCTEST_REQUIRE(rowB != nullptr);
    DOCTEST_REQUIRE(rowB->onClick != nullptr);
    rowB->onClick();
    frame();
    DOCTEST_REQUIRE(e.ui->find("md.live.input") != nullptr);
    // ① 状态换代：组字 / 光标 / undo 栈都不许跟着 id 复活
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").compositionText,
                     std::string());
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").undoStack.size(),
                     static_cast<size_t>(0));
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").cursor, 0);
    // ② 焦点被同帧补回（释放会清焦点；不补发则换笔记后打字落空）
    DOCTEST_CHECK(e.ui->isFocused("md.live.input.hit"));
    // ③ 输入框文本仍是新笔记的块源码（换代后重新按锚取块）
    const std::string got = e.ui->state<MdInputState>("md.live.input").text;
    DOCTEST_CHECK_MESSAGE(docB.find(got) != std::string::npos,
                          "换代后输入框文本 [" + got + "] 不是新文档的块");
    mod->onStop();
    return true;
})

// 块边界键
// Enter/Backspace/Delete/↑↓ 边界语义 + 边界偏移单测
//（纯函数侧在 test_md_live_edit.cpp）+ 手工。
//
// 无头边界（诚实声明）：本组直驱活动 input 的 onKeyEvent（= Runtime 内部同一
// 入口：用户 passthrough 先于内置，返回 true 即拦截）。因此「不消费」的判定
// 在无头下拿不到（内置对 Up/Down/Backspace 也会返回 true），故负向断言一律
// 走【行为】：文档不变 + 活动块不跳；正向断言走【状态】：活动块/光标/文档。

namespace {

// 控制字符转义（CRLF 用例的失败信息里 '\r'/'\n' 直接打印会和终端行为纠缠，
// 无法分辨差异）
std::string escapeCr(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\r') out += "\\r";
        else if (c == '\n') out += "\\n";
        else out.push_back(c);
    }
    return out;
}

// 给活动 input 派发一次按键（走 input 组件注册的复合处理器：passthrough → 内置）；
// 第 4 参是**快捷键修饰位**而非"Ctrl 位"（macOS 上是 Cmd，见 setShortcutModifier）
bool pressLiveKey(eui::Ui& ui, eui::InputKey key, bool shift = false,
                  bool shortcut = false) {
    const auto* hit = ui.find("md.live.input.hit");
    if (hit == nullptr || !hit->onKeyEvent) return false;
    eui::KeyEvent ev;
    ev.key = key;
    ev.action = eui::KeyAction::Press;
    ev.modifiers.shift = shift;
    test_support::setShortcutModifier(ev.modifiers, shortcut);
    return hit->onKeyEvent(ev);
}

// 进实时态并把活动块锚定到 docByte（编辑器光标 → 切态 → 锚 = 编辑器光标）
bool enterLiveAt(test_support::HeadlessFrame& e, ::core::Module* mod,
                 const eui::Screen& screen, size_t docByte) {
    e.frame([&](eui::Ui& u) { mod->compose(u, screen); });
    if (!openFirstNote(*e.ui)) return false;
    e.frame([&](eui::Ui& u) { mod->compose(u, screen); });
    if (e.ui->find(kEditorInputId) == nullptr) return false;
    e.ui->state<MdInputState>(kEditorInputId).cursor = static_cast<int>(docByte);
    if (!cycleMode(mod)) return false;   // → 实时
    e.frame([&](eui::Ui& u) { mod->compose(u, screen); });
    return e.ui->find("md.live.input") != nullptr;
}

} // namespace

// ① Backspace 于块首 = 删前一个换行（与上一块粘连、活动块自动跟随），
// 首块块首不消费（文档不变）；CRLF 成对删。
REGISTER_TEST(md_module_live_boundary_backspace,
              "markdown_module: 实时态块首退格删接缝换行（headless）", {
    //   [0,8)   "one\ntwo\n"   段落
    //   [8,13)  "\n# H\n"      标题（前导空行折叠）
    //   [13,20) "\nthree\n"    段落
    const std::string text = "one\ntwo\n\n# H\n\nthree\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "livebksp", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "bs.md", text));

    module::markdown::MdBlockModel model;
    model.update(text);
    const auto& bs = model.blocks();
    DOCTEST_REQUIRE_EQ(bs.size(), static_cast<size_t>(3));
    DOCTEST_REQUIRE_EQ(bs[1].start, static_cast<size_t>(8));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    DOCTEST_REQUIRE(enterLiveAt(e, mod.get(), screen, bs[1].start));
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     text.substr(bs[1].start, bs[1].end - bs[1].start));
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").cursor, 0);

    // ① 块首退格 → 消费（删 [7,8) 的 '\n'），文档粘连、光标落在接缝
    DOCTEST_REQUIRE(pressLiveKey(*e.ui, eui::InputKey::Backspace));
    frame();
    // 期望文档 = 原文档去掉块 0 末尾的 '\n'
    const std::string expect = "one\ntwo\n# H\n\nthree\n";
    // 重切后块 0 = [0,8)（"one\ntwo\n"），光标 7 落在块 0 内 → 活动块跟随 + 块内光标 7
    module::markdown::MdBlockModel m2;
    m2.update(expect);
    DOCTEST_REQUIRE(m2.blocks().size() >= 2);
    DOCTEST_REQUIRE_EQ(m2.blockIndexAt(7), static_cast<size_t>(0));
    DOCTEST_CHECK_MESSAGE(
        e.ui->state<MdInputState>("md.live.input").text ==
            expect.substr(m2.blocks()[0].start,
                          m2.blocks()[0].end - m2.blocks()[0].start),
        "块首退格后活动块文本 [" +
            e.ui->state<MdInputState>("md.live.input").text + "] 文档 [" + expect + "]");
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").cursor, 7);
    // ② 落盘逐字节（证明删的是接缝换行，不是活动块的字节）
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    frame();
    std::string saved;
    DOCTEST_REQUIRE(readFileBytes(env.dir / "bs.md", &saved));
    DOCTEST_CHECK_MESSAGE(saved == expect, "落盘 [" + saved + "] 期望 [" + expect + "]");

    // ③ 首块块首退格 → 不消费（前面没有换行）：文档与活动块都不动
    //    （无头下拿不到 passthrough 的返回值 —— 内置对 Backspace 恒返回 true，
    //      故负向断言一律走行为：文档逐字节不变）
    e.ui->state<MdInputState>("md.live.input").cursor = 0;
    frame();
    DOCTEST_REQUIRE_EQ(e.ui->state<MdInputState>("md.live.input").text,
                       expect.substr(m2.blocks()[0].start,
                                     m2.blocks()[0].end - m2.blocks()[0].start));
    pressLiveKey(*e.ui, eui::InputKey::Backspace);
    frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    frame();
    DOCTEST_REQUIRE(readFileBytes(env.dir / "bs.md", &saved));
    DOCTEST_CHECK_MESSAGE(saved == expect, "首块块首退格后落盘 [" + saved + "]");
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     expect.substr(m2.blocks()[0].start,
                                   m2.blocks()[0].end - m2.blocks()[0].start));
    mod->onStop();
    return true;
})

// ② Delete 于块边界（块终点字节）删该处的换行；文末不消费、块内普通
// 字符删除交回内置。
// CRLF 说明（本次实证）：Windows 文本模式读（markdown_service.cpp:641 的
// std::ifstream 未开 ios::binary）把 CRLF 归一成 LF，落盘走 core::atomic_write
// 的 binary 写（atomic_write.cpp:14）→ 模块内文档恒为 LF，CRLF 文件首次保存后
// 磁盘字节也变 LF。故模块路径按 LF 验证；CRLF 的字节语义（'\r\n' 成对删、列钳到
// '\r' 前）在 test_md_live_edit.cpp 的纯函数用例里覆盖。
REGISTER_TEST(md_module_live_boundary_delete,
              "markdown_module: 实时态块尾删除删接缝换行（headless）", {
    //   [0,4)  "# A\n"     标题
    //   [4,10) "\n# B\n"   标题（前导空行折叠 —— 块边界字节 = 行首 '\n'）
    const std::string text = "# A\n\n# B\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "livedel", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "del.md", text));

    module::markdown::MdBlockModel model;
    model.update(text);
    const auto& bs = model.blocks();
    DOCTEST_REQUIRE_EQ(bs.size(), static_cast<size_t>(2));
    DOCTEST_REQUIRE_EQ(bs[1].start, static_cast<size_t>(4));   // == 块 0 块尾
    DOCTEST_REQUIRE(bs[1].end == text.size());

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    DOCTEST_REQUIRE(enterLiveAt(e, mod.get(), screen, bs[1].start));
    const std::string blockSrc =
        text.substr(bs[1].start, bs[1].end - bs[1].start);
    DOCTEST_CHECK_MESSAGE(
        e.ui->state<MdInputState>("md.live.input").text == blockSrc,
        "初始活动块文本 [" + escapeCr(e.ui->state<MdInputState>("md.live.input").text) +
            "] 期望 [" + escapeCr(blockSrc) + "] 文档 [" + escapeCr(text) + "]");
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").cursor, 0);

    // ① 块尾删除 → 消费：删块边界处的 '\n'（块 0 与块 1 粘连）
    DOCTEST_REQUIRE(pressLiveKey(*e.ui, eui::InputKey::Delete));
    frame();
    const std::string expect = "# A\n# B\n";
    module::markdown::MdBlockModel m2;
    m2.update(expect);
    DOCTEST_REQUIRE_EQ(m2.blocks().size(), static_cast<size_t>(2));
    DOCTEST_REQUIRE_EQ(m2.blockIndexAt(4), static_cast<size_t>(1));
    DOCTEST_CHECK_MESSAGE(
        e.ui->state<MdInputState>("md.live.input").text ==
            "# B\n",
        "块尾删除后活动块文本 [" +
            escapeCr(e.ui->state<MdInputState>("md.live.input").text) + "]");
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").cursor, 0);
    // 证伪探针：删除走【模块层边界路径】（直接改文档 + 重锚），不产生 framework
    // 快照。卸载边界路径后本断言必红（内置 Delete 会先 pushUndoState）。
    // 注意：LF 文本下两条路径的【文档字节结果相同】（都删掉那个 '\n'），故文档断言
    // 骗不过 —— CRLF 成对删的字节语义由纯函数用例 md_live_boundary_edit 锁死。
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").undoStack.size(),
                     static_cast<size_t>(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    frame();
    std::string saved;
    DOCTEST_REQUIRE(readFileBytes(env.dir / "del.md", &saved));
    DOCTEST_CHECK_MESSAGE(saved == expect,
                          "落盘 [" + escapeCr(saved) + "] 期望 [" + escapeCr(expect) +
                              "]");

    // ② 文末（文档尾）Delete → 不消费：文档不变
    //    末块文本 "# B\n" 长 4，光标推到文本末尾 → 锚 = 文档尾（块边界归后一块，
    //    末块无后一块 → 仍归末块，块内光标 = 文本长）
    e.ui->state<MdInputState>("md.live.input").cursor =
        static_cast<int>(e.ui->state<MdInputState>("md.live.input").text.size());
    frame();
    pressLiveKey(*e.ui, eui::InputKey::Delete);
    frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    frame();
    DOCTEST_REQUIRE(readFileBytes(env.dir / "del.md", &saved));
    DOCTEST_CHECK_MESSAGE(saved == expect, "文末删除后落盘 [" + escapeCr(saved) + "]");

    // ③ 块内（非块边界）Delete → 不消费，交回内置删普通字符：文档少一个字节
    //    （块 1 文本 "# B\n"，光标 0 → 该字节是 '#'，不是行终结符）
    DOCTEST_REQUIRE(e.ui->state<MdInputState>("md.live.input").cursor == 4);
    e.ui->state<MdInputState>("md.live.input").cursor = 0;
    frame();
    pressLiveKey(*e.ui, eui::InputKey::Delete);   // 不消费 → 内置删 '#'（并重切块）
    frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    frame();
    DOCTEST_REQUIRE(readFileBytes(env.dir / "del.md", &saved));
    DOCTEST_CHECK_MESSAGE(saved == "# A\n B\n",
                          "内置块内删除后落盘 [" + escapeCr(saved) + "]");
    mod->onStop();
    return true;
})

// 跨块 ↑↓：边界行按 ↑/↓ 跳相邻块（列保持），非边界行交回内置。
REGISTER_TEST(md_module_live_vertical_block_move,
              "markdown_module: 实时态跨块 ↑↓（边界行跳块 + 块内行交回内置）", {
    const std::string text = "one\ntwo\n\n# H\n\nthree\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "livevert", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "vt.md", text));

    module::markdown::MdBlockModel model;
    model.update(text);
    const auto& bs = model.blocks();
    DOCTEST_REQUIRE_EQ(bs.size(), static_cast<size_t>(3));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    DOCTEST_REQUIRE(enterLiveAt(e, mod.get(), screen, bs[1].start));   // 活动块 = 1

    // ① 块 1 首行 ↑ → 块 0 末行行首（目标字节 4）
    DOCTEST_REQUIRE(pressLiveKey(*e.ui, eui::InputKey::Up));
    frame();
    DOCTEST_CHECK_MESSAGE(
        e.ui->state<MdInputState>("md.live.input").text ==
            text.substr(bs[0].start, bs[0].end - bs[0].start),
        "↑ 后活动块文本 [" + e.ui->state<MdInputState>("md.live.input").text + "]");
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").cursor, 4);

    // ② 块 0 第二行 ↓/↑ → 非边界行交回内置：文档与活动块不跳（↑ 落到块内上一行）
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     text.substr(bs[0].start, bs[0].end - bs[0].start));
    DOCTEST_REQUIRE(pressLiveKey(*e.ui, eui::InputKey::Up));   // 块内上移（内置）
    frame();
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     text.substr(bs[0].start, bs[0].end - bs[0].start));
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").cursor, 0);
    // 首块首行再 ↑ → 不消费（交回内置，停在首行）；活动块不许跳
    pressLiveKey(*e.ui, eui::InputKey::Up);
    frame();
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     text.substr(bs[0].start, bs[0].end - bs[0].start));
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").cursor, 0);

    // ③ 块 0 首行 ↓ → 非块末行，交回内置块内下移（"two" 行）；再 ↓ 从块末行跨到块 1
    pressLiveKey(*e.ui, eui::InputKey::Down);   // 内置：块内下移一行
    frame();
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     text.substr(bs[0].start, bs[0].end - bs[0].start));
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").cursor, 4);
    DOCTEST_REQUIRE(pressLiveKey(*e.ui, eui::InputKey::Down));   // 块 0 末行 → 块 1
    frame();
    DOCTEST_CHECK_MESSAGE(
        e.ui->state<MdInputState>("md.live.input").text ==
            text.substr(bs[1].start, bs[1].end - bs[1].start),
        "↓ 跨块后活动块文本 [" + e.ui->state<MdInputState>("md.live.input").text +
            "] 期望 [" + text.substr(bs[1].start, bs[1].end - bs[1].start) + "]");
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").cursor, 0);

    // ④ 继续 ↓：块 1 首行（空行）不是块末行 → 交回内置块内下移；再 ↓ 从块 1 末行
    //    进块 2（末块）——块末行才跨块
    pressLiveKey(*e.ui, eui::InputKey::Down);   // 内置：块内下移一行
    frame();
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     text.substr(bs[1].start, bs[1].end - bs[1].start));
    DOCTEST_REQUIRE(pressLiveKey(*e.ui, eui::InputKey::Down));   // 块 1 末行 → 块 2
    frame();
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     text.substr(bs[2].start, bs[2].end - bs[2].start));
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").cursor, 0);
    // ⑤ 末块 ↓：非末行交回内置（进 "three" 行）；末行 ↓ 不消费（活动块不跳）
    pressLiveKey(*e.ui, eui::InputKey::Down);   // 内置：进 "three" 行
    frame();
    const std::string before = e.ui->state<MdInputState>("md.live.input").text;
    DOCTEST_CHECK_EQ(before,
                     text.substr(bs[2].start, bs[2].end - bs[2].start));
    pressLiveKey(*e.ui, eui::InputKey::Down);   // 末块末行 → 不消费
    frame();
    DOCTEST_CHECK_MESSAGE(e.ui->state<MdInputState>("md.live.input").text == before,
                          "末块 ↓ 后活动块跳走了：[" +
                              e.ui->state<MdInputState>("md.live.input").text + "]");

    // ⑥ 全程零编辑：文档逐字节未变
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    frame();
    std::string saved;
    DOCTEST_REQUIRE(readFileBytes(env.dir / "vt.md", &saved));
    DOCTEST_CHECK_MESSAGE(saved == text, "跨块 ↑↓ 不应改文档：[" + saved + "]");
    mod->onStop();
    return true;
})

// ④ 块内按键走既有 md_keyboard 纯函数（Enter 续列表 / Tab 缩进 / Ctrl+B 包裹）
// ——写回活动 input 并上行拼文档。
REGISTER_TEST(md_module_live_inblock_keys,
              "markdown_module: 实时态块内按键（Enter 续列表/Tab/Ctrl+B）", {
    const std::string text = "- a\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "livekeys", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "kb.md", text));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    DOCTEST_REQUIRE(enterLiveAt(e, mod.get(), screen, 3));   // 光标 "- a" 之后
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text, text);

    // ① 光标行是列表项 → applyEnterList：插 "\n" + 续行前缀 "- "
    DOCTEST_REQUIRE(pressLiveKey(*e.ui, eui::InputKey::Enter));
    frame();
    std::string want = "- a\n- \n";
    DOCTEST_CHECK_MESSAGE(e.ui->state<MdInputState>("md.live.input").text == want,
                          "Enter 续列表后块文本 [" +
                              e.ui->state<MdInputState>("md.live.input").text +
                              "] 期望 [" + want + "]");
    // ② Tab → applyTab：光标处插 4 空格（续行前缀之后再缩进）
    DOCTEST_REQUIRE(pressLiveKey(*e.ui, eui::InputKey::Tab));
    frame();
    want = "- a\n-     \n";
    DOCTEST_CHECK_MESSAGE(e.ui->state<MdInputState>("md.live.input").text == want,
                          "Tab 后块文本 [" +
                              e.ui->state<MdInputState>("md.live.input").text +
                              "] 期望 [" + want + "]");
    // ③ Ctrl+B → applyWrapAround：无选区插成对 "**"，光标居中
    DOCTEST_REQUIRE(pressLiveKey(*e.ui, eui::InputKey::B, false, true));
    frame();
    want = "- a\n-     ****\n";
    DOCTEST_CHECK_MESSAGE(e.ui->state<MdInputState>("md.live.input").text == want,
                          "Ctrl+B 后块文本 [" +
                              e.ui->state<MdInputState>("md.live.input").text +
                              "] 期望 [" + want + "]");
    // ④ 端到端：三键结果全部落盘（逐字节）
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    frame();
    std::string saved;
    DOCTEST_REQUIRE(readFileBytes(env.dir / "kb.md", &saved));
    DOCTEST_CHECK_MESSAGE(saved == want, "落盘 [" + saved + "] 期望 [" + want + "]");
    mod->onStop();
    return true;
})

// ① 块级撤销/重做：跨块编辑序列后 Ctrl+Z 逐级回退（每级 = 一次编辑组），
// Ctrl+Y / Ctrl+Shift+Z 逐级重做；快照栈在模块层，
// 【拦截证据】= framework per-instance 栈的大小在被拦截的 Ctrl+Z 前后不变。
// 跨块序列是 D5 的立身之本：框架栈存的是【块局部文本】，换块后恢复会写错块。
REGISTER_TEST(md_module_live_undo_redo,
              "markdown_module: 实时态块级撤销/重做（跨块序列 + Ctrl+Z/Y/Shift+Z）", {
    const std::string text = "alpha\n\nbeta\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "liveundo", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "uo.md", text));

    module::markdown::MdBlockModel model;
    model.update(text);
    const auto& bs = model.blocks();
    DOCTEST_REQUIRE_EQ(bs.size(), static_cast<size_t>(2));
    DOCTEST_REQUIRE_EQ(bs[1].start, static_cast<size_t>(6));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    DOCTEST_REQUIRE(enterLiveAt(e, mod.get(), screen, bs[1].start));   // 活动块 = 1
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     std::string("\nbeta\n"));

    // ① 块 1 末尾打字 "x"（块末追加：不产生新块也不粘连上一块）→ 文档
    //    "alpha\n\nbeta\nx"
    e.ui->state<MdInputState>("md.live.input").cursor = static_cast<int>(
        e.ui->state<MdInputState>("md.live.input").text.size());
    frame();
    DOCTEST_REQUIRE(typeIntoLiveInput(*e.ui, "x"));
    frame();
    DOCTEST_CHECK_MESSAGE(
        e.ui->state<MdInputState>("md.live.input").text == "\nbeta\nx",
        "块 1 打字后 [" + escapeCr(e.ui->state<MdInputState>("md.live.input").text) + "]");

    // ② 点击块 0（渲染块点击 = 块切换 → 撤销合并组收口）→ 光标落块 0 行首
    const std::vector<int> sids = liveBlockSidsByY(*e.ui);
    DOCTEST_REQUIRE_EQ(sids.size(), static_cast<size_t>(2));
    const auto* wrap0 = e.ui->find("md.live.blk." + std::to_string(sids[0]));
    DOCTEST_REQUIRE(wrap0 != nullptr);
    DOCTEST_REQUIRE(clickLiveBlock(*e.ui, sids[0], wrap0->frame.height * 0.5f));
    frame();
    DOCTEST_CHECK_MESSAGE(
        e.ui->state<MdInputState>("md.live.input").text == "alpha\n",
        "点击块 0 后活动块 [" +
            escapeCr(e.ui->state<MdInputState>("md.live.input").text) + "]");

    // ③ 块 0 打字 "y" → 文档 "yalpha\n\nbeta\nx"
    DOCTEST_REQUIRE(typeIntoLiveInput(*e.ui, "y"));
    frame();
    DOCTEST_CHECK_MESSAGE(
        e.ui->state<MdInputState>("md.live.input").text == "yalpha\n",
        "块 0 打字后 [" + escapeCr(e.ui->state<MdInputState>("md.live.input").text) + "]");

    // ④ Ctrl+Z → 撤销块 0 的编辑；内置栈大小前后不变 = 按键被模块层消费
    const size_t fwUndo =
        e.ui->state<MdInputState>("md.live.input").undoStack.size();
    DOCTEST_REQUIRE(pressLiveKey(*e.ui, eui::InputKey::Z, false, true));
    frame();
    DOCTEST_CHECK_MESSAGE(
        e.ui->state<MdInputState>("md.live.input").text == "alpha\n",
        "撤销块 0 编辑后 [" +
            escapeCr(e.ui->state<MdInputState>("md.live.input").text) + "]");
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").cursor, 0);
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").undoStack.size(),
                     fwUndo);   // framework per-instance 栈未被消费

    // ⑤ Ctrl+Z → 跨块撤销到进态基线（撤销 "x"，活动块随快照回到块 1）。光标取
    //    基线快照的进态光标（= 进态时编辑器的文档字节 6 → 块 1 局部 0）
    DOCTEST_REQUIRE(pressLiveKey(*e.ui, eui::InputKey::Z, false, true));
    frame();
    DOCTEST_CHECK_MESSAGE(
        e.ui->state<MdInputState>("md.live.input").text == "\nbeta\n",
        "跨块撤销后活动块 [" +
            escapeCr(e.ui->state<MdInputState>("md.live.input").text) + "]");
    DOCTEST_CHECK_MESSAGE(
        e.ui->state<MdInputState>("md.live.input").cursor == 0,
        "跨块撤销后块内光标 [" +
            std::to_string(e.ui->state<MdInputState>("md.live.input").cursor) + "]");
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").undoStack.size(),
                     fwUndo);

    // ⑥ Ctrl+Z → 已到基线：不改文档、也不报错
    DOCTEST_REQUIRE(pressLiveKey(*e.ui, eui::InputKey::Z, false, true));
    frame();
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     std::string("\nbeta\n"));

    // ⑦ Ctrl+Y → 重做 "x"（活动块随快照回到块 0）
    DOCTEST_REQUIRE(pressLiveKey(*e.ui, eui::InputKey::Y, false, true));
    frame();
    DOCTEST_CHECK_MESSAGE(
        e.ui->state<MdInputState>("md.live.input").text == "alpha\n",
        "重做 x 后 [" + escapeCr(e.ui->state<MdInputState>("md.live.input").text) + "]");

    // ⑧ Ctrl+Shift+Z → 重做 "y"
    DOCTEST_REQUIRE(pressLiveKey(*e.ui, eui::InputKey::Z, true, true));
    frame();
    DOCTEST_CHECK_MESSAGE(
        e.ui->state<MdInputState>("md.live.input").text == "yalpha\n",
        "重做 y 后 [" + escapeCr(e.ui->state<MdInputState>("md.live.input").text) + "]");

    // ⑨ 撤销/重做的结果就是最终落盘的文档（逐字节）
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    frame();
    std::string saved;
    DOCTEST_REQUIRE(readFileBytes(env.dir / "uo.md", &saved));
    DOCTEST_CHECK_MESSAGE(saved == "yalpha\n\nbeta\nx",
                          "撤销/重做后落盘 [" + escapeCr(saved) + "]");
    mod->onStop();
    return true;
})

// ③ 切态往返 = 撤销栈换代（「模式切换前推栈」+「退出实时态
// 不清栈」的落地口径）——编辑态改过的内容不许被实时态的旧快照撤销掉。快照只覆盖
// 一个实时态编辑会话；进态那一帧按当前文档重取基线（否则 Ctrl+Z 会静默吞掉
// 编辑态/外部编辑器的改动，还会被落盘防抖写进磁盘）。
REGISTER_TEST(md_module_live_undo_mode_roundtrip,
              "markdown_module: 切态往返后撤销栈换代（旧快照不跨编辑会话）", {
    const std::string text = "para";
    test_support::MdHeadlessEnv env(kDirPrefix, "liveurt", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "urt.md", text));

    module::markdown::MdBlockModel model;
    model.update(text);
    DOCTEST_REQUIRE_EQ(model.blocks().size(), static_cast<size_t>(1));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    DOCTEST_REQUIRE(enterLiveAt(e, mod.get(), screen, 4));   // 光标在 "para" 末尾
    DOCTEST_CHECK_MESSAGE(
        e.ui->state<MdInputState>("md.live.input").cursor == 4,
        "进实时态块内光标 [" +
            std::to_string(e.ui->state<MdInputState>("md.live.input").cursor) + "]");
    DOCTEST_REQUIRE(typeIntoLiveInput(*e.ui, "x"));
    frame();
    DOCTEST_CHECK_MESSAGE(
        e.ui->state<MdInputState>("md.live.input").text == "parax",
        "实时态打 x 后活动块 [" +
            escapeCr(e.ui->state<MdInputState>("md.live.input").text) + "]");

    // 实时 → 编辑（Live→Split→Preview→Edit）：离开实时态
    for (int i = 0; i < 3; ++i) {
        DOCTEST_REQUIRE(cycleMode(mod.get()));
        frame();
    }
    DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);
    // 编辑态改一个字（不属于实时态那一次编辑会话）。两步：先走一帧让编辑器文本与
    // 文档对齐（值替换帧会把光标搬到新文本末尾），再把光标摆到文末；元素指针不能
    // 跨帧持有（ui.begin 每帧重建树），故最后重新 find 再派发输入
    frame();
    e.ui->state<MdInputState>(kEditorInputId).cursor = static_cast<int>(
        e.ui->state<MdInputState>(kEditorInputId).text.size());
    frame();
    const auto* edHit = e.ui->find("md.editor.input.hit");
    DOCTEST_REQUIRE(edHit != nullptr);
    DOCTEST_REQUIRE(edHit->onTextInput != nullptr);
    core::TextInputEvent ev;
    ev.text = "Z";
    edHit->onTextInput(ev);
    frame();

    // 编辑 → 实时（循环一格）：基线按当前文档重取
    DOCTEST_REQUIRE(cycleMode(mod.get()));
    frame();
    DOCTEST_REQUIRE(e.ui->find("md.live.input") != nullptr);
    DOCTEST_CHECK_MESSAGE(
        e.ui->state<MdInputState>("md.live.input").text == "paraxZ",
        "切回实时后活动块 [" +
            escapeCr(e.ui->state<MdInputState>("md.live.input").text) + "]");

    // Ctrl+Z → 新会话无可用快照：编辑态那一笔不许被撤销掉
    DOCTEST_REQUIRE(pressLiveKey(*e.ui, eui::InputKey::Z, false, true));
    frame();
    DOCTEST_CHECK_MESSAGE(
        e.ui->state<MdInputState>("md.live.input").text == "paraxZ",
        "切态后撤销动了编辑态的内容 [" +
            escapeCr(e.ui->state<MdInputState>("md.live.input").text) + "]");
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    frame();
    std::string saved;
    DOCTEST_REQUIRE(readFileBytes(env.dir / "urt.md", &saved));
    DOCTEST_CHECK_MESSAGE(saved == "paraxZ", "落盘 [" + escapeCr(saved) + "]");
    mod->onStop();
    return true;
})

// ② 块边界删除【不走 input 内置编辑】，故 framework 栈不增（该用例已把这条
// 锁死为 0）；
// 径因此不可撤销。本用例：块首退格改文档 → Ctrl+Z 走模块层快照栈撤回来。
REGISTER_TEST(md_module_live_boundary_undo,
              "markdown_module: 实时态块首退格可撤销", {
    const std::string text = "# A\n\n# B\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "livebund", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "bu.md", text));

    module::markdown::MdBlockModel model;
    model.update(text);
    const auto& bs = model.blocks();
    DOCTEST_REQUIRE_EQ(bs.size(), static_cast<size_t>(2));
    DOCTEST_REQUIRE_EQ(bs[1].start, static_cast<size_t>(4));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    DOCTEST_REQUIRE(enterLiveAt(e, mod.get(), screen, bs[1].start));
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     std::string("\n# B\n"));

    // ① 块首退格（边界路径）：删接缝 '\n' → 两块粘连；光标落接缝字节 3，
    DOCTEST_REQUIRE(pressLiveKey(*e.ui, eui::InputKey::Backspace));
    frame();
    const std::string merged = "# A\n# B\n";
    DOCTEST_CHECK_MESSAGE(
        e.ui->state<MdInputState>("md.live.input").text == "# A\n",
        "块首退格后活动块 [" +
            escapeCr(e.ui->state<MdInputState>("md.live.input").text) + "]");
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").cursor, 3);
    // 证伪探针（边界路径已锁的同一事实）：不产生 framework 快照
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").undoStack.size(),
                     static_cast<size_t>(0));

    // ② Ctrl+Z → 模块层快照栈把这次边界删除撤销回来
    DOCTEST_REQUIRE(pressLiveKey(*e.ui, eui::InputKey::Z, false, true));
    frame();
    DOCTEST_CHECK_MESSAGE(
        e.ui->state<MdInputState>("md.live.input").text == "\n# B\n",
        "撤销边界删除后活动块 [" +
            escapeCr(e.ui->state<MdInputState>("md.live.input").text) + "]");
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").cursor, 0);
    // 内置栈依旧为空：撤销走的是模块层快照栈（不是把框架栈喂出来）
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").undoStack.size(),
                     static_cast<size_t>(0));

    // ③ 落盘逐字节 = 原文档（撤销结果被持久化）
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    frame();
    std::string saved;
    DOCTEST_REQUIRE(readFileBytes(env.dir / "bu.md", &saved));
    DOCTEST_CHECK_MESSAGE(saved == text, "撤销后落盘 [" + escapeCr(saved) + "]");

    // ④ 反向：Ctrl+Y 重做这次边界删除 → 回到粘连文档（光标 = 撤销前的接缝字节 3，
    //    重切后落在块 0 内——与正向删除同一落点口径）
    DOCTEST_REQUIRE(pressLiveKey(*e.ui, eui::InputKey::Y, false, true));
    frame();
    DOCTEST_CHECK_MESSAGE(
        e.ui->state<MdInputState>("md.live.input").text == "# A\n",
        "重做边界删除后活动块 [" +
            escapeCr(e.ui->state<MdInputState>("md.live.input").text) + "]");
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").cursor, 3);
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    frame();
    DOCTEST_REQUIRE(readFileBytes(env.dir / "bu.md", &saved));
    DOCTEST_CHECK_MESSAGE(saved == merged, "重做后落盘 [" + escapeCr(saved) + "]");
    mod->onStop();
    return true;
})

// 滚动锚定与光标跟随
// 判定与目标在纯函数层（test_md_live_edit 的 md_live_follow_*）钉死；本组验接线：
//   · 越界才拉：点击 / 打字把 caret 落到视口安全带之外才动一次滚动，落点 = caret − 0.4H
//   · 安全带内不干预：caret 已在视口里时滚动位置一动不动
//   · 用户滚轮关跟随：滚轮后即便 caret 深深越界也不把视口拽回，直到下一次光标
//     事件（打字 / 方向键 / 点击）才恢复跟随
//     与「用户优先」的交界，对应手工点验 ④。
// 观测口径（诚实声明）：无头跑的是真实 Runtime（HeadlessFrame::frame 走
//   core::dsl::Runtime::compose）→ 程序化落地（模块调 Ui::setScrollOffset、帧末
//   消费直写实例 offset）真实生效，但看不到内容位移。可观测的是 scrollView 的
//   【声明值】（scrollOffset / scrollMaxOffset = 模块记账经框架钳位后的那一版）：
//   落地当帧的声明值还是拉前的旧值、下一帧才反映新 offset → 断言偏移前多走一帧。
//   真实滚动位移 / 观感 / 帧率由手工点验补。
namespace {

// 实时态 scrollView id 固定，全流程唯一（轮转 id 的 hack 已去）
constexpr const char* kLiveScrollId0 = "md.live.scroll";

// 活动 input 的光标 y（与模块同一口径：块顶 + input inset 10 + 显示行 × 行高）。
// 坐标系与 blockTopY 入参一致 —— 调用侧传【绝对页面坐标】的块顶（元素的 frame.y
// 就是绝对坐标），若要内容坐标自己再减去 scrollView 的 frame.y（= 内容原点）。
float liveCaretDocY(eui::Ui& ui, const std::string& blockText, float blockTopY) {
    const auto& st = ui.state<MdInputState>("md.live.input");
    using InputModel = components::input_detail::InputModel;
    const auto* blocks = ui.find("md.live.blocks");
    const float textW = (blocks != nullptr ? blocks->frame.width : 0.0f) - 20.0f;
    const float fontSize = core::theme::editorFontSize();
    const auto lines = InputModel::measureLines(blockText, "Microsoft YaHei",
                                               fontSize, textW);
    const int caretLine = InputModel::lineIndexFor(lines, st.cursor);
    return blockTopY + 10.0f +
           static_cast<float>(caretLine) * fontSize *
               module::markdown::kInputLineHeightFactor;
}

} // namespace

// 把 caret 落到视口下方的块 —— 越出下边界
// → 下一帧拉一次到 caret − 0.4H（落地走 Ui::setScrollOffset 直写实例 offset，id
// 固定不轮转，见模块注释）；随后在安全带内打字不再动滚动（「一次性，不逐帧强制」）。
REGISTER_TEST(md_module_live_scroll_follow_pull,
              "markdown_module: 实时态光标跟随 越界才拉一次（headless）", {
    const std::string doc = tallNoteDoc("F", 40);
    test_support::MdHeadlessEnv env(kDirPrefix, "livefollow", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "follow.md", doc));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();

    const auto* sv0 = e.ui->find(kLiveScrollId0);
    DOCTEST_REQUIRE(sv0 != nullptr);
    DOCTEST_REQUIRE(sv0->scrollMaxOffset > 200.0f);   // 高文档前提：可滚
    DOCTEST_CHECK_EQ(sv0->scrollOffset, 0.0f);        // 进态归零
    // id 恒定判据：跨帧断言 scrollStateId 不变 = 同一滚动实例（不再轮转）
    const std::string svStateId = sv0->scrollStateId;
    const float viewH = sv0->frame.height;

    // 点一个在视口下界之外的块（内容坐标 y > 0.75H）：caret 越出下边界
    const std::vector<int> sids = liveBlockSidsByY(*e.ui);
    DOCTEST_REQUIRE(sids.size() >= 20);
    const int deepSid = sids[sids.size() * 2 / 3];
    const auto* deepWrap = e.ui->find("md.live.blk." + std::to_string(deepSid));
    DOCTEST_REQUIRE(deepWrap != nullptr);
    // 元素 frame 是【页面绝对坐标】，blocks 列顶 = scrollView 顶（offset 0 时）
    // → 内容坐标 = 绝对 y − sv.y（本用例不模拟运行时位移，故该换算是稳定的）
    const float contentOrigin = sv0->frame.y;
    DOCTEST_REQUIRE_MESSAGE(
        deepWrap->frame.y - contentOrigin > viewH * 0.75f,
        "被测块须在视口安全带下方：块内容 y=" +
            std::to_string(deepWrap->frame.y - contentOrigin) +
            " 视口=" + std::to_string(viewH));
    // 点块的第 2 行（块首行是块间空行）：落点在视口外 → 下一帧拉一次
    DOCTEST_REQUIRE(clickLiveBlock(*e.ui, deepSid, 0.75f * deepWrap->frame.height));
    frame();   // 定位帧：锚 + 换文本（本帧不评估，见 setLiveCursor 注释）
    // 定位帧不动滚动（评估帧才动作）；id 固定
    DOCTEST_CHECK(e.ui->find(kLiveScrollId0) != nullptr);
    DOCTEST_CHECK_EQ(e.ui->find(kLiveScrollId0)->scrollOffset, 0.0f);
    // 活动块顶（几何表 blockTopY 的口径）：定位帧后重读 —— 定位前活动块是首块，
    // 它的包裹高是 input 贴合高（≠ 几何表 H_k），其下各块的列内累计会随之位移
    const auto* actWrap = e.ui->find("md.live.blk." + std::to_string(deepSid));
    DOCTEST_REQUIRE(actWrap != nullptr);
    const float caretY = liveCaretDocY(*e.ui, e.ui->state<MdInputState>(
                                                        "md.live.input").text,
                                      actWrap->frame.y) -
                         contentOrigin;   // → 内容坐标

    frame();   // 跟随评估帧：越界 → 落地（Ui::setScrollOffset，帧末生效）
    // ① id 恒定（不再换代轮转）：落地当帧声明值仍是拉前旧值
    const auto* svEval = e.ui->find(kLiveScrollId0);
    DOCTEST_REQUIRE(svEval != nullptr);
    DOCTEST_CHECK_EQ(svEval->scrollStateId, svStateId);
    frame();   // +1 帧：声明值才反映落地结果
    const auto* sv1 = e.ui->find(kLiveScrollId0);
    DOCTEST_REQUIRE(sv1 != nullptr);
    DOCTEST_CHECK_EQ(sv1->scrollStateId, svStateId);
    // ② 落点 = caret − 0.4H（目标未被 maxOffset 钳时的精确值）
    const float expected = caretY - module::markdown::kMdLiveFollowAnchorRatio * viewH;
    DOCTEST_CHECK_MESSAGE(std::fabs(sv1->scrollOffset - expected) <= 2.0f,
                          "拉后偏移 " + std::to_string(sv1->scrollOffset) +
                              " 期望 " + std::to_string(expected));
    // ③ 判据闭环：拉完后 caret 落在视口安全带内（[0.15H, 0.75H]）
    const float rel = caretY - sv1->scrollOffset;
    DOCTEST_CHECK_MESSAGE(
        rel >= module::markdown::kMdLiveFollowTopRatio * viewH &&
            rel <= module::markdown::kMdLiveFollowBottomRatio * viewH,
        "拉后 caret 视口相对位置 " + std::to_string(rel) + " 不在安全带内");

    // ④ 安全带内不干预：再走一帧（caret 没动）→ 不改偏移
    const float pulledOffset = sv1->scrollOffset;   // 元素每帧重建：存值不存指针
    frame();
    DOCTEST_CHECK(e.ui->find(kLiveScrollId0) != nullptr);
    DOCTEST_CHECK_EQ(e.ui->find(kLiveScrollId0)->scrollOffset, pulledOffset);
    // ⑤ 打字也在安全带内 → 不拉（「一次性，不逐帧强制」）
    DOCTEST_REQUIRE(typeIntoLiveInput(*e.ui, "X"));
    frame();
    DOCTEST_CHECK(e.ui->find(kLiveScrollId0) != nullptr);
    DOCTEST_CHECK_EQ(e.ui->find(kLiveScrollId0)->scrollOffset, pulledOffset);
    DOCTEST_CHECK(e.ui->state<MdInputState>("md.live.input").text.find('X') !=
                  std::string::npos);

    mod->onStop();
    return true;
})

// 用户先滚动、再打字 ——
//   · 滚轮（框架 setScrollOffset → scrollView.onChange）关跟随：caret 再越界也不拽回
//     （「其余时候不干预滚动」）；
//   · 下一次光标事件（打字）恢复跟随 → 越界 → 拉一次把 caret 带回安全带。
REGISTER_TEST(md_module_live_scroll_follow_wheel_disarms,
              "markdown_module: 实时态用户滚动关跟随 打字才回拉（headless）", {
    const std::string doc = tallNoteDoc("G", 40);
    test_support::MdHeadlessEnv env(kDirPrefix, "livefollowwheel", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "wheel.md", doc));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();

    const auto* sv0 = e.ui->find(kLiveScrollId0);
    DOCTEST_REQUIRE(sv0 != nullptr);
    DOCTEST_REQUIRE(sv0->scrollMaxOffset > 200.0f);
    const float viewH = sv0->frame.height;
    const float contentOrigin = sv0->frame.y;

    // 元素每帧重建、stableId 也可能随编辑重继承 → 一律按【块序号】现取当下元素
    auto wrapAt = [&](size_t ordinal) -> const core::dsl::Element* {
        const std::vector<int> cur = liveBlockSidsByY(*e.ui);
        if (ordinal >= cur.size()) return nullptr;
        return e.ui->find("md.live.blk." + std::to_string(cur[ordinal]));
    };
    // caret 的内容坐标 y（与模块同口径：块顶 + inset + 显示行 × 行高）
    auto caretContentY = [&](size_t ordinal) -> float {
        const core::dsl::Element* w = wrapAt(ordinal);
        if (w == nullptr) return -1.0f;
        return liveCaretDocY(*e.ui,
                             e.ui->state<MdInputState>("md.live.input").text,
                             w->frame.y) -
               contentOrigin;
    };

    // 把 caret 点到视口下方的块（越界）→ 跟随评估帧拉一次
    const std::vector<int> sids0 = liveBlockSidsByY(*e.ui);
    DOCTEST_REQUIRE(sids0.size() >= 20);
    const size_t deepOrdinal = sids0.size() * 2 / 3;
    const auto* deepWrap = wrapAt(deepOrdinal);
    DOCTEST_REQUIRE(deepWrap != nullptr);
    DOCTEST_REQUIRE(clickLiveBlock(*e.ui, sids0[deepOrdinal],
                                   0.75f * deepWrap->frame.height));
    frame();   // 定位帧（不评估）
    frame();   // 跟随评估帧：越界 → 落地（帧末生效，声明值仍是旧值）
    frame();   // +1 帧：声明值反映落地
    const auto* svPull = e.ui->find(kLiveScrollId0);
    DOCTEST_REQUIRE(svPull != nullptr);
    DOCTEST_REQUIRE(svPull->scrollOffset > 0.0f);   // 跟随拉过一次

    // ① 用户滚轮回到顶部：关跟随（onChange 写回并置 false）
    // 程序化标志已被落地当帧的 echo 消费（见模块 liveProgrammaticScroll_ 注释），
    // 此刻的 onChange 与用户滚轮无差别 —— 走关跟随分支
    DOCTEST_REQUIRE(svPull->onScrollOffsetChanged != nullptr);
    svPull->onScrollOffsetChanged(0.0f);
    frame();
    DOCTEST_REQUIRE(e.ui->find(kLiveScrollId0) != nullptr);
    DOCTEST_CHECK_EQ(e.ui->find(kLiveScrollId0)->scrollOffset, 0.0f);

    // ② caret 此刻远在视口下方（越界），但跟随已关 → 连续多帧都不拽回
    const float caretY = caretContentY(deepOrdinal);
    DOCTEST_REQUIRE_MESSAGE(caretY > viewH, "caret 须越出视口下缘（实测 " +
                                                std::to_string(caretY) + "）");
    for (int i = 0; i < 3; ++i) {
        frame();
        DOCTEST_REQUIRE(e.ui->find(kLiveScrollId0) != nullptr);
        DOCTEST_CHECK_EQ(e.ui->find(kLiveScrollId0)->scrollOffset, 0.0f);
    }

    // ③ 打字 = 光标事件 → 恢复跟随 → 越界才拉：一次回到 caret − 0.4H
    DOCTEST_REQUIRE(typeIntoLiveInput(*e.ui, "X"));
    frame();   // 光标事件帧：恢复跟随 + 评估落地（id 固定，声明值仍是 0）
    frame();   // +1 帧：声明值反映落地
    const auto* svBack = e.ui->find(kLiveScrollId0);
    DOCTEST_REQUIRE(svBack != nullptr);
    const float caretY2 = caretContentY(deepOrdinal);
    DOCTEST_REQUIRE(caretY2 > 0.0f);
    const float expected =
        caretY2 - module::markdown::kMdLiveFollowAnchorRatio * viewH;
    DOCTEST_CHECK_MESSAGE(std::fabs(svBack->scrollOffset - expected) <= 2.0f,
                          "打字回拉后偏移 " + std::to_string(svBack->scrollOffset) +
                              " 期望 " + std::to_string(expected));
    mod->onStop();
    return true;
})

// caret 的 y 用【显示行】（软折行参与计数），不是逻辑行 ——
// 折行段落里按逻辑行算会系统性低估 caret 的 y，恰是"块底部连续打字"最需要
// 这是跟手的场景。用例把两种口径的差异做成可判定的下界：
// 拉后目标必须比"逻辑行口径的期望值"至少高出半个折行高度。
REGISTER_TEST(md_module_live_scroll_follow_wrapped_caret,
              "markdown_module: 实时态折行段落 caret 按显示行跟随（headless）", {
    // 长段落在中间：30 段短段 → 1 段超长（软折行多行）→ 30 段短段（保证下方有
    // 内容 → 拉的目标不会被 maxOffset 钳掉 → 能验精确值）
    const std::string longLine = "W " + std::string(600, 'w');
    std::string doc;
    for (int i = 0; i < 30; ++i) doc += "head " + std::to_string(i) + "\n\n";
    doc += longLine + "\n\n";
    for (int i = 0; i < 30; ++i) doc += "tail " + std::to_string(i) + "\n\n";

    test_support::MdHeadlessEnv env(kDirPrefix, "livefollowwrap", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "wrap.md", doc));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();

    const auto* sv0 = e.ui->find(kLiveScrollId0);
    DOCTEST_REQUIRE(sv0 != nullptr);
    DOCTEST_REQUIRE(sv0->scrollMaxOffset > 200.0f);
    const float viewH = sv0->frame.height;
    const float contentOrigin = sv0->frame.y;
    const float fontSize = core::theme::editorFontSize();

    // 长段落 = 第 30 块（0 起：30 个短段之后）；先点它 → 它成活动块
    const std::vector<int> sids = liveBlockSidsByY(*e.ui);
    DOCTEST_REQUIRE(sids.size() >= 40);
    const int wrappedSid = sids[30];
    const auto* wrap = e.ui->find("md.live.blk." + std::to_string(wrappedSid));
    DOCTEST_REQUIRE(wrap != nullptr);
    const float wrapTop = wrap->frame.y - contentOrigin;
    DOCTEST_REQUIRE_MESSAGE(wrapTop > viewH * 0.75f, "长段落须在视口下方");
    DOCTEST_REQUIRE(clickLiveBlock(*e.ui, wrappedSid, wrap->frame.height - 2.0f));
    frame();   // 定位帧
    const auto* act = e.ui->find("md.live.blk." + std::to_string(wrappedSid));
    DOCTEST_REQUIRE(act != nullptr);
    const std::string blockText = e.ui->state<MdInputState>("md.live.input").text;
    // 块文本 = 段前空行 + 长段落（分块器把块间空行并入下一块头部）
    DOCTEST_REQUIRE_MESSAGE(blockText.find(longLine) != std::string::npos,
                            "活动块文本应包含长段落：[" + blockText.substr(0, 40) +
                                "...]");
    const float blockTop = act->frame.y - contentOrigin;
    // 点击只把 caret 放到【源码行的行首】（P3-⑤ 逐行跳转语义）——折行后的位置
    // 要落在段内后段，故直接把块内光标推到段末（等价于用户在段内按 End / 一路
    // 打字到折行处；与其它 live 用例直接驱动 InputState 同一口径）。
    // 取 size() − 1 而不是 size()：块尾字节 == 下一块块首（块边界字节归后一块，
    // spec D3 口径），落到那一字节会把活动块切换成下一块（光标回到 0）。
    {
        auto& st = e.ui->state<MdInputState>("md.live.input");
        st.cursor = static_cast<int>(blockText.size()) - 1;
        st.selectionStart = st.cursor;
        st.selectionEnd = st.cursor;
    }
    frame();   // 光标事件帧（锚重推 + 跟随恢复；安全带判定在下一帧见分晓）

    // 期望口径（测试侧独立算）：显示行号由组件同源函数给出，行高 = 字号 × 1.2
    using InputModel = components::input_detail::InputModel;
    DOCTEST_REQUIRE(e.ui->find("md.live.blocks") != nullptr);
    const float textW = e.ui->find("md.live.blocks")->frame.width - 20.0f;
    const auto lines = InputModel::measureLines(blockText, "Microsoft YaHei",
                                                fontSize, textW);
    DOCTEST_REQUIRE(lines.size() >= 3);   // 前提：确实折行了（否则本用例无意义）
    const int caretLine =
        InputModel::lineIndexFor(lines, e.ui->state<MdInputState>("md.live.input").cursor);
    DOCTEST_REQUIRE(caretLine >= 1);
    const float caretY = blockTop + 10.0f + static_cast<float>(caretLine) *
                                             fontSize * module::markdown::kInputLineHeightFactor;

    frame();   // 跟随评估帧（id 固定：落地帧末生效，本帧声明值已反映）
    const auto* sv1 = e.ui->find(kLiveScrollId0);
    DOCTEST_REQUIRE(sv1 != nullptr);
    const float expected =
        caretY - module::markdown::kMdLiveFollowAnchorRatio * viewH;
    DOCTEST_CHECK_MESSAGE(std::fabs(sv1->scrollOffset - expected) <= 2.0f,
                          "折行 caret 拉后偏移 " + std::to_string(sv1->scrollOffset) +
                              " 期望 " + std::to_string(expected));
    // 证伪逻辑行口径：按逻辑行（该段只有 1 个 '\n' → 行 0/1）算的期望值必须
    // 明显不同于实际落点 —— 两个口径的差 = (显示行 − 逻辑行) × 行高
    const float logicalExpected =
        blockTop + 10.0f + fontSize * module::markdown::kInputLineHeightFactor -
        module::markdown::kMdLiveFollowAnchorRatio * viewH;
    DOCTEST_CHECK_MESSAGE(
        sv1->scrollOffset - logicalExpected >=
            0.5f * static_cast<float>(caretLine) * fontSize *
                module::markdown::kInputLineHeightFactor,
        "落点未体现显示行：实际 " + std::to_string(sv1->scrollOffset) +
            " 逻辑行口径 " + std::to_string(logicalExpected) + " 显示行数 " +
            std::to_string(lines.size()));
    // 拉完 caret 落在安全带内
    const float rel = caretY - sv1->scrollOffset;
    DOCTEST_CHECK(rel >= module::markdown::kMdLiveFollowTopRatio * viewH &&
                  rel <= module::markdown::kMdLiveFollowBottomRatio * viewH);

    mod->onStop();
    return true;
})

// 去 id 轮转 hack 后滚动容器 id 固定——元素每帧重建，scrollStateId 稳定 =
// 跨帧恒定。连续 compose 帧 find("md.live.scroll") 都命中，且 scrollStateId 不变
//（元素每帧重建 → scrollStateId 稳定 = 同一滚动 state 实例的判据）；一次跟随拉
// 之后 id 依然不变（不再有 .g1 换代实例）；离开实时态后元素不复存在（存在性
// 回归，照 :469-485 模式）。
REGISTER_TEST(md_module_live_scroll_id_stable,
              "markdown_module: 实时态滚动容器 id 跨帧恒定（headless）", {
    const std::string doc = tallNoteDoc("H", 40);
    test_support::MdHeadlessEnv env(kDirPrefix, "liveidstable", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "stable.md", doc));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();

    // 进态首帧：id 即固定（不再是"代 0"，不存在 .g1 换代）
    const auto* sv0 = e.ui->find(kLiveScrollId0);
    DOCTEST_REQUIRE(sv0 != nullptr);
    DOCTEST_REQUIRE(sv0->scrollMaxOffset > 200.0f);
    const std::string svStateId = sv0->scrollStateId;
    DOCTEST_CHECK(e.ui->find("md.live.scroll.g1") == nullptr);

    // 连续 compose 帧：id 恒定（同一 scrollStateId、元素存在）
    for (int i = 0; i < 3; ++i) {
        frame();
        const auto* sv = e.ui->find(kLiveScrollId0);
        DOCTEST_REQUIRE(sv != nullptr);
        DOCTEST_CHECK_EQ(sv->scrollStateId, svStateId);
    }

    // 一次跟随拉之后 id 依然不变（程序化改滚动位置不再换代）：
    // 点视口下方块 → 定位 / 评估 / 落点帧 → id 恒为固定值
    const std::vector<int> sids = liveBlockSidsByY(*e.ui);
    DOCTEST_REQUIRE(sids.size() >= 20);
    const int deepSid = sids[sids.size() * 2 / 3];
    const auto* deepWrap = e.ui->find("md.live.blk." + std::to_string(deepSid));
    DOCTEST_REQUIRE(deepWrap != nullptr);
    // sv0 是进态首帧的元素指针，跨 compose 帧后不可再读——断言用当前帧的 sv
    const auto* svNow = e.ui->find(kLiveScrollId0);
    DOCTEST_REQUIRE(svNow != nullptr);
    DOCTEST_REQUIRE_MESSAGE(
        deepWrap->frame.y - svNow->frame.y > svNow->frame.height * 0.75f,
        "被测块须在视口安全带下方（实测 " +
            std::to_string(deepWrap->frame.y - svNow->frame.y) + "）");
    DOCTEST_REQUIRE(clickLiveBlock(*e.ui, deepSid, 0.75f * deepWrap->frame.height));
    frame();   // 定位帧
    frame();   // 跟随评估帧：越界 → 落地（帧末生效）
    frame();   // +1 帧：声明值反映落地
    const auto* svPulled = e.ui->find(kLiveScrollId0);
    DOCTEST_REQUIRE(svPulled != nullptr);
    DOCTEST_CHECK_EQ(svPulled->scrollStateId, svStateId);
    DOCTEST_REQUIRE(svPulled->scrollOffset > 0.0f);
    DOCTEST_CHECK(e.ui->find("md.live.scroll.g1") == nullptr);
    // 实时态元素存在性回归（id 固定后既有的这一批元素照常渲染）
    DOCTEST_REQUIRE(e.ui->find("md.live.input") != nullptr);
    DOCTEST_REQUIRE(e.ui->find("md.live.blocks") != nullptr);

    // 离开实时态：滚动容器不复存在（存在性回归，照 :469-485 模式）
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → Split
    frame();
    DOCTEST_CHECK(e.ui->find(kLiveScrollId0) == nullptr);

    mod->onStop();
    return true;
})

// 高亮与视觉一致
// 三件事：
//   ① liveHighlighter_ 独立实例喂【块文本】——runs 用块局部坐标（byteStart 以
//      块首为原点，与 input 内排版同原点）；与编辑态实例（整篇文档坐标）分
//      实例是硬约束（混用会因坐标系交替抖动围栏缓存）；块内围栏状态自洽；
//      主题切换同步换色 + bump live input 行缓存代号（与编辑态同口径）
//   ② 渲染块 CodeFence 自绘复用：同一 drawBlockFlow 通路（md_preview_highlight
//      + MdPreviewCodeCache，stableId+hash 键控）——锁 live 与预览两态下同一
//      围栏块的自绘元素（几何/圆角/背景色/着色 runs 逐项一致）
//   ③ 活动块边框（.frame：透明填充 + 主色半透明 1px + 预览块同圆角），活动块
//      互斥只建一份；验收口径 = 块间隙/圆角/色表一致（宽度无关项；live 720px
//      与预览 760px 阅读列宽差异导致的块高差属预期，不在此比对）
REGISTER_TEST(md_module_live_highlight,
              "markdown_module: 实时态活动块高亮块文本坐标（headless）", {
    const std::string text =
        "# 标题\n\n`代码` 段\n\n```cpp\nint v = 7;\n```\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "livehl", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "hl.md", text));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&] { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);
    // 光标落在第二块（`代码` 段）块首 → 进实时态活动块 = 第二块
    module::markdown::MdBlockModel model;
    model.update(text);
    DOCTEST_REQUIRE(model.blocks().size() >= 3);
    DOCTEST_CHECK(model.blockIndexAt(model.blocks()[1].start) == 1u);
    e.ui->state<MdInputState>(kEditorInputId).cursor =
        static_cast<int>(model.blocks()[1].start);

    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();
    DOCTEST_REQUIRE(e.ui->find("md.live.input") != nullptr);

    // ① 活动块 = 第二块：输入框文本 = 块源码（逐字节）；`代码` 段所在行 =
    //    1 个 code run、整段 success 色（md_highlight 行内代码语义在块文本上
    //    照常成立（高亮语义不退化）。注意块模型分块约定：
    //    段落块【包含】接续空行的 '\n'（块 1 = "\n`代码` 段\n"），故源码空行
    //    是行 0、正文在行 1 —— 断言按元素文本匹配定位行，不写死行号之外假设
    const std::string blockText =
        text.substr(model.blocks()[1].start,
                    model.blocks()[1].end - model.blocks()[1].start);
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     blockText);
    const auto* line1 = e.ui->find("md.live.input.text.1");
    DOCTEST_REQUIRE(line1 != nullptr);
    DOCTEST_CHECK_EQ(line1->text, std::string("`代码` 段"));
    DOCTEST_REQUIRE_EQ(line1->textRuns.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(line1->textRuns[0].byteStart, 0);
    DOCTEST_CHECK_EQ(line1->textRuns[0].byteEnd, 8);  // ` + 代码(6B) + `
    DOCTEST_CHECK(colorEq(line1->textRuns[0].color,
                          core::theme::current().colors.success));

    // ② 点击块 0（标题）→ 活动块切换：行 0 = 1 个 marker run [0,1)，仍块局部
    DOCTEST_REQUIRE(clickLiveBlock(*e.ui, 0, 6.0f));
    frame();
    DOCTEST_REQUIRE(e.ui->state<MdInputState>("md.live.input").text.find(
                        "# 标题") == 0u);
    const auto* hdr = e.ui->find("md.live.input.text.0");
    DOCTEST_REQUIRE(hdr != nullptr);
    DOCTEST_REQUIRE_EQ(hdr->textRuns.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(hdr->textRuns[0].byteStart, 0);
    DOCTEST_CHECK_EQ(hdr->textRuns[0].byteEnd, 1);
    DOCTEST_CHECK(colorEq(hdr->textRuns[0].color,
                          core::theme::current().colors.textTertiary));

    // ③ 点击块 2（围栏）→ 活动块 = 围栏块：开栏/内容/闭栏按行着色（块内围栏
    //    状态自洽 —— 分块器保证围栏不跨块，块文本喂 liveHighlighter_ 语义
    //    不退化）。围栏块同样带前导 '\n' → 围栏首行在行 1
    DOCTEST_REQUIRE(clickLiveBlock(*e.ui, 2, 30.0f));
    frame();
    const auto* fence0 = e.ui->find("md.live.input.text.1");
    const auto* fence1 = e.ui->find("md.live.input.text.2");
    const auto* fence2 = e.ui->find("md.live.input.text.3");
    DOCTEST_REQUIRE(fence0 != nullptr);
    DOCTEST_REQUIRE(fence1 != nullptr);
    DOCTEST_REQUIRE(fence2 != nullptr);
    DOCTEST_CHECK_EQ(fence0->text, std::string("```cpp"));
    DOCTEST_REQUIRE_EQ(fence0->textRuns.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(fence0->textRuns[0].byteStart, 0);
    DOCTEST_CHECK(colorEq(fence0->textRuns[0].color,
                          core::theme::current().colors.textTertiary));
    DOCTEST_CHECK_EQ(fence1->text, std::string("int v = 7;"));
    DOCTEST_REQUIRE_EQ(fence1->textRuns.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(fence1->textRuns[0].byteStart, 0);
    DOCTEST_CHECK_EQ(fence1->textRuns[0].byteEnd,
                     static_cast<int>(fence1->text.size()));
    DOCTEST_CHECK(colorEq(fence1->textRuns[0].color,
                          core::theme::current().colors.success));
    DOCTEST_CHECK_EQ(fence2->text, std::string("```"));
    DOCTEST_REQUIRE_EQ(fence2->textRuns.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(colorEq(fence2->textRuns[0].color,
                          core::theme::current().colors.textTertiary));

    // ④ 主题切换：live input styleRevision++ + 可视行 runs 换色（与编辑态同口径）；
    //    换回深色不污染后续用例
    const unsigned long long revBefore =
        e.ui->state<MdInputState>("md.live.input").styleRevision;
    const std::string themeBefore = core::theme::currentThemeName();
    core::theme::setLight();
    frame();
    DOCTEST_CHECK_EQ(
        e.ui->state<MdInputState>("md.live.input").styleRevision,
        revBefore + 1);
    const auto* fence1Light = e.ui->find("md.live.input.text.2");
    DOCTEST_REQUIRE(fence1Light != nullptr);
    DOCTEST_REQUIRE_EQ(fence1Light->textRuns.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(colorEq(fence1Light->textRuns[0].color,
                          core::theme::current().colors.success));
    if (themeBefore == "Light") {
        core::theme::setLight();
    } else {
        core::theme::setDark();
    }

    mod->onStop();
    return true;
})

REGISTER_TEST(md_module_live_active_frame,
              "markdown_module: 实时态活动块边框/背景（headless）", {
    const std::string text = "# A\n\n## B\n\n## C\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "liveframe", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "frame.md", text));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&] { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);
    module::markdown::MdBlockModel model;
    model.update(text);
    DOCTEST_REQUIRE(model.blocks().size() >= 3);
    e.ui->state<MdInputState>(kEditorInputId).cursor =
        static_cast<int>(model.blocks()[1].start);

    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();

    // ① 活动块包裹内出现 .frame：位置/尺寸 == 包裹（透明覆盖层）、圆角 == 预览
    //    块圆角（MarkdownStyle.radius 同源）、1px 主色半透明边框、透明填充
    const auto* wrap = e.ui->find("md.live.blk.1");
    DOCTEST_REQUIRE(wrap != nullptr);
    const auto* frameEl = e.ui->find("md.live.blk.1.frame");
    DOCTEST_REQUIRE(frameEl != nullptr);
    DOCTEST_CHECK(std::fabs(frameEl->frame.width - wrap->frame.width) <= 0.01f);
    DOCTEST_CHECK(std::fabs(frameEl->frame.height - wrap->frame.height) <=
                  0.01f);
    DOCTEST_CHECK(std::fabs(frameEl->frame.x - wrap->frame.x) <= 0.01f);
    DOCTEST_CHECK(std::fabs(frameEl->frame.y - wrap->frame.y) <= 0.01f);
    const components::MarkdownStyle style(
        core::theme::componentTokens(core::theme::current()));
    DOCTEST_CHECK(std::fabs(frameEl->radius - style.radius) <= 0.01f);
    DOCTEST_CHECK(std::fabs(frameEl->border.width - 1.0f) <= 0.01f);
    // 镜像模块常量 kLiveActiveFrameAlpha = 0.6f（cpp 文件私有，测试同值锁定
    // 语义：主色半透明）
    const eui::Color expectBorder = components::theme::withAlpha(
        core::theme::componentTokens(core::theme::current()).primary, 0.6f);
    DOCTEST_CHECK(colorEq(frameEl->border.color, expectBorder));
    DOCTEST_CHECK(frameEl->color.a == 0.0f);   // 透明填充：不挡 input 文本

    // ② 非活动块无 .frame（活动块互斥 —— 每帧只建活动块一份）
    DOCTEST_CHECK(e.ui->find("md.live.blk.0.frame") == nullptr);
    DOCTEST_CHECK(e.ui->find("md.live.blk.2.frame") == nullptr);

    // ③ 换块后边框跟到新活动块（点块 2 → .frame 在 blk.2、blk.1 消失）
    DOCTEST_REQUIRE(clickLiveBlock(*e.ui, 2, 5.0f));
    frame();
    DOCTEST_CHECK(e.ui->find("md.live.blk.2.frame") != nullptr);
    DOCTEST_CHECK(e.ui->find("md.live.blk.1.frame") == nullptr);

    mod->onStop();
    return true;
})

REGISTER_TEST(md_module_live_preview_parity,
              "markdown_module: 实时态与预览态逐块观感一致（headless）", {
    const std::string text =
        "# 头\n\n首段文字\n\n```cpp\nint v = 7;\n```\n\n尾段\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "liveparity", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "parity.md", text));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&] { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);
    // 活动块定在末块（尾段）：其前的块 0..2 与预览态逐块同源（活动块自身
    // wrap 高 = input 贴合高 ≠ 渲染块高，不在比对范围）
    module::markdown::MdBlockModel model;
    model.update(text);
    DOCTEST_REQUIRE(model.blocks().size() >= 4);
    e.ui->state<MdInputState>(kEditorInputId).cursor =
        static_cast<int>(model.blocks()[3].start);

    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();
    const std::string kLivePrefix = "md.live";
    // ① 实时态：非活动块包裹存在；代码围栏走 DevDesk 侧自绘（.code.bg），
    //    且着色 runs 非空（cpp 词法器生效 = 复用了 md_preview_highlight）。
    //    元素指针只在当帧有效（帧末元素树换代）→ 与预览态比对的值一律
    //    【按值】捕获（半径/背景色/尺寸/runs 拷贝），指针本身不跨帧使用
    const auto gapOf = [](eui::Ui& ui, const std::string& prefix) {
        const auto* b0 = ui.find(prefix + ".blk.0");
        const auto* b1 = ui.find(prefix + ".blk.1");
        const auto* b2 = ui.find(prefix + ".blk.2");
        if (b0 == nullptr || b1 == nullptr || b2 == nullptr) return -1.0f;
        const float g01 = b1->frame.y - (b0->frame.y + b0->frame.height);
        const float g12 = b2->frame.y - (b1->frame.y + b1->frame.height);
        if (std::fabs(g01 - g12) > 0.01f) return -1.0f;
        return g01;
    };
    const float gapLive = gapOf(*e.ui, kLivePrefix);
    for (int sid : {0, 1, 2}) {
        DOCTEST_CHECK(e.ui->find("md.live.blk." + std::to_string(sid)) !=
                      nullptr);
    }
    const auto* liveBg = e.ui->find("md.live.blk.2.code.bg");
    DOCTEST_REQUIRE(liveBg != nullptr);
    const auto* liveTxt = e.ui->find("md.live.blk.2.code.text");
    DOCTEST_REQUIRE(liveTxt != nullptr);
    DOCTEST_REQUIRE(liveTxt->textRuns.size() >= static_cast<size_t>(1));
    const float liveBgRadius = liveBg->radius;
    const eui::Color liveBgColor = liveBg->color;
    const float liveBgH = liveBg->frame.height;
    const auto liveRuns = liveTxt->textRuns;   // 值拷贝：跨帧比对不许持旧指针

    // 切到预览态（同文档同块序，几何表同源；阅读列宽 760 ≠ 实时 720，块高
    // 随文本折行变化属预期 —— 验收比对宽度无关项：块间隙 / 圆角 / 色表）
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → Split
    frame();
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 预览
    frame();
    for (int sid : {0, 1, 2}) {
        DOCTEST_CHECK(e.ui->find("md.preview.blk." + std::to_string(sid)) !=
                      nullptr);
    }
    const auto* pvBg = e.ui->find("md.preview.blk.2.code.bg");
    DOCTEST_REQUIRE(pvBg != nullptr);
    const auto* pvTxt = e.ui->find("md.preview.blk.2.code.text");
    DOCTEST_REQUIRE(pvTxt != nullptr);
    const float gapPv = gapOf(*e.ui, "md.preview");

    // ② 圆角/尺寸一致：围栏自绘背景两态同为 MarkdownStyle.radius、同高度
    //    （宽是阅读列宽差：实时 720 vs 预览 760 —— 预期差异，不比对；围栏短行
    //    不折行 → 高度与宽无关，两态恒等）
    const components::MarkdownStyle style(
        core::theme::componentTokens(core::theme::current()));
    DOCTEST_CHECK(std::fabs(liveBgRadius - pvBg->radius) <= 0.01f);
    DOCTEST_CHECK(std::fabs(liveBgRadius - style.radius) <= 0.01f);
    DOCTEST_CHECK(std::fabs(liveBgH - pvBg->frame.height) <= 0.01f);
    // ③ 色表一致：围栏背景色 + 着色 runs（byteStart/byteEnd/color 逐项）
    DOCTEST_CHECK(colorEq(liveBgColor, pvBg->color));
    DOCTEST_REQUIRE_EQ(liveRuns.size(), pvTxt->textRuns.size());
    for (size_t i = 0; i < liveRuns.size(); ++i) {
        DOCTEST_CHECK_EQ(liveRuns[i].byteStart, pvTxt->textRuns[i].byteStart);
        DOCTEST_CHECK_EQ(liveRuns[i].byteEnd, pvTxt->textRuns[i].byteEnd);
        DOCTEST_CHECK(colorEq(liveRuns[i].color, pvTxt->textRuns[i].color));
    }
    // ④ 块间隙一致：相邻包裹净间距两态同值 == MarkdownStyle.blockGap
    //    （column gap 与块高无关 —— 块高随阅读列宽变化是预期，间隙不是）
    DOCTEST_CHECK(gapLive > 0.0f);
    DOCTEST_CHECK(std::fabs(gapLive - gapPv) <= 0.01f);
    DOCTEST_CHECK(std::fabs(gapLive - style.blockGap) <= 0.01f);

    mod->onStop();
    return true;
})

// 重载后活动块内容映射
// 静默重载/冲突重载把整篇换成磁盘版本后，活动块按内容身份
// (kind,hash,stableId) 映射到新文档，而不是落到「覆盖旧字节的新块」。
//
// 执行前置：轮询节流 = 真实 5s 时钟（无注入缝）；无头下静默路径靠
// 原生 FileWatcher（Windows ReadDirectoryChangesW / macOS FSEvents 均为真实现；
// 非 Apple POSIX stub 无事件，该路径退化为 5s 轮询）的 credit（hasWatchCredit）
// 绕过节流（与 test_markdown_service.cpp 的 watcher 用例同一机制）。mtime 秒级严格 > 比较 →
// 外写后按「max(写前文件时间, 写后真实时间) + 2s」推进（多次外写严格递增，秒级
// floor 后仍 ≥2 整秒间隔，末次不会因落在同一墙钟秒而检测不到；等价满足「晚于
// lastKnownMtime_ 所在秒」，免 ≥1s 真睡）。watcher 送达是异步的（去抖 200ms +
// 调度），不用固定睡等：每条用例写盘后按「重载正信号」有界自旋（帧 + 120ms ×
// 60 预算），信号帧即重载帧；冲突路径的自旋每轮先补一次编辑重置 400ms 自动保存
// 防抖（复刻按观察到的全文/块文本镜像，重试次数不影响确定性）。5s 真时钟节流
// 路径本身登记 deferred。相关用例走冲突 toast 路径。

namespace {

using module::markdown::MdBlock;
using module::markdown::MdBlockUpdate;
using module::markdown::MdBlockKind;
using module::markdown::MdBlockModel;
using module::markdown::mdFindBlockByContent;

// 把笔记文件换成磁盘新版本，并把 mtime 推进到「max(写前文件时间, 写后真实时间)
// + 2s」——写盘本身会把 mtime 重置回 now（低于上一版外写的未来值），base 取
// 两者较大者保证逐次外写严格递增（秒级 floor 后仍 ≥2 整秒间隔），同秒写盘检测
// 不到的问题免真睡规避。不等待 watcher 送达：由每条用例的 md01WaitReload /
// 冲突自旋按正信号收敛。
bool md01WriteExternal(const fs::path& path, const std::string& content) {
    std::error_code ec;
    const auto prev = fs::last_write_time(path, ec);   // 上一版外写的 mtime（可能在未来）
    if (ec) return false;
    if (!writeTextFile(path, content)) return false;
    const auto now = fs::last_write_time(path, ec);    // 写盘后的真实 mtime
    if (ec) return false;
    const auto base = now > prev ? now : prev;
    fs::last_write_time(path, base + std::chrono::seconds(2), ec);
    return !ec;
}

// 有界自旋：逐帧 compose 直至 done(*ui) 为真（60 × 120ms 预算）。watcher 事件
// 送达前帧内 poll 静默跳过（credit 未置位），事件送达的帧即重载帧。仅用于
// editorDirty_ == false 的静默路径（自旋帧不触发自动保存）。
void md01WaitReload(test_support::HeadlessFrame& e, ::core::Module* mod,
                    const eui::Screen& screen,
                    const std::function<bool(eui::Ui&)>& done) {
    for (int i = 0; i < 60; ++i) {
        e.frame([&](eui::Ui& u) { mod->compose(u, screen); });
        if (done(*e.ui)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
}

// 渲染出的块包裹 id 列表（prefix = "md.live" / "md.preview"；重载正信号：块数 /
// stableId 集合变化）
std::vector<int> md01RenderedSids(eui::Ui& ui, const std::string& prefix) {
    std::vector<int> ids;
    for (int sid = 0; sid < 96; ++sid) {
        if (ui.find(prefix + ".blk." + std::to_string(sid)) != nullptr) {
            ids.push_back(sid);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

// 复刻模块的块模型演化：m 已按模块前 startIdx 步演化（update 序列与模块同序、
// stableId 计数器同为 0 起 → 逐位一致），把 texts[startIdx..] 按同序补完，返回
// 演化后各块 stableId 的排序集合（重载发生的正信号）。映射判别用同一个复刻。
std::vector<int> md01SidSeq(MdBlockModel& m, size_t startIdx,
                            const std::vector<std::string>& texts) {
    for (size_t i = startIdx; i < texts.size(); ++i) {
        m.update(texts[i]);
    }
    std::vector<int> ids;
    for (const auto& b : m.blocks()) ids.push_back(b.stableId);
    std::sort(ids.begin(), ids.end());
    return ids;
}

// 编辑器输入框真实打字（走 onTextInput → onChange → onEditorTextChanged 链路）。
// 必须在编辑态帧内调用（hit 取当前帧元素；实时态帧树里没有编辑器列）。
bool typeIntoEditor(eui::Ui& ui, const std::string& text) {
    const auto* hit = ui.find(module::markdown::kEditorInputHitId);
    if (hit == nullptr || !hit->onTextInput) return false;
    core::TextInputEvent ev;
    ev.text = text;
    hit->onTextInput(ev);
    return true;
}

} // namespace

// 用例 1：前插平移，三元组主键命中
REGISTER_TEST(md_module_md01_reload_prepend,
              "markdown_module: MD-01 reload map prepend triple hit (headless)", {
    const std::string oldDoc = "alpha\n\nbravo\n\ncharlie\n";
    const std::string newDoc = "seed line one\n\nalpha\n\nbravo\n\ncharlie\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "md01pre", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "note.md", oldDoc));

    MdBlockModel replica;   // 复刻模块演化（同 update 序列）：open(oldDoc) → reload(newDoc)
    replica.update(oldDoc);
    DOCTEST_REQUIRE(replica.blocks().size() == 3);
    const size_t cursorByte = replica.blocks()[1].start + 3;   // 活动块 = bravo
    const MdBlock oldActive = replica.blocks()[1];

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    DOCTEST_REQUIRE(enterLiveAt(e, mod.get(), screen, cursorByte));
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     oldDoc.substr(oldActive.start,
                                   oldActive.end - oldActive.start));
    DOCTEST_REQUIRE(md01RenderedSids(*e.ui, "md.live").size() == 3);   // 重载前置信号
    DOCTEST_REQUIRE(e.ui->find("md.live.input") != nullptr);

    // 外部前插一整段 → 静默重载 → 三元组主键命中（平移下 stableId 保留）
    const std::vector<int> wantSids =
        md01SidSeq(replica, 1, {oldDoc, newDoc});
    DOCTEST_REQUIRE(md01WriteExternal(env.dir / "note.md", newDoc));
    md01WaitReload(e, mod.get(), screen, [&](eui::Ui& u) {
        return md01RenderedSids(u, "md.live") == wantSids;   // 重载正信号
    });
    frame();   // 重载后稳定帧
    const auto& bs = replica.blocks();
    DOCTEST_REQUIRE(bs.size() == 4);
    const size_t mapped =
        mdFindBlockByContent(bs, oldActive.kind, oldActive.hash,
                             oldActive.stableId, oldActive.start);
    DOCTEST_REQUIRE(mapped != std::string::npos);
    DOCTEST_CHECK_EQ(mapped, static_cast<size_t>(2));   // 落在 bravo 块（非插入的 seed 块）
    const std::string want =
        newDoc.substr(bs[mapped].start, bs[mapped].end - bs[mapped].start);
    DOCTEST_CHECK_EQ(want, std::string("\nbravo\n"));
    DOCTEST_CHECK_EQ(md01RenderedSids(*e.ui, "md.live"), wantSids);
    // 公共断言：锚点与可见 input 一致
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text, want);
    // 配对断言：scope 释放帧后文本仍绑定且焦点同帧补发
    frame();
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text, want);
    DOCTEST_CHECK(e.ui->isFocused("md.live.input.hit"));
    mod->onStop();
    return true;
})

// 用例 2：不相邻双改（A B C → A' B C'），三元失配 → (kind,hash) 回退
// 单点改 A 不够——后缀对齐保 id，三元组仍命中；必须 A、C 都改，
// p=0 s=0 → B（字节未变）落变化窗口丢 stableId。
REGISTER_TEST(md_module_md01_reload_double_edit,
              "markdown_module: MD-01 reload map double edit kind-hash fallback (headless)", {
    const std::string oldDoc = "alpha\n\nbravo\n\ncharlie\n";
    const std::string newDoc = "alpha changed\n\nbravo\n\ncharlie!\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "md01dbl", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "note.md", oldDoc));

    MdBlockModel replica;   // 复刻模块演化：open(oldDoc) → reload(newDoc)
    replica.update(oldDoc);
    DOCTEST_REQUIRE(replica.blocks().size() == 3);
    const MdBlock oldActive = replica.blocks()[1];
    const size_t cursorByte = oldActive.start + 3;

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    DOCTEST_REQUIRE(enterLiveAt(e, mod.get(), screen, cursorByte));
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     oldDoc.substr(oldActive.start,
                                   oldActive.end - oldActive.start));

    const std::vector<int> wantSids =
        md01SidSeq(replica, 1, {oldDoc, newDoc});
    DOCTEST_REQUIRE(md01WriteExternal(env.dir / "note.md", newDoc));
    md01WaitReload(e, mod.get(), screen, [&](eui::Ui& u) {
        return md01RenderedSids(u, "md.live") == wantSids;
    });
    frame();   // 重载后稳定帧
    const auto& nbs = replica.blocks();
    DOCTEST_REQUIRE(nbs.size() == 3);
    // 三元组无命中（B 字节未变但落变化窗口拿新 id —— 复刻侧直接扫证）
    bool anyTriple = false;
    for (const auto& b : nbs) {
        if (b.kind == oldActive.kind && b.hash == oldActive.hash &&
            b.stableId == oldActive.stableId) {
            anyTriple = true;
        }
    }
    DOCTEST_CHECK_FALSE(anyTriple);
    // (kind,hash) 回退命中 B（唯一等价块）
    const size_t mapped =
        mdFindBlockByContent(nbs, oldActive.kind, oldActive.hash,
                             oldActive.stableId, oldActive.start);
    DOCTEST_REQUIRE(mapped != std::string::npos);
    DOCTEST_CHECK_EQ(mapped, static_cast<size_t>(1));
    const std::string want =
        newDoc.substr(nbs[mapped].start, nbs[mapped].end - nbs[mapped].start);
    DOCTEST_CHECK_EQ(want, std::string("\nbravo\n"));
    DOCTEST_CHECK_EQ(md01RenderedSids(*e.ui, "md.live"), wantSids);
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text, want);
    frame();
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text, want);
    DOCTEST_CHECK(e.ui->isFocused("md.live.input.hit"));
    mod->onStop();
    return true;
})

// 用例 3：删块零命中 → 钳位（多帧断言）
REGISTER_TEST(md_module_md01_reload_block_deleted,
              "markdown_module: MD-01 reload map block deleted clamp zero-hit (headless)", {
    const std::string oldDoc = "alpha\n\nbravo\n\ncharlie\n";
    const std::string newDoc = "alpha\n\ncharlie\n";   // bravo 整段被外部删除
    test_support::MdHeadlessEnv env(kDirPrefix, "md01del", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "note.md", oldDoc));

    MdBlockModel replica;   // 复刻模块演化：open(oldDoc) → reload(newDoc)
    replica.update(oldDoc);
    DOCTEST_REQUIRE(replica.blocks().size() == 3);
    const size_t cursorByte = replica.blocks()[1].start + 2;   // 活动块 = bravo
    const MdBlock oldActive = replica.blocks()[1];

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    DOCTEST_REQUIRE(enterLiveAt(e, mod.get(), screen, cursorByte));
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     oldDoc.substr(oldActive.start,
                                   oldActive.end - oldActive.start));

    const std::vector<int> wantSids =
        md01SidSeq(replica, 1, {oldDoc, newDoc});
    DOCTEST_REQUIRE(md01WriteExternal(env.dir / "note.md", newDoc));
    md01WaitReload(e, mod.get(), screen, [&](eui::Ui& u) {
        return md01RenderedSids(u, "md.live") == wantSids;
    });
    frame();   // 重载后稳定帧
    const auto& nbs = replica.blocks();
    DOCTEST_REQUIRE(nbs.size() == 2);
    // 零命中证：内容身份在新文档不存在
    DOCTEST_CHECK_EQ(
        mdFindBlockByContent(nbs, oldActive.kind, oldActive.hash,
                             oldActive.stableId, oldActive.start),
        std::string::npos);
    // 钳位期望（与实现同口径）：min(旧 liveCursorByte_, 新 size) → 覆盖块原文
    const size_t clamped = std::min(cursorByte, newDoc.size());
    const size_t kExp = replica.blockIndexAt(clamped);
    const std::string want =
        newDoc.substr(nbs[kExp].start, nbs[kExp].end - nbs[kExp].start);
    // 多帧断言：重载帧后 3 帧收敛不飘（下帧重推覆盖不掉）
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text, want);
    DOCTEST_CHECK_EQ(md01RenderedSids(*e.ui, "md.live"), wantSids);
    frame();
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text, want);
    DOCTEST_CHECK(e.ui->isFocused("md.live.input.hit"));
    frame();
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text, want);
    mod->onStop();
    return true;
})

// 用例 3b：非空→空文档重载（零命中最坏形态，空文档 guard）
REGISTER_TEST(md_module_md01_reload_to_empty,
              "markdown_module: MD-01 reload map non-empty to empty doc guard (headless)", {
    const std::string oldDoc = "alpha\n\nbravo\n\ncharlie\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "md01empty", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "note.md", oldDoc));

    MdBlockModel model;
    model.update(oldDoc);
    DOCTEST_REQUIRE(model.blocks().size() == 3);
    const size_t cursorByte = model.blocks()[1].start + 2;

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    DOCTEST_REQUIRE(enterLiveAt(e, mod.get(), screen, cursorByte));
    DOCTEST_REQUIRE(e.ui->find("md.live.input") != nullptr);

    // 外部清空文件 → 重载 → 空文档 guard（liveActiveBlock_/inputStart/byte 归零，
    // 不越界、不崩）；空块序列下 live 列不建 input
    DOCTEST_REQUIRE(md01WriteExternal(env.dir / "note.md", ""));
    md01WaitReload(e, mod.get(), screen, [&](eui::Ui& u) {
        return u.find("md.live.input") == nullptr;
    });
    frame();
    DOCTEST_CHECK(e.ui->find("md.live.input") == nullptr);
    frame();
    DOCTEST_CHECK(e.ui->find("md.live.input") == nullptr);
    mod->onStop();
    return true;
})

// 用例 4：重复块三元组决胜
// 段落 fixture（abc 重复段落）切分器产不出全同块（空行并入下一块头 →
// p1="abc\n" ≠ p2="\nabc\n" 字节不等）；按权威 fixture 用字节全同的
REGISTER_TEST(md_module_md01_reload_duplicate_fences,
              "markdown_module: MD-01 reload map duplicate fences triple wins (headless)", {
    const std::string unit = "\n```\n\n```\n";
    const std::string oldDoc = unit + unit;        // 2 个全同 CodeFence 块
    const std::string newDoc = unit + unit + unit; // 前插一个完整副本 → 3 全同块
    test_support::MdHeadlessEnv env(kDirPrefix, "md01dup", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "note.md", oldDoc));

    MdBlockModel model;   // 复刻
    model.update(oldDoc);
    DOCTEST_REQUIRE(model.blocks().size() == 2);
    DOCTEST_REQUIRE(model.blocks()[0].kind == MdBlockKind::CodeFence);
    DOCTEST_REQUIRE(model.blocks()[0].hash == model.blocks()[1].hash);
    DOCTEST_REQUIRE(model.blocks()[0].stableId != model.blocks()[1].stableId);
    const MdBlock oldActive = model.blocks()[1];   // 活动块 = 第二个实例
    const size_t cursorByte = oldActive.start + 2;

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    DOCTEST_REQUIRE(enterLiveAt(e, mod.get(), screen, cursorByte));
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     oldDoc.substr(oldActive.start,
                                   oldActive.end - oldActive.start));
    DOCTEST_REQUIRE(md01RenderedSids(*e.ui, "md.live").size() == 2);

    // 复刻演化：open(oldDoc) → reload(newDoc)。全同块的前缀对齐是前端贪心的：
    // p=2 → 两个原实例继承 id 到 [0][1]，前插的副本落 [2] 拿新 id。
    const std::vector<int> wantSids =
        md01SidSeq(model, 1, {oldDoc, newDoc});
    DOCTEST_REQUIRE(md01WriteExternal(env.dir / "note.md", newDoc));
    md01WaitReload(e, mod.get(), screen, [&](eui::Ui& u) {
        return md01RenderedSids(u, "md.live") == wantSids;
    });
    frame();   // 重载后稳定帧
    const auto& nbs = model.blocks();
    DOCTEST_REQUIRE(nbs.size() == 3);
    DOCTEST_REQUIRE(nbs[1].hash == nbs[0].hash);
    DOCTEST_REQUIRE(nbs[2].hash == nbs[0].hash);
    const size_t mapped =
        mdFindBlockByContent(nbs, oldActive.kind, oldActive.hash,
                             oldActive.stableId, oldActive.start);
    DOCTEST_REQUIRE(mapped != std::string::npos);
    // 命中【原实例】（stableId == 旧活动块），不是前插副本（新 id）——三元组在
    // 三个全同块里精确区分实例
    DOCTEST_CHECK_EQ(nbs[mapped].stableId, oldActive.stableId);
    DOCTEST_CHECK_EQ(mapped, static_cast<size_t>(1));
    const std::string want =
        newDoc.substr(nbs[mapped].start, nbs[mapped].end - nbs[mapped].start);
    DOCTEST_CHECK_EQ(want, unit);
    DOCTEST_CHECK_EQ(md01RenderedSids(*e.ui, "md.live"), wantSids);
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text, want);
    frame();
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text, want);
    DOCTEST_CHECK(e.ui->isFocused("md.live.input.hit"));
    mod->onStop();
    return true;
})

// 用例 6：冲突 toast 路径（确定性高于静默轮询）
// 验证两条入口共用同一 reloadCurrentNote 映射。命中语义的关键：未保存编辑必须落
// 在【非活动块】上（编辑态打字改 alpha，进实时态后活动块 = 干净的 bravo；活动块
// 被编辑则 (kind,hash) 恒与磁盘不同 → 结构性不可命中）。冲突检测在编辑态完成
//（编辑器元素在帧树里，400ms 自动保存防抖靠每轮补打一字符重置——idle < 400ms
// 不落盘，外部版本保留），弹 toast 后再切实时态点击 action：重载在 mode == Live
// 下执行，映射路径与用例 1 完全同一条。复刻按观察到的编辑器全文镜像，重试次数
// 不影响确定性。CRLF / 时钟脉冲路径登记 deferred。
REGISTER_TEST(md_module_md01_reload_conflict_toast,
              "markdown_module: MD-01 conflict toast reload path triple hit (headless)", {
    const std::string oldDoc = "alpha\n\nbravo\n\ncharlie\n";
    const std::string newDoc = "seed line one\n\nalpha\n\nbravo\n\ncharlie\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "md01conf", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "note.md", oldDoc));

    MdBlockModel replica;   // 复刻模块演化：open(oldDoc) → 编辑态打字 → 冲突重载(newDoc)
    replica.update(oldDoc);
    DOCTEST_REQUIRE(replica.blocks().size() == 3);
    const size_t bravoStart = replica.blocks()[1].start;   // 6
    const size_t cursorByte = bravoStart + 3;

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    // 编辑态打开
    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);

    // 外写盘（前插版本）+ 未来 mtime
    DOCTEST_REQUIRE(md01WriteExternal(env.dir / "note.md", newDoc));
    // 冲突自旋（编辑态）：每轮块首打一字符（dirty + 重置 400ms 防抖）→ 帧（不落
    // 盘）→ poll（watcher credit + mtime + dirty）→ 冲突 toast；复刻按观察到的
    // 全文镜像
    bool toastSeen = false;
    for (int attempt = 0; attempt < 10 && !toastSeen; ++attempt) {
        e.ui->state<MdInputState>(kEditorInputId).cursor = 0;
        DOCTEST_REQUIRE(typeIntoEditor(*e.ui, "Z"));
        replica.update(e.ui->state<MdInputState>(kEditorInputId).text);
        frame();
        toastSeen = e.ui->find("md.toast.action.hit") != nullptr;
        if (!toastSeen) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    }
    DOCTEST_REQUIRE_MESSAGE(toastSeen, "冲突 toast 未弹出：外部写盘未被检测到");
    // 捕获侧干净 bravo 块（编辑只改 alpha，bravo 字节与 stableId 都未受波及）
    DOCTEST_REQUIRE(replica.blocks().size() == 3);
    const MdBlock cleanBravo = replica.blocks()[1];
    // 切实时态（初始锚 = 干净的 bravo 块）→ 点击「重载磁盘版本」→ resolveConflict
    // (true) → reloadCurrentNote(false) —— 重载在 Live 态执行，走与用例 1 同一条
    // 映射路径
    e.ui->state<MdInputState>(kEditorInputId).cursor = static_cast<int>(cursorByte);
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();   // 锚初始化帧（活动块 = bravo；冲突 toast 同帧可见）
    DOCTEST_REQUIRE(e.ui->find("md.live.input") != nullptr);
    const auto* action = e.ui->find("md.toast.action.hit");
    DOCTEST_REQUIRE(action != nullptr && action->onClick != nullptr);
    action->onClick();
    frame();
    replica.update(newDoc);   // 重载这一步
    const auto& nbs = replica.blocks();
    const size_t mapped =
        mdFindBlockByContent(nbs, cleanBravo.kind, cleanBravo.hash,
                             cleanBravo.stableId, cleanBravo.start);
    DOCTEST_REQUIRE(mapped != std::string::npos);
    DOCTEST_CHECK_EQ(mapped, static_cast<size_t>(2));
    const std::string want =
        newDoc.substr(nbs[mapped].start, nbs[mapped].end - nbs[mapped].start);
    DOCTEST_CHECK_EQ(want, std::string("\nbravo\n"));
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text, want);
    DOCTEST_CHECK(e.ui->isFocused("md.live.input.hit"));
    frame();   // 多帧稳定
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text, want);
    mod->onStop();
    return true;
})

// MD-01 姊妹：冲突 toast 开着时按 Ctrl+S（显式保存路径之一）。
// 语义：本地内容落盘 = 「保留本地」已经被选了 —— toast 必须立刻收掉，否则它挂着的
// 「重载磁盘版本」会把刚保存的内容再丢掉（选项与现实不符）。同时钉住冲突未决期间
// 自动链路**不落盘**（既有 flushEditorIfIdle guard 的契约），只有显式保存才落盘。
REGISTER_TEST(md_module_conflict_ctrl_s_resolves_keep_local,
              "markdown_module: 冲突 toast 开着时 Ctrl+S → 本地落盘 + toast 收起（= 保留本地）", {
    const std::string oldDoc = "alpha\n\nbravo\n\ncharlie\n";
    const std::string newDoc = "seed line one\n\nalpha\n\nbravo\n\ncharlie\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "mdctrls", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "note.md", oldDoc));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdctrls");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);

    // 外写盘（前置版本 + 未来 mtime）→ 编辑自旋到冲突 toast（与 MD-01 同一条路径）
    DOCTEST_REQUIRE(md01WriteExternal(env.dir / "note.md", newDoc));
    bool toastSeen = false;
    for (int attempt = 0; attempt < 10 && !toastSeen; ++attempt) {
        e.ui->state<MdInputState>(kEditorInputId).cursor = 0;
        DOCTEST_REQUIRE(typeIntoEditor(*e.ui, "Z"));
        frame();
        toastSeen = e.ui->find("md.toast.action.hit") != nullptr;
        if (!toastSeen) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    }
    DOCTEST_REQUIRE_MESSAGE(toastSeen, "冲突 toast 未弹出：外部写盘未被检测到");
    const std::string localText = e.ui->state<MdInputState>(kEditorInputId).text;
    DOCTEST_REQUIRE(localText != newDoc);   // 本地确有未保存改动（Z 编辑）

    // 冲突未决期间自动链路不落盘：磁盘仍是外部版本（既有 guard 的契约）
    std::string onDisk;
    DOCTEST_REQUIRE(readFileBytes(env.dir / "note.md", &onDisk));
    DOCTEST_CHECK_MESSAGE(onDisk == newDoc,
                          "冲突未决时自动保存仍写了盘（conflictOpen_ guard 失效）");

    // Ctrl+S：显式保存 → 本地版本落盘
    DOCTEST_REQUIRE(mod->onKeyEvent(test_support::pressShortcut(eui::InputKey::S)));
    frame();
    DOCTEST_REQUIRE(readFileBytes(env.dir / "note.md", &onDisk));
    DOCTEST_CHECK_EQ(onDisk, localText);
    DOCTEST_CHECK_MESSAGE(
        e.ui->find("md.toast.action.hit") == nullptr,
        "Ctrl+S 已按本地落盘，冲突 toast 仍开着（其「重载磁盘版本」会把刚存的内容丢掉）");

    mod->onStop();
    return true;
})

// F3 登记项：库导入（GitHub Sync DROP 重建库后）harvest 的「当前笔记重读 / 冲突」
// 分支零覆盖 —— 既有导入用例都没打开笔记（`checkCurrentNoteOnDisk` 在 currentPath_
// 空时早退，分支从未执行）。补三面（走导入触发，与 5 秒轮询同口径）：
//   ① 导入但该笔记磁盘没变 → 不重载、不弹冲突（设计取舍：无条件重载会弹假冲突，
//      用户在假冲突里点「重载磁盘版本」还会用未变内容盖掉未保存编辑）；
//   ② 磁盘变了 + 本地干净 → 静默重载（编辑器换成磁盘版本）；
//   ③ 磁盘又变 + 本地脏 → 冲突 toast（action =「重载磁盘版本」）。
REGISTER_TEST(md_module_db_import_harvests_current_note,
              "markdown_module: kDbImported 收割当前笔记（未变不弹假冲突 / 干净静默重载 / 脏弹冲突）", {
    const std::string doc0 = "alpha\n\nbravo\n\ncharlie\n";
    const std::string doc1 = "one\n\nalpha\n\nbravo\n\ncharlie\n";
    const std::string doc2 = "two\n\none\n\nalpha\n\nbravo\n\ncharlie\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "mdimportharv", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "note.md", doc0));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdimportharv");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>(kEditorInputId).text, doc0);

    // ① 导入广播但磁盘没变 → 文本原样、不弹冲突、**也不发生重载**
    //（重载 toast 是「发生了重载」的可见证据；无条件重载读的虽是未变内容，文本反而
    // 相同，只断文本抓不到它）
    ::core::event::bus().emit(::core::event::kDbImported);
    frame();
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>(kEditorInputId).text, doc0);
    DOCTEST_CHECK_MESSAGE(e.ui->find("md.toast.action.hit") == nullptr,
                          "磁盘没变却弹了冲突（假冲突）");
    const auto* toastTitle1 = e.ui->find("md.toast.title");
    DOCTEST_CHECK_MESSAGE(toastTitle1 == nullptr ||
                              toastTitle1->text.find("自动重载") == std::string::npos,
                          "磁盘没变却发生了重载（假重载）");

    // ② 磁盘变了 + 本地干净 → 静默重载（以「已自动重载」toast 为证）
    DOCTEST_REQUIRE(md01WriteExternal(env.dir / "note.md", doc1));
    ::core::event::bus().emit(::core::event::kDbImported);
    frame();
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>(kEditorInputId).text, doc1);
    DOCTEST_CHECK(e.ui->find("md.toast.action.hit") == nullptr);
    const auto* toastTitle2 = e.ui->find("md.toast.title");
    DOCTEST_REQUIRE(toastTitle2 != nullptr);
    DOCTEST_CHECK_MESSAGE(toastTitle2->text.find("自动重载") != std::string::npos,
                          "磁盘变了 + 本地干净，未走静默重载（缺重载 toast），实测: " +
                              toastTitle2->text);

    // ③ 磁盘又变 + 本地脏（键入 1 字符，未过 400ms 自动保存防抖）→ 冲突 toast
    DOCTEST_REQUIRE(typeIntoEditor(*e.ui, "Q"));
    DOCTEST_REQUIRE(md01WriteExternal(env.dir / "note.md", doc2));
    ::core::event::bus().emit(::core::event::kDbImported);
    frame();
    DOCTEST_CHECK_MESSAGE(e.ui->find("md.toast.action.hit") != nullptr,
                          "本地脏 + 磁盘变，导入触发未弹冲突 toast");

    mod->onStop();
    return true;
})

// 用例 7：捕获越界竞态（冲突路径）
// 构造「输入 onChange 当帧收缩 blocks + 冲突重载」：活动块 = 块 2，选中其文本首
// 字节（块间空行）删除 → 3 块合并成 2 块，liveActiveBlock_ 保持陈旧值 2（越界态）
// → 冲突 toast 重载 → 步骤 1 在已收缩模型上重 derive（blockIndexAt 对非空恒
// in-range）→ 不越界读、不崩、零命中钳位。冲突自旋每轮在活动块尾（= 文档
// 末块）追加一字符重置防抖 → 文档恒为当前文本 + Z，复刻可精确镜像。
REGISTER_TEST(md_module_md01_reload_capture_race,
              "markdown_module: MD-01 capture race after blocks shrink (headless)", {
    const std::string oldDoc = "alpha\n\nbravo\n\ncharlie\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "md01race", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "note.md", oldDoc));

    MdBlockModel model;
    model.update(oldDoc);
    DOCTEST_REQUIRE(model.blocks().size() == 3);
    const size_t cursorByte = model.blocks()[2].start + 2;   // 活动块 = 块 2

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    DOCTEST_REQUIRE(enterLiveAt(e, mod.get(), screen, cursorByte));
    DOCTEST_REQUIRE(e.ui->find("md.live.input") != nullptr);

    // 块 2 文本首字节（"\ncharlie\n" 的前导空行）选区删除：3 块 → 2 块
    //（bravo 与 charlie 合并），liveActiveBlock_ 仍为陈旧值 2（== 新 size → 越界）
    MdInputState& st = e.ui->state<MdInputState>("md.live.input");
    st.cursor = 0;
    st.selectionStart = 0;
    st.selectionEnd = 1;
    DOCTEST_REQUIRE(pressLiveKey(*e.ui, eui::InputKey::Backspace));   // 内置选区删除 → onChange
    frame();   // 锚按新模型重派（合并后块 1）；输入框文本同步为 "\nbravo\ncharlie\n"
    const std::string mergedText = "\nbravo\ncharlie\n";
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text, mergedText);

    // 复刻演化（模块同序）：open(oldDoc) → 选区删块首（3→2 合并）→ 活动块尾逐次
    // 追加 Z（冲突自旋）→ 冲突重载(oldDoc)（捕获发生在已收缩模型上 = 竞态点）
    MdBlockModel replica;
    replica.update(oldDoc);
    replica.update("alpha\n\nbravo\ncharlie\n");
    // 外写回原 3 块版本 + 未来 mtime
    DOCTEST_REQUIRE(md01WriteExternal(env.dir / "note.md", oldDoc));
    // 冲突自旋：活动块尾追打 Z（重置防抖；活动块 = 文档末块 → 文档恒为当前文本
    // + Z，复刻精确镜像）
    int typedZ = 0;
    bool toastSeen = false;
    for (int attempt = 0; attempt < 10 && !toastSeen; ++attempt) {
        MdInputState& st2 = e.ui->state<MdInputState>("md.live.input");
        st2.cursor = static_cast<int>(st2.text.size());
        DOCTEST_REQUIRE(typeIntoLiveInput(*e.ui, "Z"));
        ++typedZ;
        replica.update(replica.text() + "Z");
        frame();
        toastSeen = e.ui->find("md.toast.action.hit") != nullptr;
        if (!toastSeen) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    }
    DOCTEST_REQUIRE_MESSAGE(toastSeen, "冲突 toast 未弹出：外部写盘未被检测到");
    // 点击「重载磁盘版本」→ 步骤 1 在已收缩模型上重 derive（恒 in-range）
    const auto* action = e.ui->find("md.toast.action.hit");
    DOCTEST_REQUIRE(action != nullptr && action->onClick != nullptr);
    // 点击时锚字节 = liveInputStart_(6) + 输入框光标(mergedText 长 + typedZ)
    const size_t anchorByte = 6 + mergedText.size() + static_cast<size_t>(typedZ);
    const size_t capIdx = replica.blockIndexAt(anchorByte);
    DOCTEST_REQUIRE(capIdx < replica.blocks().size());
    const MdBlock captured = replica.blocks()[capIdx];
    action->onClick();
    frame();
    replica.update(oldDoc);   // 重载这一步
    DOCTEST_REQUIRE(replica.blocks().size() == 3);
    // 捕获块（含追加字符，内容新文档不存在）→ 零命中 → 钳位到旧光标字节对应新块
    DOCTEST_CHECK_EQ(
        mdFindBlockByContent(replica.blocks(), captured.kind, captured.hash,
                             captured.stableId, captured.start),
        std::string::npos);
    const size_t clamped = std::min(anchorByte, oldDoc.size());
    const size_t kExp = replica.blockIndexAt(clamped);
    const std::string want =
        oldDoc.substr(replica.blocks()[kExp].start,
                      replica.blocks()[kExp].end - replica.blocks()[kExp].start);
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text, want);
    frame();
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text, want);
    mod->onStop();
    return true;
})

// 用例 8：非实时态回归
REGISTER_TEST(md_module_md01_reload_non_live_modes,
              "markdown_module: MD-01 reload untouched outside live mode (headless)", {
    const std::string oldDoc = "alpha\n\nbravo\n\ncharlie\n";
    const std::string newDoc = "seed line one\n\nalpha\n\nbravo\n\ncharlie\n";
    const std::string thirdDoc = "first\n\nsecond\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "md01nlm", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "note.md", oldDoc));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    // 编辑态：外写盘 → 重载 → 编辑器文本被磁盘版本替换，映射不介入（mode != Live）
    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);
    DOCTEST_REQUIRE(md01WriteExternal(env.dir / "note.md", newDoc));
    md01WaitReload(e, mod.get(), screen, [&](eui::Ui& u) {
        return u.state<MdInputState>(kEditorInputId).text == newDoc;
    });
    frame();   // 重载后稳定帧
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>(kEditorInputId).text, newDoc);
    DOCTEST_CHECK(e.ui->find("md.live.input") == nullptr);

    // 切三档到预览：模式回归（映射不介入；预览列在）
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → Live
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → Split
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → Preview
    frame();
    DOCTEST_REQUIRE(e.ui->find("md.preview.scroll") != nullptr);
    // 预览态外写盘 → 重载 → 预览块集合变化（重载正信号），不崩
    const std::vector<int> preSids = md01RenderedSids(*e.ui, "md.preview");
    DOCTEST_REQUIRE(preSids.size() == 4);
    DOCTEST_REQUIRE(md01WriteExternal(env.dir / "note.md", thirdDoc));
    md01WaitReload(e, mod.get(), screen, [&](eui::Ui& u) {
        const auto sids = md01RenderedSids(u, "md.preview");
        return !sids.empty() && sids != preSids;
    });
    frame();
    DOCTEST_CHECK(e.ui->find("md.preview.scroll") != nullptr);
    DOCTEST_CHECK(e.ui->find("md.live.input") == nullptr);
    // 切回编辑态：编辑器文本 = 最新磁盘版本（重载在非实时态同样正常换内容）
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → Edit
    frame();
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>(kEditorInputId).text, thirdDoc);
    mod->onStop();
    return true;
})
// 联动收口

// ① 大纲跳转走 setLiveCursor。jumpToHeading 若在实时态写 kEditorInputId，
// 该输入不在帧树 = 死写。
REGISTER_TEST(md_module_live_outline_jump,
              "markdown_module: 大纲跳转走 setLiveCursor（实时态换活动块+聚焦，headless）", {
    const std::string text = "# 标题A\n\n## 标题B\n\n正文段落\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "liveoutline", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "out.md", text));

    module::markdown::MdBlockModel model;
    model.update(text);
    DOCTEST_REQUIRE(model.blocks().size() >= 3);
    const size_t headingByte = text.find("## 标题B");
    DOCTEST_REQUIRE(headingByte != std::string::npos);
    const std::string headingSrc =
        text.substr(model.blocks()[1].start,
                    model.blocks()[1].end - model.blocks()[1].start);

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&] { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();
    DOCTEST_REQUIRE(e.ui->find("md.live.scroll") != nullptr);

    // 打开大纲面板；实时态点击「## 标题B」
    const auto* toggle = e.ui->find("md.outline.toggle.hit");
    DOCTEST_REQUIRE(toggle != nullptr);
    DOCTEST_REQUIRE(toggle->onClick != nullptr);
    toggle->onClick();
    frame();
    const auto* h2 = e.ui->find("md.outline.item.1.row");
    DOCTEST_REQUIRE(h2 != nullptr);
    DOCTEST_REQUIRE(h2->onClick != nullptr);
    h2->onClick();
    frame();

    // ① 活动块换到标题块：input 内容 = 该块源码（活动块不动即红）
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     headingSrc);
    // ② 光标落在标题文字行首：块局部 = 大纲字节偏移 - 块起点（块模型把前导
    //    空行 \n 归标题块，故 = 1 而非 0）
    DOCTEST_CHECK_EQ(
        static_cast<size_t>(
            e.ui->state<MdInputState>("md.live.input").cursor),
        headingByte - model.blocks()[1].start);
    // ③ 聚焦补发（setLiveCursor → liveFocusPending_ 同帧消费）
    DOCTEST_CHECK(e.ui->isFocused("md.live.input.hit"));
    // ④ 编辑器输入未被写：实时态光标权威 = liveCursorByte_（点击写了即红）
    //    kEditorInputId → 本行变 11）
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>(kEditorInputId).cursor, 0);
    // ⑤ 大纲高亮随跳转换节：item.0 失焦、item.1 加粗
    DOCTEST_CHECK_EQ(e.ui->find("md.outline.item.0.label")->fontWeight, 400);
    DOCTEST_CHECK_EQ(e.ui->find("md.outline.item.1.label")->fontWeight, 700);

    mod->onStop();
    return true;
})

// ② 实时态状态栏 (行:列:字) 的键仍是 (editorTextRev_, cursorByte)。读
// kEditorInputId 旧状态（不在实时态帧树）会让行/列/字数
REGISTER_TEST(md_module_live_status_bar,
              "markdown_module: 状态栏行:列:字（实时态光标口径 liveCursorByte_，headless）", {
    const std::string text = "alpha\nbeta\ngamma\n";   // 17 字节，单段块
    test_support::MdHeadlessEnv env(kDirPrefix, "livestatus", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "s.md", text));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&] { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };
    auto statusPos = [&](void) -> std::string {
        const auto* n = e.ui->find("md.status.pos");
        return n == nullptr ? std::string("<missing>") : n->text;
    };
    auto statusSave = [&](void) -> std::string {
        const auto* n = e.ui->find("md.status.save");
        return n == nullptr ? std::string("<missing>") : n->text;
    };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();
    DOCTEST_REQUIRE(e.ui->find("md.live.scroll") != nullptr);

    // ① 实时态光标（块内 == 文档内，单块文档）：移到 "gamma" 行首（字节 11）
    e.ui->state<MdInputState>("md.live.input").cursor = 11;
    frame();
    DOCTEST_CHECK_EQ(statusPos(), std::string("3:1 · 17 字 · 1 篇笔记"));

    // ② 实时态打字（onTextInput → 上行拼文档）：行列字数随整篇口径更新
    const auto* hit = e.ui->find("md.live.input.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_REQUIRE(hit->onTextInput != nullptr);
    core::TextInputEvent ev;
    ev.text = "X";
    hit->onTextInput(ev);
    frame();
    DOCTEST_CHECK_EQ(statusPos(), std::string("3:2 · 18 字 · 1 篇笔记"));
    // ③ 未落盘：保存状态 ●（实时态与编辑态同一脏标记链）
    DOCTEST_CHECK(statusSave().find("未保存") != std::string::npos);

    mod->onStop();
    return true;
})

// ③ 实时态标签栏接续（标签栏四态共享，零改动）。验证共享列壳的
// chip 行在实时态可添加标签、左栏筛选可用、实时列本身不受扰动。
REGISTER_TEST(md_module_live_tag_bar,
              "markdown_module: 标签栏实时态接续（chip 行/添加/筛选，headless）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "livetag", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "note.md", "# 标题\n"));
    DOCTEST_REQUIRE(writeTextFile(env.dir / "other.md", "# 别篇\n"));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&] { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };
    auto findFileRow = [&](const std::string& name) {
        for (int i = 0; i < 6; ++i) {
            const auto* nm = e.ui->find("md.tree.node." + std::to_string(i) + ".name");
            if (nm != nullptr && nm->text == name) {
                return static_cast<const core::dsl::Element*>(
                    e.ui->find("md.tree.node." + std::to_string(i) + ".row"));
            }
        }
        return static_cast<const core::dsl::Element*>(nullptr);
    };
    auto treeHasNote = [&](const std::string& name) {
        for (int i = 0; i < 6; ++i) {
            const auto* nm = e.ui->find("md.tree.node." + std::to_string(i) + ".name");
            if (nm != nullptr && nm->text == name) return true;
        }
        return false;
    };

    frame();
    const auto* fileRow = findFileRow("note.md");
    DOCTEST_REQUIRE(fileRow != nullptr);
    DOCTEST_REQUIRE(fileRow->onClick != nullptr);
    fileRow->onClick();
    frame();
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();
    DOCTEST_REQUIRE(e.ui->find("md.live.scroll") != nullptr);

    // ① 实时态正文列顶的标签 chip 行仍在（共享列壳）：添加输入 + ＋按钮
    const auto* addInput = e.ui->find("md.tags.add.hit");
    DOCTEST_REQUIRE(addInput != nullptr);
    DOCTEST_REQUIRE(addInput->onTextInput != nullptr);
    core::TextInputEvent tie;
    tie.text = "工作";
    addInput->onTextInput(tie);
    frame();
    const auto* addBtn = e.ui->find("md.tags.addbtn.bg");
    DOCTEST_REQUIRE(addBtn != nullptr);
    DOCTEST_REQUIRE(addBtn->onClick != nullptr);
    addBtn->onClick();
    frame();

    // ② chip 上屏 + 左栏筛选区同名 chip（与编辑态同一套逻辑）
    const auto* chipName = e.ui->find("md.tags.chip.0.name");
    DOCTEST_REQUIRE(chipName != nullptr);
    DOCTEST_CHECK_EQ(chipName->text, std::string("工作"));
    const auto* filterName = e.ui->find("md.tags.filter.0.name");
    DOCTEST_REQUIRE(filterName != nullptr);
    DOCTEST_CHECK(filterName->text.find("工作") != std::string::npos);

    // ③ 筛选：other.md 隐藏、note.md 保留；实时列不受扰动（仍是活动块源码）
    const auto* filterBg = e.ui->find("md.tags.filter.0.bg");
    DOCTEST_REQUIRE(filterBg != nullptr);
    DOCTEST_REQUIRE(filterBg->onClick != nullptr);
    filterBg->onClick();
    frame();
    DOCTEST_CHECK(!treeHasNote("other.md"));
    DOCTEST_CHECK(treeHasNote("note.md"));
    DOCTEST_CHECK(e.ui->find("md.live.scroll") != nullptr);
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     std::string("# 标题\n"));

    // ④ 再点一次取消筛选 → other.md 回来
    e.ui->find("md.tags.filter.0.bg")->onClick();
    frame();
    DOCTEST_CHECK(treeHasNote("other.md"));

    mod->onStop();
    return true;
})

// ④ Ctrl+S 实时态保存。四态共用模块 onKeyEvent → saveEditor
// 幂等链；Ctrl+S 在实时态应立即落盘（不等 400ms 防抖）+ 状态栏 ✓。
REGISTER_TEST(md_module_live_ctrl_s_save,
              "markdown_module: Ctrl+S 实时态保存（立即落盘+状态栏✓，headless）", {
    const std::string v1 = "hello\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "livesave", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "s.md", v1));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&] { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };
    auto statusSave = [&](void) -> std::string {
        const auto* n = e.ui->find("md.status.save");
        return n == nullptr ? std::string("<missing>") : n->text;
    };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();

    // 实时态打字（光标置文末）→ 内存已新、磁盘仍是旧版（400ms 防抖未到）
    e.ui->state<MdInputState>("md.live.input").cursor = 6;
    const auto* hit = e.ui->find("md.live.input.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_REQUIRE(hit->onTextInput != nullptr);
    core::TextInputEvent ev;
    ev.text = "world";
    hit->onTextInput(ev);
    frame();
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     std::string("hello\nworld"));
    std::string disk;
    DOCTEST_REQUIRE(readFileBytes(env.dir / "s.md", &disk));
    DOCTEST_CHECK_EQ(disk, v1);   // 未落盘
    DOCTEST_CHECK(statusSave().find("未保存") != std::string::npos);

    // Ctrl+S → 立即落盘 + 状态栏 ✓ 已保存
    eui::KeyEvent cs;
    cs.key = eui::InputKey::S;
    test_support::setShortcutModifier(cs.modifiers);
    DOCTEST_CHECK(mod->onKeyEvent(cs));
    DOCTEST_REQUIRE(readFileBytes(env.dir / "s.md", &disk));
    DOCTEST_CHECK_EQ(disk, std::string("hello\nworld"));
    frame();
    DOCTEST_CHECK(statusSave().find("已保存") != std::string::npos);

    // Ctrl+Shift+S 不消费（不是 Ctrl+S）
    eui::KeyEvent css;
    css.key = eui::InputKey::S;
    test_support::setShortcutModifier(css.modifiers);
    css.modifiers.shift = true;
    DOCTEST_CHECK_FALSE(mod->onKeyEvent(css));

    // 已落盘后再 Ctrl+S：幂等（仍消费，磁盘不变）
    DOCTEST_CHECK(mod->onKeyEvent(cs));
    DOCTEST_REQUIRE(readFileBytes(env.dir / "s.md", &disk));
    DOCTEST_CHECK_EQ(disk, std::string("hello\nworld"));

    mod->onStop();
    return true;
})

// ⑤ 实时态搜索筛选。左栏搜索框四态共享：FTS 过滤树照常、
// 当前实时列不受扰动；点开命中笔记后仍驻留实时态。
REGISTER_TEST(md_module_live_search_filter,
              "markdown_module: 搜索筛选实时态（FTS 过滤树+打开命中仍驻留实时，headless）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "livesearch", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "a.md", "apple title\n"));
    DOCTEST_REQUIRE(writeTextFile(env.dir / "b.md", "banana note\n"));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&] { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };
    auto findFileRow = [&](const std::string& name) {
        // 行 id 两族都要扫：树模式 = md.tree.node.N；搜索命中列表 = md.tree.item.N
        for (int i = 0; i < 8; ++i) {
            const auto* nm = e.ui->find("md.tree.node." + std::to_string(i) + ".name");
            if (nm == nullptr) {
                nm = e.ui->find("md.tree.item." + std::to_string(i) + ".name");
            }
            if (nm != nullptr && nm->text == name) {
                const auto* row =
                    e.ui->find("md.tree.node." + std::to_string(i) + ".row");
                if (row == nullptr) {
                    row = e.ui->find("md.tree.item." + std::to_string(i) + ".row");
                }
                return static_cast<const core::dsl::Element*>(row);
            }
        }
        return static_cast<const core::dsl::Element*>(nullptr);
    };
    auto treeHasNote = [&](const std::string& name) {
        for (int i = 0; i < 8; ++i) {
            const auto* nm = e.ui->find("md.tree.node." + std::to_string(i) + ".name");
            if (nm == nullptr) {
                nm = e.ui->find("md.tree.item." + std::to_string(i) + ".name");
            }
            if (nm != nullptr && nm->text == name) return true;
        }
        return false;
    };

    frame();
    const auto* aRow = findFileRow("a.md");
    DOCTEST_REQUIRE(aRow != nullptr);
    DOCTEST_REQUIRE(aRow->onClick != nullptr);
    aRow->onClick();
    frame();
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     std::string("apple title\n"));

    // 搜索 "banana"：树只留命中，当前实时列不动
    const auto* searchHit = e.ui->find("md.search.hit");
    DOCTEST_REQUIRE(searchHit != nullptr);
    DOCTEST_REQUIRE(searchHit->onTextInput != nullptr);
    core::TextInputEvent sev;
    sev.text = "banana";
    searchHit->onTextInput(sev);
    frame();
    DOCTEST_CHECK(treeHasNote("b.md"));
    DOCTEST_CHECK(!treeHasNote("a.md"));
    DOCTEST_CHECK(e.ui->find("md.live.scroll") != nullptr);
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     std::string("apple title\n"));

    // 点开命中笔记：仍在实时态，活动块 = 新笔记首块
    const auto* hitRow = findFileRow("b.md");
    DOCTEST_REQUIRE(hitRow != nullptr);
    DOCTEST_REQUIRE(hitRow->onClick != nullptr);
    hitRow->onClick();
    frame();
    DOCTEST_CHECK(e.ui->find("md.live.scroll") != nullptr);
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     std::string("banana note\n"));
    DOCTEST_CHECK(e.ui->find("md.status.save")->text.find("b.md") !=
                  std::string::npos);

    mod->onStop();
    return true;
})

// ⑥ 跨模块跳转驻留实时态（「其它模块跳到 markdown 实时态定位」）。
// Router 负载 open:<path> → openNote → 实时态状态群换代，活动块落目标笔记首块。
REGISTER_TEST(md_module_live_cross_module_nav,
              "markdown_module: 跨模块跳转驻留实时态（open: 负载→活动块换目标笔记，headless）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "livenav", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "a.md", "A one\n\nA two\n"));
    DOCTEST_REQUIRE(writeTextFile(env.dir / "b.md", "B one\n\nB two\n"));

    ::core::Router router;
    test_support::ScopedAppRouter routerGuard(&router);

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    router.registerModule(mod.get());   // 跳转目标页必须先注册（否则回退 Home）
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&] { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };
    auto findFileRow = [&](const std::string& name) {
        for (int i = 0; i < 8; ++i) {
            const auto* nm = e.ui->find("md.tree.node." + std::to_string(i) + ".name");
            if (nm != nullptr && nm->text == name) {
                return static_cast<const core::dsl::Element*>(
                    e.ui->find("md.tree.node." + std::to_string(i) + ".row"));
            }
        }
        return static_cast<const core::dsl::Element*>(nullptr);
    };

    // 打开 a.md → 实时态（首块活动）
    frame();
    const auto* aRow = findFileRow("a.md");
    DOCTEST_REQUIRE(aRow != nullptr);
    DOCTEST_REQUIRE(aRow->onClick != nullptr);
    aRow->onClick();
    frame();
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     std::string("A one\n"));
    DOCTEST_CHECK(e.ui->find("md.status.save")->text.find("a.md") !=
                  std::string::npos);

    // 跨模块负载 open:b.md：仍是实时态，活动块换到 b.md 首块并聚焦
    router.navigateTo(::core::PageId::Markdown, "open:b.md");
    frame();
    DOCTEST_CHECK(e.ui->find("md.live.scroll") != nullptr);
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     std::string("B one\n"));
    DOCTEST_CHECK(e.ui->find("md.status.save")->text.find("b.md") !=
                  std::string::npos);
    DOCTEST_CHECK(e.ui->isFocused("md.live.input.hit"));

    // 负载一次性：再走一帧不重复触发（内容不变）
    frame();
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     std::string("B one\n"));

    mod->onStop();
    return true;
})

// 性能实测与调优
// 封顶行为（headless）：合成 5000 行围栏 → 活动块 input 高度 = 0.7 × 视口

// 封顶行为（headless）：合成 5000 行围栏 → 活动块 input 高度 = 0.7 × 视口
//（块内滚动接管 = hit 挂 onScroll，over 探针 1 为预期）；切到小块 → 封顶解除。
REGISTER_TEST(md_module_live_block_cap_5000_fence,
              "markdown_module: 实时态 5000 行围栏活动块高度封顶 0.7×视口 + 块内滚动（headless）", {
    std::string fence = "```cpp\n";
    for (int i = 0; i < 5000; ++i) fence += "line " + std::to_string(i) + "\n";
    fence += "```\n";
    const std::string text = "# T\n\n" + fence;   // 块 0 = 标题，块 1 = 围栏
    test_support::MdHeadlessEnv env(kDirPrefix, "livecap", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "cap.md", text));

    module::markdown::MdBlockModel model;
    model.update(text);
    DOCTEST_REQUIRE(model.blocks().size() >= 2);
    const size_t fenceByte = model.blocks()[1].start;   // 围栏块首

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&] { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);
    e.ui->state<MdInputState>(kEditorInputId).cursor =
        static_cast<int>(fenceByte);
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();
    DOCTEST_REQUIRE(e.ui->find("md.live.input") != nullptr);

    // ① 封顶生效：input 高 = 0.7 × 视口高（滚动容器高 g.h），而非 5000 行贴合高
    const auto* scroll = e.ui->find("md.live.scroll");
    DOCTEST_REQUIRE(scroll != nullptr);
    const float viewportH = scroll->frame.height;
    const float capExpect = module::markdown::kMdLiveBlockCapRatio * viewportH;
    const auto* input = e.ui->find("md.live.input");
    DOCTEST_REQUIRE(input != nullptr);
    DOCTEST_CHECK(std::fabs(input->frame.height - capExpect) <= 1.0f);
    DOCTEST_CHECK(input->frame.height < 1000.0f);   // 没被撑到 9 万 px

    // ② 块内滚动接管：hit 挂 onScroll（「该块内滚轮行为符合预期」的
    // headless 形态；模块侧 over 探针 =1 且因封顶不升 WARN）
    const auto* hit = e.ui->find("md.live.input.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_CHECK(hit->onScroll != nullptr);

    // ③ 封顶不阻断编辑：打字上行仍一致（活动块仍可输入）。块身份随光标走
    //（MD-01）：X 落在 fence 块首（块首含合并进块头的空行）→ 分块器重切，
    // 活动块跟光标落到新段落块——与既有 roundtrip 口径一致：按「期望文档 +
    // 光标字节」重切分块，input 文本 == 新活动块源码逐字节（等价性锚）
    const size_t typeByte = fenceByte;   // 光标 = 围栏块首（块内 offset 0）
    DOCTEST_REQUIRE(typeIntoLiveInput(*e.ui, "X"));
    frame();
    const std::string got =
        e.ui->state<MdInputState>("md.live.input").text;
    std::string expectDoc = text;
    expectDoc.insert(typeByte, "X");
    module::markdown::MdBlockModel m2;
    m2.update(expectDoc);
    const size_t k2 = m2.blockIndexAt(typeByte + 1);
    DOCTEST_REQUIRE(k2 < m2.blocks().size());
    const std::string want = expectDoc.substr(
        m2.blocks()[k2].start, m2.blocks()[k2].end - m2.blocks()[k2].start);
    DOCTEST_CHECK_MESSAGE(got == want,
                          "封顶块打字后 input 文本 [" + got + "] 期望活动块源 [" +
                              want + "]");

    // ④ 切到小块（标题）→ 封顶解除：input 高 = 内容贴合高（远小于 0.7×视口）
    const auto* wrap0 = e.ui->find("md.live.blk.0");
    DOCTEST_REQUIRE(wrap0 != nullptr);
    DOCTEST_REQUIRE(clickLiveBlock(*e.ui, 0, 0.5f * wrap0->frame.height));
    frame();
    const auto* input2 = e.ui->find("md.live.input");
    DOCTEST_REQUIRE(input2 != nullptr);
    DOCTEST_CHECK_EQ(e.ui->state<MdInputState>("md.live.input").text,
                     std::string("# T\n"));
    DOCTEST_CHECK(input2->frame.height < capExpect - 1.0f);
    const auto* hit2 = e.ui->find("md.live.input.hit");
    DOCTEST_REQUIRE(hit2 != nullptr);
    DOCTEST_CHECK(hit2->onScroll == nullptr);   // 贴合块无内部滚动（over=0）

    mod->onStop();
    return true;
})

// 点击可见块不重置滚动（回归守卫）：有滚动偏移时点击视口内任意块 → 滚动位置
// 保持。根因修复 = contentKey 去除活动块 stableId（rv：点击换 contentKey →
// scrollView 视为全新内容 → 归零）。
REGISTER_TEST(md_module_live_scroll_click_keeps_offset,
              "markdown_module: 实时态点击视口内块不重置滚动（headless，回归守卫）", {
    const std::string doc = tallNoteDoc("S", 40);
    test_support::MdHeadlessEnv env(kDirPrefix, "liveclickscroll", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "clickscroll.md", doc));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&](void) { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(e.ui->find(kEditorInputId) != nullptr);
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();

    const auto* sv0 = e.ui->find(kLiveScrollId0);
    DOCTEST_REQUIRE(sv0 != nullptr);
    DOCTEST_REQUIRE_MESSAGE(sv0->scrollMaxOffset > 300.0f,
                            "文档高度须 > 视口（实测 maxOffset=" +
                            std::to_string(sv0->scrollMaxOffset) + ")");
    DOCTEST_CHECK_EQ(sv0->scrollOffset, 0.0f);   // 进态归零

    // ① 模拟用户向下滚动：走 Ui::setScrollOffset（直写 scrollState + 触发 onChange
    //    回调 → 同步 liveScrollY_），不走 onChange 直接调用（只更新 liveScrollY_ 不动
    //    instance.offset，会导致 element.scrollOffset ≠ instance.offset 假绿）。
    e.ui->setScrollOffset(kLiveScrollId0, 200.0f);
    frame();  // 本帧帧末消费挂起偏移（Runtime::compose 尾部）
    frame();  // 元素字段下一帧才反映实例值
    const auto* svScrolled = e.ui->find(kLiveScrollId0);
    DOCTEST_REQUIRE(svScrolled != nullptr);
    DOCTEST_REQUIRE_MESSAGE(std::fabs(svScrolled->scrollOffset - 200.0f) <= 2.0f,
                            "滚动后偏移=" + std::to_string(svScrolled->scrollOffset));

    // ② 点视口内的块（块内容 y 在 [200, 200+viewH] 区间，caret 不越界）
    const std::vector<int> sids = liveBlockSidsByY(*e.ui);
    DOCTEST_REQUIRE(sids.size() >= 15);
    // 取 ~第 8 个块，content_y 约在 ~320px——滚动 200 后正好在视口上半部
    const size_t clickOrdinal = std::min<size_t>(8, sids.size() - 3);
    const int clickSid = sids[clickOrdinal];
    const auto* wrap = e.ui->find("md.live.blk." + std::to_string(clickSid));
    DOCTEST_REQUIRE(wrap != nullptr);
    const float contentOrigin = svScrolled->frame.y;
    const float clickContentY = wrap->frame.y - contentOrigin;
    DOCTEST_REQUIRE_MESSAGE(
        clickContentY > svScrolled->scrollOffset + 10.0f &&
        clickContentY < svScrolled->scrollOffset + svScrolled->frame.height - 10.0f,
        "点击块须在视口内：content_y=" + std::to_string(clickContentY) +
        " scroll=" + std::to_string(svScrolled->scrollOffset) +
        " viewH=" + std::to_string(svScrolled->frame.height));
    DOCTEST_REQUIRE(clickLiveBlock(*e.ui, clickSid, wrap->frame.height * 0.5f));
    frame();   // 定位帧：cursor pending，不评滚动
    frame();   // 跟随评估帧：caret 在安全带内 → 不拉
    frame();   // +1 帧：声明值稳定

    // ③ 断案：滚动位置保持不变（未被点击归零）
    const auto* svPost = e.ui->find(kLiveScrollId0);
    DOCTEST_REQUIRE(svPost != nullptr);
    DOCTEST_REQUIRE_MESSAGE(
        std::fabs(svPost->scrollOffset - 200.0f) <= 4.0f,
        "点击后滚动未被重置。期望≈200 实测=" +
            std::to_string(svPost->scrollOffset));

    mod->onStop();
    return true;
})

// 记录式验收（无墙钟红线，人工/脚本扫日志复核）：200KB /
// 列整帧 compose 耗时（headless 合成，取中等规模文档作缩放基——全量 200KB/5000
// 块在 Debug 无优化 headless 下单帧 ~10s，规模记录留给真机 GUI 的 `[live] frame
// compose ms` 日志，两处口径同源）。结果打到 stdout 供脚本扫 `[live] … ms=`
// 复核；不设硬断言（避开已知脆弱点）。
REGISTER_TEST(md_module_live_perf_records_200kb,
              "markdown_module: 记录式性能日志（200KB 分段 + 成帧缩放基 ms，无硬断言）", {
    std::string big;
    big.reserve(2u * 1024u * 1024u);
    for (int i = 0; i < 5000; ++i) {
        big += "段落 " + std::to_string(i) +
               " 的正文内容填充填充填充填充填充填充文本\n\n";
    }
    //（约 200KB / 5000 行：每段 ~40 字节 + 空行）

    // (a) 分块重算耗时（≤5ms 目标；每轮换一处文本制造真实脏区间，
    module::markdown::MdBlockModel model;
    double bestSegMs = 1e9;
    uint64_t blockCount = 0;
    std::string doc = big;
    for (int i = 0; i < 5; ++i) {
        if (i > 0) doc += "追加段落 " + std::to_string(i * 131) + "\n\n";
        const auto t0 = std::chrono::steady_clock::now();
        const MdBlockUpdate upd = model.update(doc);
        const auto t1 = std::chrono::steady_clock::now();
        blockCount = model.blocks().size();
        const double ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        bestSegMs = std::min(bestSegMs, ms);
        DOCTEST_REQUIRE(upd.changed);   // 每轮都真实变更（非零重算短路）
        if (i == 4) {
            std::cout << "[live] segment n=" << blockCount
                      << " ms=" << ms
                      << " dirty=[" << upd.dirtyFirst << ","
                      << upd.dirtyLast << "]" << std::endl;
        }
    }
    std::cout << "[live] segment best5 ms=" << bestSegMs
              << "（200KB/" << blockCount << " 块）" << std::endl;

    // (b) 实时列整帧 compose 耗时（headless 合成同树，取 200 块
    // `[live] frame compose ms` 日志人工复核，本记录只给同源码同路径的基数值）
    std::string mid;
    mid.reserve(20000);
    for (int i = 0; i < 200; ++i) {
        mid += "段落 " + std::to_string(i) + " 的正文内容填充填充文本\n\n";
    }
    test_support::MdHeadlessEnv env(kDirPrefix, "liveperf", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "mid.md", mid));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&] { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();                                 // 首帧（几何表/缓存建立）
    const auto c0 = std::chrono::steady_clock::now();
    frame();
    const auto c1 = std::chrono::steady_clock::now();
    const double compMs =
        std::chrono::duration<double, std::milli>(c1 - c0).count();
    std::cout << "[live] frame compose ms=" << compMs
              << "（200 块缩放基）" << std::endl;
    DOCTEST_CHECK(e.ui->find("md.live.scroll") != nullptr);
    DOCTEST_CHECK(e.ui->find("md.live.input") != nullptr);

    mod->onStop();
    return true;
})

// ⑥ md.saved 事件接线。saveEditor 真实写盘成功 → core::event::bus() 收到
// md.saved；幂等早退（editorText_==lastSavedText_）与未落盘打字均不发。
REGISTER_TEST(md_module_event_wiring,
              "markdown_module: saveEditor 写盘成功→md.saved；幂等早退不发（headless）", {
    const std::string v1 = "hello\n";
    test_support::MdHeadlessEnv env(kDirPrefix, "event", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "s.md", v1));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    test_support::HeadlessFrame e("mdlive");
    auto frame = [&] { e.frame([&](eui::Ui& u) { mod->compose(u, screen); }); };

    int gotSaved = 0;
    auto sub = core::event::bus().subscribe(core::event::kMdSaved,
        [&](const std::string&) { ++gotSaved; });

    frame();
    DOCTEST_REQUIRE(openFirstNote(*e.ui));
    frame();
    DOCTEST_REQUIRE(cycleMode(mod.get()));   // → 实时
    frame();

    // 实时态打字：仅内存变化、未落盘 → 不发 md.saved
    e.ui->state<MdInputState>("md.live.input").cursor = 6;
    const auto* hit = e.ui->find("md.live.input.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_REQUIRE(hit->onTextInput != nullptr);
    core::TextInputEvent ev;
    ev.text = "world";
    hit->onTextInput(ev);
    frame();
    DOCTEST_CHECK_EQ(gotSaved, 0);

    // Ctrl+S → 真实写盘成功 → md.saved 恰发一次
    eui::KeyEvent cs;
    cs.key = eui::InputKey::S;
    test_support::setShortcutModifier(cs.modifiers);
    DOCTEST_CHECK(mod->onKeyEvent(cs));
    std::string disk;
    DOCTEST_REQUIRE(readFileBytes(env.dir / "s.md", &disk));
    DOCTEST_CHECK_EQ(disk, std::string("hello\nworld"));
    DOCTEST_CHECK_EQ(gotSaved, 1);

    // 已落盘后再 Ctrl+S：幂等早退（editorText_==lastSavedText_）→ 不发
    DOCTEST_CHECK(mod->onKeyEvent(cs));
    DOCTEST_CHECK_EQ(gotSaved, 1);

    mod->onStop();
    return true;
})
// 新建 / 重命名笔记也是 GitHub Sync 的 dirty 触发源：两个写路径都要发事件
//（只订 md.saved 时，新建与重命名都不会触发同步）。
// 对照：新建弹窗里输入一个已存在的名字 = 直接打开旧笔记、没写盘 → 不发 md.created。
REGISTER_TEST(md_module_create_rename_event_wiring,
              "markdown_module: 新建→md.created、重命名→md.renamed；已存在名直接打开不发（headless）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "crev", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "旧笔记.md", "# 旧\n"));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    int gotCreated = 0, gotRenamed = 0;
    auto cSub = core::event::bus().subscribe(
        core::event::kMdCreated, [&](const std::string&) { ++gotCreated; });
    auto rSub = core::event::bus().subscribe(
        core::event::kMdRenamed, [&](const std::string&) { ++gotRenamed; });

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    auto frame = [&](const char* page) {
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
    };
    const auto clickId = [&](const std::string& id) -> bool {
        const auto* el = ui.find(id);
        if (el == nullptr || el->onClick == nullptr) return false;
        el->onClick();
        return true;
    };
    const auto typeId = [&](const std::string& id, const std::string& text) -> bool {
        const auto* hit = ui.find(id + ".hit");
        if (hit == nullptr || hit->onTextInput == nullptr) return false;
        core::TextInputEvent ev;
        ev.text = text;
        hit->onTextInput(ev);
        return true;
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

    frame("cr1");

    // ① 新建弹窗输入已存在的名字 → 直接打开旧笔记，没写盘 → 不发 md.created
    DOCTEST_REQUIRE(clickId("md.tree.new.bg"));
    frame("cr2");
    DOCTEST_REQUIRE(typeId("md.dialog.new.input", "旧笔记"));
    frame("cr3");
    DOCTEST_REQUIRE(clickId("md.dialog.new.confirm.bg"));
    frame("cr4");
    DOCTEST_CHECK_EQ(gotCreated, 0);

    // ② 真新建 → 落盘成功 → md.created 恰一次
    DOCTEST_REQUIRE(clickId("md.tree.new.bg"));
    frame("cr5");
    DOCTEST_REQUIRE(typeId("md.dialog.new.input", "新笔记"));
    frame("cr6");
    DOCTEST_REQUIRE(clickId("md.dialog.new.confirm.bg"));
    frame("cr7");
    DOCTEST_CHECK(fs::is_regular_file(env.dir / "新笔记.md"));
    DOCTEST_CHECK_EQ(gotCreated, 1);

    // ③ 树行右键 →「重命名」→ 弹窗改名 → 确认 → md.renamed 恰一次
    frame("cr8");
    auto* row = findRow("新笔记.md");
    DOCTEST_REQUIRE(row != nullptr);
    DOCTEST_REQUIRE(row->onContextMenu != nullptr);
    eui::PointerEvent pe;
    pe.x = 150.0;
    pe.y = 230.0;
    row->onContextMenu(pe, eui::Rect{});
    frame("cr9");
    const auto* rnLabel = ui.find("md.ctx.level.0.label.0");
    DOCTEST_REQUIRE(rnLabel != nullptr);
    DOCTEST_CHECK_EQ(rnLabel->text, std::string("重命名"));
    DOCTEST_REQUIRE(clickId("md.ctx.level.0.item.0"));
    frame("cr10");
    {
        // 弹窗预填原路径且光标在开头：直接打字会插在路径前 → 先全选再输入 = 整段替换
        auto& st = ui.state<components::input_detail::InputModel::InputState>(
            "md.dialog.rename.input");
        st.cursor = st.selectionStart = 0;
        st.selectionEnd = static_cast<int>(st.text.size());
        const auto* hit = ui.find("md.dialog.rename.input.hit");
        DOCTEST_REQUIRE(hit != nullptr);
        DOCTEST_REQUIRE(hit->onTextInput != nullptr);
        core::TextInputEvent ev;
        ev.text = "改名后";
        hit->onTextInput(ev);
    }
    frame("cr11");
    DOCTEST_REQUIRE(clickId("md.dialog.rename.confirm.bg"));
    frame("cr12");
    DOCTEST_CHECK(fs::is_regular_file(env.dir / "改名后.md"));
    DOCTEST_CHECK_EQ(gotRenamed, 1);

    mod->onStop();
    return true;
})

// 标签写路径也是 GitHub Sync 的 dirty 触发源：标签落 DB 两张表（markdown_tags /
// markdown_note_tags，都在整库 dump 里），而且**不碰笔记文件** —— 只订 md.saved 的
// 订阅方兜不住「只加/删标签」的改动，故必须自有一条事件。两处 UI 入口（＋ 提交 /
// chip ×）都要发。对照：只在输入框打字、没提交 = 不是写路径 → 不许发。
REGISTER_TEST(md_module_tag_change_event_wiring,
              "markdown_module: 加标签/删标签各发一次 md.tags_changed；只打字不发（headless）", {
    test_support::MdHeadlessEnv env(kDirPrefix, "tagev", kDbName);
    DOCTEST_REQUIRE(env.db != nullptr);
    DOCTEST_REQUIRE(writeTextFile(env.dir / "note.md", "# A\n"));

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    int gotTags = 0;
    auto sub = core::event::bus().subscribe(
        core::event::kMdTagsChanged, [&](const std::string&) { ++gotTags; });

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    auto frame = [&](const char* page) {
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
    };
    auto findFileRow = [&](const std::string& name) -> const core::dsl::Element* {
        for (int i = 0; i < 6; ++i) {
            const auto* nm = ui.find("md.tree.node." + std::to_string(i) + ".name");
            if (nm != nullptr && nm->text == name) {
                return ui.find("md.tree.node." + std::to_string(i) + ".row");
            }
        }
        return nullptr;
    };

    frame("te1");
    const auto* fileRow = findFileRow("note.md");
    DOCTEST_REQUIRE(fileRow != nullptr);
    DOCTEST_REQUIRE(fileRow->onClick != nullptr);
    fileRow->onClick();
    frame("te2");

    // 对照：只在输入框打字（未提交）= 没碰 DB → 不发
    const auto* addInput = ui.find("md.tags.add.hit");
    DOCTEST_REQUIRE(addInput != nullptr);
    DOCTEST_REQUIRE(addInput->onTextInput != nullptr);
    core::TextInputEvent tie;
    tie.text = "工作";
    addInput->onTextInput(tie);
    frame("te3");
    DOCTEST_CHECK_EQ(gotTags, 0);

    // ① 「＋」提交 → 真写标签表 → 恰一次
    const auto* addBtn = ui.find("md.tags.addbtn.bg");
    DOCTEST_REQUIRE(addBtn != nullptr);
    DOCTEST_REQUIRE(addBtn->onClick != nullptr);
    addBtn->onClick();
    frame("te4");
    DOCTEST_CHECK_EQ(gotTags, 1);
    DOCTEST_CHECK(ui.find("md.tags.chip.0.name") != nullptr);

    // ② chip × 删除 → 再恰一次（累计 2；删除也是写路径，漏发同样会漏同步）
    const auto* del = ui.find("md.tags.chip.0.del");
    DOCTEST_REQUIRE(del != nullptr);
    DOCTEST_REQUIRE(del->onClick != nullptr);
    del->onClick();
    frame("te5");
    DOCTEST_CHECK_EQ(gotTags, 2);

    mod->onStop();
    return true;
})
