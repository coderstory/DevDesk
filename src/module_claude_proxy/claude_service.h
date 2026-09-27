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
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/sqlite_db.h"
#include "core/result.h"
#include "platform/http_client.h"
#include "platform/terminal_launch.h"
#include "settings_sync.h"

namespace platform::http { class Server; }

namespace module::claude {

class ProxyRouter;

// 上游会话标识。部分 Anthropic-compatible 网关（如 opencode.ai/zen/go）要求
// 请求带 x-opencode-session，缺失直接回 400 MissingSessionID；客户端（Claude
// Code）默认不带，由代理注入（见 upstream_dialect：只有该方言会注入）。
// 进程内稳定一份，保证上游侧路由/缓存亲和。
const std::string& claudeUpstreamSessionId();

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
    int64_t sharedConfigId = 0; // 绑定共享配置片段 id（0 = 未绑定；0..1、不嵌套）
    std::string authField = "both";  // 认证字段三态（'both'/'bearer'/'api_key'；未知/空串按 both 读）
    bool isActive = false;
    std::time_t createdAt = 0;
    std::time_t updatedAt = 0;
};

// 共享配置片段（2026-09-27 shared-config-and-failover 设计 §2.1/§2.2）：
// 纯 env 装饰层——只有名字 + extra_env，**不含 base_url / 密钥**，故不进路由、
// 不进凭据链（Keychain 零交互、is_active 唯一性零影响）；一个 profile 绑定
// 0..1 个片段，不嵌套（片段不引用片段）
struct SharedConfig {
    int64_t id = 0;
    std::string name;
    std::string extraEnvJson;   // 与 Profile::extraEnvJson 同构同语义（空串 = 未配置）
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
    //
    // 产品审计 2026-09-23 [6]：activateIfNoneActive=true 时，创建成功后若当前
    // 无任何激活 Profile 则自动激活新 Profile（首次体验最顺——新用户建完直接
    // 「启动代理」不再 503）。已有激活 Profile 时不抢占，返回值 isActive=false，
    // UI 据此分 toast 文案。默认 false：既有调用方与单测行为不变。
    // 自动激活走 setActiveProfile（代理运行中会同步接管 settings.json）。
    // sharedConfigId：建时直接绑定的共享片段 id（0 = 不绑定；与表单「新建」路
    // 径一致——先建后 update 会留下一次「未绑片段」的中间态，故走尾部默认参数）
    // authField：认证字段三态文本（'both'/'bearer'/'api_key'；非法值写侧规范化为
    // 'both'，见 2026-09-27 profile-auth-field 设计 §3 D-8）
    core::Result<Profile> createProfile(const std::string& name,
                                       const std::string& baseUrl,
                                       const std::string& apiKey,
                                       const std::string& model,
                                       bool activateIfNoneActive = false,
                                       int64_t sharedConfigId = 0,
                                       const std::string& authField = "both");

    // 更新 profile（profile.id 必须已存在）
    //   - 若 profile.apiKeyPlain 非空，则覆盖 Keychain
    core::Result<void> updateProfile(const Profile& profile);

    // 删除 profile（同时清理 Keychain 条目）。
    // Keychain 清理失败会中止删除并返回错误（SQLite 行保留，可重试）
    //
    // 产品审计 2026-09-23 [1]（provider-config 设计 §7 安全第 5 条）：
    // 删除的是激活 profile 且代理运行中 → 删除成功后 stopProxy()
    //（内部触发 syncRestore 还原 ~/.claude/settings.json），不让
    // settings.json 继续指向无激活 profile 的代理（否则 Claude Code 全量 503）。
    // proxyStopped（可选出参）：true = 本次删除顺带停用了代理，UI 据此
    // toast 告知用户「已停止代理并还原 Claude Code 配置」
    core::Result<void> deleteProfile(int64_t id, bool* proxyStopped = nullptr);

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

    // 复制 Profile（设计 §4.2/§4.3，M4）：名称自动加「副本」后缀（重名递增
    // 避让，100 次仍冲突 → fail 不建行），API key 一并复制（源 key 缺失 →
    // fail，不建无 key 的副本），**副本不激活**（S-4）。
    // 二段补写（models_config / extra_env）失败 → best-effort 删除刚建的副本
    // 回滚：回滚成功 = fail 且零残留；回滚失败 = fail 且文案注明不完整副本 id
    // shared_config_id 一并复制（属于配置，设计 §2.6）
    core::Result<Profile> duplicateProfile(int64_t srcId);

