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

// 上游会话标识。部分 Anthropic-compatible 网关（如 opencode.ai/zen/go）要求
// 请求带 x-opencode-session，缺失直接回 400 MissingSessionID；客户端（Claude
// Code）默认不带，由代理注入（见 upstream_dialect：只有该方言会注入）。
// 进程内稳定一份，保证上游侧路由/缓存亲和。
const std::string& claudeUpstreamSessionId();

// 单条 Claude API 配置 / 共享配置片段 / 两段写助手（CPX-07）的类型定义已随
// R10-A2 迁至 profile_repository.h（本头文件 include 透出，消费方零改动）

class ClaudeService {
public:
    // 构造时不持有所有权，db 由调用方确保生命周期
    // keychainService：空串（默认）= 生产形态——service 名按数据根派生
    //   "DevDesk-<短哈希>"（keychainServiceForDataRoot）；
    //   非空 = 原样使用（测试隔离形态，测试传 "DevDeskTest…"）
    explicit ClaudeService(core::data::SqliteDb& db,
                           const std::string& keychainService = std::string());
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
    // dialectOverride：上游方言覆盖文本（空串 = 未覆盖 = 按 baseUrl 的 host 推断；
    // 非法值写侧归一为空串，审计 CPX-13。副本复制经本参数带上，见 duplicateProfile）
    // fallbackProfileId：建时即带备用（0 = 无；同 sharedConfigId「建时即设」口径，
    // 设计 §3.7。副本复制经本参数带上）
    // extraSettingsJson：高级设置 JSON 原文（空串 = 未配置；advanced-json-editor
    // 设计 §3.3。副本复制经本参数带上——「复制即配置」，审计 F-3）
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
                                       // 高级设置 JSON 原文（空串 = 未配置；
                                       // advanced-json-editor 设计 §3.3 尾插默认参，
                                       // 与 dialectOverride/fallbackProfileId 同款先例）
                                       const std::string& extraSettingsJson =
                                           std::string());

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
    // fail，不建无 key 的副本；失败分三态：条目不存在 / 读失败（原样透传
    // 错误码）/ 值为空串，首、末两态文案带恢复提示「编辑该 Profile 重新填一次
    // API Key 即可」），**副本不激活**（S-4）。
    // 二段补写（models_config / extra_env）失败 → best-effort 删除刚建的副本
    // 回滚：回滚成功 = fail 且零残留；回滚失败 = fail 且文案注明不完整副本 id
    // shared_config_id 一并复制（属于配置，设计 §2.6）；
    // dialect_override 一并复制（同上，审计 CPX-13）
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
                                                 const std::string& extraSettingsJson);
    core::Result<void> updateSharedConfig(const SharedConfig& config);
    core::Result<void> deleteSharedConfig(int64_t id);

    // 立即用当前激活 profile（含所绑片段）重写 settings.json（设计 §2.5：片段
    // 保存后必须当场生效）。通道未注入 / 接管开关关 → no-op 成功
    core::Result<void> reapplyTakeOver() { return syncTakeOver(); }

    // ---- failover 备用解析（2026-09-27 shared-config-and-failover 设计
    // §3.7/§3.8）----
    // 读 profile 的 fallback_profile_id → getProfile + loadApiKey → 成功返回
    // 可用备用（含明文 key）。任一环节失败 / id==0 → std::nullopt（proxy_router
    // 原样回错，不换第三家）。只换一跳：换过去之后不再读该备用的备用。
    // 纯读（不改 is_active、不重写 settings.json——failover 是请求级，B10）
    std::optional<Profile> resolveFallbackProfile(const Profile& profile);

    // ---- 连通性测试 ----

    core::Result<platform::http::Response> testConnection(const Profile& profile);

    // ---- 调用日志 ----

    // R10-A1：数据模型与读写已抽至 CallLogStore（call_log_store.h，职责边界
    // 见 R10 spec §2.2）；此处以别名保持既有公开类型名不变，下列方法体转发到
    // callLogStore_（门面 43 签名零变化，纯内部重构）
    using CallLogEntry = CallLogStore::CallLogEntry;
    using CallLogQuery = CallLogStore::CallLogQuery;

    core::Result<void> logCall(const CallLogEntry& entry);
    core::Result<std::vector<CallLogEntry>> recentCalls(int limit = 20);

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
    // R10-A3：单槽邮箱随 ProxyRuntime 搬离（proxy_runtime.{h,cpp} 同段，与
    // 在途流登记同写者-读者对），本门面转发；常量别名保持既有消费方
    //（tests/test_claude_service.cpp）的 ClaudeService:: 限定量零改动
    static constexpr std::time_t kProxyEventFreshSec =
        ProxyRuntime::kProxyEventFreshSec;
    void recordProxyEvent(int statusCode);
    // nowSec 默认取当前墙钟（测试可显式传值验证过期分支）
    std::optional<int> takeProxyEvent(std::time_t nowSec = std::time(nullptr));

    // ---- 代理 server 生命周期 ----

    // 启动本地 HTTP 代理（固定绑定 127.0.0.1，仅本机可访问，无鉴权）
    //   - bindAddress：默认 "127.0.0.1"
    //   - port：0 = 由 OS 分配；默认值同样取 core/proxy_port.h 的唯一来源
    // R10-A3：server/路由/在途流/端口镜像已迁 ProxyRuntime；门面保留启动后的
    // 接管尾（server 启动成功 → syncTakeOver，顺序逐字保持；R10-A4 起接管协
    // 议经 SettingsTakeOver 子对象）
    core::Result<int> startProxy(const std::string& bindAddress = "127.0.0.1",
                                 int port = kDefaultProxyPort);

    // 停止代理。R10-A3 段级拆分：停机序列（cancelActiveStreams → server
    // ->stop → reset）在 ProxyRuntime::stop()；R10-A4 段级拆分：其后接的
    // settings.json 还原经 SettingsTakeOver::syncRestore()（门面 syncRestore()
    // 包装其 Result<void> 版，失败 LOG_WARN）
    void stopProxy();

    // ---- 在途流式请求登记（审计 2026-09-23 [9]）----
    // ProxyRouter 在启动上游流后立即 registerActiveStream；stopProxy() /
    // 析构先调 cancelActiveStreams() 再停 server——否则 httplib worker 阻塞
    // 在 provider 的 readChunk 里，Server::stop() 的 join 要等流自然结束
    // （Claude 活动生成期无空闲超时，停止/退出会挂起成幽灵进程）。
    // R10-A3：登记表状态与实现已迁 ProxyRuntime，门面转发（public：测试用与
    // stopProxy 相同的顺序驱动验证）。
    void registerActiveStream(
        const std::shared_ptr<platform::http::StreamingResponse>& stream);
    void cancelActiveStreams();

    bool isProxyRunning() const;
    int proxyPort() const;            // 0 if not running

    // ---- 运行：用该 Profile 生成临时 settings 直连，打开一个新的 Claude Code
    //      会话（2026-09-28 profile-run 设计 §4.2/§5；D-1：临时 settings +
    //      --settings 直连，密钥临时明文落盘，代价见 §3）----
    // 顺序**固定**：读 profile → 读 Keychain 密钥 → 生成临时 settings 文件落盘
    // → 拼 LaunchSpec（args 含 --settings <路径>）→ 启动会话。不启代理、不切
    // 激活 profile、不读也不改 ~/.claude/settings.json——零全局副作用（V3）。
    //   - id：目标 profile；0 / 不存在 / 读密钥失败 / 落盘失败 → fail（不开会话）
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
    // 常量默认端口——module_claude_proxy 不能依赖 module_settings，端口经由
    // service 单例桥接（与 ClaudeService::global() 同款模式），无环。
    // setter 钳制到 [kMinProxyPort, kMaxProxyPort]（取值见 core/proxy_port.h）。
    // R10-A3：镜像状态已迁 ProxyRuntime（端口镜像属其职责），门面转发
    int configuredProxyPort() const { return proxyRuntime_.configuredProxyPort(); }
    void setConfiguredProxyPort(int port);

    // ---- Claude Code settings 接管（provider-config Phase C，§5.2/§5.3）----
    // R10-A4：协议侧（路径/开关状态 + settings_sync:: 读写 + 上层覆盖警告）
    // 已迁 SettingsTakeOver（settings_takeover.{h,cpp}，职责边界见 R10 spec
    // §2.2）；本段方法保持 43 签名，转发 / 编排

    // 注入接管通道路径并启用（生产由 main.cpp 一次性注入
    // settings_sync::defaultPaths()）。默认不启用：直接构造的 service
    //（测试）不写任何真实文件。inline 转发（同 setSettingsTakeOverEnabled）：
    // R10-A4 起签名类型只在此处出现，claude_service.cpp 的 settings_sync::
    // 命中断言（R10 spec §2.4 P-A4 ≤1）才成立
    void setSettingsSyncPaths(const settings_sync::Paths& paths) {
        settingsTakeover_.setPaths(paths);
    }

    // 接管开关（2026-09-27 恢复为默认 **true**，用户拍板）。开着的三条路径：
    //   起代理 → 接管：settings.json 的 env 写入 BASE_URL=
    //            http://127.0.0.1:<port> + 占位 ANTHROPIC_AUTH_TOKEN
    //            （PROXY_MANAGED）+ 模型槽位 env，其余 env 键/非 env 字段原样保留
    //   停代理 → 还原：按 sidecar 快照按键级还原（settings.json 永不指向死代理）
    //   启动时 → 补还原：sidecar 残留（上次没走完还原）→ 先还原
    // 关掉 = DevDesk 完全不碰 settings.json（三条路径一起门控，见
    // settings_sync.h 的接管/还原协议）。本机另装 cc-switch 等同类工具、
    // 不愿两边互相覆盖时用。门控状态在 SettingsTakeOver（setEnabled）
    void setSettingsTakeOverEnabled(bool on) { settingsTakeover_.setEnabled(on); }
    bool settingsTakeOverEnabled() const { return settingsTakeover_.enabled(); }

    // 注入的接管路径（未注入时 settingsFile 为空）。「导入当前配置」据此
    // 定位 settings.json：生产 = main 注入的 defaultPaths，测试 = 临时目录
    const settings_sync::Paths& settingsSyncPaths() const {
        return settingsTakeover_.paths();
    }

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
        return callLogStore_.callLogVersion();
    }

