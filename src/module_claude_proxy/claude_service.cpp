// src/module_claude_proxy/claude_service.cpp
// ClaudeService 实现：Profile CRUD + Keychain + 上游连通性测试
#include "claude_service.h"

#include "core/logging.h"
#include "platform/http_server.h"
#include "platform/keychain.h"
#include "proxy_router.h"

#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>

namespace module::claude {

namespace {

// Keychain service name（约定：DevDesk 全部密钥都用此 service）
constexpr const char* kKeychainService = "DevDesk";

// Keychain account 命名：DevDesk:claude:<profile_id>  ← 但 platform::keychain
// 接受 service+account 两个独立字符串，所以这里 account 用 "claude:<id>"
std::string keychainAccountFor(int64_t profileId) {
    return "claude:" + std::to_string(profileId);
}

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

} // namespace

// ---------- 静态工具 ----------

std::string ClaudeService::sha256HexPrefix(const std::string& s) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), digest);

    std::ostringstream oss;
    // 仅取前 8 个 hex 字符（4 字节）足够做防误操作校验
    for (int i = 0; i < 4 && i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(digest[i]);
    }
    return oss.str();
}

std::string ClaudeService::makeApiKeyHint(const std::string& apiKey) {
    if (apiKey.size() <= 10) {
        // 太短就全部用 *
        return std::string(apiKey.size(), '*');
    }
    std::string hint;
    hint.reserve(apiKey.size());
    hint.append(apiKey.substr(0, 6));
    hint.append("***");
    hint.append(apiKey.substr(apiKey.size() - 4));
    return hint;
}

// ---------- 构造 / 行解析 ----------

ClaudeService::ClaudeService(core::data::SqliteDb& db) : db_(db) {}

ClaudeService::~ClaudeService() {
    // 析构时确保停止代理（Server 析构会 join 线程）
    server_.reset();
    router_.reset();
}

// ---------- 全局单例 ----------

namespace {
    ClaudeService* g_globalClaudeService = nullptr;
}

void ClaudeService::setGlobal(ClaudeService* svc) { g_globalClaudeService = svc; }
ClaudeService* ClaudeService::global() { return g_globalClaudeService; }

core::Result<Profile> ClaudeService::rowToProfile(const core::data::SqlRow& row) const {
    Profile p;
    auto id = row.getInt64("id");
    if (id.isFail()) {
        return core::Result<Profile>::fail(id.errorCode(), id.errorMessage());
    }
    p.id = id.value();

    auto name = row.getString("name");
    auto url = row.getString("base_url");
    auto hint = row.getString("api_key_hint");
    auto check = row.getString("api_key_check");
    auto acct = row.getString("keychain_account");
    auto model = row.getString("model");
    auto active = row.getInt64("is_active");
    auto created = row.getInt64("created_at");
    auto updated = row.getInt64("updated_at");

    if (name.isOk()) p.name = name.value();
    if (url.isOk()) p.baseUrl = url.value();
    if (hint.isOk()) p.apiKeyHint = hint.value();
    if (check.isOk()) p.apiKeyCheck = check.value();
    if (acct.isOk()) p.keychainAccount = acct.value();
    if (model.isOk()) p.model = model.value();
    if (active.isOk()) p.isActive = (active.value() != 0);
    if (created.isOk()) p.createdAt = static_cast<std::time_t>(created.value());
    if (updated.isOk()) p.updatedAt = static_cast<std::time_t>(updated.value());

    p.apiKeyPlain.clear();   // 默认永远为空
    return core::Result<Profile>::ok(std::move(p));
}

// ---------- Profile CRUD ----------

core::Result<std::vector<Profile>> ClaudeService::listProfiles() {
    auto prep = db_.prepare(
        "SELECT id, name, base_url, api_key_hint, api_key_check, "
        "keychain_account, model, is_active, created_at, updated_at "
        "FROM claude_profiles ORDER BY is_active DESC, name ASC");
    if (prep.isFail()) {
        return core::Result<std::vector<Profile>>::fail(
            prep.errorCode(), prep.errorMessage());
    }

    auto rows = prep.value().fetchAll();
    if (rows.isFail()) {
        return core::Result<std::vector<Profile>>::fail(
            rows.errorCode(), rows.errorMessage());
    }

    std::vector<Profile> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto p = rowToProfile(row);
        if (p.isFail()) {
            return core::Result<std::vector<Profile>>::fail(
                p.errorCode(), p.errorMessage());
        }
        out.push_back(std::move(p).value());
    }
    return core::Result<std::vector<Profile>>::ok(std::move(out));
}

