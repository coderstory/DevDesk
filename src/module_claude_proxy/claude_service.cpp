// src/module_claude_proxy/claude_service.cpp
// ClaudeService 实现：Profile CRUD + Keychain + 上游连通性测试
#include "claude_service.h"

#include "core/atomic_write.h"
#include "core/logging.h"
#include "core/paths.h"
#include "core/time.h"
#include "platform/json.h"
#include "platform/keychain.h"
#include "platform/terminal_launch_common.h"
#include "provider_config.h"
#include "settings_takeover.h"
#include "upstream_dialect.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <utility>

#include <openssl/sha.h>

namespace module::claude {

namespace {

// Keychain service 名的固定前缀（按数据根派生时拼在短哈希之前）
constexpr const char* kKeychainServicePrefix = "DevDesk";

// 取当前 unix 秒
std::time_t nowUnix() {
    return std::time(nullptr);
}

// 去除前导/尾随空白
std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// 去除 baseUrl 末尾的 '/'
std::string stripTrailingSlash(const std::string& s) {
    if (!s.empty() && s.back() == '/') return s.substr(0, s.size() - 1);
    return s;
}

// CPX-07：Keychain 当前是否处于给定值（读不到 = 条目不存在，按空串比较）。
// 反向同步写库失败后用「实际状态」判断密钥是否已回到旧值，不信补偿返回值。
bool keyAtOldValue(const std::string& keychainService,
                   const std::string& account,
                   const std::string& expected) {
    auto r = platform::keychain::loadSecret(keychainService, account);
    return (r.isOk() ? r.value() : std::string()) == expected;
}

} // namespace

// ---------- Keychain service 派生 / 构造 / 行解析 ----------

// Keychain service 名按数据根派生（声明见 claude_service.h 纯函数段）
std::string keychainServiceForDataRoot(const std::string& dataRoot) {
    if (dataRoot.empty()) {
        return std::string();   // 空根无法派生（生产不可达；调用方兜底裸前缀）
    }
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(dataRoot.data()),
           dataRoot.size(), digest);
    std::ostringstream oss;
    // 前 4 字节 8 位 hex（同 ProfileRepository::sha256HexPrefix 口径：
    // 8 位足够区分数据根，又不至于把 service 名拉长）
    for (int i = 0; i < 4 && i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(digest[i]);
    }
    return std::string(kKeychainServicePrefix) + "-" + oss.str();
}

namespace {

// 生产路径（未显式传 service）：按当前数据根派生 service 名。派生失败
//（空根，理论不可达）兜底裸前缀——名字退化但不至于抛错。
std::string resolveKeychainService() {
    auto derived = keychainServiceForDataRoot(core::paths::appDataDir().string());
    return derived.empty() ? std::string(kKeychainServicePrefix) : derived;
}

} // namespace

ClaudeService::ClaudeService(core::data::SqliteDb& db,
                             const std::string& keychainService)
    : db_(db),
      callLogStore_(db),
      profileRepository_(db, keychainService.empty() ? resolveKeychainService()
                                                     : keychainService),
      keychainService_(keychainService.empty() ? resolveKeychainService()
                                               : keychainService),
      proxyRuntime_(*this) {}

// 全局单例指针（定义必须早于 ~ClaudeService：析构要在它身上摘牌，见下）
namespace {
    ClaudeService* g_globalClaudeService = nullptr;
}

ClaudeService::~ClaudeService() {
    // 析构摘牌（终审 2026-10-01 高 finding：退出时序 UAF）。惰性装配路径
    //（main.cpp 会话内拨开 claude）让 static unique_ptr<ClaudeService> 构造在
    // armExitHook() 注册之后 → atexit LIFO：本析构先跑、exit hook 里的模块
    // onStop 后跑。g_globalClaudeService 此前只写不清，onStop 经 global() 拿到
    // 的就是已析构对象（悬垂读；isProxyRunning() 为真时还会 stopProxy() 写已
    // 释放内存）。析构即摘牌 → onStop 拿到 nullptr，走「未运行」分支。
    // `if (... == this)` 守卫：只摘自己那张牌，不误清他人注册。
    if (g_globalClaudeService == this) g_globalClaudeService = nullptr;
    // 析构时确保停止代理（Server 析构会 join 线程）。审计 2026-09-23 [9]：
    // 与 stopProxy 同款顺序——先取消在途流，否则 ~Server 的 join 被阻塞在
    // readChunk 的 worker 卡住。R10-A3：停机序列已收进 ProxyRuntime::stop()
    //（cancelActiveStreams → server->stop → reset），析构显式调用一次
    //（proxyRuntime_ 声明序末位，成员析构也会兜底）
    proxyRuntime_.stop();
}

// ---------- 全局单例 ----------

void ClaudeService::setGlobal(ClaudeService* svc) { g_globalClaudeService = svc; }
ClaudeService* ClaudeService::global() { return g_globalClaudeService; }

// ---------- Profile CRUD（R10-A2：落库/Keychain 段已迁 ProfileRepository，
// 本门面保留转发与策略尾：首建自动激活 / re-takeover 时机 / 停代理）----------

core::Result<std::vector<Profile>> ClaudeService::listProfiles() {
    return profileRepository_.listProfiles();
}

