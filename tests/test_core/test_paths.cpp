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