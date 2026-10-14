// src/module_claude_proxy/claude_service.h
// Claude API Profile 服务层 + 本地 HTTP 代理（全局单例）。
// 凭据不变量：存 claude_profiles.api_key_encrypted（分平台保护）；api_key_hint =
// 脱敏形态，api_key_check = 全量 SHA256（变更判据）；Profile.apiKeyPlain 仅
// loadApiKey() 填充或作 update 覆盖信号；is_active 全局唯一（UNIQUE INDEX）。
#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "call_log_store.h"
#include "core/proxy_port.h"
#include "core/sqlite_db.h"
#include "core/result.h"
#include "profile_repository.h"
#include "platform/http_client.h"
#include "platform/terminal_launch.h"
#include "proxy_runtime.h"
#include "settings_sync.h"
#include "settings_takeover.h"

namespace module::claude {

// 上游会话标识：部分网关要求 x-opencode-session，缺失回 400 MissingSessionID。
// 客户端不带、由代理按方言注入；进程内稳定一份，保证上游路由/缓存亲和。
const std::string& claudeUpstreamSessionId();

// Profile / SharedConfig / 两段写助手的类型定义在 profile_repository.h（本头 include 透出）

class ClaudeService {
public:
    // db 不持有所有权，由调用方保证生命周期。
    // keychainService：空 = 按数据根派生 "DevDesk-<短哈希>"；非空 = 原样用（测试隔离）。
    explicit ClaudeService(core::data::SqliteDb& db,
                           const std::string& keychainService = std::string());
    ~ClaudeService();

    ClaudeService(const ClaudeService&) = delete;
    ClaudeService& operator=(const ClaudeService&) = delete;

    // Profile CRUD

    // 列出所有 profile（apiKeyPlain 均为空串）
    core::Result<std::vector<Profile>> listProfiles();

    // 取单个 profile（apiKeyPlain 为空）
    core::Result<Profile> getProfile(int64_t id);

    // 创建并保存到 SQLite（凭据入 api_key_encrypted）。activateIfNoneActive=true：
    // 无激活则自动激活（不抢占已有）；authField 非法值写侧归一 'both'；
    // dialectOverride 空 = 按 host 推断、非法值归空。
    core::Result<Profile> createProfile(const std::string& name,
                                       const std::string& baseUrl,
                                       const std::string& apiKey,
                                       const std::string& model,
                                       bool activateIfNoneActive = false,
                                       int64_t sharedConfigId = 0,
                                       const std::string& authField = "both",
                                       const std::string& dialectOverride =
                                           std::string(),
                                       int64_t fallbackProfileId = 0,
                                       const std::string& extraSettingsJson =
                                           std::string());

    // 更新 profile（id 必须已存在）；apiKeyPlain 非空则覆盖凭据三列。
    core::Result<void> updateProfile(const Profile& profile);

    // 删除 profile（清空凭据三列再删行）；凭据清理失败则中止、行保留可重试。
    // 删激活 profile 且代理运行中 → 成功后 stopProxy()；proxyStopped 出参供 UI toast。
    core::Result<void> deleteProfile(int64_t id, bool* proxyStopped = nullptr);

    // 启动时清理旧 Keychain 残留（只删 claude:*，护栏：数据根零 profile 整体跳过）。
    core::Result<int> cleanupOrphanKeychainEntries();

    // 迁移旧 Keychain 凭据入库：api_key_encrypted 仍空且 keychain_account 列存在的 profile，
    // 逐行读旧 Keychain → protect → 写回凭据三列；失败不中断（LOG_WARN 继续）。
    core::Result<int> backfillCredentialsFromKeychain();

    // API key

    // 加载明文 API key（发请求时调用，用完即丢弃）。列空 = NotFound；
    // 解不开 = 原样透传错误，不静默回落空串。
    core::Result<std::string> loadApiKey(int64_t profileId);

