#include "doctest.h"
#include "core/module_interface.h"

// 原为占位测试（仅证 ClaudeProxy != Home/Settings——enum class 成员本就两两不等，
// 恒真、虚增用例数）。改为验证真实行为：pageIdToString 对 ClaudeProxy 的映射，
// 以及它在预期序中的编号（顺序不变量本身由 test_todo.cpp 的 todo_pageid 覆盖）。
REGISTER_TEST(claude_pageid, "claude: PageId string + order position", {
    DOCTEST_CHECK_EQ(core::pageIdToString(core::PageId::ClaudeProxy),
                     std::string("ClaudeProxy"));
    DOCTEST_CHECK_EQ(static_cast<int>(core::PageId::ClaudeProxy), 3);
    return true;
})