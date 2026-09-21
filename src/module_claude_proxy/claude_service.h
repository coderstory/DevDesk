// src/module_claude_proxy/claude_service.h
// Claude API Profile 服务层 + 本地 HTTP 代理（spec §8.3 + §10.4）
//
// 设计要点：
//   - API key 明文从不入 SQLite（spec §6.2.2）
//   - 全部 API key 由 platform::keychain 保管（macOS Keychain / 文件后备）
//   - SQLite 中只存引用 + 元数据 + 前 8 字符 SHA256 hex（防误删校验用）
//   - is_active 字段用 UNIQUE INDEX WHERE is_active=1 保证全局唯一
//   - Profile.apiKeyPlain 仅在 loadApiKey() 时填充，平时为空字符串
//   - 代理 server 由 ClaudeService 独占（仅一份实例）
#pragma once

#include <atomic>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "core/sqlite_db.h"
#include "core/result.h"
#include "platform/http_client.h"

namespace platform::http { class Server; }

namespace module::claude {

class ProxyRouter;

// 单条 Claude API 配置（spec §8.3）
struct Profile {
    int64_t id = 0;
    std::string name;
    std::string baseUrl;        // 例如 https://api.anthropic.com
    std::string apiKeyPlain;    // **仅内存** 用，不入 SQLite；loadApiKey() 后才有值
    std::string apiKeyHint;     // 例如 sk-ant-***xyz（UI 显示用）
    std::string apiKeyCheck;    // 前 8 字符的 SHA256 hex（防误操作校验）
    std::string keychainAccount;// Keychain account（约定：claude:<id>）
    std::string model;          // 默认模型
    bool isActive = false;
    std::time_t createdAt = 0;
    std::time_t updatedAt = 0;
};

class ClaudeService {
public:
    // 构造时不持有所有权，db 由调用方确保生命周期
    // keychainService 默认 "DevDesk"；测试可传 "DevDeskTest" 隔离
    explicit ClaudeService(core::data::SqliteDb& db,
                           const std::string& keychainService = "DevDesk");
    ~ClaudeService();

    ClaudeService(const ClaudeService&) = delete;
    ClaudeService& operator=(const ClaudeService&) = delete;

    // ---- Profile CRUD ----

    // 列出所有 profile（apiKeyPlain 全部为空字符串）
    core::Result<std::vector<Profile>> listProfiles();

    // 取单个 profile（apiKeyPlain 为空）
    core::Result<Profile> getProfile(int64_t id);

    // 创建并保存到 SQLite + Keychain
    core::Result<Profile> createProfile(const std::string& name,
                                       const std::string& baseUrl,
                                       const std::string& apiKey,
                                       const std::string& model);

    // 更新 profile（profile.id 必须已存在）
    //   - 若 profile.apiKeyPlain 非空，则覆盖 Keychain
    core::Result<void> updateProfile(const Profile& profile);

    // 删除 profile（同时清理 Keychain 条目）
    core::Result<void> deleteProfile(int64_t id);

    // ---- API key ----

    // 显式从 Keychain 加载明文 API key（仅在需要发请求时调用，用完即丢弃）
    core::Result<std::string> loadApiKey(int64_t profileId);

    // ---- 激活 ----

    core::Result<Profile> getActiveProfile();
    core::Result<void> setActiveProfile(int64_t id);

    // ---- 连通性测试 ----

    core::Result<platform::http::Response> testConnection(const Profile& profile);

    // ---- 调用日志 ----

    struct CallLogEntry {
        int64_t id = 0;
        int64_t profileId = 0;
        std::string method;
        std::string path;
        int statusCode = 0;
        int64_t latencyMs = 0;
        int64_t requestBytes = 0;
        int64_t responseBytes = 0;
        std::string error;
        std::time_t startedAt = 0;
    };

    core::Result<void> logCall(const CallLogEntry& entry);
    core::Result<std::vector<CallLogEntry>> recentCalls(int limit = 20);

    // ---- 代理 server 生命周期 ----

    // 启动本地 HTTP 代理
    //   - bindAddress：默认 "127.0.0.1"
    //   - port：0 = 由 OS 分配
    //   - authToken：客户端必须 Bearer 此 token 才能调用代理
    core::Result<int> startProxy(const std::string& bindAddress = "127.0.0.1",
                                 int port = 8888,
                                 const std::string& authToken = "");

    // 停止代理
    void stopProxy();

    bool isProxyRunning() const;
    int proxyPort() const;            // 0 if not running
    std::string proxyAuthToken() const;

    // ---- 全局单例（main.cpp 注册，claude_module.cpp 读状态） ----

    static void setGlobal(ClaudeService* svc);
    static ClaudeService* global();

private:
    // 从一行 SqlRow 构造 Profile（不含 apiKeyPlain）
    core::Result<Profile> rowToProfile(const core::data::SqlRow& row) const;

    static std::string sha256HexPrefix(const std::string& s);
    static std::string makeApiKeyHint(const std::string& apiKey);

    // 生成 auth token（32 hex chars from std::random_device）
    static std::string generateAuthToken();

    core::data::SqliteDb& db_;
    std::string keychainService_;

    // 代理 server 资源（延迟构造）
    std::unique_ptr<platform::http::Server> server_;
    std::unique_ptr<ProxyRouter> router_;
};

} // namespace module::claude
