#include "sqlite_db.h"
#include "file_perms.h"
#include "logging.h"

#include <cstring>
#include <sqlite3.h>

namespace core::data {

namespace {
    // 把 sqlite3 返回码转成 ErrorCode
    ErrorCode mapErrCode(int rc) {
        switch (rc) {
            case SQLITE_OK: return ErrorCode::Ok;
            case SQLITE_READONLY: case SQLITE_PERM: return ErrorCode::FilePermissionDenied;
            case SQLITE_FULL: return ErrorCode::FileWriteFailed;
            case SQLITE_CORRUPT: return ErrorCode::DatabaseCorrupted;
            case SQLITE_BUSY: case SQLITE_LOCKED: return ErrorCode::DatabaseOpenFailed;
            default: return ErrorCode::Internal;
        }
    }

    Result<void> checkRc(sqlite3* db, int rc, const std::string& op) {
        if (rc != SQLITE_OK) {
            std::string err = sqlite3_errmsg(db);
            LOG_ERROR("SQLite " + op + " failed: " + err);
            return Result<void>::fail(mapErrCode(rc), op + ": " + err);
        }
        return Result<void>::ok();
    }

    std::string columnName(sqlite3_stmt* stmt, int idx) {
        const char* name = sqlite3_column_name(stmt, idx);
        return name ? name : "";
    }
}

bool SqlRow::isNull(const std::string& col) const {
    for (size_t i = 0; i < columnNames.size(); ++i) {
        if (columnNames[i] == col) {
            return std::holds_alternative<std::monostate>(values[i]);
        }
    }
    return true;
}

Result<int64_t> SqlRow::getInt64(const std::string& col) const {
    for (size_t i = 0; i < columnNames.size(); ++i) {
        if (columnNames[i] == col) {
            if (auto* v = std::get_if<int64_t>(&values[i])) return Result<int64_t>::ok(*v);
            return Result<int64_t>::fail(ErrorCode::ValidationError, "col not int64: " + col);
        }
    }
    return Result<int64_t>::fail(ErrorCode::NotFound, "col not found: " + col);
}

Result<double> SqlRow::getDouble(const std::string& col) const {
    for (size_t i = 0; i < columnNames.size(); ++i) {
        if (columnNames[i] == col) {
            if (auto* v = std::get_if<double>(&values[i])) return Result<double>::ok(*v);
            return Result<double>::fail(ErrorCode::ValidationError, "col not double: " + col);
        }
    }
    return Result<double>::fail(ErrorCode::NotFound, "col not found: " + col);
}

Result<std::string> SqlRow::getString(const std::string& col) const {
    for (size_t i = 0; i < columnNames.size(); ++i) {
        if (columnNames[i] == col) {
            if (auto* v = std::get_if<std::string>(&values[i])) return Result<std::string>::ok(*v);
            return Result<std::string>::fail(ErrorCode::ValidationError, "col not string: " + col);
        }
    }
    return Result<std::string>::fail(ErrorCode::NotFound, "col not found: " + col);
}

// ============ Statement ============

Statement::Statement(sqlite3* db, sqlite3_stmt* stmt,
                     std::shared_ptr<std::recursive_mutex> mutex)
    : db_(db), stmt_(stmt), mutex_(std::move(mutex)) {}

Statement::Statement(Statement&& other) noexcept
    : db_(other.db_), stmt_(other.stmt_), mutex_(other.mutex_) {
    other.db_ = nullptr;
    other.stmt_ = nullptr;
}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        releaseStmt();
        db_ = other.db_;
        stmt_ = other.stmt_;
        mutex_ = other.mutex_;   // 锁跟句柄走（shared_ptr，可赋值）
        other.db_ = nullptr;
        other.stmt_ = nullptr;
    }
    return *this;
}

// 跨线程纪律（2026-10-02 真机崩溃根因，见文件头注释）：finalize 也是 sqlite3*
// 句柄操作——它把 VDBE 还回连接、归还 lookaside 空闲链，必须与 prepare/step
// 取同一把锁。锁在这里取，不能推给调用方：ProfileRepository/ClaudeService 等
// 「纯读」路径只持每次调用的锁，Statement 析构点落在锁外（旧实现即此处裸调
// finalize → 与另一线程的 step 并发 → sqlite3DbMallocRawNNTyped 段错误）。
Statement::~Statement() { releaseStmt(); }

void Statement::releaseStmt() noexcept {
    if (!stmt_) return;
    {
        std::lock_guard<std::recursive_mutex> lock(*mutex_);
        sqlite3_finalize(stmt_);
    }
    stmt_ = nullptr;
}