    // ProfileRepository 只读访问口：必须共用同一实例（两实例会「读 A 库、用 B 命名空间」）。
    ProfileRepository& profiles() { return profileRepository_; }

    // 激活

    core::Result<Profile> getActiveProfile();
    core::Result<void> setActiveProfile(int64_t id);

    // 复制 Profile：名称加「副本」后缀（重名递增避让）；API key 一并复制（源缺 → fail）；
    // 副本不激活；二段补写失败 → best-effort 删副本回滚。
    core::Result<Profile> duplicateProfile(int64_t srcId);

    // 共享配置片段（改动当场生效）
    // 改/删/解绑后，激活 profile 代理在跑则立即 re-takeover；删除同事务解绑；
    // 解绑后同名键回落 profile 自身值（片段值从不写进 profile 行）。
    core::Result<std::vector<SharedConfig>> listSharedConfigs();
    core::Result<SharedConfig> createSharedConfig(const std::string& name,
                                                 const std::string& extraSettingsJson);
    core::Result<void> updateSharedConfig(const SharedConfig& config);
    core::Result<void> deleteSharedConfig(int64_t id);

    // 用当前激活 profile（含所绑片段）重写 settings.json；通道未注入/开关关 →
    // no-op 成功。本入口不做前置校验，调用方必须先过 validateProxyPreconditions。
    core::Result<void> reapplyTakeOver() { return syncTakeOver(); }

    // failover 备用解析
    // 读 fallback_profile_id → getProfile + loadApiKey，成功返回含明文 key 的备用；
    // 任一失败 / id==0 → nullopt（proxy_router 原样回错）。只换一跳；纯读不改状态。
    std::optional<Profile> resolveFallbackProfile(const Profile& profile);

    // 模型槽位解析
    // 激活 profile 的 models_config 里 slot → 实际 model id；"def"/"default" 等价；
    // 无激活 / 槽位空 / 未知键 → nullopt，default 槽位额外回落 profile.model。
    std::optional<std::string> resolveModelSlot(const std::string& slot);

    // 连通性测试

    core::Result<platform::http::Response> testConnection(const Profile& profile);

    // 调用日志

    // 数据模型与读写在 CallLogStore；别名保持公开类型名，方法转发 callLogStore_。
    using CallLogEntry = CallLogStore::CallLogEntry;
    using CallLogQuery = CallLogStore::CallLogQuery;

    core::Result<void> logCall(const CallLogEntry& entry);
    core::Result<std::vector<CallLogEntry>> recentCalls(int limit = 20);

    // 过滤 + 分页查询（日志弹窗搜索框 / 状态码下拉 / 翻页共用）
    core::Result<std::vector<CallLogEntry>> queryCalls(const CallLogQuery& q);

    // 满足过滤条件的总条数（忽略 limit/offset；UI 算页数、判断有无下一页）
    core::Result<int> countCalls(const CallLogQuery& q);

    // 最近代理请求事件（气泡内容源）
    // ProxyRouter 在每条请求终态记一次 statusCode 进单槽（后写覆盖先写）：
    // 0 = 上游不可达。takeProxyEvent() 读走即清；滞留超 kProxyEventFreshSec 过期丢弃。
    static constexpr std::time_t kProxyEventFreshSec =
        ProxyRuntime::kProxyEventFreshSec;
    void recordProxyEvent(int statusCode);
    // nowSec 默认取当前墙钟（测试可显式传值验证过期分支）
    std::optional<int> takeProxyEvent(std::time_t nowSec = std::time(nullptr));

    // 代理 server 生命周期

    // 代理启动前置校验（无激活 profile 起代理会全量 503）；任一不过 → fail 且零副作用
    //（不 bind、不接管、不动激活行）：① 有激活且可读的 profile；② POST /v1/messages 2xx（1-token ping）。
    // 不得在 UI 线程同步调用。
    core::Result<void> validateProxyPreconditions(
        int probeTimeoutMs = kProxyPreStartProbeTimeoutMs);

