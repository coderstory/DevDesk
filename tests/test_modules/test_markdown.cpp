#include "doctest.h"
#include "core/module_interface.h"

// 注意：完整模块测试需要在 EUI-NEO 运行环境中
// 这里只验证模块接口（PageId 枚举、Module 抽象）
DOCTEST_TEST_CASE("markdown: PageId") {
    DOCTEST_CHECK(core::PageId::Markdown != core::PageId::Todo);
    DOCTEST_CHECK(core::PageId::Markdown != core::PageId::Home);
}

DOCTEST_TEST_CASE("markdown: pageIdToString") {
    DOCTEST_CHECK_EQ(core::pageIdToString(core::PageId::Home), std::string("Home"));
    DOCTEST_CHECK_EQ(core::pageIdToString(core::PageId::Markdown), std::string("Markdown"));
    DOCTEST_CHECK_EQ(core::pageIdToString(core::PageId::Todo), std::string("Todo"));
    DOCTEST_CHECK_EQ(core::pageIdToString(core::PageId::ClaudeProxy), std::string("ClaudeProxy"));
}