core::Result<Profile> ClaudeService::getProfile(int64_t id) {
    return profileRepository_.getProfile(id);
}

core::Result<Profile> ClaudeService::createProfile(const std::string& name,
                                                   const std::string& baseUrl,
                                                   const std::string& apiKey,
                                                   const std::string& model,
                                                   bool activateIfNoneActive,
                                                   int64_t sharedConfigId,
                                                   const std::string& authField,
                                                   const std::string& dialectOverride,
                                                   int64_t fallbackProfileId,
                                                   const std::string& extraSettingsJson) {
    // R10-A2 段级拆分：校验 + INSERT ... RETURNING + Keychain 两段写 + 失败回滚
    //（DB 段）已迁 ProfileRepository::createProfile；本方法保留「首建自动激活」
    // 策略尾——创建成功后若当前无任何激活 Profile 则自动激活刚建的这条（经门面
    // setActiveProfile，代理运行中会同步接管 settings.json）。已有激活 Profile
    // 时不抢占。激活失败不回滚创建：行已完整可用，用户仍可手动点「启用」；只
    // WARN 留痕，返回值按实际 is_active 状态回填（失败时维持 false，UI 走
    // 「点击启用」文案）
    auto created = profileRepository_.createProfile(
        name, baseUrl, apiKey, model, sharedConfigId, authField,
        dialectOverride, fallbackProfileId, extraSettingsJson);
    if (created.isFail()) {
        return created;
    }
    Profile out = std::move(created).value();

    bool autoActivated = false;
    if (activateIfNoneActive) {
        auto cur = getActiveProfile();
        if (cur.isFail() &&
            cur.errorCode() == core::ErrorCode::NoActiveProfile) {
            auto act = setActiveProfile(out.id);
            if (act.isOk()) {
                autoActivated = true;
                LOG_INFO("首个 Profile 自动激活 id=" + std::to_string(out.id) +
                         " name=" + out.name);
            } else {
                LOG_WARN("首个 Profile 自动激活失败（可手动启用）: " +
                         act.errorMessage());
            }
        }
        // cur.isOk()（已有激活）→ 不抢占；cur 失败但非 NoActiveProfile
        //（DB 异常等）→ 保守不动作
    }
    out.isActive = autoActivated;
    return core::Result<Profile>::ok(std::move(out));
}

core::Result<void> ClaudeService::updateProfile(const Profile& profile) {
    // R10-A2 段级拆分：校验 + 旧值读取 + 两段写 + DB 更新已迁
    // ProfileRepository::updateProfile（旧行经出参回填）；本方法保留 re-takeover
    // 策略尾——片段绑定发生变化（绑定 / 解绑）且旧行激活且代理运行 → 立即重写
    // live（解绑 = 片段键从 live 撤走、与片段同名的 profile 自身值恢复生效，
    // 片段值从未写进 profile 行，故「回落」无需搬数据）。只在绑定变化时触发：
    // 既有 updateProfile 不重写 live 的行为逐字不变
    Profile old;
    auto r = profileRepository_.updateProfile(profile, &old);
    if (r.isFail()) {
        return r;
    }
    if (profile.sharedConfigId != old.sharedConfigId && old.isActive &&
        isProxyRunning()) {
        (void)syncTakeOver();
    }
    return core::Result<void>::ok();
}

core::Result<void> ClaudeService::deleteProfile(int64_t id, bool* proxyStopped) {
    if (proxyStopped) *proxyStopped = false;
    // R10-A2 段级拆分：两段写删除（段一清 Keychain、段二删 SQLite 行，失败按
    // 旧值回写）已迁 ProfileRepository::deleteProfile（wasActive 经出参回填）；
    // 本方法保留停代理策略尾——删的是激活 profile 且代理运行中 → 停代理
    //（stopProxy 内部 syncRestore 还原 ~/.claude/settings.json），settings.json
    // 不能继续指向无激活 profile 的代理（否则 Claude Code 此后每个请求 503
    // no_active_profile）。UI 经 proxyStopped 出参获知并 toast 告知用户
    bool wasActive = false;
    auto r = profileRepository_.deleteProfile(id, &wasActive);
    if (r.isFail()) {
        return r;
    }
    if (wasActive && isProxyRunning()) {
        LOG_INFO("删除的是激活 profile 且代理运行中 → 停代理并还原 Claude Code 配置");
        stopProxy();
        if (proxyStopped) *proxyStopped = true;
    }
    return core::Result<void>::ok();
}

core::Result<int> ClaudeService::cleanupOrphanKeychainEntries() {
    // R10-A2 段级拆分：listProfiles + Keychain 孤儿条目清理（「Keychain
    // 条目与行一一对应」不变式的维护者）已迁 ProfileRepository
    return profileRepository_.cleanupOrphanKeychainEntries();
}

// ---------- API key ----------

core::Result<std::string> ClaudeService::loadApiKey(int64_t profileId) {
    return profileRepository_.loadApiKey(profileId);
}

// ---------- 激活 ----------

core::Result<Profile> ClaudeService::getActiveProfile() {
    return profileRepository_.getActiveProfile();
}