    // ---- 共享配置片段（设计 §2.1/§2.6）----
    // 生命周期语义（改动当场生效，避免 cc-switch 的「改了片段不生效」头号 bug）：
    //   改（改名 / 改 env）→ 绑定它的 profile 下次接管生效；代理在跑且属激活
    //     profile → 立即 re-takeover
    //   删 → 同事务解绑绑定它的 profile（shared_config_id = 0，不留悬空 id），
    //     随后若代理在跑且激活 profile 在其中 → re-takeover
    //   解绑（经 updateProfile 把 sharedConfigId 置 0）→ 同款当场生效，同名键
    //     回落 profile 自身值（片段值从不写进 profile 行）
    core::Result<std::vector<SharedConfig>> listSharedConfigs();
    core::Result<SharedConfig> createSharedConfig(const std::string& name,
                                                 const std::string& extraEnvJson);
    core::Result<void> updateSharedConfig(const SharedConfig& config);
    core::Result<void> deleteSharedConfig(int64_t id);

    // 片段冻结键集（G10，设计 §2.4）：profile 所绑片段 extra_env 的全部键
    //（别名归一后，与写进 live 的键形一致）。未绑定 / 片段已删 / JSON 非法 →
    // 空集（读路径容错）
    core::Result<std::vector<std::string>> sharedEnvKeysFor(int64_t profileId);

    // 立即用当前激活 profile（含所绑片段）重写 settings.json（设计 §2.5：片段
    // 保存后必须当场生效）。通道未注入 / 接管开关关 → no-op 成功
    core::Result<void> reapplyTakeOver() { return syncTakeOver(); }

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

    // 日志面板查询条件（产品审计 2026-09-23 [11]）：总设计 §8.3 第 6 条承诺
    // 「完整日志面板（支持过滤、搜索）」，此前只有 recentCalls(20) 静态列表。
    // search 按 path/method 模糊匹配（LIKE，% 与 _ 转义为字面量，ASCII 大小写
    // 不敏感）；statusClass 按状态码分桶（status_code 列可空，NULL 按 0 归
    // 「错误」桶）；limit/offset 翻页。排序与 recentCalls 一致
    // （started_at DESC, id DESC）。
    struct CallLogQuery {
        // statusClass 取值：
        static constexpr int kStatusAll = 0;  // 全部
        static constexpr int kStatus2xx = 2;  // 200..299
        static constexpr int kStatus3xx = 3;  // 300..399
        static constexpr int kStatus4xx = 4;  // 400..499
        static constexpr int kStatus5xx = 5;  // >=500
        static constexpr int kStatusErr = 9;  // <200（0/NULL = 超时、网络错误等无状态码）

        std::string search;    // 空串 = 不做文本过滤
        int statusClass = kStatusAll;
        int limit = 20;        // <=0 按 20
        int offset = 0;        // <0 按 0
    };

    // 过滤 + 分页查询（日志弹窗搜索框 / 状态码下拉 / 翻页共用）
    core::Result<std::vector<CallLogEntry>> queryCalls(const CallLogQuery& q);

    // 满足过滤条件的总条数（忽略 limit/offset；UI 算页数、判断有无下一页）
    core::Result<int> countCalls(const CallLogQuery& q);

    // ---- 最近代理请求事件（气泡内容源③，M2 §4.5，产品审计 [18]）----
    // ProxyRouter 在每条请求的终态记一次 statusCode 进单槽（后写覆盖先写）：
    //   0 = 上游不可达（DNS/TLS/超时，无响应）；其余为回给客户端的状态码。
    // takeProxyEvent() 读走即清空（take 语义）——桌宠气泡保证一个事件最多弹
    // 一次、应用挂机不重复弹最后事件。滞留超过 kProxyEventFreshSec 的事件作
    // 过期丢弃（nullopt）：桌宠关了几小时后再开不该弹旧闻；窗口取 10min，
    // 覆盖气泡 roll（onTimer ③② 平均 ~5min 一次）的典型间隔。
    // 写方 = proxy worker 线程、读方 = UI 线程，mutex 保护（与 activeStreams
    // 同款模式）。main 经本接口装配 pet 的 ProxyEventGetter app 级桥
    //（pet 不 include 本模块，模块隔离见 pet_window.h 契约注释）。
    static constexpr std::time_t kProxyEventFreshSec = 600;
    void recordProxyEvent(int statusCode);
    // nowSec 默认取当前墙钟（测试可显式传值验证过期分支）
    std::optional<int> takeProxyEvent(std::time_t nowSec = std::time(nullptr));

