// tests/test_core/test_db_backup.cpp
// core::dbmaint：启动备份 / quick_check 损坏检测 / 还原 / 滚动清理 / once 闩 / 失败不炸。
// 本文件不覆盖 main.cpp 的 UI 决策路径（handleDbCorruption 弹框与退出在 main.cpp，
// devdesk_tests 不编 main.cpp——与既有 openAppDatabase 路径同口径）。
#include "doctest.h"
#include "test_support.h"

#include "core/db_backup.h"

#include <algorithm>
#include <filesystem>
#include <string>

#include <sqlite3.h>

namespace {

const std::string kPrefix = "db_backup_";

// 造一个「页头合法但内容烂」/「根本不是库」的坏文件：两种 quick_check 都必须判 false。
// 刻意不用合法 SQLite 头 16 字节开头（避免被误认成库）
void writeGarbageFile(const std::filesystem::path& p) {
    test_support::writeTextFile(p, "this is definitely not a sqlite database 1234567890");
}

// 用裸 sqlite3 连接数 sqlite_master 行数（>0 = 库可读且有表）；RAII 关闭
int countSqliteMasterRows(const std::filesystem::path& p, bool* ok) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(p.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        *ok = false;
        return -1;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_master", -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        *ok = false;
        return -1;
    }
    int n = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        n = sqlite3_column_int(stmt, 0);
        *ok = true;
    } else {
        *ok = false;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return n;
}

int countFilesWithPrefix(const std::filesystem::path& dir, const std::string& prefix) {
    int n = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->path().filename().string().compare(0, prefix.size(), prefix) == 0) {
            ++n;
        }
    }
    return n;
}

} // namespace

// ---------- 备份成功：文件存在、库可读有表 ----------

REGISTER_TEST(db_backup_create_produces_readable_db,
               "db_backup: createBackup 产出可读库（有表）", {
    test_support::TestDb tdb(kPrefix, "create");
    auto r = core::dbmaint::createBackup(tdb.path);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_REQUIRE(std::filesystem::exists(r.value()));
    bool ok = false;
    const int tables = countSqliteMasterRows(r.value(), &ok);
    DOCTEST_CHECK(ok);
    DOCTEST_CHECK(tables > 0);
    return true;
})

// ---------- 同进程 once 闩：第二次调用跳过，不重复备份 ----------

REGISTER_TEST(db_backup_once_latch_skips_second_call,
               "db_backup: performStartupBackupOnce 同进程只备份一次", {
    test_support::TestDb tdb(kPrefix, "once");
    const std::string backupPrefix = tdb.path.filename().string() + ".backup-";
    core::dbmaint::resetStartupBackupLatchForTests();
    auto first = core::dbmaint::performStartupBackupOnce(tdb.path);
    DOCTEST_REQUIRE(first.isOk());
    DOCTEST_REQUIRE(first.value().has_value());
    auto second = core::dbmaint::performStartupBackupOnce(tdb.path);
    DOCTEST_REQUIRE(second.isOk());
    DOCTEST_CHECK(!second.value().has_value());
    DOCTEST_CHECK_EQ(countFilesWithPrefix(tdb.path.parent_path(), backupPrefix), 1);
    core::dbmaint::resetStartupBackupLatchForTests();  // 别把闩留给后续用例
    return true;
})

// ---------- 滚动删除：7 份假备份 → 只剩最新 5 份 ----------

REGISTER_TEST(db_backup_prune_keeps_newest_five,
               "db_backup: pruneFilesWithPrefix 7 份假备份 → 剩最新 5 份", {
    test_support::TempDir dir(kPrefix + "prune");
    const std::string prefix = "devdesk.db.backup-";
    for (int i = 0; i < 7; ++i) {
        std::string name = prefix + "2026010" + std::to_string(i + 1) + "-000000";
        test_support::writeTextFile(dir.path / name, "fake");
    }
    auto r = core::dbmaint::pruneFilesWithPrefix(dir.path, prefix, 5);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value(), 2);
    DOCTEST_CHECK_EQ(countFilesWithPrefix(dir.path, prefix), 5);
    // 最新的两份 06/07 在，最旧的 01/02 没了（字典序 = 时间序）
    DOCTEST_CHECK(std::filesystem::exists(dir.path / (prefix + "20260106-000000")));
    DOCTEST_CHECK(std::filesystem::exists(dir.path / (prefix + "20260107-000000")));
    DOCTEST_CHECK(!std::filesystem::exists(dir.path / (prefix + "20260101-000000")));
    DOCTEST_CHECK(!std::filesystem::exists(dir.path / (prefix + "20260102-000000")));
    return true;
})