core::Result<void> ClaudeService::setActiveProfile(int64_t id) {
    if (id == 0) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "id 不能为 0");
    }

    // 校验 profile 存在
    auto p = getProfile(id);
    if (p.isFail()) {
        return core::Result<void>::fail(p.errorCode(), p.errorMessage());
    }

    // 反向同步（设计 §3.1 调用序号 2-3）：把 settings.json 的 env 收编进**当前
    // 激活 profile**——必须发生在「激活行更新（下方 activateProfile）」之前，
    // 否则代理运行中 syncTakeOver 会把自己刚写进去的托管值当用户值捕获。
    // 无激活 profile（首建自动激活 / 首次点「启用」）→ 直接跳过、不读文件：
    // 否则一个坏 settings.json 会把 profile 首建路径整个挡死（G0）。
    // 失败 = 文件存在但读不了/解析失败（G8）→ 中止本次切换（S-2），profile
    // 一字未改；错误经既有 doSetActive 的 toast 呈现，UI 零改动。
    auto curActive = getActiveProfile();
    if (curActive.isOk()) {
        auto capture = syncCaptureActiveProfile();
        if (capture.isFail()) {
            return capture;
        }
    }

    // R10-A2 段级拆分：事务内「清零 → 置位」（is_active 全局唯一）已迁
    // ProfileRepository::activateProfile
    auto result = profileRepository_.activateProfile(id);

    // 代理运行中切 profile → settings.json env 跟随（§5.2 触发点 2）；
    // 代理未运行 → 只更新 DB，不动 settings.json（写了反而制造死指向）。
    // 条目 3：接管失败**不阻断**本次切换（DB 已切成功），失败原因已并入
    // 接管子对象的警告表（经 lastSyncWarnings 透传）供 UI
    if (result.isOk() && isProxyRunning()) {
        (void)syncTakeOver();
    }
    return result;
}

// ---------- Profile 复制（设计 §4.2/§4.3）----------

core::Result<Profile> ClaudeService::duplicateProfile(int64_t srcId) {
    // R10-A2：整方法搬入 ProfileRepository（全部是行 + Keychain 层面操作，
    // 无代理 / 接管交互，且副本不激活 S-4 → 不触发任何策略尾）
    return profileRepository_.duplicateProfile(srcId);
}

// ---------- 共享配置片段（设计 §2.1/§2.4/§2.6）----------
// 生命周期语义（改动当场生效，避免 cc-switch 的「改了片段不生效」头号 bug）：
//   改（改名 / 改 env）→ 绑定它的 profile 下次接管生效；代理在跑且属激活
//     profile → 立即 re-takeover（策略尾在本门面 updateSharedConfig）
//   删 → 同事务解绑绑定它的 profile（shared_config_id = 0，不留悬空 id），
//     随后若代理在跑且激活 profile 在其中 → re-takeover（策略尾在本门面
//     deleteSharedConfig）
//   解绑（经 updateProfile 把 sharedConfigId 置 0）→ 同款当场生效，同名键
//     回落 profile 自身值（片段值从不写进 profile 行）
// DB 段（行读写 / 存在性校验 / 同事务解绑删行 / 悬空 id 读路径容错）随
// R10-A2 迁 ProfileRepository，本门面只留 re-takeover 策略尾与转发

core::Result<std::vector<SharedConfig>> ClaudeService::listSharedConfigs() {
    return profileRepository_.listSharedConfigs();
}

core::Result<SharedConfig> ClaudeService::createSharedConfig(
    const std::string& name, const std::string& extraSettingsJson) {
    return profileRepository_.createSharedConfig(name, extraSettingsJson);
}

core::Result<void> ClaudeService::updateSharedConfig(const SharedConfig& config) {
    // R10-A2 段级拆分：校验 + 存在性 + 行更新（DB 段）已迁
    // ProfileRepository::updateSharedConfig；本方法保留「改后生效」策略尾——
    // 代理在跑且激活 profile 绑定了它 → 立即重写 settings.json（否则 live 会
    // 留旧片段值到下次切换——cc-switch 的「改了片段不生效」头号 bug）
    auto r = profileRepository_.updateSharedConfig(config);
    if (r.isFail()) {
        return r;
    }
    auto active = profileRepository_.getActiveProfile();
    if (active.isOk() && active.value().sharedConfigId == config.id &&
        isProxyRunning()) {
        (void)syncTakeOver();
    }
    return core::Result<void>::ok();
}

core::Result<void> ClaudeService::deleteSharedConfig(int64_t id) {
    // R10-A2 段级拆分：存在性校验 + 同事务解绑删行（DB 段）已迁
    // ProfileRepository::deleteSharedConfig（activeBound 经出参回填——绑定关系
    // 必须在删除前捕获：删除侧同事务把 shared_config_id 解绑为 0，删后读不到
    // 原绑定）；本方法保留「删后生效」策略尾——代理在跑且激活 profile 绑定了
    // 它 → 重写 live（片段键撤走、与片段同名的 profile 自身值恢复生效）
    bool activeBound = false;
    auto r = profileRepository_.deleteSharedConfig(id, &activeBound);
    if (r.isFail()) {
        return r;
    }
    if (activeBound && isProxyRunning()) {
        (void)syncTakeOver();
    }
    return core::Result<void>::ok();
}

// ---------- failover 备用解析（设计 §3.7/§3.8）----------

