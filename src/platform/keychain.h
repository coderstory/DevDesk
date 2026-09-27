// src/platform/keychain.h
// 平台原生密钥管理（macOS Keychain / Windows DPAPI）
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include <core/result.h>

namespace platform::keychain {

// ---------- 真实实现（按平台拆文件：keychain_win / keychain_posix / keychain_macos）----------
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

// ---------- 注入缝（CPX-14）----------
// 形态：函数指针表 + 单一可替换槽位（最小形态——不引入虚基类体系、不铺开成
// 通用后端抽象），默认指向上面的真实实现。**生产调用形态零变化**：调用点仍写作
// platform::keychain::storeSecret(...)、签名一字不变，内部只多一次间接调用；
// 存储格式 / 编码逻辑不动。
// 用途：多步写协议的失败分支原先无从单测（claude_service 的两段写
// TwoPhaseKeyWrite 依赖「Keychain 写失败」才能走到补偿，而 Keychain 是自由
// 函数、没有注入口）——测试可整表替换成假实现，按需注入 store / load / delete
// 失败并观察补偿结果。
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

// 当前后端槽位。用原子指针：代理线程可能正在调 Keychain，测试换后端不应是
// 数据竞争（生产路径只读、从不写）。
inline std::atomic<const Backend*>& backendSlot() {
    static std::atomic<const Backend*> slot{&defaultBackend()};
    return slot;
}

// 当前后端
inline const Backend& backend() {
    return *backendSlot().load(std::memory_order_relaxed);
}

// 测试注入：安装 b（传 nullptr = 恢复真实实现），返回被替换的旧后端供 RAII
// 复位。仅测试调用。
inline const Backend* setBackendForTest(const Backend* b) {
    return backendSlot().exchange(b ? b : &defaultBackend(),
                                  std::memory_order_relaxed);
}

// ---------- 调用入口（签名与从前一字不变，转发到当前后端）----------

// 存储密钥
inline core::Result<void> storeSecret(const std::string& service,
                                      const std::string& account,
                                      const std::string& secret) {
    return backend().storeSecret(service, account, secret);
}

// 读取密钥
inline core::Result<std::string> loadSecret(const std::string& service,
                                            const std::string& account) {
    return backend().loadSecret(service, account);
}

// 删除密钥
inline core::Result<void> deleteSecret(const std::string& service,
                                       const std::string& account) {
    return backend().deleteSecret(service, account);
}

// 孤儿清理：枚举 service 命名空间下的全部密钥，删除 account 不在
// keepAccounts 里的条目（调用方须拥有该 service 命名空间）。
// 返回实际删除的 account 列表（按平台存储形态返回，如 Windows 为
// 文件名安全化后的形式）。service 命名空间不存在/为空 → 返回空列表。
inline core::Result<std::vector<std::string>> deleteSecretsNotIn(
    const std::string& service,
    const std::vector<std::string>& keepAccounts) {
    return backend().deleteSecretsNotIn(service, keepAccounts);
}

} // namespace platform::keychain