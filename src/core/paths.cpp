#include "paths.h"
#include "result.h"
#include "logging.h"

#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace core::paths {

static fs::path getEnvOr(const char* var, const fs::path& fallback) {
    if (const char* v = std::getenv(var)) {
        return fs::path(v);
    }
    return fallback;
}

fs::path appDataDir() {
#if defined(__APPLE__)
    fs::path home = getEnvOr("HOME", "");
    return home / "Library" / "Application Support" / "DevDesk";
#elif defined(_WIN32) || defined(_WIN64)
    fs::path appdata = getEnvOr("APPDATA", "");
    return appdata / "DevDesk";
#else
    fs::path home = getEnvOr("HOME", "");
    return home / ".local" / "share" / "DevDesk";
#endif
}

fs::path configFile() {
    return appDataDir() / "config.json";
}

fs::path sqliteDbFile() {
    return appDataDir() / "devdesk.db";
}

fs::path markdownDir() {
    return appDataDir() / "notes";
}

fs::path logsDir() {
    return appDataDir() / "logs";
}

fs::path logFile() {
    return logsDir() / "devdesk.log";
}

void ensureDirectoriesExist() {
    std::error_code ec;
    for (const auto& dir : {appDataDir(), markdownDir(), logsDir()}) {
        fs::create_directories(dir, ec);
        if (ec) {
            LOG_WARN("创建目录失败: " + dir.string() + " (" + ec.message() + ")");
        }
    }
}

} // namespace core::paths