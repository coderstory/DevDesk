#include "doctest.h"
#include "core/paths.h"

#include <filesystem>

REGISTER_TEST(paths_app_data_dir, "paths: appDataDir not empty", {
    auto p = core::paths::appDataDir();
    DOCTEST_CHECK_FALSE(p.empty());
    DOCTEST_CHECK(p.string().find("DevDesk") != std::string::npos);
    return true;
})

REGISTER_TEST(paths_config_file, "paths: configFile in appDataDir", {
    auto root = core::paths::appDataDir();
    auto cfg = core::paths::configFile();
    // 简单检查 cfg 路径以 root 开头（macOS equivalent 不支持）
    auto cfg_str = cfg.string();
    auto root_str = root.string();
    DOCTEST_CHECK(cfg_str.find(root_str) == 0);
    DOCTEST_CHECK(cfg_str.find("config.json") != std::string::npos);
    return true;
})

REGISTER_TEST(paths_sqlite_ext, "paths: sqliteDbFile is .db", {
    auto f = core::paths::sqliteDbFile();
    DOCTEST_CHECK_EQ(f.extension().string(), std::string(".db"));
    return true;
})

REGISTER_TEST(paths_log_file, "paths: logFile in logs dir", {
    auto lf = core::paths::logFile();
    DOCTEST_CHECK(lf.string().find("logs") != std::string::npos);
    return true;
})

// 平台 API 真机断言（Windows 走 GetModuleFileNameW；macOS/POSIX 同语义）：
// 失败时必须返回空 path，绝不用 cwd 冒充 —— 所以"非空 + 绝对 + 存在"三条
// 都断言；再用"目录里有测试二进制本体"反证返回的不是别的目录
REGISTER_TEST(paths_executable_dir, "paths: executableDir = 当前可执行文件所在目录", {
    auto dir = core::paths::executableDir();
    DOCTEST_CHECK_FALSE(dir.empty());
    DOCTEST_CHECK(dir.is_absolute());
    std::error_code ec;
    DOCTEST_CHECK(std::filesystem::is_directory(dir, ec) && !ec);
    // 注意：本宏体内不能出现裸逗号（预处理器按逗号切分实参），迭代器分两句声明
    bool foundSelf = false;
    std::filesystem::directory_iterator it(dir, ec);
    const std::filesystem::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it->path().stem().string() == "devdesk_tests") {
            foundSelf = true;
            break;
        }
    }
    DOCTEST_CHECK(foundSelf);
    return true;
})