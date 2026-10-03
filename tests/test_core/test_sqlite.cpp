#include "doctest.h"
#include "core/database.h"
#include "core/sqlite_db.h"
#include "core/result.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <system_error>
#include <thread>
#include <type_traits>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

REGISTER_TEST(sqlite_open_memory, "SqliteDb: open in-memory", {
    auto r = core::data::SqliteDb::open(":memory:");
    DOCTEST_CHECK(r.isOk());
    return true;
})

REGISTER_TEST(sqlite_open_file, "SqliteDb: open file", {
    auto path = test_temp::tempPath("devdesk_test.db");
    // 预清理：无异常版本
    std::error_code ec;
    std::filesystem::remove(path, ec);
    {
        auto r = core::data::SqliteDb::open(path);
        DOCTEST_CHECK(r.isOk());
        if (r.isOk()) {
            auto exec = r.value().exec("CREATE TABLE t (id INTEGER, name TEXT)");
            DOCTEST_CHECK(exec.isOk());
        }
    } // SqliteDb 在此析构（关闭 sqlite 句柄），Windows 不允许删除打开中的文件
    std::filesystem::remove(path, ec);
    return true;
})

REGISTER_TEST(sqlite_bind_step, "SqliteDb: prepare + bind + step", {
    auto r = core::data::SqliteDb::open(":memory:");
    DOCTEST_REQUIRE(r.isOk());
    auto& db = r.value();
    DOCTEST_REQUIRE(db.exec("CREATE TABLE t (id INTEGER, name TEXT)").isOk());
    auto prep = db.prepare("INSERT INTO t VALUES (?, ?)");
    DOCTEST_REQUIRE(prep.isOk());
    auto& stmt = prep.value();
    DOCTEST_REQUIRE(stmt.bind(1, static_cast<int64_t>(1)).isOk());
    DOCTEST_REQUIRE(stmt.bind(2, std::string("hello")).isOk());
    auto step = stmt.step();
    DOCTEST_CHECK(step.isOk());
    return true;
})

REGISTER_TEST(sqlite_fetch_all, "SqliteDb: fetchAll", {
    auto r = core::data::SqliteDb::open(":memory:");
    DOCTEST_REQUIRE(r.isOk());
    auto& db = r.value();
    DOCTEST_REQUIRE(db.exec("CREATE TABLE t (id INTEGER, name TEXT)").isOk());
    DOCTEST_REQUIRE(db.exec("INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c')").isOk());
    auto prep = db.prepare("SELECT id, name FROM t ORDER BY id");
    DOCTEST_REQUIRE(prep.isOk());
    auto rows = prep.value().fetchAll();
    DOCTEST_REQUIRE(rows.isOk());
    DOCTEST_CHECK_EQ(rows.value().size(), 3u);
    return true;
})

REGISTER_TEST(sqlite_transaction, "SqliteDb: transaction commit", {
    auto r = core::data::SqliteDb::open(":memory:");
    DOCTEST_REQUIRE(r.isOk());
    auto& db = r.value();
    DOCTEST_REQUIRE(db.exec("CREATE TABLE t (x INTEGER)").isOk());
    auto tx = db.transaction([&]() -> core::Result<void> {
        if (db.exec("INSERT INTO t VALUES (1)").isFail()) return core::Result<void>::fail(core::ErrorCode::Internal, "insert 1");
        if (db.exec("INSERT INTO t VALUES (2)").isFail()) return core::Result<void>::fail(core::ErrorCode::Internal, "insert 2");
        return core::Result<void>::ok();
    });
    DOCTEST_CHECK(tx.isOk());
    auto prep = db.prepare("SELECT COUNT(*) FROM t");
    DOCTEST_REQUIRE(prep.isOk());
    auto rows = prep.value().fetchAll();
    DOCTEST_REQUIRE(rows.isOk());
    auto row = rows.value()[0];
    DOCTEST_CHECK_EQ(std::get<int64_t>(row.values[0]), 2);
    return true;
})
// 审计 2026-09-23 [8] 回归：transaction() 全程持锁，并发 INSERT 不再穿插
// 进开放事务。旧实现锁只盖 BEGIN/COMMIT，fn() 期间放开——worker 线程的
// exec INSERT 落进 UI 线程未提交的事务里，fn 失败 ROLLBACK 时被连带吞掉
// （表现为 count==0）。修复后 worker 阻塞在 BEGIN 前的锁上，事务结束才
// 落语句（count==1：只有自己的写被回滚）。
REGISTER_TEST(sqlite_transaction_serializes_concurrent_writers,
              "SqliteDb: transaction 全程持锁，并发 INSERT 不穿插（审计[8]）", {
    auto r = core::data::SqliteDb::open(":memory:");
    DOCTEST_REQUIRE(r.isOk());
    auto& db = r.value();
    DOCTEST_REQUIRE(db.exec(
        "CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, who TEXT)").isOk());

    std::atomic<bool> fnInside{false};
    core::Result<void> workerInsert = core::Result<void>::ok();

    std::thread worker([&] {
        while (!fnInside.load()) std::this_thread::yield();
        workerInsert = db.exec("INSERT INTO t (who) VALUES ('worker')");
    });

    auto tx = db.transaction([&]() -> core::Result<void> {
        auto p = db.prepare("INSERT INTO t (who) VALUES ('tx')");
        if (p.isFail()) return core::Result<void>::fail(p.errorCode(), p.errorMessage());
        p.value().step();
        fnInside.store(true);
        // 给 worker 充分的穿插机会（旧实现此时锁已放开）
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return core::Result<void>::fail(core::ErrorCode::Internal, "deliberate rollback");
    });
    worker.join();

    DOCTEST_CHECK(tx.isFail());
    DOCTEST_CHECK(workerInsert.isOk());

    auto prep = db.prepare("SELECT COUNT(*) AS c FROM t");
    DOCTEST_REQUIRE(prep.isOk());
    auto rows = prep.value().fetchAll();
    DOCTEST_REQUIRE(rows.isOk());
    DOCTEST_REQUIRE_EQ(rows.value().size(), 1u);
    auto c = rows.value()[0].getInt64("c");
    DOCTEST_REQUIRE(c.isOk());
    DOCTEST_CHECK_EQ(c.value(), static_cast<int64_t>(1));
    return true;
})

