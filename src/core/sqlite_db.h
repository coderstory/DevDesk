// src/core/sqlite_db.h
// SQLite 封装：单连接 + recursive_mutex。并发纪律：凡触碰 sqlite3* 句柄的调用一律在锁内
//（不只 prepare/step/bind）——macOS libsqlite3 为 THREADSAFE=2，连接内部零自锁，同一连接被
// 两线程同时调用会损坏内部 lookaside 空闲链（SIGSEGV 或挂死）。故 Statement 析构（finalize）、
// SqliteDb 关闭（close_v2）也在锁内完成，调用方不必也不能自己兜一层。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include <filesystem>

#include "result.h"

struct sqlite3;
struct sqlite3_stmt;

namespace core::data {

// SQLite 标量类型（替代 std::any，类型安全）
using SqlValue = std::variant<std::monostate, int64_t, double, std::string, std::vector<uint8_t>>;

// 一行结果：按列名索引到值
struct SqlRow {
    std::vector<std::string> columnNames;
    std::vector<SqlValue> values;

    bool isNull(const std::string& col) const;
    Result<int64_t> getInt64(const std::string& col) const;
    Result<double> getDouble(const std::string& col) const;
    Result<std::string> getString(const std::string& col) const;
};

// 「三件套权限未确认 0600」闩：open 的空写事务把 db/-wal/-shm 逼出来收紧，失败时伴生文件可能尚未
// 落盘（POSIX 上继承主库权限位），凭据页会留在组/其他可读的 -wal 里。置位期间每条成功写路径
//（exec/写 step，析构再兜底）补一次 restrictSqliteStoreFiles，三件套齐且全成功才落闩（落闩后零开销）。
// 必须 shared_ptr 共享：SqliteDb 装配期被移动数次，Statement 的写 step 也要补收紧
struct StorePermsLatch {
    std::filesystem::path file;          // 主库落点（:memory:/空路径 → 恒不置位）
    std::atomic<bool> pending{false};    // true = 三件套未确认 0600，写后要补收紧
    std::atomic<bool> warned{false};     // 补收紧失败只报一次（防高频写路径刷屏）
};

class Statement {
public:
    // mutex/version/perms 以 shared_ptr 共享而非裸引用：SqliteDb 装配期被移动数次，裸引用会让
    // 锁/计数器/闩留在被搬空的旧对象上 → 锁与句柄分家、脏标记永不置位、补收紧不再发生。共享所有权
    // 保证同一 sqlite3* 的 Statement 与其 SqliteDb 用同一把锁，语句比连接活得久时也不悬垂
    Statement(sqlite3* db, sqlite3_stmt* stmt,
              std::shared_ptr<std::recursive_mutex> mutex,
              std::shared_ptr<std::atomic<uint64_t>> version,
              std::shared_ptr<StorePermsLatch> perms);
    ~Statement();

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;

    Result<void> bind(int index, int64_t value);
    Result<void> bind(int index, double value);
    Result<void> bind(int index, const std::string& value);
    Result<void> bind(int index, std::nullptr_t);
    Result<void> bind(int index, const std::vector<uint8_t>& blob);

    Result<bool> step();
    Result<SqlRow> currentRow() const;
    Result<std::vector<SqlRow>> fetchAll();

    // 复位语句：复用同一条语句处理多行时换绑前必须先复位（否则第二次 bind/step 接着上次游标走）。
    // sqlite3_reset 是句柄操作 → 与 prepare/bind/step 同锁
    Result<void> reset();

    int columnCount() const;

private:
    // 释放语句（finalize）——析构与移动赋值共用；持锁
    void releaseStmt() noexcept;

    // 非只读语句改动库时推进共享版本号，必须在 mutex() 内调用（与 step 同一临界区）
    void bumpVersionOnWrite() noexcept;

    sqlite3* db_;
    sqlite3_stmt* stmt_;
    std::shared_ptr<std::recursive_mutex> mutex_;
    // 与 SqliteDb::writeVersion_ 共享同一计数器
    std::shared_ptr<std::atomic<uint64_t>> version_;
    // 与 SqliteDb::perms_ 共享同一把权限闩
    std::shared_ptr<StorePermsLatch> perms_;
    // 本语句是否写库：prepare 时由 sqlite3_stmt_readonly() 判一次并缓存（每步都查是白付代价，
    // 且已 prepare 的语句只读性不会再变）
    bool readonly_ = true;
};

class SqliteDb {
public:
    // 默认构造私有：open() 是唯一入口（返回 Result，失败即失败，不留半成品对象）；
    // 需可空/延迟打开语义的调用方用指针或 std::optional
    static Result<SqliteDb> open(const std::filesystem::path& file);
    ~SqliteDb();

    SqliteDb(const SqliteDb&) = delete;
    SqliteDb& operator=(const SqliteDb&) = delete;
    SqliteDb(SqliteDb&& other) noexcept;
    SqliteDb& operator=(SqliteDb&& other) noexcept;

    Result<void> exec(const std::string& sql);
    Result<Statement> prepare(const std::string& sql);

    Result<void> transaction(std::function<Result<void>()> fn);

    // 幂等执行整份 schema 脚本，整份包在一个事务里。无版本号、不升级旧库：
    // 脚本全是 CREATE ... IF NOT EXISTS + 带 NOT EXISTS 守卫的种子 INSERT
    Result<void> applySchema(const std::string& sql);

    std::recursive_mutex& mutex() { return *mutex_; }
    // 裸句柄逃生口：调用方须自持 mutex() 且锁覆盖到用完为止。仅供 sqlite3_changes /
    // last_insert_rowid 这类「连接级上次操作结果」与写语句的跨调用配对（FULLMUTEX 盖不住）
    sqlite3* handle() { return handle_; }

    // 写版本号：「这条连接上的数据被写过没有」的单一判据——每次语句真正改库就 +1。判据钉在
    // 「所有写必经的一层」，避免在各写路径记得挂钩子（漏一条会让读侧永久停在旧值且无报错）。
    // 读侧 acquire load、写侧 release fetch_add：读到新版本号即蕴含那些写先于本次读可见，于是
    // 「版本变了→重查」必含导致变化的那次写。别省成 relaxed，否则脏标记偶发不置位、UI 停在旧数据。
    uint64_t writeVersion() const {
        return writeVersion_->load(std::memory_order_acquire);
    }

private:
    SqliteDb() = default;   // 仅 open() 可用

    sqlite3* handle_ = nullptr;
    // 堆上分配且共享：移动 SqliteDb 不换锁对象，Statement 也不会拿到悬垂引用
    std::shared_ptr<std::recursive_mutex> mutex_ =
        std::make_shared<std::recursive_mutex>();
    // 与锁同理必须共享：值语义成员会留在被搬空的旧对象上 → 脏标记永不置位
    std::shared_ptr<std::atomic<uint64_t>> writeVersion_ =
        std::make_shared<std::atomic<uint64_t>>(0);
    // 三件套权限闩：open() 里创建并置位；与锁/版本号同理必须共享（默认 nullptr，补收紧对空闩安全跳过）
    std::shared_ptr<StorePermsLatch> perms_;
};

} // namespace core::data