    // 前置校验探测超时：只发一个 POST /v1/messages，短于 testConnection 的 15s
    //（校验失败要用户可感知地快）。
    static constexpr int kProxyPreStartProbeTimeoutMs = 2000;

    // 启动本地 HTTP 代理（固定绑定 127.0.0.1，无鉴权）。port=0 = OS 分配；
    // server/路由/在途流在 ProxyRuntime，门面保留接管尾（成功 → syncTakeOver）。
    core::Result<int> startProxy(const std::string& bindAddress = "127.0.0.1",
                                 int port = kDefaultProxyPort);

    // 停止代理：停机序列（cancelActiveStreams → server stop → reset）在
    // ProxyRuntime::stop()；随后 settings.json 还原经 SettingsTakeOver::syncRestore()。
    void stopProxy();

    // 在途流式请求登记
    // stopProxy()/析构必须先 cancelActiveStreams() 再停 server——否则 httplib worker
    // 阻塞在读 chunk 里，Server::stop() 的 join 挂成幽灵进程。
    void registerActiveStream(
        const std::shared_ptr<platform::http::StreamingResponse>& stream);
    void cancelActiveStreams();

    bool isProxyRunning() const;
    int proxyPort() const;            // 0 if not running

    // 用该 Profile 生成临时 settings 直连，打开新的 Claude Code 会话
    // 不启代理、不切激活、不碰 ~/.claude/settings.json（零全局副作用）；密钥临时明文落盘。
    // id 为 0/不存在/读密钥失败/落盘失败 → fail；hint：非致命提示出参。
    core::Result<void> runProfile(int64_t id, std::string* hint = nullptr);

    // 终端启动注入点（默认 = platform::terminal::launch）；测试注入假实现断言参数。
    void setTerminalLauncher(
        std::function<core::Result<void>(const platform::terminal::LaunchSpec&)>
            launcher);

    // 配置端口
    // 设置页「代理端口」的运行时镜像（端口经单例桥接）；setter 钳制到
    // [kMinProxyPort, kMaxProxyPort]。状态在 ProxyRuntime。
    int configuredProxyPort() const { return proxyRuntime_.configuredProxyPort(); }
    void setConfiguredProxyPort(int port);

    // Claude Code settings 接管
    // 协议侧（路径/开关/读写/上层覆盖警告）在 SettingsTakeOver；本段方法转发/编排。

    // 注入接管通道路径并启用（生产由 main.cpp 注入）。默认不启用：测试直接
    // 构造的 service 不写任何真实文件。
    void setSettingsSyncPaths(const settings_sync::Paths& paths) {
        settingsTakeover_.setPaths(paths);
    }

    // 接管开关（默认 true）。开着时门控：起代理 → 接管；停代理 → 按 sidecar 快照
    // 按键级还原；启动时 → 补还原残留。关掉 = 完全不碰 settings.json。
    void setSettingsTakeOverEnabled(bool on) { settingsTakeover_.setEnabled(on); }
    bool settingsTakeOverEnabled() const { return settingsTakeover_.enabled(); }

    // 注入的接管路径（未注入时 settingsFile 为空）；「导入当前配置」据此定位 settings.json。
    const settings_sync::Paths& settingsSyncPaths() const {
        return settingsTakeover_.paths();
    }

    // 已接管（sidecar 快照存在）
    bool isClaudeCodeManaged() const;

    // 最近一次接管的覆盖检测警告（按值返回快照：agent 线程会整体重写警告表而
    // UI 每帧读它，活引用即数据竞争/UAF；协议侧锁内拷贝，不取 switchMutex_）。
    std::vector<std::string> lastSyncWarnings() const;

    // 启动崩溃补还原：sidecar 残留（上次没走完还原）→ 先还原；main 在任何 startProxy 前调用。
    void recoverPendingSettingsRestore();

    // 全局单例
    static void setGlobal(ClaudeService* svc);
    static ClaudeService* global();

