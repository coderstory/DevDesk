// src/platform/keychain.h
// 平台原生密钥管理（macOS Keychain / Windows DPAPI）
#pragma once

#include <atomic>
#include <set>
#include <string>
#include <vector>

#include <core/result.h>

namespace platform::keychain {

// 孤儿清理白名单前缀：deleteSecretsNotIn 的语义是「凡不在 keep 里的都删」。本应用 service 下条目
// 只有 profile 密钥（account 约定 claude:<id>），前缀之外一律保留；前缀比较大小写敏感（account 由本应用恒小写生成）。
inline bool deletableOrphanAccount(const std::string& account) {
    return account.rfind("claude:", 0) == 0;
}

// 统一决策：不在 keep 里**且**命中白名单前缀才允许删。三平台删除分支都必须经它。
inline bool shouldDeleteOrphan(const std::string& account,
                               const std::set<std::string>& keepAccounts) {
    return keepAccounts.count(account) == 0 && deletableOrphanAccount(account);
}

// 真实实现
core::Result<void> realStoreSecret(const std::string& service,
                                   const std::string& account,
                                   const std::string& secret);
core::Result<std::string> realLoadSecret(const std::string& service,
                                         const std::string& account);
core::Result<void> realDeleteSecret(const std::string& service,
                                    const std::string& account);
core::Result<std::vector<std::string>> realDeleteSecretsNotIn(
    const std::string& service,
    const std::vector<std::string>& keepAccounts);

// 注入缝：函数指针表 + 单一可替换槽位，默认指向真实实现，生产调用形态零变化；
// 测试可整表替换成假实现注入失败并观察补偿。
struct Backend {
    core::Result<void> (*storeSecret)(const std::string& service,
                                      const std::string& account,
                                      const std::string& secret);
    core::Result<std::string> (*loadSecret)(const std::string& service,
                                            const std::string& account);
    core::Result<void> (*deleteSecret)(const std::string& service,
                                       const std::string& account);
    core::Result<std::vector<std::string>> (*deleteSecretsNotIn)(
        const std::string& service,
        const std::vector<std::string>& keepAccounts);
};

// 默认后端 = 真实实现（inline 函数的静态局部量 = 全程序一份）
inline const Backend& defaultBackend() {
    static const Backend b{&realStoreSecret, &realLoadSecret,
                           &realDeleteSecret, &realDeleteSecretsNotIn};
    return b;
}

// 当前后端槽位。原子指针：代理线程可能正在调 Keychain，测试换后端不应是数据竞争（生产路径只读）。
inline std::atomic<const Backend*>& backendSlot() {
    static std::atomic<const Backend*> slot{&defaultBackend()};
    return slot;
}

inline const Backend& backend() {
    return *backendSlot().load(std::memory_order_relaxed);
}

// 测试注入：安装 b（nullptr = 恢复真实实现），返回旧后端供 RAII 复位。仅测试调用。
inline const Backend* setBackendForTest(const Backend* b) {
    return backendSlot().exchange(b ? b : &defaultBackend(),
                                  std::memory_order_relaxed);
}

// 调用入口（转发到当前后端）

inline core::Result<void> storeSecret(const std::string& service,
                                      const std::string& account,
                                      const std::string& secret) {
    return backend().storeSecret(service, account, secret);
}

inline core::Result<std::string> loadSecret(const std::string& service,
                                            const std::string& account) {
    return backend().loadSecret(service, account);
}

inline core::Result<void> deleteSecret(const std::string& service,
                                       const std::string& account) {
    return backend().deleteSecret(service, account);
}

// 孤儿清理：枚举 service 命名空间下的全部密钥，删除 account 不在 keepAccounts 里的条目（调用方须
// 拥有该 service）。返回实际删除的 account 列表（按平台存储形态）；命名空间不存在/为空 → 空列表。
inline core::Result<std::vector<std::string>> deleteSecretsNotIn(
    const std::string& service,
    const std::vector<std::string>& keepAccounts) {
    return backend().deleteSecretsNotIn(service, keepAccounts);
}

} // namespace platform::keychain