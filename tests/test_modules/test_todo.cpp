#include "doctest.h"
#include "core/module_interface.h"

// PageId 枚举序是侧边栏顺序与 Router 分发依赖的契约：重排枚举会静默改变默认
// 导航顺序。原断言 `PageId::Todo >= 0` 对该 enum class 恒真、未验证任何顺序，
// 此处改为显式钉死预期序（严格单调 + 各成员预期编号），任一改动都会被抓到。
REGISTER_TEST(todo_pageid, "todo: PageId ordering", {
    using core::PageId;
    const int home = static_cast<int>(PageId::Home);
    const int markdown = static_cast<int>(PageId::Markdown);
    const int todo = static_cast<int>(PageId::Todo);
    const int claude = static_cast<int>(PageId::ClaudeProxy);
    const int settings = static_cast<int>(PageId::Settings);

    // 严格单调：Home < Markdown < Todo < ClaudeProxy < Settings
    DOCTEST_CHECK(home < markdown);
    DOCTEST_CHECK(markdown < todo);
    DOCTEST_CHECK(todo < claude);
    DOCTEST_CHECK(claude < settings);

    // 钉死预期取值，防"虽单调但整体平移/插空"绕过顺序检查而悄悄改变路由编号
    DOCTEST_CHECK_EQ(home, 0);
    DOCTEST_CHECK_EQ(markdown, 1);
    DOCTEST_CHECK_EQ(todo, 2);
    DOCTEST_CHECK_EQ(claude, 3);
    DOCTEST_CHECK_EQ(settings, 4);
    return true;
})