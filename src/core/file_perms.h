// src/core/file_perms.h
// 凭据落点文件权限（平台适配：实现拆 _posix.cpp / _win.cpp，没有 _macos.cpp，macOS 走 _posix）。
// API key 迁进 SQLite 列后 macOS/POSIX 上列里就是明文，保护级别 = 文件权限级，故承载凭据的主库
// 与 -wal/-shm 必须收紧到「仅属主可读写」（WAL 会把凭据页复制进 -wal；-wal/-shm 是首次写事务才
// 落盘，故 SqliteDb::open 显式跑一次空写事务逼出三件套再收紧，且每次打开都校正）。
// Windows：DPAPI 密文入库，文件无可读凭据、ACL 已是仅本用户，本模块是明确 no-op（返回 0）。
#pragma once

#include <filesystem>

#include "result.h"

namespace core::fileperms {

// 把单个文件收紧为「仅属主可读写」（POSIX 0600）。文件不存在 → ok（无落点可保护）；
// chmod 失败 → fail(FilePermissionDenied)。Windows 实现恒返回 ok
core::Result<void> restrictToOwnerOnly(const std::filesystem::path& file);

// 收紧 SQLite 三件套：<db>/<db>-wal/<db>-shm（存在的才动）。返回实际收紧文件数；任一失败 → fail
core::Result<int> restrictSqliteStoreFiles(const std::filesystem::path& dbFile);

} // namespace core::fileperms
