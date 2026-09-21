// src/platform/keychain_win.cpp
// Windows 实现：DPAPI（CryptProtectData/CryptUnprotectData）
// 当前为桩实现，T7 填充真实 DPAPI 逻辑
#include "keychain.h"

namespace platform::keychain {

// Windows DPAPI 桩实现
core::Result<void> storeSecret(const std::string& service, const std::string& account, const std::string& secret) {
    // TODO: DPAPI 实现
    return core::Result<void>::fail(core::ErrorCode::NotImplemented, "Windows DPAPI not yet implemented");
}
core::Result<std::string> loadSecret(const std::string& service, const std::string& account) {
    return core::Result<std::string>::fail(core::ErrorCode::NotImplemented, "Windows DPAPI not yet implemented");
}
core::Result<void> deleteSecret(const std::string& service, const std::string& account) {
    return core::Result<void>::fail(core::ErrorCode::NotImplemented, "Windows DPAPI not yet implemented");
}

} // namespace platform::keychain
