#include "sqlite_db.h"
#include "file_perms.h"
#include "logging.h"

#include <cstddef>
#include <cstring>
#include <sqlite3.h>
#include <utility>

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

    // 写路径补收紧（G8 简报 F1 m1 残留，2026-10-03 收口）：open 时空写事务失败
    // → -wal/-shm 可能由本会话稍后的首个写事务才创建，且 POSIX 上伴生文件继承
    // 主库当时的权限位——主库宽松时凭据页就留在组/其他可读的 -wal 里（macOS
    // 系统 sqlite close 后还不删伴生文件，实测——暴露面不止会话内）。闩未落时
    // 补一次 restrictSqliteStoreFiles；**三件套齐（count>=3）且全部成功才落闩**
    // ——只认 ok 会把「文件还没出现」误当收紧完成，之后晚现的文件就没人管了。
    // 并发安全：pending/warned 均为 atomic；本函数只碰文件元数据（stat/chmod）、
    // 不碰 sqlite3* 句柄，无需 db 锁，多线程同时补收紧幂等无害。失败只报一次
    // （warned），防高频写路径（代理 logCall 每次调用一条 INSERT）刷屏。
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

// ============ Statement ============

Statement::Statement(sqlite3* db, sqlite3_stmt* stmt,
                     std::shared_ptr<std::recursive_mutex> mutex,
                     std::shared_ptr<std::atomic<uint64_t>> version,
                     std::shared_ptr<StorePermsLatch> perms)
    : db_(db), stmt_(stmt), mutex_(std::move(mutex)),
      version_(std::move(version)), perms_(std::move(perms)) {
    // 只读性判一次并缓存：sqlite3_stmt_readonly() 纯查 VDBE 程序、不碰数据，
    // 且已 prepare 完的语句其结论不会再变。判据来自 sqlite 官方语义——SELECT /
    // EXPLAIN / BEGIN / PRAGMA 读法为 true，INSERT/UPDATE/DELETE/REPLACE/DDL
    // 为 false（故写路径都会被计入版本号）。
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

// 推进写版本号。**只在锁内、且只在非只读语句上调用**（两个调用点都在
// step() 的锁内，见其注释）。release：与读侧 writeVersion() 的 acquire 配对，
// 保证「读到新版本」蕴含「导致它的写已可见」。
void Statement::bumpVersionOnWrite() noexcept {
    if (readonly_ || !version_) return;
    version_->fetch_add(1, std::memory_order_release);
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
    if (rc == SQLITE_ROW) {
        // 非只读语句返回 ROW 只可能是 RETURNING（写语句的结果行），算已写。
        bumpVersionOnWrite();
        retightenStoreFilesAfterWrite(perms_);
        return Result<bool>::ok(true);
    }
    if (rc == SQLITE_DONE) {
        bumpVersionOnWrite();
        // 写路径补收紧（G8 F1 m1 残留）：写 step 成功点即伴生文件「已出现/
        // 将被首个 COMMIT 落帧」的点，闩未落就补一次收紧（只读语句被
        // bumpVersionOnWrite 的同款 readonly_ 判据排除——读不落帧、无凭据页）。
        retightenStoreFilesAfterWrite(perms_);
        return Result<bool>::ok(false);
    }
    // 失败：不推进版本号。失败语句**可能已经改了库**（部分列约束失败时
    // sqlite 会先做完能做的），但把「可能」当成「一定」会让脏标记在失败
    // 路径上白置位（多查一次，可接受）；反过来漏置位则是 UI 停在旧数据上，
    // 两者不可比。宁可漏这一次——下一次真写入会把它纠正回来。
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

// 复位：sqlite3_reset 是句柄操作，锁与 prepare/bind/step 同一把
// （2026-10-02 跨线程纪律：漏一处就是连接内部空闲链损坏 → 真机 SIGSEGV）。
// 返回值语义注意：sqlite3_reset 报的是**上一次 step** 的结果，不是本次操作的；
// 调用方在 step() 失败时不要靠它的返回值判断成败（app_config.cpp 的 setMany
// 正是 step 失败即 return，不走到这里）。
Result<void> Statement::reset() {
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    return checkRc(db_, sqlite3_reset(stmt_), "reset");
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
    //   ① 空事务失败（典型 SQLITE_BUSY/READONLY）→ 取返回码并 LOG_WARN。
    //      不阻断开库：锁竞争是瞬时的，硬失败会让「另一个 DevDesk 开着」变成
    //      「本进程起不来」，比留着宽松权限的 -wal 更糟。
    //   ② chmod 失败 → LOG_ERROR 不静默：那是「凭据可能被同机他人读到」。
    //
    // F1 m1 残留收口（2026-10-03）：① 失败时 -wal/-shm 可能尚未创建，下面的
    // restrictSqliteStoreFiles 对不存在的文件跳过——本会话稍后首个写事务才创建
    // 它们（POSIX 上继承主库当时的权限位），原实现就此失守。现在 open 末尾按
    // 「三件套是否已全部确认 0600」置 StorePermsLatch：未确认则此后的每条成功
    // 写路径（exec / 写 step，见 retightenStoreFilesAfterWrite）都会补一次收紧，
    // 三件套齐且全部成功才落闩。多进程竞态（外部工具以宽松权限重建三件套）与
    // open 时收紧失败的重试也由同一把闩覆盖。
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
    // 置闩（见上方注释）：内存库/空路径无落点恒不置位；落盘库只在三件套
    // 「全部存在且全部收紧成功」（ok 且 count>=3）时才视为已确认。
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
        // 兜底补收紧（G8 F1 m1 残留）：macOS 系统 sqlite 在 close 后不删伴生文件
        // （实测），闩还挂着就让三件套以确认过的 0600 留在盘上。只碰文件元数据、
        // 不碰句柄，无需先取锁。
        retightenStoreFilesAfterWrite(perms_);
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
    : handle_(other.handle_), mutex_(other.mutex_),
      writeVersion_(other.writeVersion_), perms_(other.perms_) {
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
        // 版本号跟着走（同 mutex_ 的理由：留在旧对象上 = Statement 推进的
        // 计数器与读侧看到的不是同一个，脏标记从此永不置位）
        writeVersion_ = other.writeVersion_;
        // 权限闩同理跟着走（见 StorePermsLatch 注释）：留在旧对象上 = 移动后
        // 新对象上的写路径拿不到闩，补收紧从此不再发生
        perms_ = other.perms_;
    }
    return *this;
}

Result<void> SqliteDb::exec(const std::string& sql) {
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    char* errMsg = nullptr;
    int rc = sqlite3_exec(handle_, sql.c_str(), nullptr, nullptr, &errMsg);
    // 写路径补收紧（G8 F1 m1 残留）：放在 rc 判分之前——sqlite3_exec 逐条
    // autocommit，脚本中途失败时前几条可能已各自提交，-wal 已落了帧，失败
    // 分支同样要补（闩的 count>=3 落闩条件保证不会把「文件未出现」误当完成）。
    retightenStoreFilesAfterWrite(perms_);
    if (rc != SQLITE_OK) {
        std::string err = errMsg ? errMsg : "unknown";
        sqlite3_free(errMsg);
        return Result<void>::fail(mapErrCode(rc), "exec: " + err);
    }
    // 裸 SQL 写入口也必须置位（静态审计 M4 的失效判据）。这里不做
    // 「是不是 SELECT」的解析判断，改为**一律置位**：exec 的调用面全是
    // 迁移/DDL/测试与探针的裸写（实测 src/ 下 exec 无 SELECT 调用），
    // 一次误置位的代价只是多查一次库，而漏置位是 UI 停在旧数据上。
    // transaction() 的 BEGIN/COMMIT 也走 exec —— 那两次置位是无害的多查。
    writeVersion_->fetch_add(1, std::memory_order_release);
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
    return Result<Statement>::ok(
        Statement(handle_, stmt, mutex_, writeVersion_, perms_));
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

    // 可重入（2026-10-03 批次 E2 踩到后补）：事务**深度**用 thread_local 记，
    // 只记本线程——别的线程进不来（上面的锁已经挡住了，且 recursive_mutex
    // 保证同一时刻只有一个线程持有）。
    // 为什么需要：sqlite 不支持嵌套事务，内层再发一次 BEGIN 会拿到
    // "cannot start a transaction within a transaction" 直接失败。组合型
    // 调用（外层要把「批量写 + 清理旧键」做成一次原子提交，内层的批量写
    // 自己也要一个事务）在配置落库那条路上是常态，不做可重入就只能二选一：
    // 要么放弃整体原子性，要么让内层不开事务 —— 后者会把「内层必须自带
    // 事务」的调用方（单独调 setMany 的那些）暴露成无原子性的裸写。
    // 现在：内层只跑 fn()，最外层统一 BEGIN/COMMIT/ROLLBACK。
    // 语义不变之处：深度 0 的调用（即绝大多数调用方）走的是原来那条路径，
    // 一行代码没改；异常安全仍由 RAII lock_guard + 显式 ROLLBACK 保证。
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
        // 内层：既不 COMMIT 也不 ROLLBACK —— 结果交给最外层裁决。
        // 失败照样往外传，外层看到 isFail() 就会整批 ROLLBACK，原子性没丢。
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