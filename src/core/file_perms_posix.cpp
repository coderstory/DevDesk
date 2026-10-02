// src/core/file_perms_posix.cpp
// POSIX（macOS / Linux）：chmod 0600（口径同 keychain_posix.cpp:34-50「写完 chmod 0600，失败即
// 回滚」——这里是「设权限，失败即报错」，因为数据库文件不能删）
#include "file_perms.h"

#include <system_error>

namespace core::fileperms {

core::Result<void> restrictToOwnerOnly(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) {
        ec.clear();
        return core::Result<void>::ok();   // 没有落点 → 无可保护
    }
    ec.clear();
    // perm_options::replace 把权限整体覆盖为 rw-------（不受 umask 影响）
    std::filesystem::permissions(
        file,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
    if (ec) {
        return core::Result<void>::fail(
            core::ErrorCode::FilePermissionDenied,
            "cannot chmod to 0600: " + file.string() + ": " + ec.message());
    }
    return core::Result<void>::ok();
}

core::Result<int> restrictSqliteStoreFiles(const std::filesystem::path& dbFile) {
    if (dbFile.empty() || dbFile.string() == ":memory:") {
        return core::Result<int>::ok(0);   // 内存库无落点
    }
    int tightened = 0;
    const std::filesystem::path companions[] = {
        dbFile,
        std::filesystem::path(dbFile.string() + "-wal"),
        std::filesystem::path(dbFile.string() + "-shm"),
    };
    for (const auto& p : companions) {
        std::error_code ec;
        const bool exists = std::filesystem::exists(p, ec) && !ec;
        if (!exists) continue;
        auto r = restrictToOwnerOnly(p);
        if (r.isFail()) return core::Result<int>::fail(r.errorCode(), r.errorMessage());
        ++tightened;
    }
    return core::Result<int>::ok(tightened);
}

} // namespace core::fileperms
