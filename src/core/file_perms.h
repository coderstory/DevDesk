// src/core/file_perms.h
// 凭据落点文件权限（平台适配：接口隔离，实现按平台拆文件
// file_perms_posix.cpp / file_perms_win.cpp —— **没有 _macos.cpp**，macOS 走
// _posix 那份；原抬头写的 file_perms_macos.cpp 不存在，别去找）
//
// 背景（2026-10-02 存储统一设计 §6.2/§6.3）：profile 的 API key 从系统钥匙串
// 迁进 SQLite 列后，macOS / POSIX 上**列里就是明文**——保护级别 = 文件权限级。
// 因此承载凭据的这几个文件必须收紧到「仅属主可读写」：
//   主库 devdesk.db 与其伴生 devdesk.db-wal / devdesk.db-shm。
// WAL 模式会把凭据页复制进 -wal（sqlite 打开时的 `PRAGMA journal_mode=WAL`
// 还不够——-wal/-shm 是**首次写事务**才落盘），故：
//   ① SqliteDb::open 里显式跑一次 `BEGIN IMMEDIATE; COMMIT;` 把三件套逼出来；
//   ② 随即调用本模块收紧三件套——**每次打开都校正**（不只在新建时设）。
// 口径抄 keychain_posix.cpp:34-50「写完 chmod 0600，失败即回滚」：宁可让调用方
// 拿到失败，也不留下他人可读的凭据落点。
//
// Windows：DPAPI 密文入库（platform/credential_protection_win.cpp），文件里
// 没有可读凭据，且用户目录默认 ACL 已是「仅本用户」——本模块在 Windows 上是
// 明确的 no-op（返回 0），不是「忘了实现」。
#pragma once

#include <filesystem>

#include "result.h"

namespace core::fileperms {

// 把单个文件收紧为「仅属主可读写」（POSIX 权限位 0600）。
//   - 文件不存在 → ok（没有落点就没什么可保护的，不算失败）
//   - chmod 失败 → fail(FilePermissionDenied)（调用方据此拒绝继续）
// Windows 实现恒返回 ok（见抬头）。
core::Result<void> restrictToOwnerOnly(const std::filesystem::path& file);

// 收紧 SQLite 三件套：<db>、<db>-wal、<db>-shm（存在的才动，缺失的跳过）。
// 返回实际收紧的文件数（供日志 / 测试断言）。任一文件收紧失败 → 原样 fail。
core::Result<int> restrictSqliteStoreFiles(const std::filesystem::path& dbFile);

} // namespace core::fileperms
