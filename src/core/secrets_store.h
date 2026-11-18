// src/core/secrets_store.h
// 应用密钥专表 app_secrets（need305）薄封装：运行时三处原经平台 keychain 的
// 通道（memory.llm / devdesk.schedule）统一改走本表，键名 (service, account)
// 沿旧 keychain 命名空间一一对应。value 恒为分平台保护形态
//（platform/credential_protection 的产物，非明文）；写侧 protect / 读侧
// unprotect 失败原样上抛，不静默回落——「解不开」与「没配过」必须可分辨
//（同 profile_repository 已验证形态）。本表不随 dump 上 git 远端
//（sql_dump defaultSkipTables，决策#11）。
#ifndef DEVDESK_CORE_SECRETS_STORE_H
#define DEVDESK_CORE_SECRETS_STORE_H

#include <string>

#include "result.h"

namespace core::data {
class SqliteDb;
}

namespace core::secrets_store {

// 取明文（行缺 → fail(NotFound)；unprotect 解不开 → 原错误上抛）
core::Result<std::string> get(core::data::SqliteDb& db,
                              const std::string& service,
                              const std::string& account);

// 存明文（protect 失败 → fail 且不落行；已存在则覆盖 value + 刷 updated_at）
core::Result<void> set(core::data::SqliteDb& db,
                       const std::string& service,
                       const std::string& account,
                       const std::string& plaintext);

// 删（条目不存在也算成功，幂等——同 keychain 删除语义）
core::Result<void> erase(core::data::SqliteDb& db,
                         const std::string& service,
                         const std::string& account);

// 以下无 db 重载：经进程级 appDatabase()（main.cpp 落完 schema 后注册的同一
// 连接，database.h）。未注册（nullptr，启动前/单测未挂库）→ fail。
core::Result<std::string> get(const std::string& service,
                              const std::string& account);
core::Result<void> set(const std::string& service,
                       const std::string& account,
                       const std::string& plaintext);
core::Result<void> erase(const std::string& service,
                         const std::string& account);

} // namespace core::secrets_store

#endif // DEVDESK_CORE_SECRETS_STORE_H
