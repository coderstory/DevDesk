#include "doctest.h"
#include "core/module_interface.h"

DOCTEST_TEST_CASE("todo: PageId ordering") {
    // 验证 PageId 枚举值
    DOCTEST_CHECK(static_cast<int>(core::PageId::Todo) >= 0);
}