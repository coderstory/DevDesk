#include "doctest.h"
#include "core/module_interface.h"

DOCTEST_TEST_CASE("claude: PageId exists") {
    DOCTEST_CHECK(core::PageId::ClaudeProxy != core::PageId::Home);
    DOCTEST_CHECK(core::PageId::ClaudeProxy != core::PageId::Settings);
}