// createBackup 全链滚动：目录里已有 5 份假备份 → 备份后恰好 5 份（最旧假备份被顶掉）
REGISTER_TEST(db_backup_create_rolls_to_five,
               "db_backup: createBackup 在 5 份存量下滚动到 5 份", {
    test_support::TestDb tdb(kPrefix, "roll");
    const std::string prefix = tdb.path.filename().string() + ".backup-";
    const std::filesystem::path dir = tdb.path.parent_path();
    for (int i = 0; i < 5; ++i) {
        std::string name = prefix + "2026010" + std::to_string(i + 1) + "-000000";
        test_support::writeTextFile(dir / name, "fake");
    }
    auto r = core::dbmaint::createBackup(tdb.path, 5);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(countFilesWithPrefix(dir, prefix), 5);
    // 假备份最旧的 01 被顶掉，本次真备份（时间戳 ≥ 2026…）在内
    DOCTEST_CHECK(!std::filesystem::exists(dir / (prefix + "20260101-000000")));
    DOCTEST_CHECK_EQ(r.value().filename().string().compare(0, prefix.size(), prefix), 0);
    return true;
})

// ---------- quick_check 判损坏 ----------

REGISTER_TEST(db_backup_quick_check_detects_garbage,
               "db_backup: quickCheckOk 对垃圾文件判 false，对正常库判 true", {
    test_support::TempDir dir(kPrefix + "qc");
    const std::filesystem::path bad = dir.file("bad.db");
    writeGarbageFile(bad);
    auto badR = core::dbmaint::quickCheckOk(bad);
    DOCTEST_REQUIRE(badR.isOk());
    DOCTEST_CHECK(!badR.value());

    test_support::TestDb tdb(kPrefix, "qc_ok");
    auto goodR = core::dbmaint::quickCheckOk(tdb.path);
    DOCTEST_REQUIRE(goodR.isOk());
    DOCTEST_CHECK(goodR.value());
    return true;
})

// ---------- 还原流程：损坏库 + 有效备份 → 数据在；corrupt 只留 1 份 ----------

REGISTER_TEST(db_backup_restore_recovers_data,
               "db_backup: 还原后库可读且数据在，corrupt 只保留 1 份", {
    const std::filesystem::path dbPath = test_support::tempDbPath(kPrefix, "restore");
    std::error_code ec;
    std::filesystem::remove(dbPath, ec);
    {
        // 造有数据的主库：marker 表 + 一行数据
        auto r = core::Database::open(dbPath);
        DOCTEST_REQUIRE(r.isOk());
        core::Database db(std::move(r.value()));
        DOCTEST_REQUIRE(db.db().exec("CREATE TABLE restore_marker(v TEXT)").isOk());
        DOCTEST_REQUIRE(db.db().exec("INSERT INTO restore_marker VALUES ('hello')").isOk());
    }  // 连接关闭后才允许文件级操作
    auto backup = core::dbmaint::createBackup(dbPath);
    DOCTEST_REQUIRE(backup.isOk());

    // 损坏主库 → 判坏 → 还原 → 数据回来
    writeGarbageFile(dbPath);
    auto bad = core::dbmaint::quickCheckOk(dbPath);
    DOCTEST_REQUIRE(bad.isOk());
    DOCTEST_CHECK(!bad.value());

    auto restored = core::dbmaint::restoreFromBackup(dbPath, backup.value());
    DOCTEST_REQUIRE(restored.isOk());
    auto good = core::dbmaint::quickCheckOk(dbPath);
    DOCTEST_REQUIRE(good.isOk());
    DOCTEST_CHECK(good.value());
    bool ok = false;
    int rows = -1;
    sqlite3* raw = nullptr;
    DOCTEST_REQUIRE(sqlite3_open_v2(dbPath.string().c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    DOCTEST_REQUIRE(sqlite3_prepare_v2(raw, "SELECT v FROM restore_marker", -1, &stmt, nullptr) == SQLITE_OK);
    DOCTEST_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    DOCTEST_CHECK_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))), std::string("hello"));
    sqlite3_finalize(stmt);
    sqlite3_close(raw);
    (void)ok; (void)rows;

    // corrupt 隔离文件恰好 1 份
    const std::string corruptPrefix = dbPath.filename().string() + ".corrupt-";
    DOCTEST_CHECK_EQ(countFilesWithPrefix(dbPath.parent_path(), corruptPrefix), 1);

    // 再走一轮（造第二份 corrupt，字典序更大）：corrupt 仍只保留 1 份
    auto backup2 = core::dbmaint::createBackup(dbPath);
    DOCTEST_REQUIRE(backup2.isOk());
    writeGarbageFile(dbPath);
    auto restored2 = core::dbmaint::restoreFromBackup(dbPath, backup2.value());
    DOCTEST_REQUIRE(restored2.isOk());
    // 同秒内两轮时间戳可能相同（文件名同名覆盖）；睡过秒边界保证新 corrupt 字典序更大
    // ——不睡则第二份隔离名相同、覆盖旧份，结果同样是 1 份，断言不受影响
    DOCTEST_CHECK_EQ(countFilesWithPrefix(dbPath.parent_path(), corruptPrefix), 1);

    // 清理本用例目录外残留（tempDbPath 生成在 %TEMP% 根，逐个删）
    std::filesystem::remove(dbPath, ec);
    std::filesystem::remove(backup.value(), ec);
    std::filesystem::remove(backup2.value(), ec);
    std::filesystem::remove(restored.value(), ec);
    std::filesystem::remove(restored2.value(), ec);
    return true;
})

