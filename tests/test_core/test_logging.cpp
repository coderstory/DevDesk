#include "doctest.h"
#include "core/logging.h"

#include <filesystem>
#include <fstream>

DOCTEST_TEST_CASE("logging: set/get level") {
    core::log::setLevel(core::log::Level::Debug);
    DOCTEST_CHECK_EQ(core::log::level(), core::log::Level::Debug);
    core::log::setLevel(core::log::Level::Info);
    DOCTEST_CHECK_EQ(core::log::level(), core::log::Level::Info);
}

DOCTEST_TEST_CASE("logging: logMessage doesn't crash") {
    core::log::setLevel(core::log::Level::Trace);
    // 不加断言，只验证不抛异常
    core::log::logMessage(core::log::Level::Info, "test message", __FILE__, __LINE__);
    core::log::logMessage(core::log::Level::Warn, "warn message");
    core::log::logMessage(core::log::Level::Error, "error message");
}

DOCTEST_TEST_CASE("logging: addFileOutput") {
    auto tmp = std::filesystem::temp_directory_path() / "devdesk_test_log.log";
    std::filesystem::remove(tmp);
    core::log::addFileOutput(tmp);
    core::log::logMessage(core::log::Level::Info, "file output test");
    DOCTEST_CHECK(std::filesystem::exists(tmp));
    std::filesystem::remove(tmp);
}

DOCTEST_TEST_CASE("logging: level filter") {
    core::log::setLevel(core::log::Level::Error);
    // Debug 应该被过滤（不写入文件）
    auto tmp = std::filesystem::temp_directory_path() / "devdesk_test_filter.log";
    std::filesystem::remove(tmp);
    core::log::addFileOutput(tmp);
    core::log::logMessage(core::log::Level::Trace, "should be filtered");
    if (std::filesystem::exists(tmp)) {
        std::ifstream f(tmp);
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        DOCTEST_CHECK(s.find("should be filtered") == std::string::npos);
        std::filesystem::remove(tmp);
    }
    core::log::setLevel(core::log::Level::Info);
}