std::optional<Profile> ClaudeService::resolveFallbackProfile(
    const Profile& profile) {
    if (profile.fallbackProfileId == 0) {
        return std::nullopt;   // 未配备用 → 不换家（最常见路径，零 I/O）
    }
    auto fbR = profileRepository_.getProfile(profile.fallbackProfileId);
    if (fbR.isFail()) {
        LOG_WARN("failover: 备用 profile 不存在（fallback_profile_id=" +
                 std::to_string(profile.fallbackProfileId) +
                 "），本次不换家: " + fbR.errorMessage());
        return std::nullopt;
    }
    Profile fb = std::move(fbR).value();
    auto keyR = profileRepository_.loadApiKey(fb.id);
    if (keyR.isFail() || keyR.value().empty()) {
        LOG_WARN("failover: 备用 profile 密钥不可用（id=" +
                 std::to_string(fb.id) + "），本次不换家" +
                 (keyR.isFail() ? ": " + keyR.errorMessage() : std::string()));
        return std::nullopt;
    }
    fb.apiKeyPlain = std::move(keyR).value();
    return fb;
}

// ---------- 模型槽位解析（agent 子系统 M1）----------

std::optional<std::string> ClaudeService::resolveModelSlot(
    const std::string& slot) {
    auto profileR = profileRepository_.getActiveProfile();
    if (profileR.isFail()) {
        return std::nullopt;   // 无激活 profile → 无可解析（最常见路径）
    }
    Profile profile = std::move(profileR).value();

    // "def" 是 "default" 的别名；表行 jsonKey 就是 "default"，先归一再查表
    const std::string key = (slot == "def") ? "default" : slot;

    // default 槽位保留旧列语义：models_config 未配置**或解析失败**时回落
    // profile.model（非 default 槽解析失败才是 nullopt，见下方分支）
    if (key == "default") {
        if (!profile.modelsConfigJson.empty()) {
            auto cfgR = parseModelsConfig(profile.modelsConfigJson);
            if (cfgR.isOk() && !cfgR.value().def.model.empty()) {
                return cfgR.value().def.model;
            }
        }
        if (profile.model.empty()) return std::nullopt;
        return profile.model;
    }

    if (profile.modelsConfigJson.empty()) return std::nullopt;
    auto cfgR = parseModelsConfig(profile.modelsConfigJson);
    if (cfgR.isFail()) return std::nullopt;
    const ModelsConfig& cfg = cfgR.value();
    // 槽位键 ↔ 成员映射的唯一真相源 = kModelSlotMetas
    //（provider_config.h 的 DEVDESK_MODEL_SLOT_LIST）；消费方遍历本表派生，
    // 不手写槽位分支（表注释契约）
    for (const ModelSlotMeta& meta : modelSlotTable()) {
        if (key == meta.jsonKey) {
            const std::string& model = (cfg.*(meta.slot)).model;
            if (model.empty()) return std::nullopt;
            return model;
        }
    }
    return std::nullopt;   // 未知槽位键
}

// ---------- 连通性测试 ----------

core::Result<platform::http::Response>
ClaudeService::testConnection(const Profile& profile) {
    // 先取 API key（无 key 时也允许，UI 可用未授权请求触发 401 用作验证）
    auto keyResult = loadApiKey(profile.id);
    std::string key;
    if (keyResult.isOk()) {
        key = keyResult.value();
    } else {
        LOG_WARN("testConnection: 无法读取 API key (" +
                 keyResult.errorMessage() + ")，将发未授权请求");
    }

    platform::http::Request req;
    req.method = "GET";
    req.url = stripTrailingSlash(profile.baseUrl) + "/v1/models";
    // 与真实转发同源：探测请求也经上游方言层装配头部（鉴权头形态 / UA 覆盖 /
    // 方言专属会话头），否则探测结果与转发结果会不一致——如 opencode.ai/zen
    // 缺 x-opencode-session、或 Cloudflare 按 UA 拦截时，转得通而探测误报失败。
    // dialectOverride 与两条转发路径（proxy_router / streaming_proxy）同样传参：
    // 漏传会让配了覆盖的 profile 出现「探测按 A 方言、转发按 B 方言」的错位
    //（空串 = 未覆盖 → 方言层回落 host 推断，与不传该参逐项一致）
    const UpstreamDialect dialect =
        UpstreamDialect::forBaseUrl(profile.baseUrl, profile.dialectOverride);
    dialect.applyHeaders(req.headers, key, authFieldFromText(profile.authField));
    req.timeoutMs = 15000;

    auto resp = platform::http::send(req);
    if (resp.isFail()) {
        LOG_WARN("testConnection 网络失败: " + resp.errorMessage());
        return core::Result<platform::http::Response>::fail(
            resp.errorCode(), resp.errorMessage());
    }

    LOG_INFO("testConnection: " + req.url +
             " -> " + std::to_string(resp.value().statusCode) +
             " (" + std::to_string(resp.value().elapsedMs) + "ms)");
    return core::Result<platform::http::Response>::ok(std::move(resp).value());
}

// ---------- 调用日志（R10-A1：读写已抽至 CallLogStore，门面转发）----------

core::Result<void> ClaudeService::logCall(const CallLogEntry& entry) {
    return callLogStore_.logCall(entry);
}

core::Result<std::vector<ClaudeService::CallLogEntry>>
ClaudeService::recentCalls(int limit) {
    return callLogStore_.recentCalls(limit);
}

