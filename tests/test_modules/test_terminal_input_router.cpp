// tests/test_modules/test_terminal_input_router.cpp
// M2 Task 1：输入路由抽缝等价性——拖选锚点/头、滚动清选区、滚轮档位换算与钳制。
// 断言口径对齐抽缝前 terminal_page.cpp 的 press/move/release/onScroll 逐行语义。
#include "doctest.h"
#include "module_terminal/terminal_input_router.h"

REGISTER_TEST(inputRouterDragSelect,
              "terminal input router: 拖选锚点/头与选区有效性", {
    terminal::TerminalInputRouter r;
    DOCTEST_CHECK_FALSE(r.hasSelection());
    r.press({2, 5});
    DOCTEST_CHECK_TRUE(r.pressing());
    DOCTEST_CHECK_FALSE(r.hasSelection()); // press 当帧不构成选区

    r.dragTo({4, 9});
    DOCTEST_CHECK_TRUE(r.hasSelection());
    DOCTEST_CHECK_EQ(r.anchor().row, 2);
    DOCTEST_CHECK_EQ(r.anchor().col, 5);
    DOCTEST_CHECK_EQ(r.head().row, 4);
    DOCTEST_CHECK_EQ(r.head().col, 9);

    r.release({4, 9});
    DOCTEST_CHECK_FALSE(r.pressing());
    DOCTEST_CHECK_TRUE(r.hasSelection());
    return true;
})

REGISTER_TEST(inputRouterClickWithoutDragClearsSelection,
              "terminal input router: 原地按下松开不构成选区", {
    terminal::TerminalInputRouter r;
    r.press({3, 7});
    r.dragTo({3, 9}); // 有过拖动 → 形成选区
    DOCTEST_CHECK_TRUE(r.hasSelection());
    r.release({3, 7}); // 落点回到锚点
    DOCTEST_CHECK_FALSE(r.hasSelection());
    return true;
})

REGISTER_TEST(inputRouterDragRequiresPress,
              "terminal input router: 未按下时拖动/松开不产生选区", {
    terminal::TerminalInputRouter r;
    r.dragTo({1, 1});
    DOCTEST_CHECK_FALSE(r.hasSelection());
    r.release({2, 2});
    DOCTEST_CHECK_FALSE(r.hasSelection());
    DOCTEST_CHECK_EQ(r.head().row, 0);
    return true;
})

REGISTER_TEST(inputRouterWheelScroll,
              "terminal input router: 滚轮 3 行/档、小数余数保留、越界钳制", {
    terminal::TerminalInputRouter r;
    DOCTEST_CHECK_EQ(r.scrollBy(1.0f, 0, 100), 3);
    DOCTEST_CHECK_EQ(r.scrollBy(-1.0f, 0, 100), 0);    // 下限钳制
    DOCTEST_CHECK_EQ(r.scrollBy(100.0f, 0, 10), 10);   // 上限钳制
    // 小数余数（触控板）：0.1 档 = 0.3 行，四连击后累计 1.2 行才滚 1 行
    terminal::TerminalInputRouter frac;
    DOCTEST_CHECK_EQ(frac.scrollBy(0.1f, 0, 100), 0);
    DOCTEST_CHECK_EQ(frac.scrollBy(0.1f, 0, 100), 0);
    DOCTEST_CHECK_EQ(frac.scrollBy(0.1f, 0, 100), 0);
    DOCTEST_CHECK_EQ(frac.scrollBy(0.1f, 0, 100), 1);
    return true;
})

REGISTER_TEST(inputRouterScrollClearsSelection,
              "terminal input router: 滚动即清选区（0 行时不清）", {
    terminal::TerminalInputRouter r;
    r.press({0, 0});
    r.dragTo({1, 3});
    DOCTEST_CHECK_TRUE(r.hasSelection());
    DOCTEST_CHECK_EQ(r.scrollBy(0.1f, 5, 100), 5); // 不足一行 → 不清选区
    DOCTEST_CHECK_TRUE(r.hasSelection());
    DOCTEST_CHECK_EQ(r.scrollBy(1.0f, 5, 100), 8);
    DOCTEST_CHECK_FALSE(r.hasSelection());
    DOCTEST_CHECK_FALSE(r.pressing());
    return true;
})