// 审计 L5 回归：析构时仍有未 finalize 的语句。旧实现用 sqlite3_close（返回
// SQLITE_BUSY 且被忽略）→ 连接静默不关、文件句柄泄漏；改 sqlite3_close_v2
// 后是延迟关闭：db 析构只是把连接变 zombie，最后一条语句 finalize 时才真正
// 回收。用例断言：析构不炸 + 语句 finalize 后 db 文件句柄确实还回来了
// （Windows 上「文件能否删除」即句柄是否泄漏的判据）。
REGISTER_TEST(sqlite_close_with_unfinalized_stmt,
              "SqliteDb: 未 finalize 语句存在时析构延迟关闭、句柄随 finalize 回收（审计 L5）", {
    auto path = test_temp::tempPath("devdesk_test_close_v2.db");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);

    std::optional<core::data::Statement> kept;
    {
        auto r = core::data::SqliteDb::open(path);
        DOCTEST_REQUIRE(r.isOk());
        auto& db = r.value();
        DOCTEST_REQUIRE(db.exec("CREATE TABLE t (id INTEGER)").isOk());
        auto prep = db.prepare("SELECT id FROM t");
        DOCTEST_REQUIRE(prep.isOk());
        // 语句比 db 活得久（移出内层作用域）。2026-10-02 起 Statement 持锁的
        // shared_ptr（不再是 db 的 mutex 引用），db 先亡也不悬垂——但连接此时已是
        // close_v2 的 zombie（未定义「继续 step」的语义），故本用例仍只让它析构；
        // 「宿主已亡的语句被跨线程继续用」的形状在 test_sqlite_concurrency.cpp。
        kept.emplace(std::move(prep.value()));
    } // db 在此析构：有未 finalize 的语句 → 延迟关闭，不得崩

    kept.reset(); // Statement 析构 → sqlite3_finalize → 延迟关闭此时生效

    DOCTEST_CHECK(std::filesystem::remove(path, ec)); // 句柄已归还（旧实现此处必失败）
    DOCTEST_CHECK_FALSE(std::filesystem::exists(path));
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
    return true;
})

// 审计 CP-07 回归：「未打开的空连接」不再是可表达状态——SqliteDb/Database 的
// 默认构造已私有化（open() 是唯一入口），下面两条 static_assert 在**编译期**
// 就拒绝 `SqliteDb db;` / `core::Database db;`；若有谁把默认构造放回 public，
// 本文件编译失败（用例跑到这里即通过）。
REGISTER_TEST(sqlite_no_default_ctor,
              "SqliteDb/Database: 默认构造已禁用，非法态不可表达（审计 CP-07）", {
    static_assert(!std::is_default_constructible_v<core::data::SqliteDb>,
                  "SqliteDb 不应可默认构造：SqliteDb::open() 是唯一入口");
    static_assert(!std::is_default_constructible_v<core::Database>,
                  "Database 不应可默认构造：Database::open() 是唯一入口");
    return true;
})

