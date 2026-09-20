#include "keychain.h"
#include <core/logging.h>

#include <filesystem>
#include <fstream>

#if defined(__APPLE__)
#include <Security/Security.h>
#elif defined(_WIN32) || defined(_WIN64)
// TODO: Windows DPAPI
#endif

namespace platform::keychain {

#if defined(__APPLE__)

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

#elif defined(_WIN32) || defined(_WIN64)
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

#else
// 其他平台：使用本地加密文件（开发用）
namespace {
    std::filesystem::path getKeyFilePath(const std::string& service, const std::string& account) {
        auto home = std::getenv("HOME");
        std::filesystem::path dir = home ? std::filesystem::path(home) / ".devdesk" : std::filesystem::temp_directory_path();
        std::filesystem::create_directories(dir);
        return dir / (service + "_" + account + ".key");
    }
}

core::Result<void> storeSecret(const std::string& service, const std::string& account, const std::string& secret) {
    auto path = getKeyFilePath(service, account);
    std::ofstream f(path, std::ios::binary);
    if (!f) return core::Result<void>::fail(core::ErrorCode::FileWriteFailed, "cannot write key file");
    f << secret;
    return core::Result<void>::ok();
}

core::Result<std::string> loadSecret(const std::string& service, const std::string& account) {
    auto path = getKeyFilePath(service, account);
    std::ifstream f(path);
    if (!f) return core::Result<std::string>::fail(core::ErrorCode::NotFound, "key file not found");
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return core::Result<std::string>::ok(std::move(s));
}

core::Result<void> deleteSecret(const std::string& service, const std::string& account) {
    auto path = getKeyFilePath(service, account);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return core::Result<void>::ok();
}

#endif

} // namespace platform::keychain