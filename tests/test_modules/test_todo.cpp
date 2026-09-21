#include "doctest.h"
#include "core/module_interface.h"

REGISTER_TEST(todo_pageid, "todo: PageId ordering", {
    DOCTEST_CHECK(static_cast<int>(core::PageId::Todo) >= 0);
    return true;
})