// src/core/sqlite_db.h
// SQLite 封装：单连接 + recursive_mutex（详见 spec §6.3）
//
// 并发纪律（2026-10-02 真机崩溃后固化，报告见
// .superpowers/sdd/2026-10-02-sqlite-thread-crash/fix-report.md）：
// **凡触碰 sqlite3* 句柄的调用，一律在锁内**——不只是 prepare/step/bind。
// 依据：macOS 系统 libsqlite3 编译为 THREADSAFE=2（多线程模式），
// `sqlite3_threadsafe()==2`、`sqlite3_db_mutex()` 返回 NULL —— 连接内部**没有**
// 任何自己的互斥锁，同一连接被两个线程同时调用就是直接损坏连接内部的
// lookaside 空闲链（表现为 sqlite3_step/sqlite3DbMallocRawNNTyped 段错误或挂死）。
// 因此 Statement 的析构（sqlite3_finalize）、SqliteDb 的关闭（sqlite3_close_v2）
// 也在锁内完成，调用方不必也不能自己兜一层。
#pragma once

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

// SQLite 支持的标量类型（替代 std::any，类型安全）
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

// Statement 句柄
class Statement {
public:
    // mutex 以 shared_ptr 共享（而非裸引用）：SqliteDb 在装配期要被移动数次
    // （Database::open → Database → Result<Database>），裸引用会让锁留在被搬空
    // 的旧对象上——锁与句柄就此分家，跨线程再无串行可言。共享所有权保证同一个
    // sqlite3* 的所有 Statement 与其 SqliteDb 锁的永远是同一把，且语句活得比
    // 连接久时锁也不会悬垂。
    Statement(sqlite3* db, sqlite3_stmt* stmt,
              std::shared_ptr<std::recursive_mutex> mutex);
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

    int columnCount() const;

private:
    // 释放语句（finalize）——析构与移动赋值共用；持锁，见 .cpp 注释
    void releaseStmt() noexcept;

    sqlite3* db_;
    sqlite3_stmt* stmt_;
    std::shared_ptr<std::recursive_mutex> mutex_;
};

class SqliteDb {
public:
    // 审计 CP-07：默认构造私有化——「未打开的空连接」不再是可表达状态，
    // open() 是唯一入口（返回 Result，失败即失败，不留半成品对象）。
    // 确实需要可空/延迟打开语义的调用方用指针或 std::optional 表达。
    static Result<SqliteDb> open(const std::filesystem::path& file);
    ~SqliteDb();

    SqliteDb(const SqliteDb&) = delete;
    SqliteDb& operator=(const SqliteDb&) = delete;
    SqliteDb(SqliteDb&& other) noexcept;
    SqliteDb& operator=(SqliteDb&& other) noexcept;

    Result<void> exec(const std::string& sql);
    Result<Statement> prepare(const std::string& sql);

    Result<void> transaction(std::function<Result<void>()> fn);

    // 幂等执行整份 schema 脚本（core::schema::kSql），整份包在一个事务里。
    // 无版本号、无 schema_version 表、**不升级旧库**：脚本全是 CREATE ... IF NOT
    // EXISTS + 一条带 NOT EXISTS 守卫的种子 INSERT，连跑多次结果相同。
    Result<void> applySchema(const std::string& sql);

    std::recursive_mutex& mutex() { return *mutex_; }
    // 裸句柄逃生口：**调用方须自持 mutex()**，且锁须覆盖到用完为止（同仓库惯例，
    // 见 proxy_router.cpp:118 lastInsertRowId 的调用点注释）。
    // 逃生口的理由：sqlite3_changes / sqlite3_last_insert_rowid 这类「连接级上次
    // 操作结果」只能从裸句柄取。它们与写语句的配对是**跨调用**原子性，
    // SQLITE_OPEN_FULLMUTEX（见 sqlite_db.cpp open()）只锁单次 API 调用、盖不住。
    sqlite3* handle() { return handle_; }

private:
    SqliteDb() = default;   // 仅 open()（本类静态成员）可用，见上方说明

    sqlite3* handle_ = nullptr;
    // 堆上分配且共享：移动 SqliteDb 不换锁对象（见 Statement 注释），
    // 引用它的 Statement 也不会因宿主移动/析构而拿到悬垂引用。
    std::shared_ptr<std::recursive_mutex> mutex_ =
        std::make_shared<std::recursive_mutex>();
};

} // namespace core::data