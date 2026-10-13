// SQLite 封装：单连接 + recursive_mutex。并发纪律：凡触碰 sqlite3* 句柄的调用一律在锁内
//（不只 prepare/step/bind）——macOS libsqlite3 为 THREADSAFE=2，连接内部零自锁，两线程同调
// 会损坏内部 lookaside 空闲链；Statement 析构、close_v2 也在锁内完成，调用方不必自己兜一层
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

// 三件套权限未确认 0600 闩：open 的空写事务失败时伴生文件可能尚未落盘，凭据页留在组/其他可读的
// -wal 里；置位期间每条成功写路径补一次 restrictSqliteStoreFiles，三件套齐且全成功才落闩。
// 必须 shared_ptr 共享：SqliteDb 装配期被移动数次，Statement 的写 step 也要补收紧
struct StorePermsLatch {
    std::filesystem::path file;          // 主库落点（:memory:/空路径 → 恒不置位）
    std::atomic<bool> pending{false};    // true = 三件套未确认 0600，写后要补收紧
    std::atomic<bool> warned{false};     // 补收紧失败只报一次（防高频写路径刷屏）
};

class Statement {
public:
    // mutex/version/perms 以 shared_ptr 共享而非裸引用：SqliteDb 装配期被移动数次，裸引用会把
    // 锁/计数器/闩留在被搬空的旧对象上；共享所有权保证同一 sqlite3* 的语句与连接共用同一把锁
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

    // 复位语句：换绑前必须先复位（否则第二次 bind/step 接着上次游标走）；sqlite3_reset 是句柄操作，同锁
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
    std::shared_ptr<std::atomic<uint64_t>> version_;   // 与 SqliteDb::writeVersion_ 共享同一计数器
    std::shared_ptr<StorePermsLatch> perms_;           // 与 SqliteDb::perms_ 共享同一权限闩
    // 本语句是否写库：prepare 时由 sqlite3_stmt_readonly() 判一次并缓存
    bool readonly_ = true;
};

class SqliteDb {
public:
    // open() 是唯一入口（返回 Result，失败不留半成品对象）；需可空/延迟打开语义用指针或 std::optional
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
    // 裸句柄逃生口：调用方须自持 mutex() 且锁覆盖到用完为止；仅供连接级上次操作结果的跨调用配对
    sqlite3* handle() { return handle_; }

    // 写版本号：连接上「数据被写过没有」的单一判据，每次真正改库 +1（判据钉在所有写必经的一层）。
    // 读侧 acquire / 写侧 release：读到新版本 ⇒ 那些写已可见；别省成 relaxed，否则脏标记偶发不置位
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