    // ---- 代理 server 生命周期 ----

    // 启动本地 HTTP 代理（固定绑定 127.0.0.1，仅本机可访问，无鉴权）
    //   - bindAddress：默认 "127.0.0.1"
    //   - port：0 = 由 OS 分配
    core::Result<int> startProxy(const std::string& bindAddress = "127.0.0.1",
                                 int port = 8888);

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

    // ---- 运行：用该 Profile 打开一个新的 Claude Code 会话（2026-09-28
    //      profile-run 设计 §5/§6；D-1：代理托管式，密钥零落盘）----
    // 顺序**固定**：确保代理运行 → 必要时 setActiveProfile → 启动会话。
    // 理由：startProxy 内部会 syncTakeOver()；随后 setActiveProfile 的
    // syncCaptureActiveProfile 读回的正是它自己刚写的受保护托管值（G2/G3 守卫
    // 跳过）——顺序反了会把 live 值误收编进 profile。
    //   - id：目标 profile；0 / 不存在 / 代理起不来 / 切换失败 → fail（不开会话）
    //   - hint：可选**非致命提示**出参（如 Windows 无 wt 已退化传统控制台，D-5）；
    //     成功时写入，无提示时清空。失败原因仍走 Result（与既有接口一致）
    core::Result<void> runProfile(int64_t id, std::string* hint = nullptr);

    // 终端启动注入点（默认指向真实现 platform::terminal::launch）。
    // 镜像既有 setSettingsSyncPaths / setProfileProvider 的值注入先例：
    // 测试注入假实现以断言调用参数，而不真起终端进程。
    void setTerminalLauncher(
        std::function<core::Result<void>(const platform::terminal::LaunchSpec&)>
            launcher);

    // ---- 配置端口（产品审计 2026-09-23 [14]）----
    // 设置页「代理端口」的运行时镜像：main.cpp 启动时从 SettingsConfig 注入，
    // 设置页改端口时同步注入。手动启动路径（Claude 页按钮）用这个值而不是
    // 硬编码 8888——module_claude_proxy 不能依赖 module_settings，端口经由
    // service 单例桥接（与 ClaudeService::global() 同款模式），无环。
    // setter 钳制到 [kMinConfiguredProxyPort, kMaxConfiguredProxyPort]。
    int configuredProxyPort() const { return configuredProxyPort_; }
    void setConfiguredProxyPort(int port);

    // ---- Claude Code settings 接管（provider-config Phase C，§5.2/§5.3）----

    // 注入接管通道路径并启用（生产由 main.cpp 一次性注入
    // settings_sync::defaultPaths()）。默认不启用：直接构造的 service
    //（测试）不写任何真实文件
    void setSettingsSyncPaths(const settings_sync::Paths& paths);

    // 接管开关（2026-09-27 恢复为默认 **true**，用户拍板）。开着的三条路径：
    //   起代理 → 接管：settings.json 的 env 写入 BASE_URL=
    //            http://127.0.0.1:<port> + 占位 ANTHROPIC_AUTH_TOKEN
    //            （PROXY_MANAGED）+ 模型槽位 env，其余 env 键/非 env 字段原样保留
    //   停代理 → 还原：按 sidecar 快照按键级还原（settings.json 永不指向死代理）
    //   启动时 → 补还原：sidecar 残留（上次没走完还原）→ 先还原
    // 关掉 = DevDesk 完全不碰 settings.json（三条路径一起门控，见
    // settings_sync.h 的接管/还原协议）。本机另装 cc-switch 等同类工具、
    // 不愿两边互相覆盖时用。
    void setSettingsTakeOverEnabled(bool on) { settingsTakeOverEnabled_ = on; }
    bool settingsTakeOverEnabled() const { return settingsTakeOverEnabled_; }

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

