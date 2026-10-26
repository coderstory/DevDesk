// src/module_claude_proxy/claude_service.cpp
// ClaudeService 实现：Profile CRUD + 凭据列 + 上游连通性测试
#include "claude_service.h"

#include "core/atomic_write.h"
#include "core/file_perms.h"
#include "core/logging.h"
#include "core/paths.h"
#include "core/time.h"
#include "platform/credential_protection.h"
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

// 旧 Keychain service 名前缀（拼在数据根短哈希之前）
constexpr const char* kKeychainServicePrefix = "DevDesk";

using core::time::nowUnix;

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

// 本行凭据当前是否等于 expected（列空/解不开 = 按空串比较）。反向同步写库失败后
// 用它按实际状态判断密钥是否回到旧值，不信补偿返回值。
bool keyAtOldValue(ProfileRepository& repo, int64_t profileId,
                   const std::string& expected) {
    auto r = repo.loadApiKey(profileId);
    return (r.isOk() ? r.value() : std::string()) == expected;
}

} // namespace

// 按数据根派生 Keychain 命名空间名（仅用于残留清理）
std::string keychainServiceForDataRoot(const std::string& dataRoot) {
    if (dataRoot.empty()) {
        return std::string();   // 空根无法派生（生产不可达）
    }
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(dataRoot.data()),
           dataRoot.size(), digest);
    std::ostringstream oss;
    // 前 4 字节 8 位 hex（同 ProfileRepository::sha256HexPrefix 口径）
    for (int i = 0; i < 4 && i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(digest[i]);
    }
    return std::string(kKeychainServicePrefix) + "-" + oss.str();
}

namespace {

// 生产路径：按数据根派生清理命名空间名；派生失败（空根）兜底裸前缀。
std::string resolveKeychainService() {
    auto derived = keychainServiceForDataRoot(core::paths::appDataDir().string());
    return derived.empty() ? std::string(kKeychainServicePrefix) : derived;
}

} // namespace

ClaudeService::ClaudeService(core::data::SqliteDb& db,
                             const std::string& keychainService)
    : db_(db),
      profileRepository_(db, keychainService.empty() ? resolveKeychainService()
                                                     : keychainService),
      proxyRuntime_(*this) {}

// 全局单例指针（析构要在它身上摘牌）
namespace {
    ClaudeService* g_globalClaudeService = nullptr;
}

ClaudeService::~ClaudeService() {
    // 析构摘牌：防止 onStop 经 global() 拿到已析构对象（悬垂读）。
    if (g_globalClaudeService == this) g_globalClaudeService = nullptr;
    // 确保停止代理（停机序列在 ProxyRuntime::stop()）；声明序末位，成员析构也会兜底。
    proxyRuntime_.stop();
}

void ClaudeService::setGlobal(ClaudeService* svc) { g_globalClaudeService = svc; }
ClaudeService* ClaudeService::global() { return g_globalClaudeService; }

// Profile CRUD：落库/凭据段在 ProfileRepository，门面留策略尾（首建自动激活 /
// re-takeover 时机 / 停代理）
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
                                                   const std::string& authField) {
    // 策略尾：无激活 Profile 时自动激活（经 setActiveProfile，代理运行中会同步
    // 接管 settings.json）。失败不回滚创建，只 WARN（UI 走「点击启用」文案）。
    auto created = profileRepository_.createProfile(
        name, baseUrl, apiKey, model, authField);
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
        // 已有激活 → 不抢占；失败但非 NoActiveProfile（DB 异常）→ 保守不动作
    }
    out.isActive = autoActivated;
    return core::Result<Profile>::ok(std::move(out));
}

core::Result<void> ClaudeService::updateProfile(const Profile& profile) {
    return profileRepository_.updateProfile(profile);
}

