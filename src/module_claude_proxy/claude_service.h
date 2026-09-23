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
#include <mutex>
#include <string>
#include <vector>

#include "core/sqlite_db.h"
#include "core/result.h"
#include "platform/http_client.h"
#include "settings_sync.h"

namespace platform::http { class Server; }

namespace module::claude {

class ProxyRouter;

// 把代理凭据写到 appDataDir()/proxy_token.txt（curl 测试便利）。
// 代理的三条启动路径（main 自启 / Claude 页手动启动 / 设置页重生成 token）
// 共用（此前 main 与 settings 各有一份复制粘贴实现）
void writeProxyTokenFile(const std::string& token, int port);

// 单条 Claude API 配置（spec §8.3）
struct Profile {
    int64_t id = 0;
    std::string name;
    std::string baseUrl;        // 例如 https://api.anthropic.com
    std::string apiKeyPlain;    // **仅内存** 用，不入 SQLite；loadApiKey() 后才有值
    std::string apiKeyHint;     // 例如 sk-ant-***xyz（UI 显示用）
    std::string apiKeyCheck;    // 前 8 字符的 SHA256 hex（防误操作校验）
    std::string keychainAccount;// Keychain account（约定：claude:<id>）
    std::string model;          // 默认模型（旧列；语义降级为 default 槽位未配置时的 fallback）
    std::string modelsConfigJson; // 6 槽位模型配置 JSON 原文（空串 = 未配置；provider-config §4.1）
    std::string extraEnvJson;   // 额外 env 键值 JSON 原文（空串 = 未配置）
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

    // 删除 profile（同时清理 Keychain 条目）。
    // Keychain 清理失败会中止删除并返回错误（SQLite 行保留，可重试）
    core::Result<void> deleteProfile(int64_t id);

    // 启动时孤儿清理：把 keychainService_ 命名空间里没有对应
    // claude_profiles 行的密钥清掉（历史 bug 遗留的 claude:* 孤儿）。
    // 返回清理数量
    core::Result<int> cleanupOrphanKeychainEntries();

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

    // ---- 在途流式请求登记（审计 2026-09-23 [9]）----
    // ProxyRouter 在启动上游流后立即 registerActiveStream；stopProxy() /
    // 析构先调 cancelActiveStreams() 再停 server——否则 httplib worker 阻塞
    // 在 provider 的 readChunk 里，Server::stop() 的 join 要等流自然结束
    // （Claude 活动生成期无空闲超时，停止/退出会挂起成幽灵进程）。
    // public：测试用与 stopProxy 相同的顺序驱动验证。
    void registerActiveStream(
        const std::shared_ptr<platform::http::StreamingResponse>& stream);
    void cancelActiveStreams();

    bool isProxyRunning() const;
    int proxyPort() const;            // 0 if not running
    std::string proxyAuthToken() const;

    // ---- Claude Code settings 接管（provider-config Phase C，§5.2/§5.3）----

    // 注入接管通道路径并启用（生产由 main.cpp 一次性注入
    // settings_sync::defaultPaths()）。默认不启用：直接构造的 service
    //（测试）不写任何真实文件
    void setSettingsSyncPaths(const settings_sync::Paths& paths);

    // 注入的接管路径（未注入时 settingsFile 为空）。「导入当前配置」据此
    // 定位 settings.json：生产 = main 注入的 defaultPaths，测试 = 临时目录
    const settings_sync::Paths& settingsSyncPaths() const { return syncPaths_; }

    // 已接管（sidecar 快照存在；设置页「接管状态」行数据源，Phase D UI 用）
    bool isClaudeCodeManaged() const;

    // 最近一次接管的上层覆盖检测警告（Phase D UI 展示；无警告为空）
    const std::vector<std::string>& lastSyncWarnings() const;

    // 启动崩溃补还原：sidecar 残留（上次没走完还原）→ 先还原。
    // main.cpp 在任何 startProxy 之前调用（调用时序见 settings_sync.h）
    void recoverPendingSettingsRestore();

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

    // 接管/还原的内部入口（settingsFile 为空 = 通道未启用，直接返回）
    void syncTakeOver();
    void syncRestore();

    core::data::SqliteDb& db_;
    std::string keychainService_;

    // 代理 server 资源（延迟构造）
    std::unique_ptr<platform::http::Server> server_;
    std::unique_ptr<ProxyRouter> router_;

    // 在途流式上游登记（[9]）。streamsStopping_：stopProxy/析构已发起，
    // 期间新登记的流立即被取消（消掉「cancel 跑完才有流登记进来」的窗口）
    std::mutex streamsMutex_;
    std::vector<std::weak_ptr<platform::http::StreamingResponse>> activeStreams_;
    bool streamsStopping_ = false;

    // Claude Code 接管通道（settingsFile 为空 = 未启用）
    settings_sync::Paths syncPaths_;
    std::vector<std::string> lastSyncWarnings_;
};

// ===== 纯函数（可单测，无 IO）=====

// 调用记录时间列（W10 复审项：只显 HH:MM 会让跨天记录的新旧判断误导）：
//   今日 → "HH:MM"；昨天 → "昨天 HH:MM"；更早 → "MM-DD"
std::string formatCallTime(std::time_t ts, std::time_t now);

} // namespace module::claude