// ============ -wal/-shm 晚现时权限收敛（G8 简报 F1 m1 残留，2026-10-03 收口）============
//
// 缺口：SqliteDb::open 用 `BEGIN IMMEDIATE; COMMIT;` 空写事务把 -wal/-shm 逼出来
// 再收紧三件套。若该空事务失败（实测场景：数据目录暂不可写，rc=8 readonly），
// 伴生文件此刻缺席，open 的收紧对不存在的文件跳过——本会话稍后**首个写事务**
// 才创建它们，且若主库此刻是宽松权限，凭据页就留在组/其他可读的 -wal 里，
// 无人再收紧（macOS 系统 sqlite 在连接关闭后**不删**伴生文件，实测——暴露面
// 不止会话内，会一直留到下次 open）。
//
// ⚠ 平台实测（2026-10-03，macOS 系统 libsqlite3）：POSIX 上 sqlite 创建
// -wal/-shm 时**继承主库文件的权限位**（主库 0600/0644/0400 → 伴生文件出生即
// 同模式）。所以「主库已被 open 收紧到 0600」时晚现伴生文件会天生 0600——
// 「落回 umask 0644」只在主库此刻为宽松权限时成立，真实可达形态：
//   ① open 时的收紧本身失败（闩要重试的那条路）；
//   ② 外部工具以宽松权限重建三件套（多进程竞态）；
//   ③ 未来 sqlite 版本不再继承主库权限位（届时回到 umask 出生）。
// 用例因此显式把主库放宽到 0644 再做晚现写——把缺口变成可观测、断言非恒真，
// 不依赖 sqlite 的继承实现细节。
//
// 修法：open 里置「三件套未确认 0600」闩，此后每条成功写路径（exec / 写 step）
// 补一次收紧，三件套齐且全部收紧成功才落闩。下面三个用例分别打三条写路径，
// 共用同一缺口初态构造（openDbAfterFailedWarmup）：
//   阶段 1：正常 open + 写 + 关，落一个 WAL 模式主库；显式删 -wal/-shm 构造
//           「主库在、伴生文件缺席」初态；
//   阶段 2：目录收紧 0500 → open 里 PRAGMA journal_mode 与空事务**双双失败**
//           （伴生文件无法创建）→ 返回的连接即缺口现场（warm-up 失败只 WARN
//           不阻断）；随后恢复目录可写、把主库放宽到 0644（见上方 ⚠ 段）。
// 阶段 3 由用例本体做「晚现后的首个写」，断言三件套 == 0600。
// 修前本组必红（三件套停在 0644）；变异（摘掉写路径补收紧）同样必红。

namespace {

#if !defined(_WIN32)
// 实际权限位（低 12 位）。test_file_perms.cpp 有同款 modeOf，但在那边的匿名
// namespace 里无法跨文件复用；只被 POSIX 分支用到，按其注释纪律与守卫放在一起。
bool statModeOf(const std::filesystem::path& p, unsigned& outMode) {
    struct stat st{};
    if (::stat(p.string().c_str(), &st) != 0) return false;
    outMode = static_cast<unsigned>(st.st_mode) & 07777u;
    return true;
}
#endif

// 目录只读作用域：0555 期间无论断言在哪失败（fail-fast return false），析构都
// 恢复 0700——不把临时目录留在 remove_all 都清不掉的状态。
struct DirWriteRestore {
    std::filesystem::path dir;
    bool armed = false;
    ~DirWriteRestore() {
        if (!armed) return;
        std::error_code ec;
        std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec);
    }
};

// 构造「open 时空事务失败、-wal/-shm 缺席」的缺口现场（见上方三阶段注释）。
// 返回 false = 前置失败（已打印）；成功时 out 持有缺口现场的连接。
bool openDbAfterFailedWarmup(const std::filesystem::path& dir,
                             const std::filesystem::path& dbPath,
                             std::optional<core::data::SqliteDb>& out) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    DOCTEST_REQUIRE(!ec);
    {
        auto first = core::data::SqliteDb::open(dbPath);
        DOCTEST_REQUIRE(first.isOk());
        DOCTEST_REQUIRE(first.value().exec("CREATE TABLE t (x INTEGER)").isOk());
    }
    // 主库保留（WAL 头就在它里面），伴生文件显式删——不能依赖「关闭即删」：
    // macOS 系统 sqlite 实测 close 后伴生文件仍在。
    std::filesystem::remove(dbPath.string() + "-wal", ec);
    std::filesystem::remove(dbPath.string() + "-shm", ec);

    std::filesystem::permissions(
        dir,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace, ec);
    DOCTEST_REQUIRE(!ec);
    DirWriteRestore restore{dir, true};

    auto r = core::data::SqliteDb::open(dbPath);
    DOCTEST_REQUIRE(r.isOk());   // 空事务失败只 WARN、不阻断开库（既有口径）

    // 阶段 3 前半：恢复目录可写（RAII 兜底之外显式做，保证断言前目录已可写）
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
    DOCTEST_REQUIRE(!ec);
    restore.armed = false;

    // 把主库放宽到 0644（可观测化，见文件头 ⚠ 段：不放宽则晚现伴生文件继承
    // 主库 0600，断言恒真、变异不红）。等价于真实世界「open 收紧曾失败 /
    // 外部工具宽松重建 / 未来 sqlite 不继承」三种形态。
    std::filesystem::permissions(
        dbPath,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
            std::filesystem::perms::group_read | std::filesystem::perms::others_read,
        std::filesystem::perm_options::replace, ec);
    DOCTEST_REQUIRE(!ec);
    out.emplace(std::move(r.value()));
    return true;
}