core::Result<void> ClaudeService::deleteProfile(int64_t id, bool* proxyStopped) {
    if (proxyStopped) *proxyStopped = false;
    // 策略尾：删的是激活 profile 且代理运行中 → stopProxy()（内部还原
    // ~/.claude/settings.json），否则 Claude Code 后续请求全 503。proxyStopped 出参供 UI toast。
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

core::Result<int> ClaudeService::backfillCredentialsFromKeychain() {
    return profileRepository_.backfillCredentialsFromKeychain();
}

core::Result<std::string> ClaudeService::loadApiKey(int64_t profileId) {
    return profileRepository_.loadApiKey(profileId);
}

core::Result<Profile> ClaudeService::getActiveProfile() {
    return profileRepository_.getActiveProfile();
}

core::Result<void> ClaudeService::setActiveProfile(int64_t id) {
    // 服务内互斥：方法体含 DB 事务 + settings.json 接管 + warnings_ 内存态，非原子；
    // agent 线程与 UI 线程可并发进入，锁护整个方法体。其余读方法保持无锁。
    std::lock_guard<std::mutex> lock(switchMutex_);

    if (id == 0) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "id 不能为 0");
    }

    auto p = getProfile(id);
    if (p.isFail()) {
        return core::Result<void>::fail(p.errorCode(), p.errorMessage());
    }

    // 反向同步必须早于 activateProfile（否则 syncTakeOver 会把自己写的托管值当
    // 用户值捕获）。无激活 → 跳过；读不了/解析失败 → 中止本次切换。
    auto curActive = getActiveProfile();
    if (curActive.isOk()) {
        auto capture = syncCaptureActiveProfile();
        if (capture.isFail()) {
            return capture;
        }
    }

    // 事务内「清零 → 置位」，保证 is_active 全局唯一（在 ProfileRepository）
    auto result = profileRepository_.activateProfile(id);

    // 代理运行中切 profile → settings.json env 跟随；未运行 → 只更新 DB（写了会
    // 制造死指向）。接管失败不阻断本次切换，原因并入警告表（经 lastSyncWarnings）。
    if (result.isOk() && isProxyRunning()) {
        (void)syncTakeOver();
    }
    return result;
}

core::Result<Profile> ClaudeService::duplicateProfile(int64_t srcId) {
    // 整方法在 ProfileRepository（纯行 + 凭据列操作，副本不激活，无策略尾）
    return profileRepository_.duplicateProfile(srcId);
}