// 审计 L5：bind 原不持锁（step/currentRow 持），跨线程共用同一语句时
// bind 与 step 可以交错——全部统一到同一把 recursive_mutex
Result<void> Statement::bind(int index, int64_t value) {
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    return checkRc(db_, sqlite3_bind_int64(stmt_, index, value), "bind_int64");
}

Result<void> Statement::bind(int index, double value) {
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    return checkRc(db_, sqlite3_bind_double(stmt_, index, value), "bind_double");
}

Result<void> Statement::bind(int index, const std::string& value) {
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    return checkRc(db_, sqlite3_bind_text(stmt_, index, value.c_str(),
        static_cast<int>(value.size()), SQLITE_TRANSIENT), "bind_text");
}

Result<void> Statement::bind(int index, std::nullptr_t) {
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    return checkRc(db_, sqlite3_bind_null(stmt_, index), "bind_null");
}

Result<void> Statement::bind(int index, const std::vector<uint8_t>& blob) {
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    return checkRc(db_, sqlite3_bind_blob(stmt_, index, blob.data(),
        static_cast<int>(blob.size()), SQLITE_TRANSIENT), "bind_blob");
}

Result<bool> Statement::step() {
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return Result<bool>::ok(true);
    if (rc == SQLITE_DONE) return Result<bool>::ok(false);
    return Result<bool>::fail(mapErrCode(rc),
        std::string("step: ") + sqlite3_errmsg(db_));
}

Result<SqlRow> Statement::currentRow() const {
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    SqlRow row;
    int n = sqlite3_column_count(stmt_);
    row.columnNames.reserve(n);
    row.values.reserve(n);
    for (int i = 0; i < n; ++i) {
        row.columnNames.push_back(columnName(stmt_, i));
        int type = sqlite3_column_type(stmt_, i);
        switch (type) {
            case SQLITE_INTEGER:
                row.values.emplace_back(static_cast<int64_t>(sqlite3_column_int64(stmt_, i)));
                break;
            case SQLITE_FLOAT:
                row.values.emplace_back(sqlite3_column_double(stmt_, i));
                break;
            case SQLITE_TEXT: {
                const unsigned char* t = sqlite3_column_text(stmt_, i);
                int len = sqlite3_column_bytes(stmt_, i);
                row.values.emplace_back(std::string(reinterpret_cast<const char*>(t), len));
                break;
            }
            case SQLITE_BLOB: {
                const void* b = sqlite3_column_blob(stmt_, i);
                int len = sqlite3_column_bytes(stmt_, i);
                std::vector<uint8_t> v(reinterpret_cast<const uint8_t*>(b),
                    reinterpret_cast<const uint8_t*>(b) + len);
                row.values.emplace_back(std::move(v));
                break;
            }
            default:
                row.values.emplace_back(std::monostate{});
                break;
        }
    }
    return Result<SqlRow>::ok(std::move(row));
}

Result<std::vector<SqlRow>> Statement::fetchAll() {
    std::vector<SqlRow> rows;
    while (true) {
        auto stepResult = step();
        if (stepResult.isFail()) return Result<std::vector<SqlRow>>::fail(
            stepResult.errorCode(), stepResult.errorMessage());
        if (!stepResult.value()) break;
        auto rowResult = currentRow();
        if (rowResult.isFail()) return Result<std::vector<SqlRow>>::fail(
            rowResult.errorCode(), rowResult.errorMessage());
        rows.push_back(std::move(rowResult).value());
    }
    return Result<std::vector<SqlRow>>::ok(std::move(rows));
}

int Statement::columnCount() const {
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    return sqlite3_column_count(stmt_);
}

// ============ SqliteDb ============

