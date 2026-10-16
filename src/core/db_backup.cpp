// src/core/db_backup.cpp
#include "db_backup.h"
#include "file_perms.h"
#include "logging.h"
#include "time.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <sqlite3.h>

namespace core::dbmaint {
namespace fs = std::filesystem;

namespace {

std::string sqliteErr(sqlite3* db, const char* what) {
    return std::string(what) + ": " + (db ? sqlite3_errmsg(db) : "null handle");
}

// 独立打开 dbFile 得裸连接（即开即关，绕开 SqliteDb 的锁/版本/权限闩装配——
// 本文件的连接生命周期全在本函数/本文件内，不与任何并发线程共享）
// flags 不带 CREATE：检查与备份源绝不能顺手造出新库
core::Result<sqlite3*> openRaw(const fs::path& file, int flags) {
    sqlite3* handle = nullptr;
    const int rc = sqlite3_open_v2(file.string().c_str(), &handle, flags, nullptr);
    if (rc != SQLITE_OK) {
        const std::string msg = handle ? sqlite3_errmsg(handle) : "sqlite3_open_v2 失败";
        if (handle) sqlite3_close(handle);
        return core::Result<sqlite3*>::fail(core::ErrorCode::DatabaseOpenFailed,
                                             "打开库失败: " + file.string() + ": " + msg);
    }
    return core::Result<sqlite3*>::ok(handle);
}

} // namespace

core::Result<bool> quickCheckOnHandle(sqlite3* handle) {
    if (handle == nullptr) {
        return core::Result<bool>::fail(core::ErrorCode::Internal,
                                        "quickCheckOnHandle: null handle");
    }
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(handle, "PRAGMA quick_check;", -1, &stmt, nullptr);
    if (rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB) {
        // prepare 阶段就能确诊的损坏（页头烂掉/根本不是库）——这是「损坏」不是「失败」
        if (stmt) sqlite3_finalize(stmt);
        return core::Result<bool>::ok(false);
    }
    if (rc != SQLITE_OK) {
        if (stmt) sqlite3_finalize(stmt);
        return core::Result<bool>::fail(core::ErrorCode::DatabaseOpenFailed,
                                        sqliteErr(handle, "quick_check prepare 失败"));
    }
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB) {
        sqlite3_finalize(stmt);
        return core::Result<bool>::ok(false);
    }
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return core::Result<bool>::fail(core::ErrorCode::DatabaseOpenFailed,
                                        sqliteErr(handle, "quick_check 无结果行"));
    }
    const unsigned char* text = sqlite3_column_text(stmt, 0);
    const bool ok = text != nullptr &&
                    std::string(reinterpret_cast<const char*>(text)) == "ok";
    // 损坏库的首行即错误清单（"wrong number of records..."），后续行不必读
    sqlite3_finalize(stmt);
    return core::Result<bool>::ok(ok);
}

core::Result<bool> quickCheckOk(const fs::path& dbFile, bool readOnlyOpen) {
    std::error_code ec;
    if (!fs::exists(dbFile, ec) || ec) {
        return core::Result<bool>::fail(core::ErrorCode::FileNotFound,
                                        "库文件不存在: " + dbFile.string());
    }
    auto opened = openRaw(dbFile, readOnlyOpen ? SQLITE_OPEN_READONLY
                                              : SQLITE_OPEN_READWRITE);
    if (opened.isFail()) return core::Result<bool>::fail(opened.errorCode(),
                                                         opened.errorMessage());
    sqlite3* handle = opened.value();
    auto r = quickCheckOnHandle(handle);
    sqlite3_close(handle);
    return r;
}

std::string timestampForFileSuffix() {
    // 刻意用 UTC 而非本地时间：latestBackup/pruneFilesWithPrefix 按文件名字典序取
    // 「最新」，本地时间在 DST 回拨时非单调（新备份名可能小于旧备份 → 误删新备份/取错还原源）
    std::tm utc{};
    core::time::utcTime(core::time::nowUnix(), &utc);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec);
    return buf;
}

