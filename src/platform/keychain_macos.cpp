// src/platform/keychain_macos.cpp
// macOS 实现：SecKeychain（Keychain Services，Security.framework）
#include "keychain.h"
#include "keychain_macos_scan.h"

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
namespace {

// ── 空 service / 空 account 守卫（2026-09-29）──────────────────────────
// 为什么不靠 Keychain 自身返回 notFound：`SecKeychainFindGenericPassword` 的
// serviceLength / accountLength 传 0 是**通配**语义——该维度匹配「任意值」
//（两个都传 0 则匹配整库任意 generic password），只要命中一条就返回
// errSecSuccess，而不是 errSecItemNotFound。于是「account 为空」会被静默当成
// 「随便给我一条」：
//   · load   → 返回同 service 下**别的**条目的密钥（不是「没有」）
//   · delete → SecKeychainItemDelete 删掉的是随机一条**别人的**条目
//   · store  → 先做的那步「删已存在条目」同样通配 → 通配覆盖 + 写一条
//              空 account 的条目（此后 load 再也认不出它，成孤儿）
// 而空 account 在本项目里是**有确切含义**的：ProfileRepository 用空
// keychain_account 表示「该 profile 没有密钥」（反向同步「缺席即清空」把三列
// 元数据一并清空）。此时唯一正确的回答是「没有这条」，绝不能是「随手拿一条」。
// 空 service 同理（serviceLength 0 也是通配，会跨 service 命中）。
// 故在进入 Keychain API **之前**显式处置；三种操作的语义各自定死：
//   load   → fail(NotFound)：条目身份不成立 = 查不到（沿用本函数既有的失败码）
//   store  → fail(ValidationError)：无身份可写；既不做那步通配删旧，也不写一条
//                                  「查出不来」的孤儿条目
//   delete → ok()（幂等空操作）：无身份 = 没有这条，与既有「delete 不存在 →
//                                 ok（幂等）」一致，且不进入通配删除
// ─────────────────────────────────────────────────────────────────────
bool blankKeyIdentity(const std::string& service, const std::string& account) {
    return service.empty() || account.empty();
}

} // namespace

// ---- macOS 扫描路径测试缝（keychain_macos_scan.h，merge-review R2）----
// 默认 nullptr = 真实 Security.framework 调用；只把「枚举」与「删除」两个边界
// 动作换成可注入函数指针，中间的 service 过滤 + shouldDeleteOrphan 决策 +
// removed 累积 + 失败短路一行不动（生产路径逐字同原实现）。
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

// 真实实现（CPX-14 起经 keychain.h 的函数指针表转发，见那里的「注入缝」段）
core::Result<void> realStoreSecret(const std::string& service,
                                   const std::string& account,
                                   const std::string& secret) {
    if (blankKeyIdentity(service, account)) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "SecKeychainAddGenericPassword rejected: empty service/account "
            "identifies no item (0 长度在 Keychain API 里是通配，见文件内守卫说明)");
    }

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
    if (blankKeyIdentity(service, account)) {
        return core::Result<std::string>::fail(core::ErrorCode::NotFound,
            "Keychain item not found: empty service/account identifies no item "
            "(0 长度在 Keychain API 里是通配，见文件内守卫说明)");
    }

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
    if (blankKeyIdentity(service, account)) {
        // 无身份 = 没有这条 → 幂等空操作（见文件内守卫说明）
        return core::Result<void>::ok();
    }

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

// 真实枚举：扫全库 generic password（kSecMatchLimitAll），取出 service 属性与
// 本次查询 service 相同的 (service, account) 对。**不做任何删除决策**——决策留在
// realDeleteSecretsNotIn 的扫描循环里；两段职责单一，测试缝只需替换枚举/删除
// 两个边界动作就能覆盖整条接线。
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
    // service 命名空间为空 → 无条目（上层据此返回空 removed，与原行为一致）
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

    // 边界动作取注入钩子（keychain_macos_scan.h 测试缝，merge-review R2）：
    // 默认 nullptr = 真实 Security.framework 枚举 / 真实删除，生产路径与拆分前
    // 逐字等价（决策、service 过滤、removed 累积、失败短路一行未动）
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
        // service 命名空间过滤：只处理本次查询 service 下的条目。放在循环里
        // （而不是只在枚举段）——这样**任何**枚举来源（含测试注入的假命名空间）
        // 都过这道闸，「扫到就删」的回退写法必红。真实枚举段的同款过滤是
        // 纯优化（提前跳过，省掉全库 account 的字符串转换），不是唯一防线。
        if (entryService != service) continue;
        // P0 护栏②：白名单前缀（claude:）之外的 account 一律保留——
        // shouldDeleteOrphan 统一决策，见 keychain.h 说明
        if (!shouldDeleteOrphan(account, keep)) continue;

        // 直调真实实现（不经 dispatcher）：真实实现保持自洽——注入层只服务
        // 调用方，不反向渗进 real* 内部（CPX-14）。钩子的 erase 同理只替换边界
        // 动作，决策仍在本循环内。
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
