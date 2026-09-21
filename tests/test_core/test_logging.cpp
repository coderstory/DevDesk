#include "doctest.h"
#include "core/logging.h"

#include <filesystem>
#include <fstream>

REGISTER_TEST(logging_set_get_level, "logging: set/get level", {
    core::log::setLevel(core::log::Level::Debug);
    DOCTEST_CHECK_EQ(core::log::level(), core::log::Level::Debug);
    core::log::setLevel(core::log::Level::Info);
    DOCTEST_CHECK_EQ(core::log::level(), core::log::Level::Info);
    return true;
})

REGISTER_TEST(logging_no_crash, "logging: logMessage doesn't crash", {
    core::log::setLevel(core::log::Level::Trace);
    core::log::logMessage(core::log::Level::Info, "test message", __FILE__, __LINE__);
    core::log::logMessage(core::log::Level::Warn, "warn message");
    core::log::logMessage(core::log::Level::Error, "error message");
    return true;
})

REGISTER_TEST(logging_file_output, "logging: addFileOutput", {
    auto tmp = std::filesystem::temp_directory_path() / "devdesk_test_log.log";
    // 预清理：无异常版本
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    core::log::addFileOutput(tmp);
    core::log::logMessage(core::log::Level::Info, "file output test");
    DOCTEST_CHECK(std::filesystem::exists(tmp));
    // logging 模块没有关闭文件输出的接口，g_file(ofstream) 进程内保持打开；
    // Windows 不允许删除打开中的文件，这里用 ec 版本让删除静默失败（残留临时文件无害）
    std::filesystem::remove(tmp, ec);
    return true;
})