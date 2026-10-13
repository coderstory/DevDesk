// core::detail::replaceFile 的 POSIX 实现（macOS / Linux 共用，CMake 按平台选源）
#include "atomic_write.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace core::detail {

bool prepareRestrictedFile(const std::filesystem::path& tmp, std::error_code& ec) {
    ec.clear();
    const std::string raw = tmp.string();
    // O_EXCL + 显式 mode：文件出生即 0600（mode 与 umask 相与，不会放宽）；收紧须在 open(O_CREAT) 之前
    const int fd = ::open(raw.c_str(), O_CREAT | O_EXCL | O_WRONLY,
                          S_IRUSR | S_IWUSR);
    if (fd >= 0) {
        ::close(fd);
        return true;
    }
    if (errno != EEXIST) {
        ec = std::error_code(errno, std::generic_category());
        return false;
    }
    // 已存在（崩溃残留）：权限位对已存在文件不生效，先收紧再复用（写入不出现在宽权限时段）
    std::filesystem::permissions(tmp,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
    return !ec;
}

void replaceFile(const std::filesystem::path& tmp,
                 const std::filesystem::path& target,
                 std::error_code& ec) {
    // rename 覆盖已存在目标 = 一步原子替换；不能先删后建（目标短暂消失，退化成有崩溃窗口的非原子写）
    std::filesystem::rename(tmp, target, ec);
}

} // namespace core::detail