core::Result<platform::http::Response>
ClaudeService::probeProfile(const Profile& profile, int timeoutMs) {
    // 无 key 也允许：UI 可用未授权请求触发 401 作验证
    auto keyResult = loadApiKey(profile.id);
    std::string key;
    if (keyResult.isOk()) {
        key = keyResult.value();
    } else {
        LOG_WARN("testConnection: 无法读取 API key (" +
                 keyResult.errorMessage() + ")，将发未授权请求");
    }

    platform::http::Request req;
    // 探测路径与真实转发同源：claude CLI 走 POST /v1/messages（proxy_router 原样拼
    // base+path）。GET /v1/models 是 OpenAI 惯例，Anthropic-compatible 网关常不实现
    // ——实测 bailian 网关带有效 key 也 404，旧探测把好配置误报成「连通异常」。
    req.method = "POST";
    req.url = stripTrailingSlash(profile.baseUrl) + "/v1/messages";
    // 1-token ping；模型取该 profile 实际会用的（models_config 的 default 槽回落
    // profile.model），都没有则发占位名——上游 4xx 仍可区分「可达+鉴权已过」与
    // 「密钥/网络问题」（无 key 请求会被鉴权中间件 401 挡在路由之前）。
    std::string probeModel;
    if (!profile.modelsConfigJson.empty()) {
        auto cfgR = parseModelsConfig(profile.modelsConfigJson);
        if (cfgR.isOk() && !cfgR.value().def.model.empty()) {
            probeModel = cfgR.value().def.model;
        }
    }
    if (probeModel.empty()) probeModel = profile.model;
    if (probeModel.empty()) probeModel = "ping";
    req.body = "{\"model\":\"" + probeModel +
               "\",\"max_tokens\":1,\"messages\":[{\"role\":\"user\",\"content\":\"ping\"}]}";
    req.headers["content-type"] = "application/json";   // 转发时由客户端头带来，探测需自备
    // 探测请求经上游方言层装配头部，与真实转发同源（否则转得通而探测误报失败）。
    const UpstreamDialect dialect = UpstreamDialect::forBaseUrl(profile.baseUrl);
    dialect.applyHeaders(req.headers, key, authFieldFromText(profile.authField));
    if (dialect.normalizeBody(req.body)) {
        LOG_INFO("testConnection: 按上游方言剥掉模型名 [1M] 标记");
    }
    req.timeoutMs = timeoutMs;

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

core::Result<platform::http::Response>
ClaudeService::testConnection(const Profile& profile) {
    // 探测体在 probeProfile（前置校验传短超时）；本方法超时恒为 15000
    return probeProfile(profile, kTestConnectionTimeoutMs);
}

core::Result<void> ClaudeService::validateProxyPreconditions(int probeTimeoutMs) {
    // 检查 1：激活 profile（零网络）。失败原样透传错误码 + 指引，前缀标明是本校验挡下的
    auto active = getActiveProfile();
    if (active.isFail()) {
        LOG_WARN("代理启动前置校验未通过：无激活 profile - " + active.errorMessage());
        return core::Result<void>::fail(
            active.errorCode(),
            "代理启动前置校验未通过（1/2 激活 Profile）：" + active.errorMessage());
    }
    const Profile prof = std::move(active).value();

    // 检查 2：连通测试（联网，最长 probeTimeoutMs）。探测失败 / 非 2xx 都算不过
    auto resp = probeProfile(prof, probeTimeoutMs);
    if (resp.isFail()) {
        LOG_WARN("代理启动前置校验未通过：连通测试失败（profile id=" +
                 std::to_string(prof.id) + "）- " + resp.errorMessage());
        return core::Result<void>::fail(
            resp.errorCode(),
            "代理启动前置校验未通过（2/2 连通测试）：" + resp.errorMessage());
    }
    const int code = resp.value().statusCode;
    if (code < 200 || code >= 300) {
        LOG_WARN("代理启动前置校验未通过：上游返回 HTTP " + std::to_string(code) +
                 "（profile id=" + std::to_string(prof.id) + "）");
        return core::Result<void>::fail(
            core::ErrorCode::HttpError,
            "代理启动前置校验未通过（2/2 连通测试）：上游返回 HTTP " +
                std::to_string(code) + "（检查 API Key 或 Base URL）");
    }
    LOG_INFO("代理启动前置校验通过：profile「" + prof.name + "」(id=" +
             std::to_string(prof.id) + ") 连通 OK（HTTP " + std::to_string(code) + "）");
    return core::Result<void>::ok();
}

// 最近代理请求事件：单槽邮箱在 ProxyRuntime（与在途流登记同写者-读者对），门面转发。
void ClaudeService::recordProxyEvent(int statusCode) {
    proxyRuntime_.recordProxyEvent(statusCode);
}

std::optional<int> ClaudeService::takeProxyEvent(std::time_t nowSec) {
    return proxyRuntime_.takeProxyEvent(nowSec);
}

// 代理 server 生命周期：server/路由/在途流/端口镜像在 ProxyRuntime。门面保留两条
// settings.json 策略尾——startProxy 尾 syncTakeOver（失败不阻断），stopProxy 尾 syncRestore。
core::Result<int> ClaudeService::startProxy(const std::string& bindAddress,
                                            int port) {
    auto started = proxyRuntime_.startProxy(bindAddress, port);
    if (started.isOk()) {
        // 接管 settings.json；失败不阻断（代理已可用，接入失败经警告表上报）
        (void)syncTakeOver();
    }
    return started;
}

void ClaudeService::stopProxy() {
    proxyRuntime_.stop();
    // 停止即还原：settings.json 永不指向死代理
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

core::Result<void> ClaudeService::runProfile(int64_t id, std::string* hint,
                                           const std::string& workingDir) {
    if (hint != nullptr) hint->clear();
    if (id == 0) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "profile.id 不能为 0");
    }

    // 顺序 1：profile 必须存在（标题/baseUrl/认证/模型都取自它）
    auto prof = getProfile(id);
    if (prof.isFail()) {
        return core::Result<void>::fail(prof.errorCode(), prof.errorMessage());
    }
    const Profile& p = prof.value();

    // 顺序 2：读密钥（直连形态，密钥带进临时 settings）。失败 → 中止，不落盘、不开窗。
    auto key = loadApiKey(id);
    if (key.isFail()) {
        return core::Result<void>::fail(key.errorCode(),
            "读取 API Key 失败，未打开会话: " + key.errorMessage());
    }

    // 顺序 3：buildDirectEnv（规则见 provider_config.h）→ 序列化 {"env":{...}} →
    // 落盘 run-settings/。零全局改动：不启代理、不切激活、不碰 ~/.claude/settings.json。
    const auto env = buildDirectEnv(p.baseUrl, key.value(), p.authField,
                                    p.modelsConfigJson, p.extraEnvJson);
    platform::json::JsonObject envObj;
    for (const auto& [k, v] : env) {
        envObj.set(k, platform::json::makeString(v));
    }
    platform::json::JsonObject root;
    root.set("env", platform::json::JsonValue(std::move(envObj)));
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
    // per-launch 24h 清扫：兜底上次崩溃/强杀留下的孤儿文件
    platform::terminal::sweepStaleRunSettingsFiles(dir);

    const auto writtenAt = nowUnix();
    // 键用毫秒而非秒：run_key 有 UNIQUE 约束，同一秒内连开两次会撞键；撞键只能
    // 让该次会话失败（补偿路径拿的是调用点算出的键，换键重试会删不掉、留假记录）。
    const auto writtenAtMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    const std::string runKey = "run-" + std::to_string(id) + "-" +
                               std::to_string(writtenAtMs);
    const std::filesystem::path file = dir / (runKey + ".json");

    // DB 记录先行：DB 行是真值，文件是临时载体（会话可能被强杀、文件会被清扫）。
    // settings_json 含明文 API key，经分平台保护入库。
    auto record = recordRunSettings(runKey, id, p.name, writtenAt, file.string(),
                                    text.value());
    if (record.isFail()) {
        return core::Result<void>::fail(record.errorCode(),
            "记录直连会话 settings 失败，未打开会话: " + record.errorMessage());
    }

    auto wrote = core::atomicWriteFile(file, text.value(), "临时 settings");
    if (wrote.isFail()) {
        // 文件没落地 → 撤掉这条记录（不留指向不存在文件的记录）
        forgetRunSettings(runKey);
        return core::Result<void>::fail(wrote.errorCode(),
            "写临时 settings 失败，未打开会话: " + wrote.errorMessage());
    }
    // 权限收紧：临时 settings 含明文 API key，atomicWriteFile 不设权限 → umask 022
    // 下落成 0644，同机其他用户可直读。收紧失败即删掉已落盘的明文文件并按失败返回。
    if (auto permR = core::fileperms::restrictToOwnerOnly(file); permR.isFail()) {
        std::error_code rmEc;
        std::filesystem::remove(file, rmEc);
        forgetRunSettings(runKey);
        return core::Result<void>::fail(permR.errorCode(),
            "临时 settings 权限收紧失败，已删除该文件且未打开会话: " +
                permR.errorMessage());
    }

    // 顺序 4：claude --settings <临时文件>，分离式启动（DevDesk 不持有会话句柄）。
    platform::terminal::LaunchSpec spec;
    spec.title = p.name + " · Claude Code";
    spec.workingDir = workingDir.empty()
        ? platform::terminal::homeDir()
        : workingDir;
    spec.program = "claude";
    spec.args = {"--settings", file.string()};

    auto launched = terminalLauncher_(spec);
    if (launched.isFail()) {
        // 会话没起来 → 替它做掉「退出后自删」（best-effort），不留孤儿等 24h 清扫
        std::error_code rmEc;
        std::filesystem::remove(file, rmEc);
        forgetRunSettings(runKey);
        return launched;
    }

    // wt 缺失 → 平台层已退化传统控制台，补一句非致命提示。文案中性：「传统控制台」不说没装。
    if (hint != nullptr && platform::terminal::windowsTerminalMissing()) {
        *hint = "未检测到 Windows Terminal，已用系统默认终端打开会话";
    }
    LOG_INFO("已直连打开 Claude Code 会话（profile id=" + std::to_string(id) +
             "，临时 settings=" + file.string() + "）");
    return core::Result<void>::ok();
}

