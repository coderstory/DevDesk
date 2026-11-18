// src/core/secrets_store.cpp
// app_secrets 读写实现（契约见 secrets_store.h）。值内容与密钥等价，任何失败
// 路径都不得把它带进日志——本文件不 LOG 值，只上抛错误。
#include "secrets_store.h"

#include "database.h"
#include "time.h"

#include "platform/credential_protection.h"

namespace core::secrets_store {

core::Result<std::string> get(core::data::SqliteDb& db,
                              const std::string& service,
                              const std::string& account) {
    auto prep = db.prepare(
        "SELECT value FROM app_secrets WHERE service = ? AND account = ?");
    if (prep.isFail()) {
        return core::Result<std::string>::fail(
            prep.errorCode(), "app_secrets 查询准备失败: " + prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto b = stmt.bind(1, service); b.isFail()) {
        return core::Result<std::string>::fail(
            b.errorCode(), "app_secrets bind service: " + b.errorMessage());
    }
    if (auto b = stmt.bind(2, account); b.isFail()) {
        return core::Result<std::string>::fail(
            b.errorCode(), "app_secrets bind account: " + b.errorMessage());
    }
    auto rows = stmt.fetchAll();
    if (rows.isFail()) {
        return core::Result<std::string>::fail(
            rows.errorCode(), "app_secrets 查询执行失败: " + rows.errorMessage());
    }
    if (rows.value().empty()) {
        return core::Result<std::string>::fail(
            core::ErrorCode::NotFound,
            "app_secrets 无此条目（service=" + service +
                " account=" + account + "）");
    }
    auto stored = rows.value()[0].getString("value");
    if (stored.isFail()) {
        return core::Result<std::string>::fail(
            stored.errorCode(),
            "app_secrets 取 value 列失败: " + stored.errorMessage());
    }
    // unprotect 失败原样上抛：把不可用伪装成「没配过」会静默改变调用方语义
    return platform::credential::unprotect(stored.value());
}

core::Result<void> set(core::data::SqliteDb& db,
                       const std::string& service,
                       const std::string& account,
                       const std::string& plaintext) {
    auto protectedR = platform::credential::protect(plaintext);
    if (protectedR.isFail()) {
        return core::Result<void>::fail(protectedR.errorCode(),
                                        "app_secrets 凭据保护失败: " +
                                            protectedR.errorMessage());
    }
    auto prep = db.prepare(
        "INSERT OR REPLACE INTO app_secrets(service, account, value, updated_at) "
        "VALUES (?, ?, ?, ?)");
    if (prep.isFail()) {
        return core::Result<void>::fail(prep.errorCode(),
                                        "app_secrets 写入准备失败: " +
                                            prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto b = stmt.bind(1, service); b.isFail())
        return core::Result<void>::fail(b.errorCode(),
                                        "app_secrets bind service: " + b.errorMessage());
    if (auto b = stmt.bind(2, account); b.isFail())
        return core::Result<void>::fail(b.errorCode(),
                                        "app_secrets bind account: " + b.errorMessage());
    if (auto b = stmt.bind(3, protectedR.value()); b.isFail())
        return core::Result<void>::fail(b.errorCode(),
                                        "app_secrets bind value: " + b.errorMessage());
    if (auto b = stmt.bind(4, static_cast<int64_t>(core::time::nowUnix())); b.isFail())
        return core::Result<void>::fail(b.errorCode(),
                                        "app_secrets bind updated_at: " + b.errorMessage());
    auto stepR = stmt.step();
    if (stepR.isFail()) {
        return core::Result<void>::fail(stepR.errorCode(),
                                        "app_secrets 写入执行失败: " +
                                            stepR.errorMessage());
    }
    return core::Result<void>::ok();
}

core::Result<void> erase(core::data::SqliteDb& db,
                         const std::string& service,
                         const std::string& account) {
    auto prep = db.prepare(
        "DELETE FROM app_secrets WHERE service = ? AND account = ?");
    if (prep.isFail()) {
        return core::Result<void>::fail(prep.errorCode(),
                                        "app_secrets 删除准备失败: " +
                                            prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto b = stmt.bind(1, service); b.isFail())
        return core::Result<void>::fail(b.errorCode(),
                                        "app_secrets bind service: " + b.errorMessage());
    if (auto b = stmt.bind(2, account); b.isFail())
        return core::Result<void>::fail(b.errorCode(),
                                        "app_secrets bind account: " + b.errorMessage());
    auto stepR = stmt.step();
    if (stepR.isFail()) {
        return core::Result<void>::fail(stepR.errorCode(),
                                        "app_secrets 删除执行失败: " +
                                            stepR.errorMessage());
    }
    return core::Result<void>::ok();
}

core::Result<std::string> get(const std::string& service,
                              const std::string& account) {
    core::data::SqliteDb* db = core::appDatabase();
    if (!db) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                               "app_secrets：appDatabase 未注册");
    }
    return get(*db, service, account);
}

core::Result<void> set(const std::string& service,
                       const std::string& account,
                       const std::string& plaintext) {
    core::data::SqliteDb* db = core::appDatabase();
    if (!db) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "app_secrets：appDatabase 未注册");
    }
    return set(*db, service, account, plaintext);
}

core::Result<void> erase(const std::string& service,
                         const std::string& account) {
    core::data::SqliteDb* db = core::appDatabase();
    if (!db) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "app_secrets：appDatabase 未注册");
    }
    return erase(*db, service, account);
}

} // namespace core::secrets_store
