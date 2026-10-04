#include "core/fs_text.h"
#include "paths.h"
#include "atomic_write.h"
#include "logging.h"
#include "str_util.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace core::paths {

// appDataDir() 平台默认实现在 paths_*.cpp，CMake 按平台选编（零平台宏）

namespace {

// DEVDESK_DATA_ROOT 覆盖（单一数据根入口）：已设且非空 → 该目录；未设/空串 → 空 path（调用方
// 回落平台默认）。只此一处读数据根，其它路径一律经 appDataDir() 派生
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

fs::path sqliteDbFile() {
    return appDataDir() / "devdesk.db";
}

fs::path dataRootGitignoreFile() {
    // 覆盖优先级：DEVDESK_GITIGNORE_PATH > DEVDESK_DATA_ROOT（<根>/.gitignore）> 平台默认根
    if (const char* env = std::getenv("DEVDESK_GITIGNORE_PATH")) {
        if (*env != '\0') return fs::path(env);
    }
    return appDataDir() / ".gitignore";
}

namespace {

// 数据根 .gitignore 必须包含的屏蔽项
constexpr const char* kRequiredGitignoreLines[] = {
    "*.db",                           // 主库（凭据进库后凭据页就在里面）
    "*.db-*",                         // WAL 伴生（-wal / -shm，会复制凭据页）
    "config.json*",                   // 旧配置文件残骸（应用不再读写，但用户盘上可能还有）
    "run-settings/",                  // 每会话临时 settings（含明文 API key）
    "claude_settings_snapshot.json",  // 接管前 settings.json 原 env 快照（可能含真 token）
    "logs/",                          // 日志（含 profile 名与路径）
    "pets/",                          // 桌宠皮肤（混了用户自加皮肤，属本机资产）
    "!/.gitignore",                   // 放行自身（数据根本身要能进 git）
};

using core::str::trimAscii;

} // namespace

core::Result<void> ensureDataRootGitignore() {
    // 幂等追加语义：读现有内容 → 只补缺失的必需行 → 写回；已齐全则一个字节都不动（连 mtime 都不变）。
    // 不整体重写，用户自己加的规则不会被清掉
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

    const std::string existing = core::readTextFile(file);

    // 已有的规则行（跳过空行与注释行）
    std::set<std::string> present;
    {
        std::istringstream iss(existing);
        std::string line;
        while (std::getline(iss, line)) {
            const std::string t = trimAscii(line);
            if (t.empty() || t[0] == '#') continue;
            present.insert(t);
        }
    }

    std::string missing;
    for (const char* want : kRequiredGitignoreLines) {
        if (present.count(want) == 0) {
            missing += want;
            missing += '\n';
        }
    }

    if (existing.empty()) {
        static constexpr const char* kHeader =
            "# DevDesk 数据根忽略规则（本文件由应用维护，可自行追加规则）\n"
            "# 为什么需要它：凭据存在 devdesk.db 里，WAL 伴生文件会把凭据页复制进去；\n"
            "# 数据根若被 `git add .` 扫走，密钥就跟着上远端。\n"
            "# 应用只**追加**缺失的必需项，不动你写的任何一行。\n";
        return core::atomicWriteFile(file, std::string(kHeader) + missing,
                                     ".gitignore");
    }

    if (missing.empty()) {
        return core::Result<void>::ok();   // 齐全 → 不动内容（用户规则原样保留）
    }

    std::string content = existing;
    if (content.back() != '\n') content += '\n';
    content += "\n# ↓ 以下为 DevDesk 自动补充的缺失屏蔽项（你原有的内容未改动）\n";
    content += missing;
    return core::atomicWriteFile(file, content, ".gitignore");
}

fs::path markdownDir() {
    // 覆盖优先级：DEVDESK_NOTES_DIR > DEVDESK_DATA_ROOT > 平台默认根
    if (const char* env = std::getenv("DEVDESK_NOTES_DIR")) {
        return fs::path(env);
    }
    return appDataDir() / "notes";
}

fs::path petsDir() {
    // 覆盖优先级：DEVDESK_PETS_DIR > DEVDESK_DATA_ROOT > 平台默认根
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
