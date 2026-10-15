// src/core/db_backup.cpp
#include "db_backup.h"
#include "file_perms.h"
#include "logging.h"
#include "time.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
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

core::Result<bool> quickCheckOk(const fs::path& dbFile) {
    std::error_code ec;
    if (!fs::exists(dbFile, ec) || ec) {
        return core::Result<bool>::fail(core::ErrorCode::FileNotFound,
                                        "库文件不存在: " + dbFile.string());
    }
    auto opened = openRaw(dbFile, SQLITE_OPEN_READWRITE);
    if (opened.isFail()) return core::Result<bool>::fail(opened.errorCode(),
                                                         opened.errorMessage());
    sqlite3* handle = opened.value();
    auto r = quickCheckOnHandle(handle);
    sqlite3_close(handle);
    return r;
}

std::string timestampForFileSuffix() {
    std::tm local{};
    core::time::localTime(core::time::nowUnix(), &local);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec);
    return buf;
}

core::Result<std::optional<fs::path>> latestBackup(const fs::path& dbFile) {
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
        if (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        break;  // 其余错误码（含 CORRUPT/IOERR）不可重试
    }
    const int finishRc = sqlite3_backup_finish(bk);
    const bool done = rc == SQLITE_DONE && finishRc == SQLITE_OK;
    sqlite3_close(dst.value());
    sqlite3_close(src.value());
    if (!done) {
        fs::remove(dest, rmEc);   // 删半成品，不留会误入 latestBackup 的坏备份
        return core::Result<fs::path>::fail(
            core::ErrorCode::FileWriteFailed,
            "sqlite3_backup 未完成: " + dbFile.string() + " → " + dest.string());
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
        return core::Result<fs::path>::fail(core::ErrorCode::FileWriteFailed, msg);
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

    // 校验：备份本身也坏 → fail（走调用方的「还原失败→退出」路径），现场复原
    auto verify = quickCheckOk(dbFile);
    if (verify.isFail() || !verify.value()) {
        return failRestore("还原后的库未通过完整性检查（备份本身损坏?）: " +
                           (verify.isFail() ? verify.errorMessage() : "quick_check 非 ok"));
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