core::Result<void> ClaudeService::recordRunSettings(
    const std::string& runKey, int64_t profileId,
    const std::string& profileName, std::time_t writtenAt,
    const std::string& filePath, const std::string& settingsJson) const {
    // 含明文 API key → 走分平台保护再入库
    auto protectedJson =
        platform::credential::protect(settingsJson);
    if (protectedJson.isFail()) {
        return core::Result<void>::fail(protectedJson.errorCode(),
            "临时 settings 凭据保护失败: " + protectedJson.errorMessage());
    }
    auto prep = db_.prepare(
        "INSERT INTO claude_run_settings (run_key, profile_id, profile_name, "
        "written_at, file_path, settings_json) VALUES (?, ?, ?, ?, ?, ?)");
    if (prep.isFail()) {
        return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    }
    if (auto b = prep.value().bind(1, runKey); b.isFail()) return b;
    if (auto b = prep.value().bind(2, profileId); b.isFail()) return b;
    if (auto b = prep.value().bind(3, profileName); b.isFail()) return b;
    if (auto b = prep.value().bind(4, static_cast<int64_t>(writtenAt)); b.isFail()) return b;
    if (auto b = prep.value().bind(5, filePath); b.isFail()) return b;
    if (auto b = prep.value().bind(6, protectedJson.value()); b.isFail()) return b;
    auto stepped = prep.value().step();
    if (stepped.isFail()) {
        return core::Result<void>::fail(stepped.errorCode(), stepped.errorMessage());
    }
    return core::Result<void>::ok();
}