// ---------- 无备份分支 ----------

REGISTER_TEST(db_backup_latest_backup_none_when_absent,
               "db_backup: 无备份时 latestBackup 返回 nullopt", {
    test_support::TestDb tdb(kPrefix, "nobackup");
    auto r = core::dbmaint::latestBackup(tdb.path);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(!r.value().has_value());
    return true;
})

REGISTER_TEST(db_backup_latest_backup_picks_newest,
               "db_backup: 多份备份时 latestBackup 取字典序最大者", {
    test_support::TestDb tdb(kPrefix, "latest");
    const std::string prefix = tdb.path.filename().string() + ".backup-";
    test_support::writeTextFile(tdb.path.parent_path() / (prefix + "20260101-000000"), "old");
    test_support::writeTextFile(tdb.path.parent_path() / (prefix + "20260304-050607"), "new");
    auto r = core::dbmaint::latestBackup(tdb.path);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_REQUIRE(r.value().has_value());
    DOCTEST_CHECK_EQ(r.value()->filename().string(), prefix + "20260304-050607");
    return true;
})

// ---------- 备份失败不炸（Result fail，绝不异常） ----------

REGISTER_TEST(db_backup_failure_returns_fail_not_throw,
               "db_backup: 对不存在的库 createBackup 返回 fail 不炸", {
    test_support::TempDir dir(kPrefix + "fail");
    const std::filesystem::path missing = dir.file("missing.db");
    auto r = core::dbmaint::createBackup(missing);
    DOCTEST_CHECK(r.isFail());
    // once 包装同样只把失败传出去，不抛、不崩
    core::dbmaint::resetStartupBackupLatchForTests();
    auto once = core::dbmaint::performStartupBackupOnce(missing);
    DOCTEST_CHECK(once.isFail());
    core::dbmaint::resetStartupBackupLatchForTests();
    return true;
})

// ---------- 备份本身也坏 → 还原 fail，现场复原 ----------

REGISTER_TEST(db_backup_restore_fails_on_bad_backup,
               "db_backup: 备份本身坏 → 还原 fail 且主库保持原样", {
    const std::filesystem::path dbPath = test_support::tempDbPath(kPrefix, "badbak");
    std::error_code ec;
    std::filesystem::remove(dbPath, ec);
    {
        auto r = core::Database::open(dbPath);
        DOCTEST_REQUIRE(r.isOk());
        core::Database db(std::move(r.value()));
        DOCTEST_REQUIRE(db.db().exec("CREATE TABLE t(v TEXT)").isOk());
    }
    // 伪造一份「字典序最新」的坏备份
    const std::filesystem::path badBackup =
        dbPath.parent_path() / (dbPath.filename().string() + ".backup-20991231-235959");
    writeGarbageFile(badBackup);
    // 损坏主库
    writeGarbageFile(dbPath);
    auto r = core::dbmaint::restoreFromBackup(dbPath, badBackup);
    DOCTEST_CHECK(r.isFail());
    // 现场复原：主库文件还在（回到损坏前的原文件，即垃圾内容），没被弄丢
    DOCTEST_CHECK(std::filesystem::exists(dbPath));
    std::filesystem::remove(badBackup, ec);
    std::filesystem::remove(dbPath, ec);
    return true;
})

// ---------- 只读备份文件可被还原接受（MINOR-A：校验不得用 READWRITE 误拒只读备份） ----------