core::Result<Profile> ClaudeService::getProfile(int64_t id) {
    auto prep = db_.prepare(
        "SELECT id, name, base_url, api_key_hint, api_key_check, "
        "keychain_account, model, is_active, created_at, updated_at "
        "FROM claude_profiles WHERE id = ?");
    if (prep.isFail()) {
        return core::Result<Profile>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto b = stmt.bind(1, id); b.isFail()) {
        return core::Result<Profile>::fail(b.errorCode(), b.errorMessage());
    }
    auto stepR = stmt.step();
    if (stepR.isFail()) {
        return core::Result<Profile>::fail(stepR.errorCode(), stepR.errorMessage());
    }
    if (!stepR.value()) {
        return core::Result<Profile>::fail(core::ErrorCode::ProfileNotFound,
            "Profile not found: id=" + std::to_string(id));
    }

    auto row = stmt.currentRow();
    if (row.isFail()) {
        return core::Result<Profile>::fail(row.errorCode(), row.errorMessage());
    }
    return rowToProfile(row.value());
}

core::Result<Profile> ClaudeService::createProfile(const std::string& name,
                                                   const std::string& baseUrl,
                                                   const std::string& apiKey,
                                                   const std::string& model) {
    // 校验
    if (trim(name).empty()) {
        return core::Result<Profile>::fail(core::ErrorCode::ValidationError,
            "name 不能为空");
    }
    if (trim(baseUrl).empty()) {
        return core::Result<Profile>::fail(core::ErrorCode::ValidationError,
            "baseUrl 不能为空");
    }
    if (apiKey.empty()) {
        return core::Result<Profile>::fail(core::ErrorCode::InvalidApiKey,
            "apiKey 不能为空");
    }

    const std::time_t now = nowUnix();
    const std::string cleanedUrl = stripTrailingSlash(trim(baseUrl));
    const std::string cleanedName = trim(name);
    const std::string cleanedModel = trim(model);
    const std::string hint = makeApiKeyHint(apiKey);
    const std::string check = sha256HexPrefix(apiKey);

    // 先插入一行（keychain_account 临时用一个占位符）
    auto prep = db_.prepare(
        "INSERT INTO claude_profiles "
        "(name, base_url, api_key_hint, api_key_check, keychain_account, "
        " model, is_active, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, 0, ?, ?)");
    if (prep.isFail()) {
        return core::Result<Profile>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    stmt.bind(1, cleanedName);
    stmt.bind(2, cleanedUrl);
    stmt.bind(3, hint);
    stmt.bind(4, check);
    stmt.bind(5, std::string(""));   // 插入后用 UPDATE 回填真实 account
    stmt.bind(6, cleanedModel);
    stmt.bind(7, static_cast<int64_t>(now));
    stmt.bind(8, static_cast<int64_t>(now));

    auto stepR = stmt.step();
    if (stepR.isFail()) {
        // UNIQUE 约束冲突 → ProfileAlreadyExists
        std::string msg = stepR.errorMessage();
        if (msg.find("UNIQUE") != std::string::npos) {
            return core::Result<Profile>::fail(
                core::ErrorCode::ProfileAlreadyExists,
                "Profile name 已存在: " + cleanedName);
        }
        return core::Result<Profile>::fail(stepR.errorCode(), msg);
    }

    // 取新分配的 rowid
    auto idPrep = db_.prepare("SELECT last_insert_rowid()");
    if (idPrep.isFail()) {
        return core::Result<Profile>::fail(idPrep.errorCode(), idPrep.errorMessage());
    }
    auto idStep = idPrep.value().step();
    if (idStep.isFail() || !idStep.value()) {
        return core::Result<Profile>::fail(core::ErrorCode::Internal,
            "无法读取 last_insert_rowid");
    }
    auto idRow = idPrep.value().currentRow();
    if (idRow.isFail()) {
        return core::Result<Profile>::fail(idRow.errorCode(), idRow.errorMessage());
    }
    auto idVal = idRow.value().getInt64("last_insert_rowid()");
    if (idVal.isFail()) {
        return core::Result<Profile>::fail(idVal.errorCode(), idVal.errorMessage());
    }
    const int64_t newId = idVal.value();

    // 把 API key 入 Keychain
    const std::string account = keychainAccountFor(newId);
    auto kcStore = platform::keychain::storeSecret(kKeychainService, account, apiKey);
    if (kcStore.isFail()) {
        // Keychain 失败 → 回滚（删 SQLite 行）
        auto delPrep = db_.prepare("DELETE FROM claude_profiles WHERE id = ?");
        if (delPrep.isOk()) {
            delPrep.value().bind(1, newId);
            delPrep.value().step();
        }
        return core::Result<Profile>::fail(kcStore.errorCode(),
            "Keychain 存储失败: " + kcStore.errorMessage());
    }

    // 回填 keychain_account
    auto updPrep = db_.prepare(
        "UPDATE claude_profiles SET keychain_account = ? WHERE id = ?");
    if (updPrep.isFail()) {
        return core::Result<Profile>::fail(updPrep.errorCode(), updPrep.errorMessage());
    }
    auto& updStmt = updPrep.value();
    updStmt.bind(1, account);
    updStmt.bind(2, newId);
    auto uStep = updStmt.step();
    if (uStep.isFail()) {
        return core::Result<Profile>::fail(uStep.errorCode(), uStep.errorMessage());
    }

    LOG_INFO("已创建 Claude profile id=" + std::to_string(newId) +
             " name=" + cleanedName);

    // 返回完整 Profile
    Profile out;
    out.id = newId;
    out.name = cleanedName;
    out.baseUrl = cleanedUrl;
    out.apiKeyHint = hint;
    out.apiKeyCheck = check;
    out.keychainAccount = account;
    out.model = cleanedModel;
    out.isActive = false;
    out.createdAt = now;
    out.updatedAt = now;
    // apiKeyPlain 留空（不返回明文）
    return core::Result<Profile>::ok(std::move(out));
}

core::Result<void> ClaudeService::updateProfile(const Profile& profile) {
    if (profile.id == 0) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "profile.id 不能为 0");
    }
    if (trim(profile.name).empty()) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "name 不能为空");
    }

    // 查旧 profile（用于回滚 Keychain）
    auto old = getProfile(profile.id);
    if (old.isFail()) {
        return core::Result<void>::fail(old.errorCode(), old.errorMessage());
    }

    const std::string cleanedUrl = stripTrailingSlash(trim(profile.baseUrl));
    const std::string cleanedName = trim(profile.name);
    const std::string cleanedModel = trim(profile.model);
    const std::time_t now = nowUnix();

    // 1. 如果 apiKeyPlain 非空，覆盖 Keychain
    if (!profile.apiKeyPlain.empty()) {
        const std::string newCheck = sha256HexPrefix(profile.apiKeyPlain);
        const std::string newHint = makeApiKeyHint(profile.apiKeyPlain);
        auto kcStore = platform::keychain::storeSecret(
            kKeychainService, old.value().keychainAccount, profile.apiKeyPlain);
        if (kcStore.isFail()) {
            return core::Result<void>::fail(kcStore.errorCode(),
                "Keychain 更新失败: " + kcStore.errorMessage());
        }

        auto prep = db_.prepare(
            "UPDATE claude_profiles SET "
            "name = ?, base_url = ?, api_key_hint = ?, api_key_check = ?, "
            "model = ?, updated_at = ? WHERE id = ?");
        if (prep.isFail()) {
            return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
        }
        auto& stmt = prep.value();
        stmt.bind(1, cleanedName);
        stmt.bind(2, cleanedUrl);
        stmt.bind(3, newHint);
        stmt.bind(4, newCheck);
        stmt.bind(5, cleanedModel);
        stmt.bind(6, static_cast<int64_t>(now));
        stmt.bind(7, profile.id);
        auto stepR = stmt.step();
        if (stepR.isFail()) {
            std::string msg = stepR.errorMessage();
            if (msg.find("UNIQUE") != std::string::npos) {
                return core::Result<void>::fail(
                    core::ErrorCode::ProfileAlreadyExists,
                    "Profile name 已存在: " + cleanedName);
            }
            return core::Result<void>::fail(stepR.errorCode(), msg);
        }
    } else {
        // 只更新 name / url / model
        auto prep = db_.prepare(
            "UPDATE claude_profiles SET "
            "name = ?, base_url = ?, model = ?, updated_at = ? WHERE id = ?");
        if (prep.isFail()) {
            return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
        }
        auto& stmt = prep.value();
        stmt.bind(1, cleanedName);
        stmt.bind(2, cleanedUrl);
        stmt.bind(3, cleanedModel);
        stmt.bind(4, static_cast<int64_t>(now));
        stmt.bind(5, profile.id);
        auto stepR = stmt.step();
        if (stepR.isFail()) {
            std::string msg = stepR.errorMessage();
            if (msg.find("UNIQUE") != std::string::npos) {
                return core::Result<void>::fail(
                    core::ErrorCode::ProfileAlreadyExists,
                    "Profile name 已存在: " + cleanedName);
            }
            return core::Result<void>::fail(stepR.errorCode(), msg);
        }
    }

    LOG_INFO("已更新 Claude profile id=" + std::to_string(profile.id));
    return core::Result<void>::ok();
}

