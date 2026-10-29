#include "doctest.h"
#include "core/module_interface.h"

// PageId 的顺序不变量不在此重复——已由 test_todo.cpp 的 todo_pageid 覆盖；本例只钉
// pageIdToString 映射与序号。
// 序号 = 枚举声明位（Home=0 起），须与 src/app_registry.h kModuleRegistry 表序一致：
// Schedule 插入 Todo 之后（27807adb 14 页终态序重钉）后，ClaudeProxy 排第 5 位 = 4。
REGISTER_TEST(claude_pageid, "claude: PageId string + order position", {
    DOCTEST_CHECK_EQ(core::pageIdToString(core::PageId::ClaudeProxy),
                     std::string("ClaudeProxy"));
    DOCTEST_CHECK_EQ(static_cast<int>(core::PageId::ClaudeProxy), 4);
    return true;
})