REGISTER_TEST(db_backup_restore_accepts_readonly_backup,
               "db_backup: 只读备份文件可被还原接受，且落盘主库清掉只读位", {
    const std::filesystem::path dbPath = test_support::tempDbPath(kPrefix, "robak");
    std::error_code ec;
    std::filesystem::remove(dbPath, ec);
    {
        auto r = core::Database::open(dbPath);
        DOCTEST_REQUIRE(r.isOk());
        core::Database db(std::move(r.value()));
        DOCTEST_REQUIRE(db.db().exec("CREATE TABLE t(v TEXT)").isOk());
        DOCTEST_REQUIRE(db.db().exec("INSERT INTO t VALUES ('data')").isOk());
    }
    auto backup = core::dbmaint::createBackup(dbPath);
    DOCTEST_REQUIRE(backup.isOk());

    // 用户拷贝来的/属性设了只读的备份：Windows 上映射 FILE_ATTRIBUTE_READONLY
    std::filesystem::permissions(backup.value(),
                                 std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::remove);
    DOCTEST_CHECK(
        (std::filesystem::status(backup.value()).permissions() &
         std::filesystem::perms::owner_write) == std::filesystem::perms::none);

    writeGarbageFile(dbPath);
    auto restored = core::dbmaint::restoreFromBackup(dbPath, backup.value());
    // 关键断言：只读备份不得被校验环节误拒
    DOCTEST_REQUIRE(restored.isOk());
    // 落盘主库的只读位被清掉（copy 连属性一起拷来，还原方负责恢复可写）
    DOCTEST_CHECK(
        (std::filesystem::status(dbPath).permissions() &
         std::filesystem::perms::owner_write) != std::filesystem::perms::none);
    auto good = core::dbmaint::quickCheckOk(dbPath, /*readOnlyOpen=*/true);
    DOCTEST_REQUIRE(good.isOk());
    DOCTEST_CHECK(good.value());
    // 落盘库可正常写开（还原产物真的可用）
    {
        sqlite3* raw = nullptr;
        DOCTEST_REQUIRE(sqlite3_open_v2(dbPath.string().c_str(), &raw,
                                        SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
        sqlite3_close(raw);
    }

    std::filesystem::remove(dbPath, ec);
    std::filesystem::remove(backup.value(), ec);
    std::filesystem::remove(restored.value(), ec);
    return true;
})

// ---------- 瞬态错误（BUSY）不触发损坏判定（MINOR-B：fail ≠ ok(false)） ----------
// 注入方式：rollback-journal 模式库被另一连接 BEGIN EXCLUSIVE 锁住，
// quickCheckOk 自开连接 prepare 时拿到 SQLITE_BUSY——fail 而不是 ok(false)，
// 调用方（main.cpp）据此跳过还原流程而不是弹还原对话框。

REGISTER_TEST(db_backup_busy_lock_is_not_corruption,
               "db_backup: BUSY 锁库 → quickCheckOk fail（检查失败），不判损坏", {
    const std::filesystem::path dbPath = test_support::tempDbPath(kPrefix, "busy");
    std::error_code ec;
    std::filesystem::remove(dbPath, ec);
    // 造一个非 WAL（默认 rollback journal）的合法库——WAL 下读不受写锁阻塞，注不出 BUSY
    sqlite3* maker = nullptr;
    DOCTEST_REQUIRE(sqlite3_open_v2(dbPath.string().c_str(), &maker,
                                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                                    nullptr) == SQLITE_OK);
    DOCTEST_REQUIRE(sqlite3_exec(maker, "CREATE TABLE t(v TEXT);", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(maker);

    sqlite3* locker = nullptr;
    DOCTEST_REQUIRE(sqlite3_open_v2(dbPath.string().c_str(), &locker,
                                    SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    sqlite3_busy_timeout(locker, 0);
    DOCTEST_REQUIRE(sqlite3_exec(locker, "BEGIN EXCLUSIVE;", nullptr, nullptr, nullptr) == SQLITE_OK);

    auto qc = core::dbmaint::quickCheckOk(dbPath);
    // 瞬态错误：isFail（检查失败），绝不能是 ok(false)（损坏）——损坏判定会触发还原流程
    DOCTEST_CHECK(qc.isFail());
    DOCTEST_CHECK(!qc.isOk());

    sqlite3_exec(locker, "ROLLBACK;", nullptr, nullptr, nullptr);
    sqlite3_close(locker);
    // 解锁后同一文件判 ok —— 证明上面的 fail 是瞬态锁导致、不是文件本身坏
    auto qcAfter = core::dbmaint::quickCheckOk(dbPath);
    DOCTEST_REQUIRE(qcAfter.isOk());
    DOCTEST_CHECK(qcAfter.value());
    std::filesystem::remove(dbPath, ec);
    return true;
})
