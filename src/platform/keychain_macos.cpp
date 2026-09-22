// src/platform/keychain_macos.cpp
// macOS 实现：SecKeychain（Keychain Services，Security.framework）
#include "keychain.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <set>
#include <vector>

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

core::Result<std::vector<std::string>> deleteSecretsNotIn(
    const std::string& service,
    const std::vector<std::string>& keepAccounts) {
    std::vector<std::string> removed;
    std::set<std::string> keep(keepAccounts.begin(), keepAccounts.end());

    // 枚举该 service 下的所有 generic password（拿 account 属性）
    const void* keys[] = {kSecClass, kSecMatchLimit, kSecReturnAttributes};
    const void* values[] = {kSecClassGenericPassword, kSecMatchLimitAll,
                            kCFBooleanTrue};
    CFDictionaryRef query = CFDictionaryCreate(
        nullptr, keys, values, 3,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!query) {
        return core::Result<std::vector<std::string>>::fail(
            core::ErrorCode::Internal, "CFDictionaryCreate failed");
    }

    auto fail = [&](const std::string& what, OSStatus st) {
        return core::Result<std::vector<std::string>>::fail(
            core::ErrorCode::Internal,
            what + " failed: " + std::to_string(st));
    };

    CFTypeRef result = nullptr;
    OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (status == errSecItemNotFound) {
        // service 命名空间为空 → 无孤儿
        return core::Result<std::vector<std::string>>::ok(std::move(removed));
    }
    if (status != errSecSuccess) {
        return fail("SecItemCopyMatching", status);
    }

    CFArrayRef entries = static_cast<CFArrayRef>(result);
    const CFIndex count = CFArrayGetCount(entries);
    for (CFIndex i = 0; i < count; ++i) {
        CFDictionaryRef attrs =
            static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(entries, i));
        CFStringRef svc = static_cast<CFStringRef>(
            CFDictionaryGetValue(attrs, kSecAttrService));
        CFStringRef acct = static_cast<CFStringRef>(
            CFDictionaryGetValue(attrs, kSecAttrAccount));
        if (!svc || !acct) continue;

        const char* svcBuf = CFStringGetCStringPtr(svc, kCFStringEncodingUTF8);
        std::string svcStr;
        if (svcBuf) {
            svcStr = svcBuf;
        } else {
            char buf[256];
            if (CFStringGetCString(svc, buf, sizeof(buf),
                                   kCFStringEncodingUTF8)) {
                svcStr = buf;
            }
        }
        if (svcStr != service) continue;

        char acctBuf[256];
        if (!CFStringGetCString(acct, acctBuf, sizeof(acct),
                                kCFStringEncodingUTF8)) {
            continue;
        }
        const std::string account = acctBuf;
        if (keep.count(account)) continue;

        auto del = deleteSecret(service, account);
        if (del.isFail()) {
            CFRelease(entries);
            return core::Result<std::vector<std::string>>::fail(
                del.errorCode(), del.errorMessage());
        }
        removed.push_back(account);
    }
    CFRelease(entries);
    return core::Result<std::vector<std::string>>::ok(std::move(removed));
}

} // namespace platform::keychain
