#include "doctest.h"
#include "core/result.h"

DOCTEST_TEST_CASE("Result<int> ok") {
    auto r = core::Result<int>::ok(42);
    DOCTEST_CHECK(r.isOk());
    DOCTEST_CHECK_EQ(r.value(), 42);
}

DOCTEST_TEST_CASE("Result<int> fail") {
    auto r = core::Result<int>::fail(core::ErrorCode::FileNotFound, "missing");
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK_EQ(r.errorCode(), core::ErrorCode::FileNotFound);
    DOCTEST_CHECK_EQ(r.errorMessage(), std::string("missing"));
}

DOCTEST_TEST_CASE("Result<int> valueOr") {
    auto ok = core::Result<int>::ok(10);
    auto fail = core::Result<int>::fail(core::ErrorCode::Internal, "x");
    DOCTEST_CHECK_EQ(ok.valueOr(99), 10);
    DOCTEST_CHECK_EQ(fail.valueOr(99), 99);
}

DOCTEST_TEST_CASE("Result<void>") {
    auto ok = core::Result<void>::ok();
    auto fail = core::Result<void>::fail(core::ErrorCode::Internal, "x");
    DOCTEST_CHECK(ok.isOk());
    DOCTEST_CHECK(fail.isFail());
    DOCTEST_CHECK_EQ(fail.errorMessage(), std::string("x"));
}

DOCTEST_TEST_CASE("ErrorCode values") {
    DOCTEST_CHECK_EQ(static_cast<int>(core::ErrorCode::Ok), 0);
    DOCTEST_CHECK_NE(static_cast<int>(core::ErrorCode::FileNotFound),
                     static_cast<int>(core::ErrorCode::Ok));
}