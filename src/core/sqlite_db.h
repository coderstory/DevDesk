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

// 「三件套权限未确认 0600」闩（G8 简报 F1 m1 残留，2026-10-03 收口）。
// SqliteDb::open 的空写事务（BEGIN IMMEDIATE; COMMIT;）负责把 db/-wal/-shm
// 逼出来再统一收紧；它失败时伴生文件可能尚未落盘，open 的收紧对缺席文件跳过
// ——本会话稍后首个写事务才创建它们（POSIX 上还继承主库当时的权限位，主库
// 宽松时伴生文件就是宽松的），凭据页留在组/其他可读的 -wal 里。此闩 =「三件套
// 尚未全部确认 0600」：置位期间每条成功写路径（exec / 写 step，析构再兜底一次）
// 补一次 fileperms::restrictSqliteStoreFiles，三件套齐且全部收紧成功才落闩——
// 落闩后写路径零开销（一次 relaxed load）。
// 必须共享（shared_ptr 持有）：SqliteDb 装配期被移动数次，且 Statement 的写
// step 也要补收紧（与 mutex_/writeVersion_ 同一理由，见 Statement 构造注释）。
struct StorePermsLatch {
    std::filesystem::path file;          // 主库落点（:memory:/空路径 → 恒不置位）
    std::atomic<bool> pending{false};    // true = 三件套未确认 0600，写后要补收紧
    std::atomic<bool> warned{false};     // 补收紧失败只报一次（防高频写路径刷屏）
};

// Statement 句柄
class Statement {
public:
    // mutex 以 shared_ptr 共享（而非裸引用）：SqliteDb 在装配期要被移动数次
    // （Database::open → Database → Result<Database>），裸引用会让锁留在被搬空
    // 的旧对象上——锁与句柄就此分家，跨线程再无串行可言。共享所有权保证同一个
    // sqlite3* 的所有 Statement 与其 SqliteDb 锁的永远是同一把，且语句活得比
    // 连接久时锁也不会悬垂。
    //
    // version 同理是 shared_ptr：它是「这条连接上的数据变了没有」的**单一判据**
    // （见 SqliteDb::writeVersion）。裸指针/成员值都会在 SqliteDb 被移动时分家
    // ——语句推进的是搬空的旧计数器，读侧看的是新计数器，脏标记从此永不置位
    // （首页余量卡会永远停在旧值，且没有任何报错）。
    //
    // perms 同理（G8 F1 m1 残留收口，2026-10-03）：写 step 成功后要按闩补一次
    // 三件套权限收紧（见 StorePermsLatch）——SqliteDb 被移动后语句必须还能拿到
    // 同一把闩，否则移动后创建的语句就补不了收紧。
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

    // 复位语句：下一次 step() 从头开始跑（sqlite3_reset）。用途 = 复用**同一条**
    // 语句处理多行时换绑前必须先复位，否则第二次 bind/step 接着上次的游标走
    // （core/app_config.cpp 的 setMany 逐行 UPSERT 就靠它，一行一次 prepare
    // 是浪费）。sqlite3_reset 是句柄操作 → 与 prepare/bind/step 同锁
    // （2026-10-02 纪律，见文件头）。
    Result<void> reset();

    int columnCount() const;

private:
    // 释放语句（finalize）——析构与移动赋值共用；持锁，见 .cpp 注释
    void releaseStmt() noexcept;

    // 非只读语句真正改动库时推进共享版本号。**必须在 mutex() 内调用**
    // （与 step 同一临界区），实现见 .cpp。
    void bumpVersionOnWrite() noexcept;

    sqlite3* db_;
    sqlite3_stmt* stmt_;
    std::shared_ptr<std::recursive_mutex> mutex_;
    // 与 SqliteDb::writeVersion_ 共享同一计数器（见构造函数的注释）
    std::shared_ptr<std::atomic<uint64_t>> version_;
    // 与 SqliteDb::perms_ 共享同一把权限闩（见构造函数与 StorePermsLatch 的注释）
    std::shared_ptr<StorePermsLatch> perms_;
    // 本语句是否会写库。prepare 时由 sqlite3_stmt_readonly() 判一次并缓存：
    // step() 每次都要据此决定要不要推进版本号，而 sqlite3_stmt_readonly()
    // 是纯查 VDBE 程序、不碰数据，但每步都调一次仍属白付的代价。
    // 缓存它是安全的：一条已 prepare 完的语句，其「只读性」不会再变。
    bool readonly_ = true;
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

    // ---- 写版本号（2026-10-03 静态审计 M4 的失效判据）--------------------
    //
    // 「这条连接上的数据被写过没有」的**单一判据**：每次有语句真正改动库
    // （Statement::step 在非只读语句上跑到 SQLITE_DONE/ROW、或 exec 跑了
    // 非 SELECT 脚本）就 fetch_add(1)。
    //
    // **为什么放在这一层而不是各个写路径上**（这是本条设计的全部要点）：
    // 失效判据必须覆盖**所有**写入口，否则漏掉的那一条会让读侧永久停在旧值
    // 且无任何报错。本仓至少存在三条绕过「业务写路径」的合法写入口：
    //   ① 测试与探针的裸 SQL（tests/probes/home_probes.cpp 组 H(b) 直接
    //      `exec("DELETE FROM profile_quota_config")`，随后断言 compose 已反映）；
    //   ② core::app_config 的 setMany 等 core 层批量写；
    //   ③ 将来任何新增的写路径（今天的 QuotaStore 只是**恰好**是余量两表的
    //      唯一入口——check-quota-write-paths.sh 守的正是这条，而它连
    //      profile_quota_config 都没覆盖）。
    // 把判据钉在「所有写必经的那一层」，就不必在每个新写入口上记得挂钩子。
    //
    // 读侧用 acquire load（写侧 release fetch_add）：读到新版本号即意味着
    // 那些写在内存序上**先于**本次读可见，于是「版本变了 → 重查」重查到的
    // 一定包含导致它变化的那次写。这是 release/acquire 在这里唯一的作用，
    // 别把它省成 relaxed（省了就退化成「可能读到旧值」——于是脏标记偶发不置位，
    // UI 停在旧数据上，且是低概率、难复现的那种）。
    uint64_t writeVersion() const {
        return writeVersion_->load(std::memory_order_acquire);
    }

private:
    SqliteDb() = default;   // 仅 open()（本类静态成员）可用，见上方说明

    sqlite3* handle_ = nullptr;
    // 堆上分配且共享：移动 SqliteDb 不换锁对象（见 Statement 注释），
    // 引用它的 Statement 也不会因宿主移动/析构而拿到悬垂引用。
    std::shared_ptr<std::recursive_mutex> mutex_ =
        std::make_shared<std::recursive_mutex>();
    // 与锁同理必须共享：SqliteDb 在装配期被移动数次，值语义的成员会留在
    // 被搬空的旧对象上（Statement 推进旧计数器、读侧读新计数器 → 脏标记
    // 永不置位）。见 Statement 构造函数的注释。
    std::shared_ptr<std::atomic<uint64_t>> writeVersion_ =
        std::make_shared<std::atomic<uint64_t>>(0);
    // 三件套权限闩（G8 F1 m1 残留收口，见 StorePermsLatch）：open() 里创建并
    // 按三件套确认结果置位；与锁/版本号同理必须共享。open 是唯一构造入口，
    // 此处默认 nullptr，析构/移动路径的补收紧对空闩安全跳过。
    std::shared_ptr<StorePermsLatch> perms_;
};

} // namespace core::data