core::Result<std::optional<fs::path>> latestBackup(
    const fs::path& dbFile, const std::optional<fs::path>& exclude) {
    const fs::path dir = dbFile.parent_path();
    const std::string prefix = dbFile.filename().string() + ".backup-";
    std::error_code ec;
    if (!fs::exists(dir, ec) || ec) {
        return core::Result<std::optional<fs::path>>::ok(std::nullopt);
    }
    std::optional<fs::path> newest;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        // exclude：跳过本轮已尝试过的备份（main.cpp 损坏还原循环里，同一份刚验证
        // 失败的备份再弹「还原」没有意义——应递补更早一份或提示无可用备份）。
        // 按文件名比较：exclude 由上一次 latestBackup 在同一目录下选出，名字即唯一键
        if (exclude.has_value() && name == exclude->filename().string()) {
            continue;
        }
        if (!newest || name > newest->filename().string()) {
            newest = it->path();
        }
    }
    if (ec) {
        return core::Result<std::optional<fs::path>>::fail(
            core::ErrorCode::FileReadFailed,
            "扫描备份目录失败: " + dir.string() + ": " + ec.message());
    }
    return core::Result<std::optional<fs::path>>::ok(newest);
}

core::Result<int> pruneFilesWithPrefix(const fs::path& dir,
                                       const std::string& prefix, int keep) {
    if (prefix.empty()) {
        return core::Result<int>::fail(core::ErrorCode::ValidationError,
                                        "pruneFilesWithPrefix: 空 prefix");
    }
    if (keep < 0) keep = 0;
    std::error_code ec;
    if (!fs::exists(dir, ec) || ec) {
        return core::Result<int>::ok(0);
    }
    std::vector<std::string> names;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.size() > prefix.size() &&
            name.compare(0, prefix.size(), prefix) == 0) {
            names.push_back(std::move(name));
        }
    }
    if (ec) {
        return core::Result<int>::fail(core::ErrorCode::FileReadFailed,
                                       "扫描目录失败: " + dir.string() + ": " + ec.message());
    }
    // 文件名字典序 = 时间戳时间序（timestampForFileSuffix 保证），升序排 → 尾部是最新
    std::sort(names.begin(), names.end());
    if (static_cast<int>(names.size()) <= keep) {
        return core::Result<int>::ok(0);
    }
    int removed = 0;
    for (size_t i = 0; i + static_cast<size_t>(keep) < names.size(); ++i) {
        std::error_code rmEc;
        fs::remove(dir / names[i], rmEc);
        if (rmEc) {
            return core::Result<int>::fail(
                core::ErrorCode::FileWriteFailed,
                "滚动删除失败: " + (dir / names[i]).string() + ": " + rmEc.message());
        }
        ++removed;
    }
    return core::Result<int>::ok(removed);
}

