// src/core/db_backup.h
// 数据库启动备份 / 损坏检测 / 还原（core 层纯函数，不含任何 UI 决策——弹框与退出留 main.cpp）。
// 契约：所有函数只在「模块尚未写入主库」的启动期调用（restore 是文件级改名/拷贝，
// 若此刻有别的连接在写主库，会留下半途状态）；备份本身用 sqlite3_backup 在线进行，允许并发写。
#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "result.h"

struct sqlite3;

namespace core::dbmaint {

// 在已打开的连接上跑 PRAGMA quick_check；true = 完整，false = 判定损坏。
// 契约：调用方必须持有该连接的锁（Database::quickCheck() 内部已持 db_.mutex()）
core::Result<bool> quickCheckOnHandle(sqlite3* handle);

// 独立打开 dbFile 跑 quick_check（自开连接、即开即关；不带 CREATE——绝不在检查里顺手造库）。
// 文件不存在 → fail(FileNotFound)；prepare/step 报 CORRUPT/NOTADB → ok(false)（判定损坏）；
// 其余 sqlite 错误（BUSY/IOERR 等瞬态错误）→ fail（检查失败，调用方绝不能当损坏处理）。
// readOnlyOpen：true = SQLITE_OPEN_READONLY 打开——用于校验备份文件（备份可能被用户置为只读，
//   READWRITE 打开会误拒还原）；false = READWRITE——用于主库检测路径，主库是 WAL 模式，
//   只读连接在存在热 -wal 且 -shm 不可写时会打开失败，会误判，不能改成只读。
core::Result<bool> quickCheckOk(const std::filesystem::path& dbFile,
                                bool readOnlyOpen = false);

// 主库最新的备份文件（<文件名>.backup-* 里字典序最大者）；无 → ok(nullopt)。
// exclude：跳过该备份（文件名比较）——损坏还原循环里用于排除本轮已尝试过的那份，
// 使下一轮递补更早一份；exclude 之外再无备份 → ok(nullopt)（调用方提示无可用备份）
core::Result<std::optional<std::filesystem::path>> latestBackup(
    const std::filesystem::path& dbFile,
    const std::optional<std::filesystem::path>& exclude = std::nullopt);

// sqlite3_backup 在线备份主库到同目录 <文件名>.backup-<YYYYMMDD-HHMMSS>，滚动保留最近 keepBackups 份。
// 备份文件落成后按 POSIX 收紧到 0600（内容含凭据页，与主库同保护级别），失败仅 LOG_WARN 不回滚
core::Result<std::filesystem::path> createBackup(
    const std::filesystem::path& dbFile, int keepBackups = 5);

// 进程级「每进程只备份一次」闩包装的 createBackup：
// 首次 → ok(备份路径)；已备份过 → ok(nullopt)（跳过，不是失败）。
// 失败（磁盘满/权限/库文件不存在）→ fail，调用方 LOG_ERROR 后继续启动，绝不阻断
core::Result<std::optional<std::filesystem::path>> performStartupBackupOnce(
    const std::filesystem::path& dbFile);

// 测试缝：复位 once 闩（仅供单测在单进程内反复触发首备，生产代码禁止调用）
void resetStartupBackupLatchForTests();

// 还原：损坏主库改名隔离为 <文件名>.corrupt-<时间戳>（corrupt 只保留最近 1 份），
// 拷贝 backupFile 到主库路径，quick_check 校验通过才判成功。
// 任一步失败：把隔离文件改回原名（原样恢复现场）并 fail——绝不让用户两头落空（既没还原又丢了原文件）。
// 前置：调用前必须已关闭主库的全部连接（还原是文件级操作，有连接开着在 Windows 上 rename 会失败）
core::Result<std::filesystem::path> restoreFromBackup(
    const std::filesystem::path& dbFile, const std::filesystem::path& backupFile);

// 目录下所有 <prefix>* 文件按文件名字典序（= 时间戳时间序）滚动删除，保留最新 keep 份；
// 返回删除数。prefix 必须自带判别段（如 "devdesk.db.backup-"），空 prefix 是误用、直接 fail
core::Result<int> pruneFilesWithPrefix(const std::filesystem::path& dir,
                                       const std::string& prefix, int keep);

// UTC 时间文件名时间戳 "YYYYMMDD-HHMMSS"（字典序 = 时间序，滚动删除依赖这一点。
// 刻意不用本地时间：DST 回拨时本地时间非单调，新备份名字典序可能小于旧备份）
std::string timestampForFileSuffix();

} // namespace core::dbmaint
