// src/platform/keychain_macos.cpp
// macOS 实现：SecKeychain（Keychain Services，Security.framework）
#include "keychain.h"
#include "keychain_macos_scan.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <set>
#include <vector>

// 弃用 API 窄抑制：SecKeychain* 系列在 macOS 27 SDK 被标记弃用，平台栈带 -Werror；每个调用点逐语句
// 抑制，不覆盖整文件、不给 target 加 -Wno-deprecated-declarations（会连带屏蔽其它危险的弃用告警）。

namespace platform::keychain {
namespace {

// 空 service / account 守卫：SecKeychainFindGenericPassword 的长度传 0 是**通配**语义（该维度匹配
// 任意值，两个 0 匹配整库任意条目），空 account 会被静默当成「随便一条」→ load 返回别人的密钥、
// delete 删掉别人的条目。本项目空 account 表示「该 profile 没有密钥」，故进 Keychain API 前显式处置：
// load → fail(NotFound)；store → fail(ValidationError)；delete → ok()（幂等空操作）。
bool blankKeyIdentity(const std::string& service, const std::string& account) {
    return service.empty() || account.empty();
}

} // namespace

// macOS 扫描路径测试缝：默认 nullptr = 真实 Security.framework；只把「枚举」「删除」两个边界动作换成
// 可注入函数指针，中间的 service 过滤 + shouldDeleteOrphan 决策 + removed 累积 + 失败短路不动。
namespace macos_scan {
namespace {
const Hooks* g_hooks = nullptr;
} // namespace

const Hooks* setHooksForTest(const Hooks* h) {
    const Hooks* prev = g_hooks;
    g_hooks = h;
    return prev;
}

const Hooks* currentHooks() { return g_hooks; }
} // namespace macos_scan

// 真实实现（经 keychain.h 的函数指针表转发）
core::Result<void> realStoreSecret(const std::string& service,
                                   const std::string& account,
                                   const std::string& secret) {
    if (blankKeyIdentity(service, account)) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "SecKeychainAddGenericPassword rejected: empty service/account "
            "identifies no item (0 长度在 Keychain API 里是通配，见文件内守卫说明)");
    }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    // 先删已存在条目避免冲突
    SecKeychainItemRef itemRef = nullptr;
    OSStatus status = SecKeychainFindGenericPassword(
        nullptr,
        static_cast<UInt32>(service.size()), service.c_str(),
        static_cast<UInt32>(account.size()), account.c_str(),
        nullptr, nullptr, &itemRef);
#pragma clang diagnostic pop
    if (status == errSecSuccess && itemRef) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        SecKeychainItemDelete(itemRef);
#pragma clang diagnostic pop
        CFRelease(itemRef);
    }

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
    if (blankKeyIdentity(service, account)) {
        return core::Result<std::string>::fail(core::ErrorCode::NotFound,
            "Keychain item not found: empty service/account identifies no item "
            "(0 长度在 Keychain API 里是通配，见文件内守卫说明)");
    }

    SecKeychainItemRef itemRef = nullptr;
    UInt32 length = 0;
    void* data = nullptr;

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
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    SecKeychainItemFreeContent(nullptr, data);
#pragma clang diagnostic pop
    if (itemRef) CFRelease(itemRef);
    return core::Result<std::string>::ok(std::move(result));
}

core::Result<void> realDeleteSecret(const std::string& service, const std::string& account) {
    if (blankKeyIdentity(service, account)) {
        // 无身份 = 没有这条 → 幂等空操作
        return core::Result<void>::ok();
    }

    SecKeychainItemRef itemRef = nullptr;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    OSStatus status = SecKeychainFindGenericPassword(
        nullptr,
        static_cast<UInt32>(service.size()), service.c_str(),
        static_cast<UInt32>(account.size()), account.c_str(),
        nullptr, nullptr, &itemRef);
#pragma clang diagnostic pop
    if (status == errSecSuccess && itemRef) {
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

// 真实枚举：扫全库 generic password（kSecMatchLimitAll），取出 service 与本次查询相同的条目。
// **不做删除决策**——决策留在 realDeleteSecretsNotIn 的循环里，测试缝只需替换枚举/删除两个边界动作。
core::Result<std::vector<macos_scan::ScannedEntry>> realEnumerateScanEntries(
    const std::string& service) {
    std::vector<macos_scan::ScannedEntry> found;

    const void* keys[] = {kSecClass, kSecMatchLimit, kSecReturnAttributes};
    const void* values[] = {kSecClassGenericPassword, kSecMatchLimitAll,
                            kCFBooleanTrue};
    CFDictionaryRef query = CFDictionaryCreate(
        nullptr, keys, values, 3,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!query) {
        return core::Result<std::vector<macos_scan::ScannedEntry>>::fail(
            core::ErrorCode::Internal, "CFDictionaryCreate failed");
    }

    CFTypeRef result = nullptr;
    OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    // service 命名空间为空 → 无条目（上层据此返回空 removed）
    if (status == errSecItemNotFound) {
        return core::Result<std::vector<macos_scan::ScannedEntry>>::ok(
            std::move(found));
    }
    if (status != errSecSuccess) {
        return core::Result<std::vector<macos_scan::ScannedEntry>>::fail(
            core::ErrorCode::Internal,
            "SecItemCopyMatching failed: " + std::to_string(status));
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
        found.emplace_back(svcStr, std::string(acctBuf));
    }
    CFRelease(entries);
    return core::Result<std::vector<macos_scan::ScannedEntry>>::ok(
        std::move(found));
}

core::Result<std::vector<std::string>> realDeleteSecretsNotIn(
    const std::string& service,
    const std::vector<std::string>& keepAccounts) {
    std::vector<std::string> removed;
    std::set<std::string> keep(keepAccounts.begin(), keepAccounts.end());

    // 边界动作取注入钩子：默认 nullptr = 真实枚举/真实删除
    const macos_scan::Hooks* hooks = macos_scan::currentHooks();

    auto foundR = (hooks != nullptr && hooks->enumerate != nullptr)
                      ? hooks->enumerate(service)
                      : realEnumerateScanEntries(service);
    if (foundR.isFail()) {
        return core::Result<std::vector<std::string>>::fail(
            foundR.errorCode(), foundR.errorMessage());
    }
    const std::vector<macos_scan::ScannedEntry>& found = foundR.value();

    for (const auto& entry : found) {
        const std::string& entryService = entry.first;
        const std::string& account = entry.second;
        // service 过滤放在循环里（不只在枚举段）：任何枚举来源（含测试注入）都过这道闸。
        if (entryService != service) continue;
        // 白名单前缀之外的 account 一律保留——shouldDeleteOrphan 统一决策，见 keychain.h
        if (!shouldDeleteOrphan(account, keep)) continue;

        // 直调真实实现（不经 dispatcher）：注入层只服务调用方，不渗进 real* 内部。
        core::Result<void> del =
            (hooks != nullptr && hooks->erase != nullptr)
                ? hooks->erase(entryService, account)
                : realDeleteSecret(entryService, account);
        if (del.isFail()) {
            return core::Result<std::vector<std::string>>::fail(
                del.errorCode(), del.errorMessage());
        }
        removed.push_back(account);
    }
    return core::Result<std::vector<std::string>>::ok(std::move(removed));
}

} // namespace platform::keychain
