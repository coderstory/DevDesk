#include "doctest.h"
#include "core/module_interface.h"

REGISTER_TEST(claude_pageid, "claude: PageId exists", {
    DOCTEST_CHECK(core::PageId::ClaudeProxy != core::PageId::Home);
    DOCTEST_CHECK(core::PageId::ClaudeProxy != core::PageId::Settings);
    return true;
})