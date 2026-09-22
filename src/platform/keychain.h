// src/platform/keychain.h
// 平台原生密钥管理（macOS Keychain / Windows DPAPI）
#pragma once

#include <string>
#include <vector>

#include <core/result.h>

namespace platform::keychain {

// 存储密钥
core::Result<void> storeSecret(const std::string& service,
                                 const std::string& account,
                                 const std::string& secret);

// 读取密钥
core::Result<std::string> loadSecret(const std::string& service,
                                      const std::string& account);

// 删除密钥
core::Result<void> deleteSecret(const std::string& service,
                                 const std::string& account);

// 孤儿清理：枚举 service 命名空间下的全部密钥，删除 account 不在
// keepAccounts 里的条目（调用方须拥有该 service 命名空间）。
// 返回实际删除的 account 列表（按平台存储形态返回，如 Windows 为
// 文件名安全化后的形式）。service 命名空间不存在/为空 → 返回空列表。
core::Result<std::vector<std::string>> deleteSecretsNotIn(
    const std::string& service,
    const std::vector<std::string>& keepAccounts);

} // namespace platform::keychain