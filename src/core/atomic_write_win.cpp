// core::detail::replaceFile 的 Windows 实现（CMake 按平台选源）
#include "atomic_write.h"

#include <fstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace core::detail {

bool prepareRestrictedFile(const std::filesystem::path& tmp, std::error_code& ec) {
    // Windows 没有可收紧的 POSIX 模式位：数据根在 %USERPROFILE% 下，NTFS 继承的默认 ACL 已是
    // 「仅本用户 + Administrators/SYSTEM」，故只固定「先建 tmp 再写」的次序（建不出就早失败）
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
    // 一步原子替换，与 POSIX 侧 rename 同语义：MoveFileExW + MOVEFILE_REPLACE_EXISTING。
    // 不能「先删再改名」——MinGW 的 std::filesystem::rename 不覆盖已存在目标，旧写法在
    // remove 与 rename 之间留一个「目标不存在」的窗口：FileWatcher / 同步 harvest 撞上
    // 会把这次写入误判成「文件被删」，进而走本地删除的破坏性分支（F3 审计 ④）。
    // 读者侧补 fs::exists 复检只是缓解，消除窗口才是根因修法。
    // MOVEFILE_WRITE_THROUGH：返回时数据已落盘（跨卷时该移动退化为拷贝+删除，标志保证
    // 落盘后再返回；同卷是纯改名，无此成本）。
    if (::MoveFileExW(tmp.wstring().c_str(), target.wstring().c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ec.clear();
        return;
    }
    ec = std::error_code(static_cast<int>(::GetLastError()),
                         std::system_category());
}

} // namespace core::detail
