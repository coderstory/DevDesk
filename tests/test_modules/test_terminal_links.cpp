// tests/test_modules/test_terminal_links.cpp
// M2 Task 7：终端行内链接识别（URL / 绝对路径）纯逻辑单测。
// 钉死：截止/剥尾规则、路径前缀与排除项、CJK 列口径、多链接升序不重叠、linkAtColumn 边界。
#include "doctest.h"
#include "module_terminal/terminal_links.h"

using terminal::findLinks;
using terminal::LinkKind;
using terminal::LinkSpan;

REGISTER_TEST(terminalLinksUrlTrailingPunct,
              "terminal links: URL 遇空白截止，尾部逗号/句点连续剥除", {
    const auto links = findLinks("see https://a.b/c?d=1, then.");
    DOCTEST_REQUIRE(links.size() == 1);
    DOCTEST_CHECK(links[0].kind == LinkKind::Url);
    DOCTEST_CHECK_EQ(links[0].target, std::string("https://a.b/c?d=1"));
    DOCTEST_CHECK_EQ(links[0].colBegin, 4);
    DOCTEST_CHECK_EQ(links[0].colEnd, 21);
    return true;
})

REGISTER_TEST(terminalLinksUrlQuoted,
              "terminal links: 引号包裹的 URL 不含引号（引号为截止符非尾部标点）", {
    const auto links = findLinks("\"https://x.y/z\"");
    DOCTEST_REQUIRE(links.size() == 1);
    DOCTEST_CHECK_EQ(links[0].target, std::string("https://x.y/z"));
    DOCTEST_CHECK_EQ(links[0].colBegin, 1);
    DOCTEST_CHECK_EQ(links[0].colEnd, 14);
    return true;
})

REGISTER_TEST(terminalLinksUrlSchemeCase,
              "terminal links: scheme 大小写不敏感，target 保留原文字节", {
    const auto links = findLinks("HTTPS://A.B/C");
    DOCTEST_REQUIRE(links.size() == 1);
    DOCTEST_CHECK(links[0].kind == LinkKind::Url);
    DOCTEST_CHECK_EQ(links[0].target, std::string("HTTPS://A.B/C"));
    DOCTEST_CHECK_EQ(links[0].colBegin, 0);
    DOCTEST_CHECK_EQ(links[0].colEnd, 13);

    // URL 内的 :// 不会被重复识别成路径（整行仍只有一个链接）
    const auto urlOnly = findLinks("https://a.b/c");
    DOCTEST_REQUIRE(urlOnly.size() == 1);
    DOCTEST_CHECK(urlOnly[0].kind == LinkKind::Url);
    return true;
})

REGISTER_TEST(terminalLinksPathParen,
              "terminal links: 括号包裹的绝对路径去掉右括号", {
    const auto links = findLinks("(/usr/local/bin/x)");
    DOCTEST_REQUIRE(links.size() == 1);
    DOCTEST_CHECK(links[0].kind == LinkKind::Path);
    DOCTEST_CHECK_EQ(links[0].target, std::string("/usr/local/bin/x"));
    DOCTEST_CHECK_EQ(links[0].colBegin, 1);
    DOCTEST_CHECK_EQ(links[0].colEnd, 17);
    return true;
})

REGISTER_TEST(terminalLinksHomePath,
              "terminal links: ~/ 开头识别为路径", {
    const auto links = findLinks("~/.zshrc");
    DOCTEST_REQUIRE(links.size() == 1);
    DOCTEST_CHECK(links[0].kind == LinkKind::Path);
    DOCTEST_CHECK_EQ(links[0].target, std::string("~/.zshrc"));
    DOCTEST_CHECK_EQ(links[0].colBegin, 0);
    DOCTEST_CHECK_EQ(links[0].colEnd, 8);
    return true;
})

REGISTER_TEST(terminalLinksWindowsDrive,
              "terminal links: 盘符 X:\\ / X:/ 识别，遇空格截止", {
    const auto backslash = findLinks("C:\\Users\\a b");
    DOCTEST_REQUIRE(backslash.size() == 1);
    DOCTEST_CHECK(backslash[0].kind == LinkKind::Path);
    DOCTEST_CHECK_EQ(backslash[0].target, std::string("C:\\Users\\a"));
    DOCTEST_CHECK_EQ(backslash[0].colBegin, 0);
    DOCTEST_CHECK_EQ(backslash[0].colEnd, 10);

    const auto slash = findLinks("C:/Users/a");
    DOCTEST_REQUIRE(slash.size() == 1);
    DOCTEST_CHECK(slash[0].kind == LinkKind::Path);
    DOCTEST_CHECK_EQ(slash[0].target, std::string("C:/Users/a"));
    DOCTEST_CHECK_EQ(slash[0].colEnd, 10);
    return true;
})

