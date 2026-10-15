// 凭据落点不止主库：WAL 模式会把凭据页复制进 <db>-wal，-shm 同族。三件套必须一律
// 0600，且每次打开都校正——-wal/-shm 在连接关闭时被 sqlite 删掉，下次打开是全新 0644
// 文件，只在建库时设等于没设。
// 本用例只断言 macOS / Linux 的文件权限位；Windows 走 DPAPI 加密级 + no-op，真实断言
// 在 test_credential_protection.cpp 的 storesPlaintext 分支。
#include "doctest.h"
#include "test_support.h"

#include "core/database.h"
#include "core/file_perms.h"
#include "core/paths.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {

constexpr const char* kPermPrefix = "devdesk_test_fileperms_";

std::string readAll(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

#if !defined(_WIN32)
// 只被 checkStoreFilePerms 的 POSIX 分支调用。放到 `#if` 外面时，Windows 编译它们就是
// 无人引用的函数 → clang -Werror,-Wunused-function 直接红，MSYS2 GCC 同理。
// 只被平台分支用到的辅助函数必须和那个分支的守卫放在一起。
std::array<std::filesystem::path, 3> storeFiles(
    const std::filesystem::path& dbPath) {
    return {dbPath,
            std::filesystem::path(dbPath.string() + "-wal"),
            std::filesystem::path(dbPath.string() + "-shm")};
}

// 权限位 & 07777（低 12 位：suid/sgid/sticky + rwxrwxrwx）
bool modeOf(const std::filesystem::path& p, unsigned& outMode) {
    struct stat st{};
    if (::stat(p.string().c_str(), &st) != 0) return false;
    outMode = static_cast<unsigned>(st.st_mode) & 07777u;
    return true;
}
#endif

// 平台分支只能放在函数体内：doctest 宏参数里嵌预处理指令是 UB，-Werror,-Wembedded-directive 直接编译失败
bool checkStoreFilePerms(const std::filesystem::path& dbPath) {
#if defined(_WIN32)
    DOCTEST_CHECK(core::fileperms::restrictSqliteStoreFiles(dbPath).isOk());
#else
    const auto files = storeFiles(dbPath);
    int checked = 0;
    for (const auto& f : files) {
        std::error_code ec;
        if (!std::filesystem::exists(f, ec) || ec) continue;
        unsigned mode = 0;
        DOCTEST_REQUIRE(modeOf(f, mode));
        DOCTEST_CHECK_MESSAGE(mode == 0600u, f.filename().string() +
                                                 " mode=" + std::to_string(mode));
        ++checked;
    }
    // -wal / -shm 必须真的存在（SqliteDb::open 里那次空写事务把它们逼出来），否则本用例什么都没验
    DOCTEST_CHECK(checked >= 3);

    for (const auto& f : files) {
        std::error_code ec;
        if (!std::filesystem::exists(f, ec) || ec) continue;
        std::filesystem::permissions(
            f,
            std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_write |
                std::filesystem::perms::group_read |
                std::filesystem::perms::others_read,
            std::filesystem::perm_options::replace, ec);
        DOCTEST_REQUIRE(!ec);
        unsigned relaxed = 0;
        DOCTEST_REQUIRE(modeOf(f, relaxed));
        DOCTEST_CHECK_EQ(relaxed, 0644u);   // 先证明「放宽」真的生效，否则后面那条 0600 可能只是从没被改过
    }
    {
        auto reopened = core::Database::open(dbPath);
        DOCTEST_REQUIRE(reopened.isOk());
    }
    for (const auto& f : files) {
        std::error_code ec;
        if (!std::filesystem::exists(f, ec) || ec) continue;
        unsigned mode = 0;
        DOCTEST_REQUIRE(modeOf(f, mode));
        DOCTEST_CHECK_MESSAGE(mode == 0600u, "重开后 " + f.filename().string() +
                                                 " mode=" + std::to_string(mode));
    }
#endif
    return true;
}

} // namespace

REGISTER_TEST(fileperms_sqlite_store_files_are_owner_only,
              "file_perms: 主库 + -wal + -shm 一律 0600（打开时校正）", {
    test_support::TestDb tdb(kPermPrefix, "store");
    // 建一行 → 确保写事务真的发生过，凭证页正是靠这类写进 -wal 的
    DOCTEST_REQUIRE(tdb.handle().exec(
        "INSERT INTO claude_profiles (name, base_url, api_key_hint, "
        "api_key_check, api_key_encrypted, model, created_at, updated_at) "
        "VALUES ('p', 'http://x', 'sk-***', 'chk', 'sk-ant-secret', 'm', 0, 0)")
                        .isOk());

    DOCTEST_REQUIRE(checkStoreFilePerms(tdb.path));
    return true;
})

