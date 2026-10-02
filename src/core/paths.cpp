#include "paths.h"
#include "atomic_write.h"
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

fs::path dataRootGitignoreFile() {
    // 覆盖优先级：DEVDESK_GITIGNORE_PATH（整文件路径，测试隔离用）>
    // DEVDESK_DATA_ROOT（<根>/.gitignore）> 平台默认根
    if (const char* env = std::getenv("DEVDESK_GITIGNORE_PATH")) {
        if (*env != '\0') return fs::path(env);
    }
    return appDataDir() / ".gitignore";
}

core::Result<void> ensureDataRootGitignore() {
    // 内容固定且短：每次整体重写（幂等），这样后续新增忽略项对既有用户也立即
    // 生效。为什么这些条目（2026-10-02 存储统一 §6.3 硬约束 1）：
    //   *.db / *.db-*  库与 WAL 伴生件（WAL 会把凭据页复制进去；-shm 同族）
    //   config.json*   应用配置与它的 .bak 备份
    //   run-settings/  每会话临时 settings（带明文 API key 的那个目录）
    //   logs/          日志（含 profile 名与路径，不必进仓库）
    //   pets/          皮肤（混了用户自加皮肤，属本机资产）
    // 结尾显式放行 .gitignore 自身——数据根本身要能进 git。
    static constexpr const char* kContent =
        "# DevDesk 数据根忽略规则（本文件由应用自动写入，可自行追加）\n"
        "# 为什么：凭据存在 devdesk.db 里，WAL 伴生文件会把凭据页复制进去；\n"
        "# 数据根若被 `git add .` 扫走，密钥就跟着上远端。\n"
        "*.db\n"
        "*.db-*\n"
        "config.json*\n"
        "run-settings/\n"
        "logs/\n"
        "pets/\n"
        "!/.gitignore\n";

    const fs::path file = dataRootGitignoreFile();
    std::error_code ec;
    if (!file.parent_path().empty()) {
        fs::create_directories(file.parent_path(), ec);
        if (ec) {
            return core::Result<void>::fail(
                core::ErrorCode::FileWriteFailed,
                "创建数据根失败: " + file.parent_path().string() + ": " +
                    ec.message());
        }
    }
    auto written = core::atomicWriteFile(file, kContent, ".gitignore");
    if (written.isFail()) {
        return core::Result<void>::fail(written.errorCode(),
                                        written.errorMessage());
    }
    return core::Result<void>::ok();
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
