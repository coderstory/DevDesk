#include "sqlite_db.h"
#include "file_perms.h"
#include "logging.h"

#include <cstddef>
#include <cstring>
#include <sqlite3.h>
#include <utility>

namespace core::data {

namespace {
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

    // 写路径补收紧：open 时空写事务失败时 -wal/-shm 可能尚未创建（POSIX 上继承主库权限位）。
    // 三件套齐（count>=3）且全部成功才落闩（只认 ok 会把「文件还没出现」误当完成）。并发安全：
    // pending/warned 均 atomic，只碰文件元数据不碰句柄；失败只报一次防高频写路径刷屏
    void retightenStoreFilesAfterWrite(const std::shared_ptr<StorePermsLatch>& perms) {
        if (!perms || !perms->pending.load(std::memory_order_relaxed)) return;
        auto r = core::fileperms::restrictSqliteStoreFiles(perms->file);
        if (r.isOk() && r.value() >= 3) {
            perms->pending.store(false, std::memory_order_relaxed);
            return;
        }
        if (r.isFail()) {
            bool alreadyWarned = false;
            if (perms->warned.compare_exchange_strong(alreadyWarned, true)) {
                LOG_ERROR("数据库文件权限补收紧失败（-wal/-shm 可能以宽松权限存在，"
                          "凭据落点可被同机其他用户读取）: " + r.errorMessage());
            }
        }
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

Statement::Statement(sqlite3* db, sqlite3_stmt* stmt,
                     std::shared_ptr<std::recursive_mutex> mutex,
                     std::shared_ptr<std::atomic<uint64_t>> version,
                     std::shared_ptr<StorePermsLatch> perms)
    : db_(db), stmt_(stmt), mutex_(std::move(mutex)),
      version_(std::move(version)), perms_(std::move(perms)) {
    // 只读性判一次并缓存：sqlite3_stmt_readonly() 纯查 VDBE，已 prepare 的语句结论不再变。
    // SELECT/EXPLAIN/BEGIN/PRAGMA 读法为 true，INSERT/UPDATE/DELETE/REPLACE/DDL 为 false
    readonly_ = (sqlite3_stmt_readonly(stmt_) != 0);
}

Statement::Statement(Statement&& other) noexcept
    : db_(other.db_), stmt_(other.stmt_), mutex_(other.mutex_),
      version_(other.version_), perms_(other.perms_), readonly_(other.readonly_) {
    other.db_ = nullptr;
    other.stmt_ = nullptr;
}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        releaseStmt();
        db_ = other.db_;
        stmt_ = other.stmt_;
        mutex_ = other.mutex_;   // 锁跟句柄走（shared_ptr，可赋值）
        version_ = other.version_;
        perms_ = other.perms_;   // 权限闩同理跟句柄走（见 StorePermsLatch 注释）
        readonly_ = other.readonly_;
        other.db_ = nullptr;
        other.stmt_ = nullptr;
    }
    return *this;
}

// finalize 也是句柄操作（还 VDBE/归还 lookaside 空闲链），必须与 prepare/step 同锁。
// 锁在此取不能推给调用方：Statement 析构点常在调用方锁外，裸调 finalize 会与其他线程 step 并发而崩溃
Statement::~Statement() { releaseStmt(); }

void Statement::releaseStmt() noexcept {
    if (!stmt_) return;
    {
        std::lock_guard<std::recursive_mutex> lock(*mutex_);
        sqlite3_finalize(stmt_);
    }
    stmt_ = nullptr;
}

// 推进写版本号，只在锁内且只对非只读语句调用。release 与读侧 acquire 配对，
// 保证「读到新版本」蕴含「导致它的写已可见」
void Statement::bumpVersionOnWrite() noexcept {
    if (readonly_ || !version_) return;
    version_->fetch_add(1, std::memory_order_release);
}

// bind 与 step/currentRow 统一到同一把 recursive_mutex（否则跨线程共用同一语句时 bind/step 可交错）
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
    if (rc == SQLITE_ROW) {
        // 非只读语句返回 ROW 只可能是 RETURNING（写语句的结果行），算已写。
        bumpVersionOnWrite();
        retightenStoreFilesAfterWrite(perms_);
        return Result<bool>::ok(true);
    }
    if (rc == SQLITE_DONE) {
        bumpVersionOnWrite();
        // 写 step 成功即伴生文件已出现的点，闩未落就补收紧（只读语句被 readonly_ 排除）
        retightenStoreFilesAfterWrite(perms_);
        return Result<bool>::ok(false);
    }
    // 失败不推进版本号：失败语句可能已改库，但宁可漏置（多查一次可接受）——漏置由下一次真写纠正
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

// sqlite3_reset 是句柄操作，与 prepare/bind/step 同一把锁。注意其返回值是上一次 step 的结果，
// 不是本次操作的——step 失败时不要靠它判断成败
Result<void> Statement::reset() {
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    return checkRc(db_, sqlite3_reset(stmt_), "reset");
}

int Statement::columnCount() const {
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    return sqlite3_column_count(stmt_);
}

Result<SqliteDb> SqliteDb::open(const std::filesystem::path& file) {
    sqlite3* handle = nullptr;
    // SQLITE_OPEN_FULLMUTEX 兜底：macOS libsqlite3 是 THREADSAFE=2，裸 open 的连接内部零自锁，
    // 加此 flag 至少让未来漏掉的锁从崩溃降级为串行化。但 recursive_mutex 必须留着——FULLMUTEX
    // 只锁单次 API 调用，盖不住 transaction() 的 BEGIN+fn()+COMMIT 整段与 sqlite3_changes 等跨调用配对
    int rc = sqlite3_open_v2(file.string().c_str(), &handle,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        std::string err = handle ? sqlite3_errmsg(handle) : "unknown";
        if (handle) sqlite3_close(handle);
        return Result<SqliteDb>::fail(ErrorCode::DatabaseOpenFailed,
            "open: " + err);
    }
    sqlite3_exec(handle, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    sqlite3_exec(handle, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);

    // 凭据落点保护：API key 就在库里、WAL 把凭据页复制进 -wal，故 db/-wal/-shm 三件套一律收紧为
    // 仅属主可读写。PRAGMA journal_mode=WAL 本身不建 -wal/-shm（首个写事务才落盘），这里显式跑一次
    // 空写事务逼出三件套再统一 chmod；且每次 open 都做（-wal/-shm 连接关闭即删，下次是全新 0644）。
    // 失败处置：空事务失败 LOG_WARN 不阻断开库（锁竞争是瞬时的），chmod 失败 LOG_ERROR 不静默
    char* warmupErr = nullptr;
    const int warmupRc = sqlite3_exec(handle, "BEGIN IMMEDIATE; COMMIT;", nullptr,
                                       nullptr, &warmupErr);
    if (warmupRc != SQLITE_OK) {
        LOG_WARN(std::string("SQLite 空写事务失败（-wal/-shm 可能未被创建，"
                             "将由写路径的权限闩在首个成功写后补收紧）：") +
                 (warmupErr ? warmupErr : sqlite3_errmsg(handle)));
    }
    if (warmupErr != nullptr) sqlite3_free(warmupErr);
    auto permR = core::fileperms::restrictSqliteStoreFiles(file);
    if (permR.isFail()) {
        LOG_ERROR("数据库文件权限收紧失败（凭据落点可能对同机其他用户可读）: " +
                  permR.errorMessage());
    }

    SqliteDb db;
    db.handle_ = handle;
    // 置闩：内存库/空路径恒不置位；落盘库只在三件套全部存在且收紧成功（ok 且 count>=3）时已确认
    auto perms = std::make_shared<StorePermsLatch>();
    perms->file = file;
    const bool noStoreFile =
        file.string().empty() || file.string() == ":memory:";
    const bool trioConfirmed = permR.isOk() && permR.value() >= 3;
    perms->pending.store(!noStoreFile && !trioConfirmed,
                         std::memory_order_relaxed);
    db.perms_ = std::move(perms);
    return Result<SqliteDb>::ok(std::move(db));
}

SqliteDb::~SqliteDb() {
    if (handle_) {
        // 兜底补收紧：macOS sqlite close 后不删伴生文件，闩还挂着就让三件套以 0600 留盘（只碰元数据无需锁）
        retightenStoreFilesAfterWrite(perms_);
        // 用 close_v2：sqlite3_close 遇未 finalize 语句返回 SQLITE_BUSY 会被忽略 → 句柄静默不关；
        // close_v2 延迟关闭，最后一条语句 finalize 时回收。close 是句柄操作，持锁执行
        std::lock_guard<std::recursive_mutex> lock(*mutex_);
        sqlite3_close_v2(handle_);
        handle_ = nullptr;
    }
}

// 移动不换锁：mutex_ 是 shared_ptr，搬入/搬出对象共用同一把，锁与句柄不分家
SqliteDb::SqliteDb(SqliteDb&& other) noexcept
    : handle_(other.handle_), mutex_(other.mutex_),
      writeVersion_(other.writeVersion_), perms_(other.perms_) {
    other.handle_ = nullptr;
}

SqliteDb& SqliteDb::operator=(SqliteDb&& other) noexcept {
    if (this != &other) {
        if (handle_) {
            std::lock_guard<std::recursive_mutex> lock(*mutex_);
            // 与析构同款：close_v2 延迟关闭（避免语句未 finalize 时静默不关导致句柄泄漏）
            sqlite3_close_v2(handle_);
        }
        handle_ = other.handle_;
        other.handle_ = nullptr;
        mutex_ = other.mutex_;
        // 版本号跟着走：留在旧对象上 = 推进的计数器与读侧不是同一个，脏标记永不置位
        writeVersion_ = other.writeVersion_;
        // 权限闩同理：留在旧对象上 = 新对象的写路径拿不到闩，补收紧不再发生
        perms_ = other.perms_;
    }
    return *this;
}

Result<void> SqliteDb::exec(const std::string& sql) {
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    char* errMsg = nullptr;
    int rc = sqlite3_exec(handle_, sql.c_str(), nullptr, nullptr, &errMsg);
    // 补收紧放在 rc 判分之前：exec 逐条 autocommit，中途失败时前几条可能已提交（闩的 count>=3 防误判）
    retightenStoreFilesAfterWrite(perms_);
    if (rc != SQLITE_OK) {
        std::string err = errMsg ? errMsg : "unknown";
        sqlite3_free(errMsg);
        return Result<void>::fail(mapErrCode(rc), "exec: " + err);
    }
    // 裸 SQL 写入口一律置位，不解析「是不是 SELECT」：exec 调用面全是迁移/DDL/测试裸写，
    // 误置位只是多查一次库，漏置位是 UI 停在旧数据上（transaction 的 BEGIN/COMMIT 走此亦无害）
    writeVersion_->fetch_add(1, std::memory_order_release);
    return Result<void>::ok();
}

Result<Statement> SqliteDb::prepare(const std::string& sql) {
    // prepare 与 exec/step/currentRow 共用同一把锁
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(handle_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Result<Statement>::fail(mapErrCode(rc),
            std::string("prepare: ") + sqlite3_errmsg(handle_));
    }
    return Result<Statement>::ok(
        Statement(handle_, stmt, mutex_, writeVersion_, perms_));
}

Result<void> SqliteDb::transaction(std::function<Result<void>()> fn) {
    // 全程持锁：BEGIN + fn() + COMMIT/ROLLBACK 必须同一把 recursive_mutex。锁只盖 BEGIN/COMMIT
    // 时，跨线程无串行——代理线程的 INSERT 能穿插进 UI 事务被 ROLLBACK 吞掉，两个 BEGIN 也会互撞。
    // fn() 内取的是同一把锁（同线程重入不死锁）；约束：fn() 内只做 DB 操作，不得长阻塞 IO/网络
    std::lock_guard<std::recursive_mutex> lock(*mutex_);

    // 事务深度用 thread_local 记（只记本线程，别的线程被上面的锁挡住）：sqlite 不支持嵌套事务，
    // 内层再发 BEGIN 会失败。内层只跑 fn()，最外层统一 BEGIN/COMMIT/ROLLBACK；深度 0 的调用路径不变。
    // 异常安全由 lock_guard + 显式 ROLLBACK 保证。
    static thread_local int txDepth = 0;
    const bool outermost = (txDepth == 0);
    if (outermost) {
        char* err = nullptr;
        if (sqlite3_exec(handle_, "BEGIN", nullptr, nullptr, &err) != SQLITE_OK) {
            std::string e = err ? err : "?";
            sqlite3_free(err);
            return Result<void>::fail(ErrorCode::DatabaseCorrupted, "BEGIN: " + e);
        }
    }
    ++txDepth;
    auto result = fn();
    --txDepth;
    if (!outermost) {
        // 内层不 COMMIT/ROLLBACK：失败照样外传，外层 isFail() 即整批 ROLLBACK，原子性不丢
        return result;
    }
    char* err = nullptr;
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
    // 整份 schema 包在一个事务里（失败整体回滚）。幂等由脚本自身保证（DDL 全 CREATE IF NOT EXISTS，
    // 种子 INSERT 带 NOT EXISTS 守卫）；无 schema_version、不迁移不兼容旧数据（缺列重来即可）
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