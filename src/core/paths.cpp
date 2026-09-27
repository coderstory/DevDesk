#include "paths.h"
#include "logging.h"

#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace core::paths {

// appDataDir() 的平台默认实现在 paths_macos.cpp / paths_win.cpp / paths_posix.cpp，
// 由 CMake 按平台选择编译（接口隔离，业务代码零平台宏）

namespace {

// DEVDESK_DATA_ROOT 覆盖（审计 CP-04 的**单一数据根入口**）：已设且非空 →
// 该目录；未设 / 空串 → 空 path（调用方回落平台默认）。空串与「没设」同义，
// 照 claudeConfigDirOverride 的既有约定（Windows _putenv_s 传空串即删除变量）。
// 只此一处读数据根，其它路径一律经 appDataDir() 派生——不再有「各自读各自的
// 环境变量」的散落隔离缝。
fs::path dataRootOverride() {
    const char* env = std::getenv("DEVDESK_DATA_ROOT");
    if (env != nullptr && *env != '\0') {
        return fs::path(env);
    }
    return {};
}

} // namespace

fs::path appDataDir() {
    const fs::path root = dataRootOverride();
    if (!root.empty()) {
        return root;
    }
    return platformDefaultAppDataDir();
}

fs::path configFile() {
    // 覆盖优先级（审计 CP-04）：DEVDESK_CONFIG_PATH（整文件路径，最细粒度）
    // > DEVDESK_DATA_ROOT（经 appDataDir 派生）> 平台默认根。
    // 单项覆盖保留是向后兼容：既有测试仍按老变量隔离单条路径。
    if (const char* env = std::getenv("DEVDESK_CONFIG_PATH")) {
        return fs::path(env);
    }
    return appDataDir() / "config.json";
}

fs::path sqliteDbFile() {
    return appDataDir() / "devdesk.db";
}

fs::path markdownDir() {
    // 覆盖优先级：DEVDESK_NOTES_DIR > DEVDESK_DATA_ROOT > 平台默认根
    // （用户也可借单项覆盖把笔记目录挪到任意位置）
    if (const char* env = std::getenv("DEVDESK_NOTES_DIR")) {
        return fs::path(env);
    }
    return appDataDir() / "notes";
}

fs::path petsDir() {
    // 覆盖优先级：DEVDESK_PETS_DIR > DEVDESK_DATA_ROOT > 平台默认根
    // 注：本函数只决定皮肤**写在哪**；「能从哪儿发现皮肤」见 module_pet 的
    // skinRootCandidates()——那里已不再读环境变量（审计 CP-04 去产品行为分支）
    if (const char* env = std::getenv("DEVDESK_PETS_DIR")) {
        return fs::path(env);
    }
    return appDataDir() / "pets";
}

fs::path logsDir() {
    return appDataDir() / "logs";
}

fs::path logFile() {
    return logsDir() / "devdesk.log";
}

fs::path claudeConfigDirOverride() {
    if (const char* env = std::getenv("DEVDESK_CLAUDE_CONFIG_DIR")) {
        return fs::path(env) / "settings.json";
    }
    return {};
}

void ensureDirectoriesExist() {
    std::error_code ec;
    for (const auto& dir : {appDataDir(), markdownDir(), logsDir(), petsDir()}) {
        fs::create_directories(dir, ec);
        if (ec) {
            LOG_WARN("创建目录失败: " + dir.string() + " (" + ec.message() + ")");
        }
    }
}

} // namespace core::paths