// 三件套（db / -wal / -shm）全部存在且 0600。POSIX 分支真断言；Windows 的保护
// 级别是 DPAPI 密文 + 用户目录 ACL（file_perms_win 明确 no-op），权限位口径
// 不适用，只验形状（与 test_file_perms.cpp 同款平台分派）。
bool checkStoreTrioOwnerOnly(const std::filesystem::path& dbPath) {
#if defined(_WIN32)
    return true;
#else
    const std::filesystem::path trio[] = {
        dbPath,
        std::filesystem::path(dbPath.string() + "-wal"),
        std::filesystem::path(dbPath.string() + "-shm"),
    };
    for (const auto& f : trio) {
        std::error_code ec;
        DOCTEST_REQUIRE(std::filesystem::exists(f, ec));
        unsigned mode = 0;
        DOCTEST_REQUIRE(statModeOf(f, mode));
        DOCTEST_CHECK_MESSAGE(mode == 0600u,
                              f.filename().string() + " mode=" + std::to_string(mode));
    }
    return true;
#endif
}

} // namespace

REGISTER_TEST(sqlite_wal_late_exec,
              "SqliteDb: warm-up 失败后晚现的 -wal/-shm 收敛 0600（exec 写路径，G8 F1 m1 残留）", {
    const auto dir = test_temp::tempPath("devdesk_test_wallate_exec");
    const auto dbPath = dir / "gap.db";
    std::optional<core::data::SqliteDb> db;
    DOCTEST_REQUIRE(openDbAfterFailedWarmup(dir, dbPath, db));
    DOCTEST_REQUIRE(db->exec("INSERT INTO t VALUES (1)").isOk());   // 晚现后的首个写
    DOCTEST_REQUIRE(checkStoreTrioOwnerOnly(dbPath));
    db.reset();   // 先关连接（Windows 不允许删除打开中的文件）
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return true;
})

REGISTER_TEST(sqlite_wal_late_tx,
              "SqliteDb: warm-up 失败后晚现的 -wal/-shm 收敛 0600（事务路径，G8 F1 m1 残留）", {
    const auto dir = test_temp::tempPath("devdesk_test_wallate_tx");
    const auto dbPath = dir / "gap.db";
    std::optional<core::data::SqliteDb> db;
    DOCTEST_REQUIRE(openDbAfterFailedWarmup(dir, dbPath, db));
    auto tx = db->transaction([&]() -> core::Result<void> {
        auto p = db->prepare("INSERT INTO t VALUES (?)");
        if (p.isFail()) return core::Result<void>::fail(p.errorCode(), p.errorMessage());
        if (auto b = p.value().bind(1, static_cast<int64_t>(7)); b.isFail()) {
            return core::Result<void>::fail(b.errorCode(), b.errorMessage());
        }
        auto s = p.value().step();
        if (s.isFail()) return core::Result<void>::fail(s.errorCode(), s.errorMessage());
        return core::Result<void>::ok();
    });
    DOCTEST_REQUIRE(tx.isOk());
    DOCTEST_REQUIRE(checkStoreTrioOwnerOnly(dbPath));
    db.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return true;
})

REGISTER_TEST(sqlite_wal_late_stmt,
              "SqliteDb: warm-up 失败后晚现的 -wal/-shm 收敛 0600（autocommit step 写路径，G8 F1 m1 残留）", {
    const auto dir = test_temp::tempPath("devdesk_test_wallate_stmt");
    const auto dbPath = dir / "gap.db";
    std::optional<core::data::SqliteDb> db;
    DOCTEST_REQUIRE(openDbAfterFailedWarmup(dir, dbPath, db));
    auto p = db->prepare("INSERT INTO t VALUES (1)");
    DOCTEST_REQUIRE(p.isOk());
    DOCTEST_REQUIRE(p.value().step().isOk());
    DOCTEST_REQUIRE(checkStoreTrioOwnerOnly(dbPath));
    db.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return true;
})
