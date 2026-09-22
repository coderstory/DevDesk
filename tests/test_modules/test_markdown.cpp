#include "doctest.h"
#include "core/module_interface.h"
#include "module_markdown/md_line_scanner.h"

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