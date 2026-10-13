// Windows：no-op（不是「忘了实现」）
// 凭据列在 Windows 上是 DPAPI 密文，文件里没有可直读凭据；数据根在
// %USERPROFILE% 下，NTFS 默认 ACL 已是「仅本用户 + Administrators/SYSTEM」。
// 保留同签名实现而非在调用点写平台宏：业务代码零 #ifdef 是项目铁律。
#include "file_perms.h"

namespace core::fileperms {

core::Result<void> restrictToOwnerOnly(const std::filesystem::path&) {
    return core::Result<void>::ok();
}

core::Result<int> restrictSqliteStoreFiles(const std::filesystem::path&) {
    return core::Result<int>::ok(0);
}

} // namespace core::fileperms
