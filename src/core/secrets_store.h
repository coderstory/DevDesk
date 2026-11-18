// src/core/secrets_store.h
// 应用密钥专表 app_secrets（need305）薄封装：运行时三处密钥通道
//（memory.llm / devdesk.schedule）统一走本表，键名 (service, account)
// 与调用方常量一一对应。value 恒为分平台保护形态
//（经下方保护后端缝的产物，非明文）；写侧 protect / 读侧
// unprotect 失败原样上抛，不静默回落——「解不开」与「没配过」必须可分辨
//（同 profile_repository 已验证形态）。本表不随 dump 上 git 远端
//（sql_dump defaultSkipTables，决策#11）。
#ifndef DEVDESK_CORE_SECRETS_STORE_H
#define DEVDESK_CORE_SECRETS_STORE_H

#include <atomic>
#include <string>

#include "result.h"

namespace core::data {
class SqliteDb;
}

namespace core::secrets_store {

// ---- 凭据保护后端缝（need305 审计 P0，依赖倒置）----
// AGENTS §六：core 不依赖 platform。本表原在 .cpp 直接 include
// platform/credential_protection.h，是全仓唯一破口；修法=缝下沉 core，
// platform 侧变实现，由 main.cpp 启动时 installBackend 装配，单测经
// setBackendForTest 注入假实现（形态同 platform::credential 自带注入缝）。
struct Backend {
    core::Result<std::string> (*protect)(const std::string& plaintext);
    core::Result<std::string> (*unprotect)(const std::string& stored);
};

// 默认后端=明文直通，镜像 credential_protection 的 2026-11-03 全平台明文
// 口径：测试环境未装配平台实现时行为与生产一致（test_wecom_channel /
// test_memory_engine 依赖此默认，不经注入缝）。
inline const Backend& defaultBackend() {
    static const Backend b{
        [](const std::string& p) { return core::Result<std::string>::ok(p); },
        [](const std::string& s) { return core::Result<std::string>::ok(s); }};
    return b;
}

// 原子指针：后台线程可能正在调 protect，换后端不能是数据竞争。
inline std::atomic<const Backend*>& backendSlot() {
    static std::atomic<const Backend*> slot{&defaultBackend()};
    return slot;
}

// 生产装配（main.cpp 启动时一次；b 非空才覆盖，幂等）。
inline void installBackend(const Backend* b) {
    if (b) backendSlot().store(b, std::memory_order_relaxed);
}

// 注入缝：仅测试调用；nullptr = 恢复默认直通，返回旧后端供 RAII 复位。
inline const Backend* setBackendForTest(const Backend* b) {
    return backendSlot().exchange(b ? b : &defaultBackend(),
                                  std::memory_order_relaxed);
}

// 明文 → 存储形态：空串→空串（口径同 platform::credential::protect）。
inline core::Result<std::string> protect(const std::string& plaintext) {
    if (plaintext.empty()) return core::Result<std::string>::ok(std::string());
    return backendSlot().load(std::memory_order_relaxed)->protect(plaintext);
}

// 存储形态 → 明文；解不开由后端原样上抛，不静默回落空串。
inline core::Result<std::string> unprotect(const std::string& stored) {
    if (stored.empty()) return core::Result<std::string>::ok(std::string());
    return backendSlot().load(std::memory_order_relaxed)->unprotect(stored);
}


// 取明文（行缺 → fail(NotFound)；unprotect 解不开 → 原错误上抛）
core::Result<std::string> get(core::data::SqliteDb& db,
                              const std::string& service,
                              const std::string& account);

// 存明文（protect 失败 → fail 且不落行；已存在则覆盖 value + 刷 updated_at）
core::Result<void> set(core::data::SqliteDb& db,
                       const std::string& service,
                       const std::string& account,
                       const std::string& plaintext);

// 删（条目不存在也算成功，幂等删除语义）
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