    // 调用日志写版本号（logCall 成功 +1）；compose 只在版本变化时重查 recent。
    uint64_t callLogVersion() const {
        return callLogStore_.callLogVersion();
    }

public:
    // 代理端口范围与默认值，唯一来源 = core/proxy_port.h。
    static constexpr int kDefaultProxyPort = ::core::kDefaultProxyPort;
    static constexpr int kMinProxyPort = ::core::kMinProxyPort;
    static constexpr int kMaxProxyPort = ::core::kMaxProxyPort;

private:
    // 行解析/哈希/片段校验等私有工具与 TwoPhaseKeyWrite 在 profile_repository.h

    // 两条编排入口：syncTakeOver() 取激活 Profile + 端口，生效高级设置 JSON 预先装配
    // 进 Profile 副本（片段赢）；失败并入 warnings_，不阻断上层。syncRestore() 失败只 LOG_WARN。
    core::Result<void> syncTakeOver();
    void syncRestore();

    // testConnection 的探测体（testConnection 恒 15s，前置校验传短超时）。
    core::Result<platform::http::Response> probeProfile(const Profile& profile,
                                                        int timeoutMs);
    static constexpr int kTestConnectionTimeoutMs = 15000;

    // 反向同步：读 settings.json 的 env → 对齐到激活 profile（覆盖/缺席清空/守卫键不写也不清）。
    // 由 setActiveProfile 在置位事务之前调用；文件不存在/空/无 env → no-op，解析失败 → fail。
    core::Result<void> syncCaptureActiveProfile();

    // 直连会话 settings 的 DB 记录
    // 写入顺序：recordRunSettings → 落文件 → chmod 0600 → 启动会话，任一步失败都
    // forgetRunSettings 撤行。settingsJson 含明文 API key，入库前分平台保护。
    core::Result<void> recordRunSettings(const std::string& runKey,
                                         int64_t profileId,
                                         const std::string& profileName,
                                         std::time_t writtenAt,
                                         const std::string& filePath,
                                         const std::string& settingsJson) const;

    // 撤掉一次未成行的记录；幂等（行不存在即成功）。失败只记日志，不盖过原始错误。
    void forgetRunSettings(const std::string& runKey) const;

    // setActiveProfile 的服务内互斥锁：方法体有 DB 事务 + 接管 + warnings_ 内存态。
    // 只护这一个写方法，其余纯读随 SqliteDb recursive_mutex 串行化、保持无锁。
    std::mutex switchMutex_;

    core::data::SqliteDb& db_;
    CallLogStore callLogStore_;
    // claude_profiles 行 + SharedConfig + Keychain 条目的唯一持久化入口
    ProfileRepository profileRepository_;
    std::string keychainService_;   // 仅用于旧 Keychain 残留清理

    // settings.json 接管/还原协议侧；门面只留编排与转发。
    SettingsTakeOver settingsTakeover_;

    // 代理生命周期 + 在途流 + 端口镜像 + 事件邮箱。声明序末位 → 析构最先，
    // ~ProxyRuntime 内 stop() 即停机序列。
    ProxyRuntime proxyRuntime_;

    // 默认 = platform::terminal::launch；测试经 setTerminalLauncher 注入假实现。
    std::function<core::Result<void>(const platform::terminal::LaunchSpec&)>
        terminalLauncher_ = &platform::terminal::launch;
};

// 调用记录时间列（只显 HH:MM 会让跨天记录误判）：今日 "HH:MM"；昨天 "昨天 HH:MM"；更早 "MM-DD"
std::string formatCallTime(std::time_t ts, std::time_t now);

// 旧 Keychain 命名空间名："DevDesk-" + SHA256(dataRoot) 前 4 字节 hex。Keychain
// service 是全局命名空间不随 DEVDESK_DATA_ROOT 隔离；空根 → 空串；只用于残留清理。
std::string keychainServiceForDataRoot(const std::string& dataRoot);

} // namespace module::claude