core::Result<void> ClaudeService::deleteProfile(int64_t id) {
    if (id == 0) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "id 不能为 0");
    }

    auto p = getProfile(id);
    if (p.isFail()) {
        return core::Result<void>::fail(p.errorCode(), p.errorMessage());
    }

    // 先删 SQLite 行（FK CASCADE 会清空 claude_call_logs）
    auto prep = db_.prepare("DELETE FROM claude_profiles WHERE id = ?");
    if (prep.isFail()) {
        return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    stmt.bind(1, id);
    auto stepR = stmt.step();
    if (stepR.isFail()) {
        return core::Result<void>::fail(stepR.errorCode(), stepR.errorMessage());
    }

    // 再清 Keychain（即使失败也不回滚 SQLite，Keychain 会泄漏一个孤儿条目
    // 但不影响功能；下次启动时可通过 keychain_check 检测清理）
    auto kcDel = platform::keychain::deleteSecret(
        kKeychainService, p.value().keychainAccount);
    if (kcDel.isFail()) {
        LOG_WARN("清理 Keychain 失败: " + kcDel.errorMessage());
    }

    LOG_INFO("已删除 Claude profile id=" + std::to_string(id));
    return core::Result<void>::ok();
}

// ---------- API key ----------

core::Result<std::string> ClaudeService::loadApiKey(int64_t profileId) {
    auto p = getProfile(profileId);
    if (p.isFail()) {
        return core::Result<std::string>::fail(p.errorCode(), p.errorMessage());
    }
    return platform::keychain::loadSecret(kKeychainService,
                                          p.value().keychainAccount);
}

