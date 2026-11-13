// tests/test_platform/test_git_search.cpp
// git 管理器 v2 全局搜索（need:14）：匹配模型纯函数——asciiFold 折叠 /
// matchLines 子串命中与 5 条上限 / truncateAtCodepoint 码点边界截断。
// 纯函数直打，零文件 / git 依赖。
#include "doctest.h"

#include "module_git/git_search_model.h"

#include <string>
#include <vector>

using module::git::SearchHit;
using module::git::asciiFold;
using module::git::matchLines;
using module::git::truncateAtCodepoint;

namespace {
constexpr const char* kEllipsis = "\xE2\x80\xA6";  // … U+2026
constexpr const char* kCjk = "\xE4\xB8\x80";       // 一 U+4E00（3 字节）
} // namespace

REGISTER_TEST(git_search_asciifold, "git_search: asciiFold ASCII 折叠与非 ASCII 原样", {
    DOCTEST_CHECK(asciiFold("HeLLo-World_123") == "hello-world_123");
    DOCTEST_CHECK(asciiFold("") == "");
    // 非 ASCII（拉丁扩展 / CJK）不折叠
    DOCTEST_CHECK(asciiFold("\xC3\x84pfel") == "\xC3\x84pfel");  // Äpfel
    DOCTEST_CHECK(asciiFold(std::string("中文ABC")) == "中文abc");
    return true;
})

REGISTER_TEST(git_search_match_basic, "git_search: matchLines 子串命中与 1 起行号", {
    const std::vector<std::string> lines = {"Hello World", "foo hello bar", "nope",
                                            "HELLO"};
    const auto hits = matchLines(lines, "hello");
    DOCTEST_REQUIRE(hits.size() == 3);
    DOCTEST_CHECK(hits[0].line == 1);
    DOCTEST_CHECK(hits[1].line == 2);
    DOCTEST_CHECK(hits[2].line == 4);
    DOCTEST_CHECK_FALSE(hits[0].moreInFile);
    return true;
})

REGISTER_TEST(git_search_nonascii_nofold, "git_search: 非 ASCII 不折叠（Ä/ä 不互匹配）", {
    const std::vector<std::string> lines = {"\xC3\x84pfel", "\xC3\xA4pfel"};  // Äpfel / äpfel
    const auto hits = matchLines(lines, "\xC3\xA4pfel");  // 搜 äpfel
    DOCTEST_REQUIRE(hits.size() == 1);
    DOCTEST_CHECK(hits[0].line == 2);
    return true;
})

REGISTER_TEST(git_search_per_file_cap, "git_search: 单文件 5 条上限 + moreInFile 收口", {
    const std::vector<std::string> seven(7, "x");
    auto hits = matchLines(seven, "x");
    DOCTEST_REQUIRE(hits.size() == 5);
    DOCTEST_CHECK(hits.back().moreInFile);
    const std::vector<std::string> four(4, "x");
    hits = matchLines(four, "x");
    DOCTEST_REQUIRE(hits.size() == 4);
    DOCTEST_CHECK_FALSE(hits.back().moreInFile);
    return true;
})

REGISTER_TEST(git_search_empty_needle, "git_search: 空 needle 返回空结果", {
    const std::vector<std::string> lines = {"a", "b"};
    DOCTEST_CHECK(matchLines(lines, "").empty());
    return true;
})

REGISTER_TEST(git_search_preview_cjk_boundary, "git_search: preview 码点边界截断（不切半个多字节字符）", {
    // 10 CJK(30B) + NEEDLE(6B) + 10 CJK(30B)：窗内放得下 → 全行 preview，无省略号
    std::string line;
    for (int i = 0; i < 10; ++i) line += kCjk;
    line += "NEEDLE";
    for (int i = 0; i < 10; ++i) line += kCjk;
    size_t start = 0, len = 0;
    const std::string preview = truncateAtCodepoint(line, 30, 6, 40, &start, &len);
    DOCTEST_CHECK(preview == line);
    DOCTEST_CHECK(start == 30);
    DOCTEST_CHECK(len == 6);
    return true;
})

REGISTER_TEST(git_search_preview_ellipsis, "git_search: preview 前后省略号与 matchStart 平移", {
    // 60 CJK(180B) + NEEDLE(6B) + 60 CJK(180B)：前后各留 40 码点，两侧省略号
    std::string line;
    for (int i = 0; i < 60; ++i) line += kCjk;
    line += "NEEDLE";
    for (int i = 0; i < 60; ++i) line += kCjk;
    size_t start = 0, len = 0;
    const std::string preview = truncateAtCodepoint(line, 180, 6, 40, &start, &len);
    // preview = … + 40 CJK + NEEDLE + 40 CJK + …
    DOCTEST_CHECK(preview.rfind(kEllipsis, 0) == 0);
    DOCTEST_CHECK(preview.size() == 3 + 120 + 6 + 120 + 3);
    DOCTEST_CHECK(start == 3 + 120);
    DOCTEST_CHECK(len == 6);
    // 窗左界落在 CJK 序列起点（不切半个字符）：省略号后首字节是码点起始
    const unsigned char c = static_cast<unsigned char>(preview[3]);
    DOCTEST_CHECK((c & 0xC0) != 0x80);
    return true;
})

REGISTER_TEST(git_search_preview_at_start, "git_search: 命中在行首无前省略号（matchStart 不平移）", {
    std::string line = "NEEDLE";
    for (int i = 0; i < 60; ++i) line += kCjk;
    size_t start = 1, len = 0;
    const std::string preview = truncateAtCodepoint(line, 0, 6, 40, &start, &len);
    DOCTEST_CHECK(preview.rfind("NEEDLE", 0) == 0);
    DOCTEST_CHECK(start == 0);
    DOCTEST_CHECK(len == 6);
    return true;
})
