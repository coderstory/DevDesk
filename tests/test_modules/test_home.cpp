#include "doctest.h"
#include "core/module_interface.h"

DOCTEST_TEST_CASE("home: PageId Home is first") {
    DOCTEST_CHECK_EQ(static_cast<int>(core::PageId::Home), 0);
}