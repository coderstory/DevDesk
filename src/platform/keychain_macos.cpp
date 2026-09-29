// src/platform/keychain_macos.cpp
// macOS 实现：SecKeychain（Keychain Services，Security.framework）
#include "keychain.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <set>
#include <vector>

// ── 弃用 API 窄抑制（macOS 27 SDK，2026-09-28）──────────────────────────
// 本文件是 macOS 平台实现文件。`SecKeychain*` 系列（FindGenericPassword /
// AddGenericPassword / ItemDelete / ItemFreeContent）在 macOS 27 SDK 被标记
// 弃用，平台栈自身带 -Werror（CMakeLists.txt:122 / src/platform/CMakeLists.txt:79）
// → 构建阻塞。下面的每个调用点用 `#pragma clang diagnostic push/ignored/pop`
// **逐语句**窄抑制，不覆盖整文件、不给 target 加 -Wno-deprecated-declarations
//（那会连带屏蔽其它真正危险的弃用告警）。
// 迁移到 `SecItem*`（SecItemAdd / SecItemCopyMatching / SecItemDelete）是**独立
// 重构**：涉及访问控制与权限语义变化（kSecAttrAccessible、app 沙箱／钥匙串
// 访问组的语义不同），在本轮无法真机验证的前提下不做。本次仅为消除构建阻塞。
// ──────────────────────────────────────────────────────────────────────

namespace platform::keychain {

// 真实实现（CPX-14 起经 keychain.h 的函数指针表转发，见那里的「注入缝」段）
core::Result<void> realStoreSecret(const std::string& service,
                                   const std::string& account,
                                   const std::string& secret) {
    // SecKeychain* 为 macOS 27 SDK 弃用 API：仅抑制本语句告警（见文件头说明）
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    // 删除已存在条目（避免冲突）
    SecKeychainItemRef itemRef = nullptr;
    OSStatus status = SecKeychainFindGenericPassword(
        nullptr,
        static_cast<UInt32>(service.size()), service.c_str(),
        static_cast<UInt32>(account.size()), account.c_str(),
        nullptr, nullptr, &itemRef);
#pragma clang diagnostic pop
    if (status == errSecSuccess && itemRef) {
        // SecKeychain* 为 macOS 27 SDK 弃用 API：仅抑制本语句告警（见文件头说明）
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        SecKeychainItemDelete(itemRef);
#pragma clang diagnostic pop
        CFRelease(itemRef);
    }

    // SecKeychain* 为 macOS 27 SDK 弃用 API：仅抑制本语句告警（见文件头说明）
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    status = SecKeychainAddGenericPassword(
        nullptr,
        static_cast<UInt32>(service.size()), service.c_str(),
        static_cast<UInt32>(account.size()), account.c_str(),
        static_cast<UInt32>(secret.size()), secret.c_str(),
        nullptr);
#pragma clang diagnostic pop

    if (status != errSecSuccess) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
            "SecKeychainAddGenericPassword failed: " + std::to_string(status));
    }
    return core::Result<void>::ok();
}

core::Result<std::string> realLoadSecret(const std::string& service,
                                         const std::string& account) {
    SecKeychainItemRef itemRef = nullptr;
    UInt32 length = 0;
    void* data = nullptr;

    // SecKeychain* 为 macOS 27 SDK 弃用 API：仅抑制本语句告警（见文件头说明）
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    OSStatus status = SecKeychainFindGenericPassword(
        nullptr,
        static_cast<UInt32>(service.size()), service.c_str(),
        static_cast<UInt32>(account.size()), account.c_str(),
        &length, &data, &itemRef);
#pragma clang diagnostic pop

    if (status != errSecSuccess) {
        if (itemRef) CFRelease(itemRef);
        return core::Result<std::string>::fail(core::ErrorCode::NotFound,
            "Keychain item not found");
    }

    std::string result(static_cast<char*>(data), length);
    // SecKeychain* 为 macOS 27 SDK 弃用 API：仅抑制本语句告警（见文件头说明）
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    SecKeychainItemFreeContent(nullptr, data);
#pragma clang diagnostic pop
    if (itemRef) CFRelease(itemRef);
    return core::Result<std::string>::ok(std::move(result));
}

core::Result<void> realDeleteSecret(const std::string& service, const std::string& account) {
    SecKeychainItemRef itemRef = nullptr;
    // SecKeychain* 为 macOS 27 SDK 弃用 API：仅抑制本语句告警（见文件头说明）
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    OSStatus status = SecKeychainFindGenericPassword(
        nullptr,
        static_cast<UInt32>(service.size()), service.c_str(),
        static_cast<UInt32>(account.size()), account.c_str(),
        nullptr, nullptr, &itemRef);
#pragma clang diagnostic pop
    if (status == errSecSuccess && itemRef) {
        // SecKeychain* 为 macOS 27 SDK 弃用 API：仅抑制本语句告警（见文件头说明）
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        status = SecKeychainItemDelete(itemRef);
#pragma clang diagnostic pop
        CFRelease(itemRef);
        if (status != errSecSuccess) {
            return core::Result<void>::fail(core::ErrorCode::Internal,
                "SecKeychainItemDelete failed: " + std::to_string(status));
        }
    }
    return core::Result<void>::ok();
}

core::Result<std::vector<std::string>> realDeleteSecretsNotIn(
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
        if (!CFStringGetCString(acct, acctBuf, sizeof(acctBuf),
                                kCFStringEncodingUTF8)) {
            continue;
        }
        const std::string account = acctBuf;
        if (keep.count(account)) continue;

        // 直调真实实现（不经 dispatcher）：真实实现保持自洽——注入层只服务
        // 调用方，不反向渗进 real* 内部（CPX-14）
        auto del = realDeleteSecret(service, account);
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
