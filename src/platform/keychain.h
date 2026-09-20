// src/platform/keychain.h
// 平台原生密钥管理（macOS Keychain / Windows DPAPI）
#pragma once

#include <string>

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

} // namespace platform::keychain