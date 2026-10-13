#include "doctest.h"
#include "core/logging.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>

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
    auto tmp = test_temp::tempPath("devdesk_test_log.log");
    // 预清理：用 ec 版本，删除失败不当异常抛出
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    core::log::addFileOutput(tmp);
    core::log::logMessage(core::log::Level::Info, "file output test");
    DOCTEST_CHECK(std::filesystem::exists(tmp));
    // logging 没有关闭文件输出的接口，ofstream 进程内一直开着；Windows 不允许删除打开中的文件，故删除走 ec 版本静默失败（残留无害）
    std::filesystem::remove(tmp, ec);
    return true;
})

// 轮转契约：base 超 8MB → 改名 .1，原 .1/.2 依次后移，最多保留 2 份，最老的一份删除
REGISTER_TEST(logging_rotate_by_size, "logging: 超阈值轮转 + 保留份数上限", {
    const auto base = test_temp::tempPath("devdesk_test_rotate.log");
    const auto r1 = std::filesystem::path(base.string() + ".1");
    const auto r2 = std::filesystem::path(base.string() + ".2");
    const auto r3 = std::filesystem::path(base.string() + ".3");

    // 预清理：base 可能被上个用例留下，它仍被进程内 ofstream 打开时删除会静默失败
    std::error_code ec;
    std::filesystem::remove(base, ec);
    std::filesystem::remove(r1, ec);
    std::filesystem::remove(r2, ec);

    // 造场景：base 超阈值，.1 是上一代，.2 是最老一代
    constexpr std::uintmax_t kOverThreshold = 9 * 1024 * 1024; // > 8MB 阈值
    {
        std::ofstream big(base, std::ios::binary | std::ios::trunc);
        std::string chunk(64 * 1024, 'x');
        for (std::uintmax_t written = 0; written < kOverThreshold; written += chunk.size()) {
            big << chunk;
        }
    }
    { std::ofstream(r1, std::ios::trunc) << "OLD-1"; }
    { std::ofstream(r2, std::ios::trunc) << "OLD-2"; }

    core::log::addFileOutput(base); // 触发轮转 + 新开空文件

    DOCTEST_CHECK(std::filesystem::exists(r1));
    DOCTEST_CHECK(std::filesystem::exists(r2));
    DOCTEST_CHECK_FALSE(std::filesystem::exists(r3));
    DOCTEST_CHECK(std::filesystem::file_size(r1, ec) >= kOverThreshold);  // 旧 base 滚成 .1
    DOCTEST_CHECK_EQ(std::filesystem::file_size(base, ec), 0u);          // .1 被挤到 .2、原 .2 的 OLD-2 已删除：证明是滚动而非覆盖
    std::string shifted;
    std::ifstream in(r2, std::ios::binary);
    in >> shifted;
    DOCTEST_CHECK_EQ(shifted, std::string("OLD-1"));

    core::log::logMessage(core::log::Level::Info, "rotated file line");
    DOCTEST_CHECK(std::filesystem::file_size(base, ec) > 0);  // 新内容写进新建的 base

    std::filesystem::remove(r1, ec);
    std::filesystem::remove(r2, ec);
    std::filesystem::remove(base, ec);
    return true;
})