core::Result<fs::path> createBackup(const fs::path& dbFile, int keepBackups) {
    if (keepBackups < 1) keepBackups = 1;
    std::error_code ec;
    if (!fs::exists(dbFile, ec) || ec) {
        return core::Result<fs::path>::fail(core::ErrorCode::FileNotFound,
                                            "备份源不存在: " + dbFile.string());
    }
    const fs::path dir = dbFile.parent_path();
    const std::string prefix = dbFile.filename().string() + ".backup-";
    const fs::path dest = dir / (prefix + timestampForFileSuffix());

    // 同秒重名（多进程并发启动/同进程闩失守）：整体覆盖为一份全新备份，不 append 到旧文件
    std::error_code rmEc;
    fs::remove(dest, rmEc);

    auto src = openRaw(dbFile, SQLITE_OPEN_READWRITE);
    if (src.isFail()) return core::Result<fs::path>::fail(src.errorCode(), src.errorMessage());
    auto dst = openRaw(dest, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    if (dst.isFail()) {
        sqlite3_close(src.value());
        fs::remove(dest, rmEc);
        return core::Result<fs::path>::fail(dst.errorCode(), dst.errorMessage());
    }

    sqlite3_backup* bk = sqlite3_backup_init(dst.value(), "main", src.value(), "main");
    if (bk == nullptr) {
        const std::string msg = sqliteErr(dst.value(), "sqlite3_backup_init 失败");
        sqlite3_close(dst.value());
        sqlite3_close(src.value());
        fs::remove(dest, rmEc);
        return core::Result<fs::path>::fail(core::ErrorCode::FileWriteFailed, msg);
    }

    // step(-1) 一次拷全部页；源库并发写入时 sqlite 自动重启备份并重试（在线备份语义）。
    // BUSY/LOCKED 上限重试 250 次（~5s）：启动期写入方是模块 onStart，理论上早已写完，
    // 持续锁死更可能是真异常，宁可返回失败让调用方 LOG_ERROR 继续启动
    int rc = SQLITE_OK;
    for (int tries = 0; tries < 250; ++tries) {
        rc = sqlite3_backup_step(bk, -1);
        if (rc == SQLITE_DONE) break;
        // step(-1) 只返回 SQLITE_DONE 或错误码（BUSY/LOCKED 才可重试），绝不返回 SQLITE_OK
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        break;  // 其余错误码（含 CORRUPT/IOERR）不可重试
    }
    const int finishRc = sqlite3_backup_finish(bk);
    const bool done = rc == SQLITE_DONE && finishRc == SQLITE_OK;
    if (done) {
        // 备份须是自包含库：sqlite3_backup 连页头一起抄，源库是 WAL（主库恒 WAL）时备份
        // 页头仍是 2/2 却没有 -wal——此时只读打开（备份校验口径，见 quickCheckOk 的
        // readOnlyOpen 入参）建不出 -shm，prepare 直接 SQLITE_CANTOPEN，还原校验必失败。
        // 关连接前把落盘页头归一为 rollback journal；失败只 WARN 不废备份——代价是这份
        // 备份只剩读写能开、只读校验过不去（还原会走 failRestore），所以不静默
        char* jmErr = nullptr;
        if (sqlite3_exec(dst.value(), "PRAGMA journal_mode=DELETE;", nullptr, nullptr,
                         &jmErr) != SQLITE_OK) {
            LOG_WARN("备份库 journal_mode 归一失败（该备份可能过不了只读校验）: " +
                     dest.string() + ": " + (jmErr ? jmErr : "unknown error"));
        }
        sqlite3_free(jmErr);
    }
    sqlite3_close(dst.value());
    sqlite3_close(src.value());
    if (!done) {
        fs::remove(dest, rmEc);   // 删半成品，不留会误入 latestBackup 的坏备份
        return core::Result<fs::path>::fail(
            core::ErrorCode::FileWriteFailed,
            "sqlite3_backup 未完成: " + dbFile.string() + " → " + dest.string());
    }
    // 归一动作会现建 <备份名>-wal/-shm：不是备份的一部分，且名字字典序大于备份本身，
    // 残留会被 latestBackup/pruneFilesWithPrefix（按前缀匹配名）当成一份更新的备份
    // → 还原必挂，故删不掉不静默
    for (const char* suffix : {"-wal", "-shm"}) {
        std::error_code sideEc;
        fs::remove(fs::path(dest.string() + suffix), sideEc);
        if (sideEc) {
            LOG_WARN("备份伴生文件清理失败（会被 latestBackup 误选为最新备份）: " +
                     dest.string() + suffix + ": " + sideEc.message());
        }
    }
    // 备份含凭据页，与主库同保护级别；收紧失败只 WARN——备份已可用，权限降级不应导致备份作废
    if (auto perm = core::fileperms::restrictToOwnerOnly(dest); perm.isFail()) {
        LOG_WARN("备份文件权限收紧失败: " + dest.string() + ": " + perm.errorMessage());
    }
    if (auto pruned = pruneFilesWithPrefix(dir, prefix, keepBackups); pruned.isFail()) {
        LOG_WARN("备份滚动清理失败（不影响本次备份）: " + pruned.errorMessage());
    }
    return core::Result<fs::path>::ok(dest);
}

// 进程级「每进程只备份一次」闩：文件级 static（而非函数局部 static）——复位测试缝要摸到它
std::atomic<bool> g_startupBackupDone{false};

core::Result<std::optional<fs::path>> performStartupBackupOnce(
    const fs::path& dbFile) {
    if (g_startupBackupDone.exchange(true)) {
        return core::Result<std::optional<fs::path>>::ok(std::nullopt);
    }
    auto r = createBackup(dbFile, 5);
    if (r.isFail()) {
        return core::Result<std::optional<fs::path>>::fail(r.errorCode(), r.errorMessage());
    }
    return core::Result<std::optional<fs::path>>::ok(std::move(r.value()));
}

void resetStartupBackupLatchForTests() {
    g_startupBackupDone.store(false);
}

core::Result<fs::path> restoreFromBackup(const fs::path& dbFile,
                                          const fs::path& backupFile) {
    std::error_code ec;
    if (!fs::exists(backupFile, ec) || ec) {
        return core::Result<fs::path>::fail(core::ErrorCode::FileNotFound,
                                            "备份文件不存在: " + backupFile.string());
    }
    const fs::path dir = dbFile.parent_path();
    const std::string corruptName =
        dbFile.filename().string() + ".corrupt-" + timestampForFileSuffix();
    const fs::path corruptPath = dir / corruptName;

    auto failRestore = [&](const std::string& msg) {
        // 现场复原：隔离的坏库改回原名，别让用户两头落空
        std::error_code rmEc, rnEc;
        fs::remove(dbFile, rmEc);
        fs::rename(corruptPath, dbFile, rnEc);
        // 复原本身也可能失败（目标被占用/权限问题）：逐个 LOG_ERROR 并把实情追加进
        // 返回给用户的 msg——绝不让「主库下落不明」静默发生
        std::string finalMsg = msg;
        if (rmEc) {
            LOG_ERROR("还原失败后清理半还原主库失败: " + dbFile.string() + ": " +
                      rmEc.message());
            finalMsg += "\n半还原的主库文件未能清理: " + dbFile.string();
        }
        if (rnEc) {
            LOG_ERROR("还原失败后隔离文件改回原名失败: " + corruptPath.string() +
                      " → " + dbFile.string() + ": " + rnEc.message());
            finalMsg += "\n原损坏文件已保留为: " + corruptPath.string();
        }
        return core::Result<fs::path>::fail(core::ErrorCode::FileWriteFailed, finalMsg);
    };

    // 同秒重名：上一份 corrupt 同名会被 rename 挡（Windows rename 不覆盖目标），先删
    std::error_code rmEc;
    fs::remove(corruptPath, rmEc);
    // 主库 WAL 伴生文件一并隔离删除：留着会让还原后的库读到旧 WAL 里的脏/坏页
    fs::remove(fs::path(dbFile.string() + "-wal"), rmEc);
    fs::remove(fs::path(dbFile.string() + "-shm"), rmEc);

    fs::rename(dbFile, corruptPath, ec);
    if (ec) {
        return core::Result<fs::path>::fail(
            core::ErrorCode::FileWriteFailed,
            "损坏库隔离失败: " + dbFile.string() + " → " + corruptPath.string() + ": " +
                ec.message());
    }
    std::error_code cpEc;
    fs::copy_file(backupFile, dbFile, cpEc);
    if (cpEc) {
        return failRestore("备份拷贝失败: " + backupFile.string() + ": " + cpEc.message());
    }
    // copy_file 会把源文件的只读属性一并拷到主库（Windows CopyFile 保留属性位），
    // 只读主库会让后续 Database::open 失败：清掉只读位，失败仅 WARN（校验/启动自会兜底报错）
    {
        std::error_code permEc;
        fs::permissions(dbFile, fs::perms::owner_write, fs::perm_options::add, permEc);
        if (permEc) {
            LOG_WARN("还原库清除只读位失败: " + dbFile.string() + ": " + permEc.message());
        }
    }

    // 校验：备份本身也坏 → fail（走调用方的「还原失败→退出」路径），现场复原。
    // 只读方式打开——备份文件可能是用户拷贝来的/被置了只读属性，READWRITE 会误拒还原。
    // isFail（BUSY/IOERR 等瞬态错误）同样走还原失败：此刻无法证明还原结果可用
    auto verify = quickCheckOk(dbFile, /*readOnlyOpen=*/true);
    if (verify.isFail()) {
        return failRestore("还原后的库完整性检查无法执行（备份损坏或瞬态 IO 错误）: " +
                           verify.errorMessage());
    }
    if (!verify.value()) {
        return failRestore("还原后的库未通过完整性检查（备份本身损坏?）: quick_check 非 ok");
    }

    if (auto perm = core::fileperms::restrictToOwnerOnly(dbFile); perm.isFail()) {
        LOG_WARN("还原库权限收紧失败: " + dbFile.string() + ": " + perm.errorMessage());
    }
    if (auto pruned = pruneFilesWithPrefix(
            dir, dbFile.filename().string() + ".corrupt-", 1);
        pruned.isFail()) {
        LOG_WARN("corrupt 滚动清理失败（不影响还原结果）: " + pruned.errorMessage());
    }
    return core::Result<fs::path>::ok(corruptPath);
}

} // namespace core::dbmaint