void ClaudeService::forgetRunSettings(const std::string& runKey) const {
    // 键走绑定参数而非拼进 SQL 文本
    auto prep = db_.prepare("DELETE FROM claude_run_settings WHERE run_key = ?");
    if (prep.isFail()) {
        LOG_WARN("recordRunSettings 补偿删除失败（prepare）: " +
                 prep.errorMessage());
        return;
    }
    if (auto b = prep.value().bind(1, runKey); b.isFail()) {
        LOG_WARN("recordRunSettings 补偿删除失败（bind）: " + b.errorMessage());
        return;
    }
    auto stepped = prep.value().step();
    if (stepped.isFail()) {
        LOG_WARN("recordRunSettings 补偿删除失败（留下一条指向不存在文件的记录）: " +
                 stepped.errorMessage());
    }
}

// 在途流式上游登记：状态与实现在 ProxyRuntime，门面转发。
void ClaudeService::registerActiveStream(
    const std::shared_ptr<platform::http::StreamingResponse>& stream) {
    proxyRuntime_.registerActiveStream(stream);
}

void ClaudeService::cancelActiveStreams() {
    proxyRuntime_.cancelActiveStreams();
}

// Claude Code settings 接管：协议侧在 SettingsTakeOver，本段为门面转发 + 编排。
bool ClaudeService::isClaudeCodeManaged() const {
    return settingsTakeover_.isClaudeCodeManaged();
}

std::vector<std::string> ClaudeService::lastSyncWarnings() const {
    // 值快照，不是活引用（写入口见 setActiveProfile → syncTakeOver，agent 线程）
    return settingsTakeover_.warningsSnapshot();
}

void ClaudeService::recoverPendingSettingsRestore() {
    // 路径/开关门控 + 补还原协议在 SettingsTakeOver
    settingsTakeover_.recoverPendingSettingsRestore();
}

