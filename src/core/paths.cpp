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

namespace {

// 数据根 .gitignore 必须包含的屏蔽项（2026-10-02 存储统一 §6.3 硬约束 1；
// 2026-10-03 安全审计 F2 补 claude_settings_snapshot.json——它就在数据根里、
// 0644、可能含用户真实 token，漏了它等于打穿这个文件存在的理由）。
constexpr const char* kRequiredGitignoreLines[] = {
    "*.db",                           // 主库（凭据进库后凭据页就在里面）
    "*.db-*",                         // WAL 伴生（-wal / -shm，会复制凭据页）
    "config.json*",                   // 旧配置文件的磁盘残骸（E2 后应用不再读写，
                                      // 但用户盘上可能还有，仍须防误提交）
    "run-settings/",                  // 每会话临时 settings（含明文 API key）
    "claude_settings_snapshot.json",  // 接管前 settings.json 原 env 快照（可能含真 token）
    "logs/",                          // 日志（含 profile 名与路径）
    "pets/",                          // 桌宠皮肤（混了用户自加皮肤，属本机资产）
    "!/.gitignore",                   // 放行自身（数据根本身要能进 git）
};

// trimAscii 已下沉 core::str（2026-10-03 轻量化：与
// module_markdown/markdown_service.cpp 那份逐字等价，合并到 core/str_util.h）。
using core::str::trimAscii;

std::string readWholeFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::string();
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

} // namespace

core::Result<void> ensureDataRootGitignore() {
    // 幂等**追加**语义（2026-10-03 安全审计 F4）：原实现每次启动整体重写，
    // 用户自己加的规则会被静默清掉——而文件正文还写着「可自行追加」，自相矛盾。
    // 现在的口径：读现有内容 → 只补缺失的必需行 → 写回；内容已齐全则**一个字节
    // 都不动**（连 mtime 都不变）。
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

    const std::string existing = readWholeFile(file);

    // 已有的**规则行**（跳过空行与注释行——`# *.db` 是用户注释掉的规则，不算已有）
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
