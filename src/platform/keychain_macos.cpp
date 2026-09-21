// src/platform/keychain_macos.cpp
// macOS 实现：SecKeychain（Keychain Services，Security.framework）
#include "keychain.h"

#include <Security/Security.h>

namespace platform::keychain {

core::Result<void> storeSecret(const std::string& service,
                                 const std::string& account,
                                 const std::string& secret) {
    // 删除已存在条目（避免冲突）
    SecKeychainItemRef itemRef = nullptr;
    OSStatus status = SecKeychainFindGenericPassword(
        nullptr,
        static_cast<UInt32>(service.size()), service.c_str(),
        static_cast<UInt32>(account.size()), account.c_str(),
        nullptr, nullptr, &itemRef);
    if (status == errSecSuccess && itemRef) {
        SecKeychainItemDelete(itemRef);
        CFRelease(itemRef);
    }

    status = SecKeychainAddGenericPassword(
        nullptr,
        static_cast<UInt32>(service.size()), service.c_str(),
        static_cast<UInt32>(account.size()), account.c_str(),
        static_cast<UInt32>(secret.size()), secret.c_str(),
        nullptr);

    if (status != errSecSuccess) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
            "SecKeychainAddGenericPassword failed: " + std::to_string(status));
    }
    return core::Result<void>::ok();
}

core::Result<std::string> loadSecret(const std::string& service,
                                      const std::string& account) {
    SecKeychainItemRef itemRef = nullptr;
    UInt32 length = 0;
    void* data = nullptr;

    OSStatus status = SecKeychainFindGenericPassword(
        nullptr,
        static_cast<UInt32>(service.size()), service.c_str(),
        static_cast<UInt32>(account.size()), account.c_str(),
        &length, &data, &itemRef);

    if (status != errSecSuccess) {
        if (itemRef) CFRelease(itemRef);
        return core::Result<std::string>::fail(core::ErrorCode::NotFound,
            "Keychain item not found");
    }

    std::string result(static_cast<char*>(data), length);
    SecKeychainItemFreeContent(nullptr, data);
    if (itemRef) CFRelease(itemRef);
    return core::Result<std::string>::ok(std::move(result));
}

core::Result<void> deleteSecret(const std::string& service, const std::string& account) {
    SecKeychainItemRef itemRef = nullptr;
    OSStatus status = SecKeychainFindGenericPassword(
        nullptr,
        static_cast<UInt32>(service.size()), service.c_str(),
        static_cast<UInt32>(account.size()), account.c_str(),
        nullptr, nullptr, &itemRef);
    if (status == errSecSuccess && itemRef) {
        status = SecKeychainItemDelete(itemRef);
        CFRelease(itemRef);
        if (status != errSecSuccess) {
            return core::Result<void>::fail(core::ErrorCode::Internal,
                "SecKeychainItemDelete failed: " + std::to_string(status));
        }
    }
    return core::Result<void>::ok();
}

} // namespace platform::keychain