// 反向同步：settings.json → 激活 profile
core::Result<void> ClaudeService::syncCaptureActiveProfile() {
    if (settingsTakeover_.paths().settingsFile.empty()) {
        return core::Result<void>::ok();  // 通道未注入（唯一开关）
    }

    // 调用方已判「无激活不读文件」；此处防御式再查，避免被误调时写错 profile。
    auto activeR = getActiveProfile();
    if (activeR.isFail()) {
        return core::Result<void>::ok();
    }
    const Profile prof = std::move(activeR).value();

    // 读侧：不存在/空文件/无 env → present=false（no-op 成功）；读不了/解析失败/
    // env 非对象 → ok=false。这是本文件唯一保留的接管协议调用（读侧留门面）。
    auto read = settings_sync::readSettingsEnvForSync(settingsTakeover_.paths());
    if (!read.ok) {
        return core::Result<void>::fail(core::ErrorCode::FileReadFailed,
                                        read.error);
    }
    if (!read.present) {
        return core::Result<void>::ok();
    }

    // 纯函数层：映射表 + 守卫表（跳过的键记在 skippedKeys）。
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

    // 逐字段比较：无变化 → 零写入；base_url 命中 loopback 守卫 → 保持原值不清
    const std::string newBaseUrl = isSkipped("ANTHROPIC_BASE_URL")
        ? prof.baseUrl
        : stripTrailingSlash(trim(parsed.cfg.baseUrl));
    const bool baseUrlChanged = (newBaseUrl != prof.baseUrl);

    // token：命中跳过族（托管键 / loopback 联动 / 占位 / 空值 / 非字符串或超长）
    // → 不写也不清（保持原密钥）
    const bool tokenSkipped = isSkipped("ANTHROPIC_AUTH_TOKEN") ||
                              isSkipped("ANTHROPIC_API_KEY");
    auto curKeyR = profileRepository_.loadApiKey(prof.id);
    const std::string curKey = curKeyR.isOk() ? curKeyR.value() : std::string();
    bool keyWrite = false;  // 出现 → 覆盖本行凭据列（经 updateProfile）
    bool keyClear = false;  // 缺席 → 清空（凭据三列一起清）
    if (!tokenSkipped) {
        if (!parsed.cfg.authToken.empty()) {
            keyWrite = (parsed.cfg.authToken != curKey);
        } else {
            keyClear = !curKey.empty();
        }
    }

    if (!baseUrlChanged && !merged.modelsChanged && !merged.extraEnvChanged &&
        !keyWrite && !keyClear) {
        return core::Result<void>::ok();  // 无变化：不 UPDATE、不碰凭据列
    }

    Profile upd = prof;
    upd.baseUrl = newBaseUrl;
    if (merged.modelsChanged) {
        upd.modelsConfigJson = merged.modelsConfigJson;
        // model 列显式跟随 default 槽位：updateProfile 内的同款跟随拿不到空值，清空落不了库。
        upd.model = effectiveModelsConfig(merged.modelsConfigJson).def.model;
    }
    if (merged.extraEnvChanged) {
        upd.extraEnvJson = merged.extraEnvJson;
    }

    // 两段写：凭据三列走助手，旧值由 capture 读回；任一落库段失败 → 按旧值回写
    // 三列，profile 回到改之前。
    TwoPhaseKeyWrite keyTx = TwoPhaseKeyWrite::capture(db_, prof.id);

    // key 缺席清空：先清凭据三列（同 deleteProfile 顺序），失败即中止、profile 未改。
    if (keyClear) {
        auto del = keyTx.writeKey(std::string());   // 空串 = 清空三列
        if (del.isFail()) {
            LOG_ERROR("反向同步清理凭据失败，中止本次切换（profile id=" +
                      std::to_string(prof.id) + "）: " + del.errorMessage());
            return core::Result<void>::fail(del.errorCode(),
                "清理该 Profile 的密钥失败，已中止本次切换: " + del.errorMessage());
        }
    }

    // 字段（+ key 写入）同属段二：updateProfile 内部也是「凭据三列 → 其余列」两段写，
    // 任一步失败由助手回写三列。keyClear 的三列清除已由上面 writeKey("") 完成。
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
            return core::Result<void>::ok();
        });
        if (w.isFail()) {
            std::string msg =
                "反向同步写库失败，已中止本次切换: " + w.errorMessage();
            // 提示「密钥可能不可用」只看凭据列实际状态，不信补偿返回值。
            if ((keyWrite || keyClear) &&
                !keyAtOldValue(profileRepository_, prof.id, curKey)) {
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
    // 编排：取激活 Profile + 端口传入 SettingsTakeOver；无激活仍接管 BASE_URL/占位
    // key（转发层报 503）。
    Profile active;
    auto a = getActiveProfile();
    if (a.isOk()) {
        active = std::move(a).value();
    }
    return settingsTakeover_.syncTakeOver(active, proxyPort());
}

void ClaudeService::syncRestore() {
    // 协议侧已返 Result<void>（失败并入 warnings_）；门面保持 void，失败 LOG_WARN。
    auto r = settingsTakeover_.syncRestore();
    if (r.isFail()) {
        LOG_WARN("Claude Code settings 还原失败: " + r.errorMessage());
    }
}

} // namespace module::claude