REGISTER_TEST(fileperms_restrict_is_idempotent_and_tolerates_missing,
              "file_perms: 缺失文件不算失败 + 重复调用幂等", {
    // 文件不存在 → ok（没有落点就没什么可保护），不是失败
    const std::string missing =
        test_support::tempDbPath(kPermPrefix, "missing").string() + "-nope";
    DOCTEST_CHECK(core::fileperms::restrictToOwnerOnly(missing).isOk());
    DOCTEST_CHECK(core::fileperms::restrictSqliteStoreFiles(missing).isOk());

    // 内存库无落点：不得报错
    DOCTEST_CHECK(core::fileperms::restrictSqliteStoreFiles(":memory:").isOk());
    return true;
})

REGISTER_TEST(paths_data_root_gitignore_content,
              "paths: 数据根 .gitignore 屏蔽 db 三件套 / config / run-settings", {
    test_support::DataRootEnv root(kPermPrefix, "gitignore");

    const std::filesystem::path gi = core::paths::dataRootGitignoreFile();
    DOCTEST_CHECK_EQ(gi.parent_path().string(), root.dir);

    DOCTEST_REQUIRE(core::paths::ensureDataRootGitignore().isOk());
    DOCTEST_REQUIRE(std::filesystem::exists(gi));

    const std::string text = readAll(gi);
    // 至少 `*.db` / `*.db-*` / `config.json*` / `run-settings/` 四条必须在内：少一条就有一条明文凭据路径会被 `git add .` 带走（-wal 里就有凭据页）
    DOCTEST_CHECK(text.find("*.db\n") != std::string::npos);
    DOCTEST_CHECK(text.find("*.db-*") != std::string::npos);
    // 启动备份 / corrupt 隔离文件（db_backup.cpp 产物）：后缀是 .backup-/.corrupt-，
    // `*.db-*` 匹配不到，必须有独立的两条
    DOCTEST_CHECK(text.find("*.db.backup-*") != std::string::npos);
    DOCTEST_CHECK(text.find("*.db.corrupt-*") != std::string::npos);
    DOCTEST_CHECK(text.find("config.json*") != std::string::npos);
    DOCTEST_CHECK(text.find("run-settings/") != std::string::npos);
    // 接管前的 settings.json env 快照（可能含用户真 token）就在数据根里，漏了它等于打穿这个文件存在的理由
    DOCTEST_CHECK(text.find("claude_settings_snapshot.json") != std::string::npos);
    // `!/.gitignore` 放行 .gitignore 自身：数据根要能进 git
    DOCTEST_CHECK(text.find("!/.gitignore") != std::string::npos);

    DOCTEST_REQUIRE(core::paths::ensureDataRootGitignore().isOk());
    DOCTEST_CHECK_EQ(readAll(gi), text);
    return true;
})

// ensureDataRootGitignore 的口径是「只追加缺失项」，绝不覆盖用户自己写的行
REGISTER_TEST(paths_gitignore_appends_only_and_preserves_user_rules,
              "paths: 数据根 .gitignore 只追加缺失项，用户规则逐字保留", {
    test_support::DataRootEnv root(kPermPrefix, "gi_keep");
    const std::filesystem::path gi = core::paths::dataRootGitignoreFile();

    const std::string userContent =
        "# 我自己写的规则（下面三行不许被应用改动）\n"
        "my-notes/\n"
        "*.tmp\n"
        "\n"
        "# 我故意注释掉 db 屏蔽，核对它不会被当成「已有规则」\n"
        "# *.db\n";
    {
        std::ofstream out(gi, std::ios::binary);
        DOCTEST_REQUIRE(out.good());
        out << userContent;
    }

    DOCTEST_REQUIRE(core::paths::ensureDataRootGitignore().isOk());
    const std::string after = readAll(gi);

    DOCTEST_CHECK(after.rfind(userContent, 0) == 0);
    DOCTEST_CHECK(after.find("my-notes/") != std::string::npos);
    DOCTEST_CHECK(after.find("*.tmp") != std::string::npos);

    // 必需项被补齐，包括上面那条被注释掉的：注释行不算「已有规则」
    DOCTEST_CHECK(after.find("\n*.db\n") != std::string::npos);
    DOCTEST_CHECK(after.find("*.db-*") != std::string::npos);
    DOCTEST_CHECK(after.find("*.db.backup-*") != std::string::npos);
    DOCTEST_CHECK(after.find("*.db.corrupt-*") != std::string::npos);
    DOCTEST_CHECK(after.find("config.json*") != std::string::npos);
    DOCTEST_CHECK(after.find("run-settings/") != std::string::npos);
    DOCTEST_CHECK(after.find("claude_settings_snapshot.json") != std::string::npos);

    DOCTEST_REQUIRE(core::paths::ensureDataRootGitignore().isOk());
    DOCTEST_CHECK_EQ(readAll(gi), after);
    return true;
})
