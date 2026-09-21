#include "doctest.h"
#include "core/module_interface.h"

REGISTER_TEST(home_pageid_first, "home: PageId Home is first", {
    DOCTEST_CHECK_EQ(static_cast<int>(core::PageId::Home), 0);
    return true;
})