core::Result<std::vector<ClaudeService::CallLogEntry>>
ClaudeService::queryCalls(const CallLogQuery& q) {
    return callLogStore_.queryCalls(q);
}

core::Result<int> ClaudeService::countCalls(const CallLogQuery& q) {
    return callLogStore_.countCalls(q);
}

// ---------- 最近代理请求事件（气泡③，审计 [18]）----------
// R10-A3：单槽邮箱（mailbox）已随 ProxyRuntime 迁至 proxy_runtime.cpp 同段
//（与在途流登记同写者-读者对，见 R10 spec §2.2），本门面转发

void ClaudeService::recordProxyEvent(int statusCode) {
    proxyRuntime_.recordProxyEvent(statusCode);
}

std::optional<int> ClaudeService::takeProxyEvent(std::time_t nowSec) {
    return proxyRuntime_.takeProxyEvent(nowSec);
}

// ---------- 代理 server 生命周期 ----------
// R10-A3 段级拆分：server/路由/在途流登记/端口镜像/停机序列已随 ProxyRuntime
// 迁至 proxy_runtime.cpp（上游会话标识定义同迁）。门面保留两条
// settings.json 策略尾并逐字编排顺序：startProxy 尾 syncTakeOver（启动成功 →
// 接管，失败不阻断），stopProxy 尾 syncRestore（停机后还原，settings.json 永
// 不指向死代理）；R10-A4 起两条尾的协议侧都经 SettingsTakeOver 子对象

core::Result<int> ClaudeService::startProxy(const std::string& bindAddress,
                                            int port) {
    auto started = proxyRuntime_.startProxy(bindAddress, port);
    if (started.isOk()) {
        // 接管 Claude Code settings.json（§5.2 触发点 1；含手动启动、设置页
        // 改端口的重启）。失败不阻断：代理已可用，接入失败经接管子对象的
        // 警告表上报（条目 3，经 lastSyncWarnings 透传），用户可感知
        (void)syncTakeOver();
    }
    return started;
}

void ClaudeService::stopProxy() {
    proxyRuntime_.stop();
    // 停止即还原（§5.3）：settings.json 永不指向死代理
    syncRestore();
}

bool ClaudeService::isProxyRunning() const {
    return proxyRuntime_.isProxyRunning();
}

int ClaudeService::proxyPort() const {
    return proxyRuntime_.proxyPort();
}

void ClaudeService::setConfiguredProxyPort(int port) {
    proxyRuntime_.setConfiguredProxyPort(port);
}

void ClaudeService::setTerminalLauncher(
    std::function<core::Result<void>(const platform::terminal::LaunchSpec&)>
        launcher) {
    if (launcher) terminalLauncher_ = std::move(launcher);
}

