// src/core/atomic_write_posix.cpp
// core::detail::replaceFile 的 POSIX 实现（macOS / Linux 共用）。按平台拆文件
// （接口隔离，业务代码零平台宏），由 CMake 按平台选源（src/core/CMakeLists.txt）。
#include "atomic_write.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace core::detail {

bool prepareRestrictedFile(const std::filesystem::path& tmp, std::error_code& ec) {
    ec.clear();
    const std::string raw = tmp.string();
    // O_EXCL + 显式 mode：文件**出生即 0600**，不受 umask 影响（mode 与 umask
    // 是与关系，0600 只留属主两位，umask 再怎么收也不会放宽）。这是「收紧必须
    // 发生在 open(O_CREAT) 之前」的唯一落法——事后 chmod 追不回已开出的 fd。
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
    // 已存在（上一轮崩溃留下的残留）：权限位对**已存在**的文件不生效，所以这里
    // 显式收紧后再复用。收紧发生在本次打开写句柄之前，本次写入的内容不会
    // 出现在权限还宽的时段里。
    std::filesystem::permissions(tmp,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
    return !ec;
}

void replaceFile(const std::filesystem::path& tmp,
                 const std::filesystem::path& target,
                 std::error_code& ec) {
    // POSIX rename 覆盖已存在目标 = 原子替换：一步到位，不再"先删后建"
    //（此前为迁就 MinGW 语义所有平台都先 remove，POSIX 上的"原子写"因此退化成
    // 有崩溃窗口的非原子写——目标会在 remove 与 rename 之间短暂消失）。
    std::filesystem::rename(tmp, target, ec);
}

} // namespace core::detail