Result<SqliteDb> SqliteDb::open(const std::filesystem::path& file) {
    sqlite3* handle = nullptr;
    // SQLITE_OPEN_FULLMUTEX = 兜底（2026-10-02 评审收尾，防的是**将来**再漏一处锁）：
    // 本进程只有这一条连接、流量极小，代价只是 sqlite 内部的连接级互斥——单线程
    // 路径上就是一次无竞争的加解锁；换来的是任何未来漏掉的锁从「用户侧崩溃
    // （SIGSEGV/SIGBUS，见 sqlite_db.h 抬头）」降级成「正确串行化」。
    // 本机实测（macOS 系统 libsqlite3，sqlite3_threadsafe()==2）：裸 sqlite3_open 的
    // sqlite3_db_mutex(handle)==NULL（连接内部零自锁），加上本 flag 后非 NULL。
    //
    // 上面那把 recursive_mutex **必须留着**，FULLMUTEX 替代不了它：FULLMUTEX 只保证
    // **单次** sqlite3 API 调用互斥，盖不住跨调用的原子性——transaction() 的
    // BEGIN+fn()+COMMIT 整段、以及 sqlite3_changes / sqlite3_last_insert_rowid
    // 这类「连接级上次操作结果」与写语句的配对（AgentStore/TodoService 靠它判断
    // 这一行是否被自己改到），只有本仓库这把锁在保证；去掉它 FULLMUTEX 不会补上。
    int rc = sqlite3_open_v2(file.string().c_str(), &handle,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        std::string err = handle ? sqlite3_errmsg(handle) : "unknown";
        if (handle) sqlite3_close(handle);
        return Result<SqliteDb>::fail(ErrorCode::DatabaseOpenFailed,
            "open: " + err);
    }
    // 启用外键 + WAL 模式
    sqlite3_exec(handle, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    sqlite3_exec(handle, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);

    // 凭据落点保护（2026-10-02 存储统一 §6.3）：profile 的 API key 现在**就在
    // 库里**（macOS/POSIX 明文列 / Windows DPAPI 密文列），而 WAL 把凭据页复制进
    // -wal。三件套（db / -wal / -shm）一律收紧为「仅属主可读写」。
    // 坑：PRAGMA journal_mode=WAL 本身**不建** -wal/-shm（实测），它们是首个写
    // 事务才落盘——所以这里显式跑一次空写事务把三件套逼出来，再统一 chmod。
    // 每次 open 都做（不只在新建时设）：-wal/-shm 在连接关闭时被 sqlite 删除，
    // 下次打开是全新 0644 文件，只在建库时设等于没设。
    //
    // 失败处置（原注释声称「WARN 不阻断」，但返回值被直接丢弃 → 实际零日志）：
    //   ① 空事务失败（典型 SQLITE_BUSY，另一进程持锁）→ 取返回码并 LOG_WARN。
    //      不阻断开库：锁竞争是瞬时的，硬失败会让「另一个 DevDesk 开着」变成
    //      「本进程起不来」，比留着 0644 的 -wal 更糟。
    //   ② chmod 失败 → LOG_ERROR 不静默：那是「凭据可能被同机他人读到」。
    //
    // ⚠️ **残留缺口（本行不掩盖它）**：若 ① 失败，此刻 -wal/-shm 尚未被创建，
    // 下面的 restrictSqliteStoreFiles 对不存在的文件跳过（file_perms_posix.cpp:41
    // `if (!exists) continue`）。本会话内稍后由首个写事务创建的 -wal/-shm 因此
    // **不会**被 chmod，落回 umask 的 0644，凭证页就留在 0644 的 -wal 里。
    // 也就是说「每次打开都校正」只在空事务成功时成立。要彻底关掉这个洞需要把
    // 收紧动作放到一次**成功写事务之后**（例如 applySchema 落库后），那会动
    // sqlite_db.h 的成员或 database.cpp 的装配顺序，超出本条范围——记在此处，
    // 由后续批次接手，不要把上面 ① 的 WARN 当成「已经处理好了」。
    char* warmupErr = nullptr;
    const int warmupRc = sqlite3_exec(handle, "BEGIN IMMEDIATE; COMMIT;", nullptr,
                                       nullptr, &warmupErr);
    if (warmupRc != SQLITE_OK) {
        LOG_WARN(std::string("SQLite 空写事务失败（-wal/-shm 可能未被创建，"
                             "本会话稍后由写事务创建时不会自动 chmod 0600）：") +
                 (warmupErr ? warmupErr : sqlite3_errmsg(handle)));
    }
    if (warmupErr != nullptr) sqlite3_free(warmupErr);
    if (auto permR = core::fileperms::restrictSqliteStoreFiles(file); permR.isFail()) {
        LOG_ERROR("数据库文件权限收紧失败（凭据落点可能对同机其他用户可读）: " +
                  permR.errorMessage());
    }

    SqliteDb db;
    db.handle_ = handle;
    return Result<SqliteDb>::ok(std::move(db));
}

SqliteDb::~SqliteDb() {
    if (handle_) {
        // 审计 L5：原用 sqlite3_close，未 finalize 的语句会让它返回 SQLITE_BUSY
        // 且被忽略——句柄静默不关（Windows 上文件句柄泄漏）。close_v2 是延迟
        // 关闭语义：连接变 zombie，最后一条语句 finalize 时真正回收。
        // 跨线程纪律（2026-10-02）：close 同样是句柄操作，持锁执行——否则
        // 会和另一线程正在跑的最后一次 step 并发。
        std::lock_guard<std::recursive_mutex> lock(*mutex_);
        sqlite3_close_v2(handle_);
        handle_ = nullptr;
    }
}

// 移动不换锁：mutex_ 是 shared_ptr，被搬走的对象与搬入的对象共用同一把
// （旧实现每次移动都新造一把 mutex，锁与句柄就此分家——见 sqlite_db.h 注释）
SqliteDb::SqliteDb(SqliteDb&& other) noexcept
    : handle_(other.handle_), mutex_(other.mutex_) {
    other.handle_ = nullptr;
}

SqliteDb& SqliteDb::operator=(SqliteDb&& other) noexcept {
    if (this != &other) {
        if (handle_) {
            std::lock_guard<std::recursive_mutex> lock(*mutex_);
            // 与析构同款：close_v2 延迟关闭（原来用 sqlite3_close，语句未
            // finalize 时静默不关 → 句柄泄漏；审计 L5 的结论漏在了这条路径）
            sqlite3_close_v2(handle_);
        }
        handle_ = other.handle_;
        other.handle_ = nullptr;
        mutex_ = other.mutex_;
    }
    return *this;
}

Result<void> SqliteDb::exec(const std::string& sql) {
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    char* errMsg = nullptr;
    int rc = sqlite3_exec(handle_, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string err = errMsg ? errMsg : "unknown";
        sqlite3_free(errMsg);
        return Result<void>::fail(mapErrCode(rc), "exec: " + err);
    }
    return Result<void>::ok();
}

Result<Statement> SqliteDb::prepare(const std::string& sql) {
    // 审计 L5：prepare 与 exec/step/currentRow 共用同一把锁
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(handle_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Result<Statement>::fail(mapErrCode(rc),
            std::string("prepare: ") + sqlite3_errmsg(handle_));
    }
    return Result<Statement>::ok(Statement(handle_, stmt, mutex_));
}

Result<void> SqliteDb::transaction(std::function<Result<void>()> fn) {
    // 全程持锁（审计 2026-09-23 [8]）：BEGIN + fn() + COMMIT/ROLLBACK 必须
    // 在同一把 recursive_mutex 内完成。旧实现锁只盖住 BEGIN 和 COMMIT，
    // fn() 期间是放开的——recursive_mutex 只对同线程重入有效，跨线程没有
    // 任何串行：代理 worker 线程的单条 INSERT（logCall）能穿插进 UI 线程
    // 的开放事务（被 ROLLBACK 连带吞掉），两个线程的 BEGIN 也会互相撞
    // "cannot start a transaction within a transaction"，多语句原子性形同
    // 虚设。现在并发方在 BEGIN 前就阻塞在这把锁上，等事务结束才能落语句。
    // fn() 内部的 exec/prepare/step 取的是同一把锁（同线程重入，不死锁）；
    // 前提约束：fn() 内只做 DB 操作（现有全部调用方满足），不得做长阻塞
    // 的 IO/网络，否则会拖住其他线程的语句。
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    char* err = nullptr;
    if (sqlite3_exec(handle_, "BEGIN", nullptr, nullptr, &err) != SQLITE_OK) {
        std::string e = err ? err : "?";
        sqlite3_free(err);
        return Result<void>::fail(ErrorCode::DatabaseCorrupted, "BEGIN: " + e);
    }
    auto result = fn();
    if (result.isOk()) {
        err = nullptr;
        if (sqlite3_exec(handle_, "COMMIT", nullptr, nullptr, &err) != SQLITE_OK) {
            std::string e = err ? err : "?";
            sqlite3_free(err);
            return Result<void>::fail(ErrorCode::DatabaseCorrupted, "COMMIT: " + e);
        }
    } else {
        err = nullptr;
        sqlite3_exec(handle_, "ROLLBACK", nullptr, nullptr, &err);
        if (err) sqlite3_free(err);
    }
    return result;
}

Result<void> SqliteDb::applySchema(const std::string& sql) {
    // 单份当前 schema 幂等落库：整份脚本包在一个事务里（失败整体回滚，库里不留半截结构）。
    // 幂等性由脚本自身保证——DDL 全部 CREATE ... IF NOT EXISTS，种子 INSERT 自带
    // `WHERE NOT EXISTS` 守卫。没有 schema_version 表、没有版本比较、没有「数据库比
    // 二进制新」守卫：项目不迁移、不兼容旧数据（口径见 src/core/schema.h 抬头）。
    // 旧库不会被升级，也不为此加检测/兜底——缺列就是缺列，重来即可。
    auto txResult = transaction([&]() -> core::Result<void> {
        return exec(sql);
    });
    if (txResult.isFail()) {
        const std::string msg = "应用 schema 失败: " + txResult.errorMessage();
        LOG_ERROR(msg);
        return Result<void>::fail(ErrorCode::DatabaseMigrationFailed, msg);
    }
    LOG_INFO("数据库 schema 已就绪（幂等 schema 脚本）");
    return Result<void>::ok();
}

} // namespace core::data