core::Result<void> ClaudeService::runProfile(int64_t id, std::string* hint) {
    if (hint != nullptr) hint->clear();
    if (id == 0) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "profile.id 不能为 0");
    }

    // 顺序 1：目标 profile 必须存在（标题 / baseUrl / 认证字段 / 模型配置都取自它）
    auto prof = getProfile(id);
    if (prof.isFail()) {
        return core::Result<void>::fail(prof.errorCode(), prof.errorMessage());
    }
    const Profile& p = prof.value();

    // 顺序 2：读 Keychain 密钥（D-1 直连形态：密钥由本功能带进临时 settings 文件，
    // 不再由代理转发注入）。失败 / 条目不存在 → 中止，不落盘、不开窗（§5）。
    auto key = loadApiKey(id);
    if (key.isFail()) {
        return core::Result<void>::fail(key.errorCode(),
            "读取 Keychain 密钥失败，未打开会话: " + key.errorMessage());
    }

    // 顺序 3：直连 env（纯函数 buildDirectEnv：BASE_URL 去尾斜杠 / 密钥键三态 /
    // 槽位 + extra_env，托管键压过 extra_env；共享片段已迁 JSON 域 §8.4，
    // 不再进本函数）→ 序列化成 settings JSON（{"env": {...}}）→ 落盘
    // appDataDir()/run-settings/run-<id>-<ts>.json。
    // 关键：**零全局改动**——不启代理、不切激活、不读也不写 ~/.claude/settings.json
    const auto env = buildDirectEnv(p.baseUrl, key.value(), p.authField,
                                    p.modelsConfigJson, p.extraEnvJson);
    platform::json::JsonObject envObj;
    for (const auto& [k, v] : env) {
        envObj.set(k, platform::json::makeString(v));
    }
    platform::json::JsonObject root;
    root.set("env", platform::json::JsonValue(std::move(envObj)));
    // 生效高级设置 JSON 并入顶层（2026-09-30 advanced-json-editor 设计 §4.1
    // 裁决①：直连会话同样带 hooks/permissions，三链不分叉；临时文件随会话
    // 自删 + 24h 清扫兜底，无 sidecar 记账负担）。生效 = profile JSON ⊕ 片段
    // JSON（片段赢，§8.2 生成时叠加；§4.2 步骤 1）
    {
        std::vector<std::string> effWarnings;
        const std::string effective = effectiveExtraSettingsJson(
            p.extraSettingsJson,
            profileRepository_.sharedExtraSettingsJsonFor(p.sharedConfigId),
            &effWarnings);
        for (const auto& w : effWarnings) {
            LOG_WARN("runProfile: " + w);
        }
        auto apply = applyExtraSettingsToSettingsDoc(root, effective);
        for (const auto& w : apply.warnings) {
            LOG_WARN("runProfile: 高级设置 JSON: " + w);
        }
    }
    auto text = platform::json::stringify(platform::json::JsonValue(std::move(root)));
    if (text.isFail()) {
        return core::Result<void>::fail(text.errorCode(),
            "序列化临时 settings 失败: " + text.errorMessage());
    }

    const std::filesystem::path dir = core::paths::appDataDir() / "run-settings";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
            "创建 run-settings 目录失败: " + dir.string() + "（" + ec.message() + "）");
    }
    // per-launch 24h 清扫（§5）：上次崩溃 / 强制结束的会话留下的孤儿文件在此兜底
    platform::terminal::sweepStaleRunSettingsFiles(dir);

    const std::filesystem::path file =
        dir / ("run-" + std::to_string(id) + "-" +
               std::to_string(static_cast<long long>(nowUnix())) + ".json");
    auto wrote = core::atomicWriteFile(file, text.value(), "临时 settings");
    if (wrote.isFail()) {
        return core::Result<void>::fail(wrote.errorCode(),
            "写临时 settings 失败，未打开会话: " + wrote.errorMessage());
    }

    // 顺序 4：派发终端（D-1：claude --settings <临时文件>；会话正文退出后自删
    // 该文件。分离式启动，DevDesk 不持有会话句柄——D-9 只提示、不阻止）
    platform::terminal::LaunchSpec spec;
    spec.title = p.name + " · Claude Code";
    spec.workingDir = platform::terminal::homeDir();
    spec.program = "claude";
    spec.args = {"--settings", file.string()};

    auto launched = terminalLauncher_(spec);
    if (launched.isFail()) {
        // 会话没起来 → 替它做掉「退出后自删」（best-effort），不留孤儿等 24h 清扫
        std::error_code rmEc;
        std::filesystem::remove(file, rmEc);
        return launched;
    }

    // D-5：wt 缺失 → 平台层已退化传统控制台，这里补一句非致命提示（不静默、不阻断）。
    // 文案保持中性：「传统控制台」只说「不是 Windows Terminal 那条路径」，
    // 不臆断机器上没装——系统默认终端本身可能就是 Windows Terminal（此时措辞仍成立）。
    if (hint != nullptr && platform::terminal::windowsTerminalMissing()) {
        *hint = "未检测到 Windows Terminal，已用系统默认终端打开会话";
    }
    LOG_INFO("已直连打开 Claude Code 会话（profile id=" + std::to_string(id) +
             "，临时 settings=" + file.string() + "）");
    return core::Result<void>::ok();
}

// ---------- 在途流式上游登记（审计 2026-09-23 [9]）----------
// R10-A3：登记表状态与实现已迁 ProxyRuntime（在途流登记状态与停止标志在
// proxy_runtime.{h,cpp}），本门面转发

void ClaudeService::registerActiveStream(
    const std::shared_ptr<platform::http::StreamingResponse>& stream) {
    proxyRuntime_.registerActiveStream(stream);
}

void ClaudeService::cancelActiveStreams() {
    proxyRuntime_.cancelActiveStreams();
}

// ---------- Claude Code settings 接管（Phase C；R10-A4：协议侧已迁
// SettingsTakeOver，本段为门面转发 + 编排：setSettingsSyncPaths 注入（inline，
// 在 claude_service.h）/ recoverPendingSettingsRestore 补还原 / 状态 getter）
// ----------

bool ClaudeService::isClaudeCodeManaged() const {
    return settingsTakeover_.isClaudeCodeManaged();
}

const std::vector<std::string>& ClaudeService::lastSyncWarnings() const {
    return settingsTakeover_.warnings();
}

void ClaudeService::recoverPendingSettingsRestore() {
    // R10-A4：路径/开关门控 + 补还原协议（recoverPendingSnapshot 的调用在
    // settings_takeover.cpp）已迁 SettingsTakeOver
    settingsTakeover_.recoverPendingSettingsRestore();
}

// ---------- 反向同步（settings.json → 激活 profile，设计 §3.1/§3.4/§3.5）----------