// ---------- 激活 ----------

core::Result<Profile> ClaudeService::getActiveProfile() {
    auto prep = db_.prepare(
        "SELECT id, name, base_url, api_key_hint, api_key_check, "
        "keychain_account, model, is_active, created_at, updated_at "
        "FROM claude_profiles WHERE is_active = 1 LIMIT 1");
    if (prep.isFail()) {
        return core::Result<Profile>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto stepR = prep.value().step();
    if (stepR.isFail()) {
        return core::Result<Profile>::fail(stepR.errorCode(), stepR.errorMessage());
    }
    if (!stepR.value()) {
        return core::Result<Profile>::fail(core::ErrorCode::NoActiveProfile,
            "没有激活的 Claude profile");
    }
    auto row = prep.value().currentRow();
    if (row.isFail()) {
        return core::Result<Profile>::fail(row.errorCode(), row.errorMessage());
    }
    return rowToProfile(row.value());
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

    // 事务内：清零 → 设置目标
    return db_.transaction([this, id]() -> core::Result<void> {
        auto c = db_.prepare("UPDATE claude_profiles SET is_active = 0 "
                            "WHERE is_active = 1");
        if (c.isFail()) {
            return core::Result<void>::fail(c.errorCode(), c.errorMessage());
        }
        auto cStep = c.value().step();
        if (cStep.isFail()) {
            return core::Result<void>::fail(cStep.errorCode(), cStep.errorMessage());
        }

        auto s = db_.prepare("UPDATE claude_profiles SET is_active = 1, "
                            "updated_at = ? WHERE id = ?");
        if (s.isFail()) {
            return core::Result<void>::fail(s.errorCode(), s.errorMessage());
        }
        auto& stmt = s.value();
        stmt.bind(1, static_cast<int64_t>(nowUnix()));
        stmt.bind(2, id);
        auto sStep = stmt.step();
        if (sStep.isFail()) {
            return core::Result<void>::fail(sStep.errorCode(), sStep.errorMessage());
        }
        return core::Result<void>::ok();
    });
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
    req.headers["x-api-key"] = key;
    req.headers["anthropic-version"] = "2023-06-01";
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

// ---------- 调用日志 ----------

core::Result<void> ClaudeService::logCall(const CallLogEntry& entry) {
    auto prep = db_.prepare(
        "INSERT INTO claude_call_logs "
        "(profile_id, method, path, status_code, latency_ms, "
        " request_bytes, response_bytes, error, started_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (prep.isFail()) {
        return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    stmt.bind(1, entry.profileId);
    stmt.bind(2, entry.method);
    stmt.bind(3, entry.path);
    stmt.bind(4, static_cast<int64_t>(entry.statusCode));
    stmt.bind(5, entry.latencyMs);
    stmt.bind(6, entry.requestBytes);
    stmt.bind(7, entry.responseBytes);
    if (entry.error.empty()) {
        stmt.bind(8, nullptr);
    } else {
        stmt.bind(8, entry.error);
    }
    stmt.bind(9, static_cast<int64_t>(entry.startedAt == 0 ? nowUnix() : entry.startedAt));

    auto stepR = stmt.step();
    if (stepR.isFail()) {
        return core::Result<void>::fail(stepR.errorCode(), stepR.errorMessage());
    }
    return core::Result<void>::ok();
}

core::Result<std::vector<ClaudeService::CallLogEntry>>
ClaudeService::recentCalls(int limit) {
    if (limit <= 0) limit = 20;

    auto prep = db_.prepare(
        "SELECT id, profile_id, method, path, status_code, latency_ms, "
        "request_bytes, response_bytes, error, started_at "
        "FROM claude_call_logs ORDER BY started_at DESC, id DESC LIMIT ?");
    if (prep.isFail()) {
        return core::Result<std::vector<CallLogEntry>>::fail(
            prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    stmt.bind(1, static_cast<int64_t>(limit));

    auto rows = stmt.fetchAll();
    if (rows.isFail()) {
        return core::Result<std::vector<CallLogEntry>>::fail(
            rows.errorCode(), rows.errorMessage());
    }

    std::vector<CallLogEntry> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        CallLogEntry e;
        auto idV = row.getInt64("id");
        auto pidV = row.getInt64("profile_id");
        auto mV = row.getString("method");
        auto pV = row.getString("path");
        auto sV = row.getInt64("status_code");
        auto lV = row.getInt64("latency_ms");
        auto rbV = row.getInt64("request_bytes");
        auto rxbV = row.getInt64("response_bytes");
        auto errV = row.getString("error");
        auto saV = row.getInt64("started_at");
        if (idV.isOk()) e.id = idV.value();
        if (pidV.isOk()) e.profileId = pidV.value();
        if (mV.isOk()) e.method = mV.value();
        if (pV.isOk()) e.path = pV.value();
        if (sV.isOk()) e.statusCode = static_cast<int>(sV.value());
        if (lV.isOk()) e.latencyMs = lV.value();
        if (rbV.isOk()) e.requestBytes = rbV.value();
        if (rxbV.isOk()) e.responseBytes = rxbV.value();
        if (errV.isOk()) e.error = errV.value();
        if (saV.isOk()) e.startedAt = static_cast<std::time_t>(saV.value());
        out.push_back(std::move(e));
    }
    return core::Result<std::vector<CallLogEntry>>::ok(std::move(out));
}

// ---------- 代理 server 生命周期 ----------

std::string ClaudeService::generateAuthToken() {
    // 32 hex chars (16 bytes) from std::random_device
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    const uint64_t a = dist(gen);
    const uint64_t b = dist(gen);

    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << a
        << std::setw(16) << std::setfill('0') << b;
    return oss.str();
}

core::Result<int> ClaudeService::startProxy(const std::string& bindAddress,
                                            int port,
                                            const std::string& authToken) {
    if (server_) {
        return core::Result<int>::fail(core::ErrorCode::AlreadyExists,
            "代理 server 已在运行（先 stopProxy）");
    }

    // 决定实际 token：未提供就生成一个
    const std::string token = authToken.empty() ? generateAuthToken() : authToken;

    // 构造 Server
    server_ = std::make_unique<platform::http::Server>(bindAddress, port, token);

    // 构造并注册路由
    router_ = std::make_unique<ProxyRouter>(*this, *server_);
    router_->registerRoutes();

    if (!server_->start()) {
        const int bound = server_->actualPort();
        server_.reset();
        router_.reset();
        return core::Result<int>::fail(core::ErrorCode::Internal,
            "代理 server 启动失败 (port=" + std::to_string(bound) + ")");
    }

    const int actual = server_->actualPort();
    LOG_INFO("Claude 代理已启动 " + bindAddress + ":" + std::to_string(actual) +
             " (auth=" + (token.empty() ? std::string("off") : std::string("on")) + ")");
    return core::Result<int>::ok(actual);
}

void ClaudeService::stopProxy() {
    if (!server_) return;
    LOG_INFO("停止 Claude 代理");
    server_->stop();
    server_.reset();
    router_.reset();
}

bool ClaudeService::isProxyRunning() const {
    return server_ && server_->isRunning();
}

int ClaudeService::proxyPort() const {
    return server_ ? server_->actualPort() : 0;
}

std::string ClaudeService::proxyAuthToken() const {
    return server_ ? server_->authToken() : std::string();
}

} // namespace module::claude
