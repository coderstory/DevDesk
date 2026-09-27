#include "doctest.h"
#include "core/result.h"

REGISTER_TEST(test_result_ok, "Result<int> ok", {
    auto r = core::Result<int>::ok(42);
    DOCTEST_CHECK(r.isOk());
    DOCTEST_CHECK_EQ(r.value(), 42);
    return true;
})

REGISTER_TEST(test_result_fail, "Result<int> fail", {
    auto r = core::Result<int>::fail(core::ErrorCode::FileNotFound, "missing");
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK_EQ(r.errorCode(), core::ErrorCode::FileNotFound);
    DOCTEST_CHECK_EQ(r.errorMessage(), std::string("missing"));
    return true;
})

REGISTER_TEST(test_result_valueOr, "Result<int> valueOr", {
    auto ok = core::Result<int>::ok(10);
    auto fail = core::Result<int>::fail(core::ErrorCode::Internal, "x");
    DOCTEST_CHECK_EQ(ok.valueOr(99), 10);
    DOCTEST_CHECK_EQ(fail.valueOr(99), 99);
    return true;
})

REGISTER_TEST(test_result_void, "Result<void>", {
    auto ok = core::Result<void>::ok();
    auto fail = core::Result<void>::fail(core::ErrorCode::Internal, "x");
    DOCTEST_CHECK(ok.isOk());
    DOCTEST_CHECK(fail.isFail());
    DOCTEST_CHECK_EQ(fail.errorMessage(), std::string("x"));
    return true;
})

// M8（2026-09-27 专家审计）：默认构造态是「伪失败态」——isOk()==false 但
// errorCode()==Ok，而旧 errorMessage() 直接走 std::get<2> 会抛
// std::bad_variant_access（app 用 -fno-exceptions 编译，抛即 terminate）。
// 现返回占位串；本用例锁住默认态行为（若哪天真抛了，异常会让本用例失败）。
REGISTER_TEST(test_result_default_state, "Result 默认构造态不抛异常", {
    core::Result<int> defT;
    DOCTEST_CHECK(defT.isFail());
    DOCTEST_CHECK_EQ(defT.errorMessage(), std::string("（未初始化）"));

    // Result<void> 默认态行为一致：同为失败态、errorMessage() 同样不抛
    //（其默认 message_ 为空串，与 Result<int> 的占位串不同是既有事实）
    core::Result<void> defV;
    DOCTEST_CHECK(defV.isFail());
    DOCTEST_CHECK(defV.errorMessage().empty());
    return true;
})

REGISTER_TEST(test_error_code_values, "ErrorCode values", {
    DOCTEST_CHECK_EQ(static_cast<int>(core::ErrorCode::Ok), 0);
    DOCTEST_CHECK_NE(static_cast<int>(core::ErrorCode::FileNotFound),
                     static_cast<int>(core::ErrorCode::Ok));
    return true;
})