public:
    // 代理端口合法范围与默认值（审计 [14]）。**取值唯一来源 = core/proxy_port.h**
    //（R0/D7）：此前本类与 module_settings 的 SettingsConfig 各自定义同值常量、
    // 靠注释约定「保持字面一致」；现两侧都从 core 取，改值的唯一落点在那里。
    // 名字保留（本文件与 .cpp 的既有引用不变）
    static constexpr int kDefaultProxyPort = ::core::kDefaultProxyPort;
    static constexpr int kMinProxyPort = ::core::kMinProxyPort;
    static constexpr int kMaxProxyPort = ::core::kMaxProxyPort;

private:
    // R10-A2：行解析 / 哈希 / 片段存在性校验等私有工具与 TwoPhaseKeyWrite 已随
    // ProfileRepository 迁至 profile_repository.h（见其 private 区）

    // R10-A4：接管/还原的协议侧已迁 SettingsTakeOver（settings_takeover.
    // {h,cpp}，职责边界见 R10 spec §2.2）；本门面保留两条编排入口：
    //   - syncTakeOver()：从 ProfileRepository 取激活 Profile、从 ProxyRuntime
    //     取端口；生效高级设置 JSON 预先装配进 Profile 副本的 extraSettingsJson
    //    （片段叠加片段赢，§5.2①/§8.2）后一并传入
    //     settingsTakeover_.syncTakeOver(active, port)
    //     （P-A4 双入参，子对象不反向依赖彼此）。失败原因经子对象并入
    //     warnings_（UI 经 lastSyncWarnings 透传）；**失败不阻断上层操作**
    //   - syncRestore()：settingsTakeover_.syncRestore()（CPX-02 已改返
    //     Result<void>，失败并入 warnings_）的 void 包装——失败 LOG_WARN
    //     （保持门面签名不变）
    core::Result<void> syncTakeOver();
    void syncRestore();

    // 反向同步（设计 §3.1/§3.4/§3.5，M3）：读 settings.json 的 env → 对齐到
    // **当前激活 profile**（出现覆盖 / 缺席清空 / 守卫键不写也不清）。
    // 由 setActiveProfile 在「校验目标存在之后、清零置位事务之前」调用，
    // 无激活 profile 时调用方已跳过（G0，不读文件）。
    //   - 文件不存在 / 空文件 / 无 env → no-op 成功（照常切换）
    //   - 存在但读不了 / 解析失败 → fail（S-2：中止本次切换，profile 一字未改）
    //   - 无变化 → 零写入（不 UPDATE、不写 Keychain、不 bump updated_at）
    // 不受接管开关门控（只读外部文件）；唯一开关是
    // settingsTakeover_.paths().settingsFile 为空 → 直接返回
    core::Result<void> syncCaptureActiveProfile();

    core::data::SqliteDb& db_;
    CallLogStore callLogStore_;
    // R10-A2：claude_profiles 行 + SharedConfig 片段 + Keychain 条目的唯一
    // 持久化入口（职责边界见 R10 spec §2.2；门面只留策略编排与转发）
    ProfileRepository profileRepository_;
    std::string keychainService_;

    // R10-A4：settings.json 接管/还原的协议侧（路径注入 / 接管开关 / 托管
    // 快照检测 / 上层覆盖警告状态都在这；见 settings_takeover.{h,cpp}）。
    // 门面只留编排（取激活 Profile / 代理端口 / re-takeover 时机）与转发
    SettingsTakeOver settingsTakeover_;

    // R10-A3：代理 server/路由生命周期 + 在途流登记 + 端口镜像 + 终态事件
    // 邮箱已迁 ProxyRuntime（proxy_runtime.{h,cpp}，职责边界见 R10 spec §2.2）。
    // 声明序末位 → 成员析构最先执行，其 ~ProxyRuntime 内 stop() 即旧析构的
    // 停机序列（cancelActiveStreams → server 销毁 join），顺序不变。
    // 构造经 *this 注入（ProxyRuntime 仅把它转交给 ProxyRouter，自身不调门面）
    ProxyRuntime proxyRuntime_;

    // 终端启动实现（profile-run 设计 §6）：默认 = platform::terminal::launch；
    // 测试经 setTerminalLauncher 注入假实现（断言参数、不真起进程）
    std::function<core::Result<void>(const platform::terminal::LaunchSpec&)>
        terminalLauncher_ = &platform::terminal::launch;
};

// ===== 纯函数（可单测，无 IO）=====

// 调用记录时间列（W10 复审项：只显 HH:MM 会让跨天记录的新旧判断误导）：
//   今日 → "HH:MM"；昨天 → "昨天 HH:MM"；更早 → "MM-DD"
std::string formatCallTime(std::time_t ts, std::time_t now);

// Keychain service 名按数据根派生："DevDesk-" + SHA256(dataRoot) 前 4 字节的
// 8 位 hex。Keychain 的 service 是**全局**命名空间、不随 DEVDESK_DATA_ROOT
// 隔离——所有数据根共用一个名字的话，多根互踩之外，任一数据根的孤儿清理都能
// 扫到其它根的真实密钥；按根派生后每个数据根（含测试实例）拿独立命名空间，
// 互不可见、互不可清。dataRoot 为空 → 返回空串（生产不可达，调用方兜底裸前缀）。
std::string keychainServiceForDataRoot(const std::string& dataRoot);

} // namespace module::claude
