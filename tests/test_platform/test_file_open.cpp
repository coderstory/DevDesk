// tests/test_platform/test_file_open.cpp
// platform::file_open::openDirectory（产品审计 2026-09-23 [15]）：
// 只测失败路径——成功路径会真的拉起资源管理器/Finder 窗口，属人工点验项；
// 失败路径（空参数 / 不存在目录）无 UI 副作用（Win 侧 SEE_MASK_FLAG_NO_UI）。
#include "doctest.h"

#include "platform/file_open.h"

#include <filesystem>
#include <string>

REGISTER_TEST(file_open_directory_empty_rejected,
              "file_open: openDirectory 空路径 → ValidationError", {
    auto r = platform::file_open::openDirectory("");
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK(r.errorCode() == core::ErrorCode::ValidationError);
    return true;
})

REGISTER_TEST(file_open_directory_missing_fails,
              "file_open: openDirectory 不存在目录 → 失败返回（不弹窗）", {
    namespace fs = std::filesystem;
    const auto p = fs::temp_directory_path() /
                   "devdesk_file_open_no_such_dir_9e31";
    std::error_code ec;
    fs::remove_all(p, ec);  // 确保不存在（幂等，防上次残留）
    auto r = platform::file_open::openDirectory(p.string());
    DOCTEST_CHECK(r.isFail());
    return true;
})
