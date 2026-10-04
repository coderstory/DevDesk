// core::detail::replaceFile 的 Windows 实现（CMake 按平台选源）
#include "atomic_write.h"

#include <fstream>

namespace core::detail {

bool prepareRestrictedFile(const std::filesystem::path& tmp, std::error_code& ec) {
    // Windows 没有可收紧的 POSIX 模式位：数据根在 %USERPROFILE% 下，NTFS 继承
    // 的默认 ACL 已是「仅本用户 + Administrators/SYSTEM」，别的用户无 traverse
    // 权限，故此处不假装加固，只固定「先建 tmp 再写」的次序（建不出就早失败）
    ec.clear();
    if (std::filesystem::exists(tmp, ec)) return !ec;
    if (ec) return false;
    std::ofstream create(tmp, std::ios::binary);
    if (!create) {
        ec = std::make_error_code(std::errc::permission_denied);
        return false;
    }
    create.close();
    ec.clear();
    return true;
}

void replaceFile(const std::filesystem::path& tmp,
                 const std::filesystem::path& target,
                 std::error_code& ec) {
    // 先删再 rename：迁就 MinGW 的 rename 不覆盖已存在目标的历史路径
    //（最后写者胜，窗口期可接受）。
    std::error_code rmEc;
    std::filesystem::remove(target, rmEc);
    std::filesystem::rename(tmp, target, ec);
}

} // namespace core::detail
