#include "doctest.h"
#include "core/module_interface.h"

// PageId 的顺序不变量不在此重复——已由 test_todo.cpp 的 todo_pageid 覆盖；本例只钉
// pageIdToString 映射与序号。
REGISTER_TEST(claude_pageid, "claude: PageId string + order position", {
    DOCTEST_CHECK_EQ(core::pageIdToString(core::PageId::ClaudeProxy),
                     std::string("ClaudeProxy"));
    DOCTEST_CHECK_EQ(static_cast<int>(core::PageId::ClaudeProxy), 3);
    return true;
})