REGISTER_TEST(terminalLinksCjkAdjacent,
              "terminal links: 中文句号剥除且列偏移按 CJK 2 列/字", {
    const auto links = findLinks("错误见 https://a.b/c。");
    DOCTEST_REQUIRE(links.size() == 1);
    DOCTEST_CHECK(links[0].kind == LinkKind::Url);
    DOCTEST_CHECK_EQ(links[0].target, std::string("https://a.b/c"));
    // 3 个 CJK ×2 列 + 1 空格 = 7
    DOCTEST_CHECK_EQ(links[0].colBegin, 7);
    DOCTEST_CHECK_EQ(links[0].colEnd, 20);
    return true;
})

REGISTER_TEST(terminalLinksRelativeAndCommentsExcluded,
              "terminal links: 相对路径 a/b、// 注释、/* 注释不识别", {
    DOCTEST_CHECK(findLinks("a/b").empty());
    DOCTEST_CHECK(findLinks("//comment").empty());
    DOCTEST_CHECK(findLinks("/* note */").empty());
    DOCTEST_CHECK(findLinks("/").empty());
    return true;
})

REGISTER_TEST(terminalLinksTwoLinksColumns,
              "terminal links: 一行两个链接升序不重叠，CJK 前缀列偏移正确", {
    const auto links = findLinks("看 https://a.b/c 和 /usr/bin/x 完");
    DOCTEST_REQUIRE(links.size() == 2);

    DOCTEST_CHECK(links[0].kind == LinkKind::Url);
    DOCTEST_CHECK_EQ(links[0].target, std::string("https://a.b/c"));
    DOCTEST_CHECK_EQ(links[0].colBegin, 3); // 看 2 列 + 空格 1 列
    DOCTEST_CHECK_EQ(links[0].colEnd, 16);

    DOCTEST_CHECK(links[1].kind == LinkKind::Path);
    DOCTEST_CHECK_EQ(links[1].target, std::string("/usr/bin/x"));
    DOCTEST_CHECK_EQ(links[1].colBegin, 20); // 空格 + 和(2) + 空格
    DOCTEST_CHECK_EQ(links[1].colEnd, 30);

    DOCTEST_CHECK(links[0].colEnd <= links[1].colBegin);
    return true;
})

REGISTER_TEST(terminalLinksLinkAtColumn,
              "terminal links: linkAtColumn 命中 [begin,end) 边界与未命中", {
    const auto links = findLinks("看 https://a.b/c 和 /usr/bin/x 完");
    DOCTEST_REQUIRE(links.size() == 2);

    const LinkSpan* first = terminal::linkAtColumn(links, 3);
    DOCTEST_REQUIRE(first != nullptr);
    DOCTEST_CHECK_EQ(first->target, std::string("https://a.b/c"));
    DOCTEST_CHECK_TRUE(terminal::linkAtColumn(links, 15) == first); // colEnd-1 命中
    DOCTEST_CHECK_FALSE(terminal::linkAtColumn(links, 16) == first); // colEnd 不含
    DOCTEST_CHECK_TRUE(terminal::linkAtColumn(links, 16) == nullptr); // 空格

    const LinkSpan* second = terminal::linkAtColumn(links, 20);
    DOCTEST_REQUIRE(second != nullptr);
    DOCTEST_CHECK_EQ(second->target, std::string("/usr/bin/x"));
    DOCTEST_CHECK_TRUE(terminal::linkAtColumn(links, 29) == second); // colEnd-1 命中
    DOCTEST_CHECK_TRUE(terminal::linkAtColumn(links, 30) == nullptr); // colEnd 不含

    DOCTEST_CHECK_TRUE(terminal::linkAtColumn(links, 2) == nullptr);  // 链接前空格
    DOCTEST_CHECK_TRUE(terminal::linkAtColumn(links, 0) == nullptr);  // CJK 前缀
    DOCTEST_CHECK_TRUE(terminal::linkAtColumn(links, -1) == nullptr); // 负列
    DOCTEST_CHECK_TRUE(terminal::linkAtColumn({}, 0) == nullptr);
    return true;
})