core::Result<void> ClaudeService::syncCaptureActiveProfile() {
    if (settingsTakeover_.paths().settingsFile.empty()) {
        return core::Result<void>::ok();  // 通道未注入（§3.6 唯一开关）
    }

    // G0 由调用方（setActiveProfile）判断：「无激活 profile 不读文件」。此处
    // 防御式再查一次，避免被其它入口误调时把值写进错误的 profile
    auto activeR = getActiveProfile();
    if (activeR.isFail()) {
        return core::Result<void>::ok();
    }
    const Profile prof = std::move(activeR).value();

    // 读侧（M1）：不存在 / 0 字节空文件 / 无 env → present=false（no-op 成功，
    // 照常切换）；存在但读不了 / 解析失败 / env 非对象 → ok=false（G8 → S-2）
    // R10-A4：这是本文件唯一保留的接管协议调用（Q2 否决 4 口径 b，R10 spec
    // §2.4 P-A4 断言 ≤1）——读侧留门面、不搬 SettingsTakeOver
    auto read = settings_sync::readSettingsEnvForSync(settingsTakeover_.paths());
    if (!read.ok) {
        return core::Result<void>::fail(core::ErrorCode::FileReadFailed,
                                        read.error);
    }
    if (!read.present) {
        return core::Result<void>::ok();
    }

    // 纯函数层（M2）：映射表 + 守卫表（G1–G4/G6 跳过的键记在 skippedKeys）。
    //（原 G10「绑定片段 ownedKeys」入参随 advanced-json-editor §8.4 拆除：
    // 片段不再携带 env 键，守卫为永不触发的死通道）
    auto parsed = parseEnvForReverseSync(read.envJson, read.managedKeys);
    if (!parsed.ok) {
        return core::Result<void>::fail(core::ErrorCode::JsonParseError,
            "settings.json env 解析失败（" +
            settingsTakeover_.paths().settingsFile.string() + "）: " + parsed.error);
    }
    for (const auto& w : parsed.cfg.warnings) {
        LOG_WARN("反向同步（settings.json → profile）: " + w);
    }

    const std::vector<std::string>& skipped = parsed.cfg.skippedKeys;
    const auto isSkipped = [&skipped](const char* key) {
        return std::find(skipped.begin(), skipped.end(), key) != skipped.end();
    };

    const ReverseSyncMerge merged = mergeReverseSyncEnv(
        parsed.cfg, prof.modelsConfigJson, prof.extraEnvJson);

    // ---- 逐字段比较（§3.5 无变化 → 零写入，不 bump updated_at）----
    // base_url：G2 命中（loopback 本地代理形态）→ 保持 profile 原值，不清
    const std::string newBaseUrl = isSkipped("ANTHROPIC_BASE_URL")
        ? prof.baseUrl
        : stripTrailingSlash(trim(parsed.cfg.baseUrl));
    const bool baseUrlChanged = (newBaseUrl != prof.baseUrl);

    // token：G1/G2 联动/G3 占位/G4 空值/G6 非法 → 不写也不清（保持原密钥）
    const bool tokenSkipped = isSkipped("ANTHROPIC_AUTH_TOKEN") ||
                              isSkipped("ANTHROPIC_API_KEY");
    auto curKeyR =
        platform::keychain::loadSecret(keychainService_, prof.keychainAccount);
    const std::string curKey = curKeyR.isOk() ? curKeyR.value() : std::string();
    bool keyWrite = false;  // 出现 → 覆盖 Keychain（经 updateProfile）
    bool keyClear = false;  // 缺席 → 清空（Keychain 条目 + 三列元数据）
    if (!tokenSkipped) {
        if (!parsed.cfg.authToken.empty()) {
            keyWrite = (parsed.cfg.authToken != curKey);
        } else {
            keyClear = !curKey.empty();
        }
    }

    if (!baseUrlChanged && !merged.modelsChanged && !merged.extraEnvChanged &&
        !keyWrite && !keyClear) {
        return core::Result<void>::ok();  // 无变化：不 UPDATE、不写 Keychain
    }

    // ---- 目标值 ----
    Profile upd = prof;
    upd.baseUrl = newBaseUrl;
    if (merged.modelsChanged) {
        upd.modelsConfigJson = merged.modelsConfigJson;
        // model 列显式跟随 default 槽位：updateProfile 内虽有同款跟随，但
        // 「default 槽位被清空」时它拿不到空值，清空就落不了库
        upd.model = effectiveModelsConfig(merged.modelsConfigJson).def.model;
    }
    if (merged.extraEnvChanged) {
        upd.extraEnvJson = merged.extraEnvJson;
    }

    // 两段写（CPX-07）：Keychain 侧统一走助手（§3.5 表末行的失败补偿即其
    // 内置行为），旧值 = 上面 loadSecret 取到的 curKey。任一落库段失败 → 按
    // 旧值回写 Keychain，profile 回到「改之前」的状态。回写失败的风险如实
    // 登记于设计 §8.5
    TwoPhaseKeyWrite keyTx(keychainService_, prof.keychainAccount, curKey);

    // key 缺席清空：先删 Keychain 条目（同 deleteProfile 的顺序——先 Keychain
    // 后落库，失败即中止且 profile 一字未改；keychain 层不新增 API）
    if (keyClear) {
        auto del = keyTx.writeKey(std::string());   // 空串 = 删除该条目
        if (del.isFail()) {
            LOG_ERROR("反向同步清理 Keychain 失败，中止本次切换（profile id=" +
                      std::to_string(prof.id) + "）: " + del.errorMessage());
            return core::Result<void>::fail(del.errorCode(),
                "清理该 Profile 的密钥失败，已中止本次切换: " + del.errorMessage());
        }
    }

    // 字段（+ key 写入）与元数据清除同属段二：updateProfile 内部也是
    // Keychain → DB 的两段写（同一助手语义），keyClear 的「清三列」紧随其后；
    // 任一步失败都由助手把 Keychain 回写到旧值
    if (baseUrlChanged || merged.modelsChanged || merged.extraEnvChanged ||
        keyWrite || keyClear) {
        if (keyWrite) upd.apiKeyPlain = parsed.cfg.authToken;
        auto w = keyTx.writeDb([&]() -> core::Result<void> {
            if (baseUrlChanged || merged.modelsChanged ||
                merged.extraEnvChanged || keyWrite) {
                auto u = updateProfile(upd);
                if (u.isFail()) {
                    return core::Result<void>::fail(u.errorCode(),
                                                    u.errorMessage());
                }
            }
            // 三列元数据清除（keychain_account / api_key_hint / api_key_check）：
            // updateProfile 的空 apiKeyPlain 分支不触碰它们，故走这条最小 UPDATE
            if (keyClear) {
                auto prep = db_.prepare(
                    "UPDATE claude_profiles SET keychain_account = '', "
                    "api_key_hint = '', api_key_check = '', updated_at = ? "
                    "WHERE id = ?");
                if (prep.isFail()) {
                    return core::Result<void>::fail(prep.errorCode(),
                                                    prep.errorMessage());
                }
                auto& stmt = prep.value();
                stmt.bind(1, static_cast<int64_t>(nowUnix()));
                stmt.bind(2, prof.id);
                auto stepR = stmt.step();
                if (stepR.isFail()) {
                    return core::Result<void>::fail(stepR.errorCode(),
                                                    stepR.errorMessage());
                }
            }
            return core::Result<void>::ok();
        });
        if (w.isFail()) {
            std::string msg =
                "反向同步写库失败，已中止本次切换: " + w.errorMessage();
            // 是否要提示「密钥可能不可用」只看 Keychain 的实际状态，不信补偿
            // 返回值：仍在旧值 = 已回到改之前（含「压根没写成」这种情形）
            if ((keyWrite || keyClear) && !keyAtOldValue(keychainService_,
                                                         prof.keychainAccount,
                                                         curKey)) {
                msg += "；该 Profile 的密钥可能处于不可用态，"
                       "重新填一次 API Key 即可恢复";
            }
            return core::Result<void>::fail(w.errorCode(), msg);
        }
    }

    LOG_INFO("反向同步：已把 " + settingsTakeover_.paths().settingsFile.string() +
             " 的 env 收编进激活 profile id=" + std::to_string(prof.id));
    return core::Result<void>::ok();
}

