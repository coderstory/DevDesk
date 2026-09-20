#include "doctest.h"
#include "core/paths.h"

#include <filesystem>

DOCTEST_TEST_CASE("paths: appDataDir not empty") {
    auto p = core::paths::appDataDir();
    DOCTEST_CHECK_FALSE(p.empty());
    DOCTEST_CHECK(p.string().find("DevDesk") != std::string::npos);
}

DOCTEST_TEST_CASE("paths: configFile in appDataDir") {
    auto root = core::paths::appDataDir();
    auto cfg = core::paths::configFile();
    DOCTEST_CHECK(std::filesystem::equivalent(cfg.parent_path(), root));
}

DOCTEST_TEST_CASE("paths: sqliteDbFile is .db") {
    auto f = core::paths::sqliteDbFile();
    DOCTEST_CHECK_EQ(f.extension().string(), std::string(".db"));
}

DOCTEST_TEST_CASE("paths: logFile in logs dir") {
    auto lf = core::paths::logFile();
    DOCTEST_CHECK(lf.string().find("logs") != std::string::npos);
}