    // 审计 perf [13]：调用日志写版本号（logCall 成功 +1，proxy worker 线程写）。
    // Claude 页 compose 用它感知代理侧异步落库，只在版本变化时重查 recentCalls，
    // 不再每次重组合同步查库。
    uint64_t callLogVersion() const {
        return callLogVersion_.load(std::memory_order_acquire);
    }

public:
    // 代理端口合法范围与默认值（审计 [14]；与 module_settings 的
    // SettingsConfig::proxyPort 钳制范围/默认值保持字面一致——两模块不能
    // 互相依赖，故各自定义同值常量）
    static constexpr int kDefaultProxyPort = 8888;
    static constexpr int kMinProxyPort = 1;
    static constexpr int kMaxProxyPort = 65535;

private:
    // 从一行 SqlRow 构造 Profile（不含 apiKeyPlain）
    core::Result<Profile> rowToProfile(const core::data::SqlRow& row) const;

    // 从一行 SqlRow 构造 SharedConfig
    core::Result<SharedConfig> rowToSharedConfig(const core::data::SqlRow& row) const;

    // 片段 extra_env 原文（0 / 片段不存在 / 读失败 → 空串 = 未绑定；读路径容错，
    // 悬空 id 退化成「未绑定」而不是让接管/G10 失败）
    std::string sharedExtraEnvJsonFor(int64_t sharedConfigId) const;

    // 片段存在性校验（update / delete 前置，缺失 → NotFound）
    core::Result<void> requireSharedConfigExists(int64_t id) const;

    static std::string sha256HexPrefix(const std::string& s);
    static std::string makeApiKeyHint(const std::string& apiKey);

    // 接管/还原的内部入口（settingsFile 为空 = 通道未启用，直接返回）。
    // syncTakeOver 返回失败原因（settings.json / sidecar 读写或解析失败），供
    // 调用方决定呈现方式；**接管失败不阻断上层操作**（启代理/切 profile 照常），
    // 失败原因一并并入 lastSyncWarnings_（UI 经 getter 可读）
    core::Result<void> syncTakeOver();
    void syncRestore();

    // 反向同步（设计 §3.1/§3.4/§3.5，M3）：读 settings.json 的 env → 对齐到
    // **当前激活 profile**（出现覆盖 / 缺席清空 / 守卫键不写也不清）。
    // 由 setActiveProfile 在「校验目标存在之后、清零置位事务之前」调用，
    // 无激活 profile 时调用方已跳过（G0，不读文件）。
    //   - 文件不存在 / 空文件 / 无 env → no-op 成功（照常切换）
    //   - 存在但读不了 / 解析失败 → fail（S-2：中止本次切换，profile 一字未改）
    //   - 无变化 → 零写入（不 UPDATE、不写 Keychain、不 bump updated_at）
    // 不受 settingsTakeOverEnabled_ 门控（只读外部文件）；唯一开关是
    // syncPaths_.settingsFile 为空 → 直接返回
    core::Result<void> syncCaptureActiveProfile();

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

    // 设置页配置的代理端口（审计 [14]；默认 8888 = 历史硬编码值）
    int configuredProxyPort_ = kDefaultProxyPort;

    // 终端启动实现（profile-run 设计 §6）：默认 = platform::terminal::launch；
    // 测试经 setTerminalLauncher 注入假实现（断言参数、不真起进程）
    std::function<core::Result<void>(const platform::terminal::LaunchSpec&)>
        terminalLauncher_ = &platform::terminal::launch;

    // 最近代理请求事件单槽（气泡③，审计 [18]）：proxy worker 写 / UI take 读
    std::mutex proxyEventMutex_;
    std::optional<int> proxyEventStatus_;
    std::time_t proxyEventAt_ = 0;

    // Claude Code 接管通道（settingsFile 为空 = 未启用）
    settings_sync::Paths syncPaths_;
    bool settingsTakeOverEnabled_ = true;  // 接管默认开（见 setSettingsTakeOverEnabled）
    std::vector<std::string> lastSyncWarnings_;

    // 审计 perf [13]：调用日志版本号（见 callLogVersion()）
    std::atomic<uint64_t> callLogVersion_{0};
};

// ===== 纯函数（可单测，无 IO）=====

// 调用记录时间列（W10 复审项：只显 HH:MM 会让跨天记录的新旧判断误导）：
//   今日 → "HH:MM"；昨天 → "昨天 HH:MM"；更早 → "MM-DD"
std::string formatCallTime(std::time_t ts, std::time_t now);

} // namespace module::claude