core::Result<void> ClaudeService::syncTakeOver() {
    // R10-A4：编排留门面——从 ProfileRepository 取激活 Profile、从 ProxyRuntime
    // 取端口，一并传入 SettingsTakeOver（P-A4 双入参，子对象不反向依赖
    // ProfileRepository / ProxyRuntime）。路径/开关门控与接管/还原协议在子对象
    // 内部，调用顺序逐字保持。
    // 无激活 profile：仍接管 BASE_URL/api key 占位（接入路径先就位，转发层会
    // 报 503 提示用户先启用 profile）。
    // advanced-json-editor §5.2①：门面把「生效 JSON」预先算好放进传入的
    // Profile 副本（生效 = profile JSON ⊕ 片段 JSON，片段赢 §8.2 生成时叠加；
    // 非法/超限一侧按空处理并 WARN），settings_sync::takeOver 零新增参数直接
    // 消费 profile.extraSettingsJson
    Profile active;
    auto a = getActiveProfile();
    if (a.isOk()) {
        active = std::move(a).value();
    }
    {
        std::vector<std::string> effWarnings;
        active.extraSettingsJson = effectiveExtraSettingsJson(
            active.extraSettingsJson,
            profileRepository_.sharedExtraSettingsJsonFor(
                active.sharedConfigId),
            &effWarnings);
        for (const auto& w : effWarnings) {
            LOG_WARN("syncTakeOver: " + w);
        }
    }
    return settingsTakeover_.syncTakeOver(active, proxyPort());
}

void ClaudeService::syncRestore() {
    // CPX-02（R10-A4 收尾）：协议侧已迁 SettingsTakeOver::syncRestore() 并改返
    // Result<void>（失败并入其 warnings_，UI 经 lastSyncWarnings 透传）；
    // 门面保持 void 签名不变，失败 LOG_WARN 留痕
    auto r = settingsTakeover_.syncRestore();
    if (r.isFail()) {
        LOG_WARN("Claude Code settings 还原失败: " + r.errorMessage());
    }
}

std::string formatCallTime(std::time_t ts, std::time_t now) {
    if (ts <= 0) return "--:--";
    std::tm tmv{};
    core::time::localTime(ts, &tmv);
    std::tm nowTm{};
    core::time::localTime(now, &nowTm);
    const auto sameDay = [](const std::tm& a, const std::tm& b) {
        return a.tm_year == b.tm_year && a.tm_yday == b.tm_yday;
    };
    char buf[24];
    if (sameDay(tmv, nowTm)) {
        std::snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        return std::string(buf);
    }
    // ponytail: 昨天判定取 now-86400 的本地日；DST 切换日误差 ≤1 小时，
    // 只影响「昨天/日期」文案边界，无正确性风险
    std::tm yestTm{};
    core::time::localTime(now - 86400, &yestTm);
    if (sameDay(tmv, yestTm)) {
        std::snprintf(buf, sizeof(buf), "昨天 %02d:%02d", tmv.tm_hour, tmv.tm_min);
    } else {
        std::snprintf(buf, sizeof(buf), "%02d-%02d", tmv.tm_mon + 1, tmv.tm_mday);
    }
    return std::string(buf);
}

} // namespace module::claude
