#include "paths.h"
#include "logging.h"

#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace core::paths {

// appDataDir() 的平台实现在 paths_macos.cpp / paths_win.cpp / paths_posix.cpp，
// 由 CMake 按平台选择编译（接口隔离，业务代码零平台宏）

fs::path configFile() {
    return appDataDir() / "config.json";
}

fs::path sqliteDbFile() {
    return appDataDir() / "devdesk.db";
}

fs::path markdownDir() {
    // 环境变量覆盖（无头测试隔离用；用户也可借此把笔记目录挪到任意位置）
    if (const char* env = std::getenv("DEVDESK_NOTES_DIR")) {
        return fs::path(env);
    }
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
