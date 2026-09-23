// src/module_claude_proxy/claude_service.cpp
// ClaudeService 实现：Profile CRUD + Keychain + 上游连通性测试
#include "claude_service.h"

#include "core/logging.h"
#include "core/paths.h"
#include "core/time.h"
#include "platform/http_server.h"
#include "platform/keychain.h"
#include "provider_config.h"
#include "proxy_router.h"

#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <utility>

namespace module::claude {

namespace {

// 默认 Keychain service name（约定：DevDesk 全部密钥都用此 service）
constexpr const char* kDefaultKeychainService = "DevDesk";

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

ClaudeService::ClaudeService(core::data::SqliteDb& db,
                             const std::string& keychainService)
    : db_(db),
      keychainService_(keychainService.empty() ? kDefaultKeychainService
                                               : keychainService) {}

ClaudeService::~ClaudeService() {
    // 析构时确保停止代理（Server 析构会 join 线程）。审计 2026-09-23 [9]：
    // 与 stopProxy 同款顺序——先取消在途流，否则 ~Server 的 join 被阻塞在
    // readChunk 的 worker 卡住。
    cancelActiveStreams();
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
    auto modelsCfg = row.getString("models_config");
    auto extraEnv = row.getString("extra_env");
    auto active = row.getInt64("is_active");
    auto created = row.getInt64("created_at");
    auto updated = row.getInt64("updated_at");

    if (name.isOk()) p.name = name.value();
    if (url.isOk()) p.baseUrl = url.value();
    if (hint.isOk()) p.apiKeyHint = hint.value();
    if (check.isOk()) p.apiKeyCheck = check.value();
    if (acct.isOk()) p.keychainAccount = acct.value();
    if (model.isOk()) p.model = model.value();
    if (modelsCfg.isOk()) p.modelsConfigJson = modelsCfg.value();
    if (extraEnv.isOk()) p.extraEnvJson = extraEnv.value();
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
        "keychain_account, model, models_config, extra_env, "
        "is_active, created_at, updated_at "
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
        "keychain_account, model, models_config, extra_env, "
        "is_active, created_at, updated_at "
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

    // 先插入一行并直接取回 rowid（审计 2026-09-23 [7]）：旧实现是
    // INSERT 与 SELECT last_insert_rowid() 两条语句，中间不持锁——代理
    // worker 线程的 logCall INSERT 穿插其间会改写连接级 last_insert_rowid，
    // 导致 newId 取错：密钥存进错误 account、UPDATE 回填落空（profile 损坏
    // + 孤儿密钥）。INSERT ... RETURNING id（SQLite ≥3.35）单语句原子取回，
    // rowid 随本条 INSERT 的结果行返回，不再依赖连接级状态。
    // （keychain_account 仍先占位空串、storeSecret 成功后 UPDATE 回填，
    // Keychain 失败时按 newId 精确 DELETE 回滚。）
    auto prep = db_.prepare(
        "INSERT INTO claude_profiles "
        "(name, base_url, api_key_hint, api_key_check, keychain_account, "
        " model, is_active, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, 0, ?, ?) RETURNING id");
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
    if (!stepR.value()) {
        // RETURNING 理论上必返回一行；防御式处理（DONE 意味着异常路径）
        return core::Result<Profile>::fail(core::ErrorCode::Internal,
            "INSERT ... RETURNING id 未返回行");
    }

    // 取新分配的 rowid（本语句结果行，无跨语句竞态）
    auto idRow = stmt.currentRow();
    if (idRow.isFail()) {
        return core::Result<Profile>::fail(idRow.errorCode(), idRow.errorMessage());
    }
    auto idVal = idRow.value().getInt64("id");
    if (idVal.isFail()) {
        return core::Result<Profile>::fail(idVal.errorCode(), idVal.errorMessage());
    }
    const int64_t newId = idVal.value();

    // 把 API key 入 Keychain
    const std::string account = keychainAccountFor(newId);
    auto kcStore = platform::keychain::storeSecret(keychainService_, account, apiKey);
    if (kcStore.isFail()) {
        // Keychain 失败 → 回滚（删 SQLite 行）。回滚本身再失败会留下无密钥的
        // 幽灵 profile 行，至少留 ERROR 痕迹可诊断（审计 2026-09-23 [25]）
        auto delPrep = db_.prepare("DELETE FROM claude_profiles WHERE id = ?");
        if (delPrep.isOk()) {
            auto& delStmt = delPrep.value();
            delStmt.bind(1, newId);
            auto delStep = delStmt.step();
            if (delStep.isFail()) {
                LOG_ERROR("Keychain 失败后的回滚 DELETE 也失败，存在悬空 "
                          "profile 行 id=" + std::to_string(newId) + ": " +
                          delStep.errorMessage());
            }
        } else {
            LOG_ERROR("Keychain 失败后的回滚 DELETE prepare 失败，存在悬空 "
                      "profile 行 id=" + std::to_string(newId) + ": " +
                      delPrep.errorMessage());
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
    const std::time_t now = nowUnix();

    // 写入前校验两个 JSON 原文（信任边界：不把坏 JSON 落库；读侧另有容错）
    if (!profile.modelsConfigJson.empty()) {
        auto mc = parseModelsConfig(profile.modelsConfigJson);
        if (mc.isFail()) {
            return core::Result<void>::fail(mc.errorCode(),
                "models_config 非法: " + mc.errorMessage());
        }
    }
    if (!profile.extraEnvJson.empty()) {
        auto ee = parseExtraEnv(profile.extraEnvJson);
        if (ee.isFail()) {
            return core::Result<void>::fail(ee.errorCode(),
                "extra_env 非法: " + ee.errorMessage());
        }
    }

    // 旧 model 列与 default 槽位保持一致（provider-config §4.2：
    // 双向同步两字段成本低于迁移存量数据）
    std::string cleanedModel = trim(profile.model);
    if (!profile.modelsConfigJson.empty()) {
        auto mc = parseModelsConfig(profile.modelsConfigJson);
        if (mc.isOk() && !mc.value().def.model.empty()) {
            cleanedModel = mc.value().def.model;
        }
    }

    // 1. 如果 apiKeyPlain 非空，覆盖 Keychain
    if (!profile.apiKeyPlain.empty()) {
        const std::string newCheck = sha256HexPrefix(profile.apiKeyPlain);
        const std::string newHint = makeApiKeyHint(profile.apiKeyPlain);
        auto kcStore = platform::keychain::storeSecret(
            keychainService_, old.value().keychainAccount, profile.apiKeyPlain);
        if (kcStore.isFail()) {
            return core::Result<void>::fail(kcStore.errorCode(),
                "Keychain 更新失败: " + kcStore.errorMessage());
        }

        auto prep = db_.prepare(
            "UPDATE claude_profiles SET "
            "name = ?, base_url = ?, api_key_hint = ?, api_key_check = ?, "
            "model = ?, models_config = ?, extra_env = ?, updated_at = ? "
            "WHERE id = ?");
        if (prep.isFail()) {
            return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
        }
        auto& stmt = prep.value();
        stmt.bind(1, cleanedName);
        stmt.bind(2, cleanedUrl);
        stmt.bind(3, newHint);
        stmt.bind(4, newCheck);
        stmt.bind(5, cleanedModel);
        stmt.bind(6, profile.modelsConfigJson);
        stmt.bind(7, profile.extraEnvJson);
        stmt.bind(8, static_cast<int64_t>(now));
        stmt.bind(9, profile.id);
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
            "name = ?, base_url = ?, model = ?, models_config = ?, "
            "extra_env = ?, updated_at = ? WHERE id = ?");
        if (prep.isFail()) {
            return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
        }
        auto& stmt = prep.value();
        stmt.bind(1, cleanedName);
        stmt.bind(2, cleanedUrl);
        stmt.bind(3, cleanedModel);
        stmt.bind(4, profile.modelsConfigJson);
        stmt.bind(5, profile.extraEnvJson);
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
    }

    LOG_INFO("已更新 Claude profile id=" + std::to_string(profile.id));
    return core::Result<void>::ok();
}

core::Result<void> ClaudeService::deleteProfile(int64_t id, bool* proxyStopped) {
    if (proxyStopped) *proxyStopped = false;
    if (id == 0) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "id 不能为 0");
    }

    auto p = getProfile(id);
    if (p.isFail()) {
        return core::Result<void>::fail(p.errorCode(), p.errorMessage());
    }
    const bool wasActive = p.value().isActive;

    // 先清 Keychain，失败则中止删除并返回错误（SQLite 行保留，用户可重试，
    // API key 不丢）。顺序不能反：先删行会让 Keychain 失败时留下永久孤儿密钥。
    auto kcDel = platform::keychain::deleteSecret(
        keychainService_, p.value().keychainAccount);
    if (kcDel.isFail()) {
        LOG_ERROR("清理 Keychain 失败，中止删除 profile id=" +
                  std::to_string(id) + ": " + kcDel.errorMessage());
        return core::Result<void>::fail(
            kcDel.errorCode(),
            "清理 Keychain 失败，profile 未删除: " + kcDel.errorMessage());
    }

    // 再删 SQLite 行（FK CASCADE 会清空 claude_call_logs）
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

    LOG_INFO("已删除 Claude profile id=" + std::to_string(id));

    // 产品审计 2026-09-23 [1]（provider-config 设计 §7 安全第 5 条）：
    // 删的是激活 profile 且代理运行中 → 停代理（stopProxy 内部 syncRestore
    // 还原 ~/.claude/settings.json），settings.json 不能继续指向无激活
    // profile 的代理（否则 Claude Code 此后每个请求 503 no_active_profile）。
    // UI 经 proxyStopped 出参获知并 toast 告知用户。
    if (wasActive && isProxyRunning()) {
        LOG_INFO("删除的是激活 profile 且代理运行中 → 停代理并还原 Claude Code 配置");
        stopProxy();
        if (proxyStopped) *proxyStopped = true;
    }
    return core::Result<void>::ok();
}

core::Result<int> ClaudeService::cleanupOrphanKeychainEntries() {
    // 一次扫描：keychainService_ 命名空间归 Claude profile 独占，
    // 不在 claude_profiles.keychain_account 里的条目即孤儿密钥
    auto profilesR = listProfiles();
    if (profilesR.isFail()) {
        return core::Result<int>::fail(profilesR.errorCode(),
                                       profilesR.errorMessage());
    }
    std::vector<std::string> keep;
    keep.reserve(profilesR.value().size());
    for (const auto& prof : profilesR.value()) {
        keep.push_back(prof.keychainAccount);
    }

    auto delR = platform::keychain::deleteSecretsNotIn(keychainService_, keep);
    if (delR.isFail()) {
        return core::Result<int>::fail(delR.errorCode(), delR.errorMessage());
    }
    const int removed = static_cast<int>(delR.value().size());
    if (removed > 0) {
        LOG_INFO("已清理 " + std::to_string(removed) +
                 " 个孤儿 Keychain 密钥（无对应 profile）");
    }
    return core::Result<int>::ok(removed);
}

// ---------- API key ----------

core::Result<std::string> ClaudeService::loadApiKey(int64_t profileId) {
    auto p = getProfile(profileId);
    if (p.isFail()) {
        return core::Result<std::string>::fail(p.errorCode(), p.errorMessage());
    }
    return platform::keychain::loadSecret(keychainService_,
                                          p.value().keychainAccount);
}

// ---------- 激活 ----------

core::Result<Profile> ClaudeService::getActiveProfile() {
    auto prep = db_.prepare(
        "SELECT id, name, base_url, api_key_hint, api_key_check, "
        "keychain_account, model, models_config, extra_env, "
        "is_active, created_at, updated_at "
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
    auto result = db_.transaction([this, id]() -> core::Result<void> {
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

    // 代理运行中切 profile → settings.json env 跟随（§5.2 触发点 2）；
    // 代理未运行 → 只更新 DB，不动 settings.json（写了反而制造死指向）
    if (result.isOk() && isProxyRunning()) {
        syncTakeOver();
    }
    return result;
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
    CallLogQuery q;
    q.limit = limit;
    return queryCalls(q);
}

namespace {

// LIKE 模式转义（审计 [11]）：用户输入的 % / _ / \ 按字面量匹配
// （SQL 侧统一 ESCAPE '\'，SQLite 的 LIKE 对 ASCII 默认大小写不敏感）
std::string escapeLikePattern(const std::string& term) {
    std::string out;
    out.reserve(term.size() + 8);
    for (const char c : term) {
        if (c == '\\' || c == '%' || c == '_') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// queryCalls / countCalls 共用 WHERE 片段（无命中返回空串 → 调用方省略 WHERE）。
// status_code 列可 NULL（早期直插行），COALESCE 归 0 → 落「错误」桶
std::string buildCallLogWhere(const ClaudeService::CallLogQuery& q) {
    std::string where;
    if (!q.search.empty()) {
        where = "(path LIKE ? ESCAPE '\\' OR method LIKE ? ESCAPE '\\')";
    }
    std::string status;
    switch (q.statusClass) {
        case ClaudeService::CallLogQuery::kStatus2xx:
            status = "COALESCE(status_code, 0) BETWEEN 200 AND 299";
            break;
        case ClaudeService::CallLogQuery::kStatus3xx:
            status = "COALESCE(status_code, 0) BETWEEN 300 AND 399";
            break;
        case ClaudeService::CallLogQuery::kStatus4xx:
            status = "COALESCE(status_code, 0) BETWEEN 400 AND 499";
            break;
        case ClaudeService::CallLogQuery::kStatus5xx:
            status = "COALESCE(status_code, 0) >= 500";
            break;
        case ClaudeService::CallLogQuery::kStatusErr:
            status = "COALESCE(status_code, 0) < 200";
            break;
        default:
            break;  // kStatusAll：不加状态条件
    }
    if (!status.empty()) {
        where = where.empty() ? status : (where + " AND " + status);
    }
    return where;
}

ClaudeService::CallLogEntry callLogFromRow(const core::data::SqlRow& row) {
    ClaudeService::CallLogEntry e;
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
    return e;
}

} // namespace

core::Result<std::vector<ClaudeService::CallLogEntry>>
ClaudeService::queryCalls(const CallLogQuery& q) {
    const int limit = q.limit > 0 ? q.limit : 20;
    const int offset = q.offset > 0 ? q.offset : 0;

    std::string sql =
        "SELECT id, profile_id, method, path, status_code, latency_ms, "
        "request_bytes, response_bytes, error, started_at "
        "FROM claude_call_logs";
    const std::string where = buildCallLogWhere(q);
    if (!where.empty()) sql += " WHERE " + where;
    sql += " ORDER BY started_at DESC, id DESC LIMIT ? OFFSET ?";

    auto prep = db_.prepare(sql);
    if (prep.isFail()) {
        return core::Result<std::vector<CallLogEntry>>::fail(
            prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    int bindIdx = 1;
    if (!q.search.empty()) {
        const std::string like = "%" + escapeLikePattern(q.search) + "%";
        stmt.bind(bindIdx++, like);
        stmt.bind(bindIdx++, like);
    }
    stmt.bind(bindIdx++, static_cast<int64_t>(limit));
    stmt.bind(bindIdx++, static_cast<int64_t>(offset));

    auto rows = stmt.fetchAll();
    if (rows.isFail()) {
        return core::Result<std::vector<CallLogEntry>>::fail(
            rows.errorCode(), rows.errorMessage());
    }

    std::vector<CallLogEntry> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        out.push_back(callLogFromRow(row));
    }
    return core::Result<std::vector<CallLogEntry>>::ok(std::move(out));
}

core::Result<int> ClaudeService::countCalls(const CallLogQuery& q) {
    std::string sql = "SELECT COUNT(*) AS n FROM claude_call_logs";
    const std::string where = buildCallLogWhere(q);
    if (!where.empty()) sql += " WHERE " + where;

    auto prep = db_.prepare(sql);
    if (prep.isFail()) {
        return core::Result<int>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (!q.search.empty()) {
        const std::string like = "%" + escapeLikePattern(q.search) + "%";
        stmt.bind(1, like);
        stmt.bind(2, like);
    }

    auto rows = stmt.fetchAll();
    if (rows.isFail()) {
        return core::Result<int>::fail(rows.errorCode(), rows.errorMessage());
    }
    if (rows.value().empty()) return core::Result<int>::ok(0);
    auto nV = rows.value()[0].getInt64("n");
    if (nV.isFail()) {
        return core::Result<int>::fail(nV.errorCode(), nV.errorMessage());
    }
    return core::Result<int>::ok(static_cast<int>(nV.value()));
}

// ---------- 代理 server 生命周期 ----------

std::string ClaudeService::generateAuthToken() {
    // 32 hex chars (16 bytes) —— 直接从 std::random_device 逐 32 位采样
    // 4 次填满 128 位（审计 2026-09-23 [10]）。旧实现用单次 random_device()
    // 采样给 mt19937_64 播种，之后两次的 64 位输出全由那一个 ≤2^32 取值
    // 的种子决定——token 看着 128 位、实际熵 ≤32 位，loopback 暴力可枚举。
    // Windows 上 std::random_device 由 BCryptGenRandom 驱动，非确定。
    // （失败不用 Result：random_device 构造/采样失败抛 std::runtime_error，
    // 项目约定不抛异常——MinGW/GCC 的实现同样基于系统熵源，启动失败时
    // 与旧实现行为一致，属于环境级故障。）
    std::random_device rd;
    std::uint32_t words[4];
    for (auto& w : words) w = rd();

    std::ostringstream oss;
    for (const auto w : words) {
        oss << std::hex << std::setw(8) << std::setfill('0') << w;
    }
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

    // 上一轮 stopProxy 置位的停止标志清零（否则 startProxy 后新登记到
    // router 的流一进来就被误取消）
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        streamsStopping_ = false;
        activeStreams_.clear();
    }

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

    // 接管 Claude Code settings.json（§5.2 触发点 1；含手动启动、设置页
    // 重生成 token 的重启）。失败只 WARN：代理已可用，接入失败用户可感知
    syncTakeOver();
    return core::Result<int>::ok(actual);
}

void ClaudeService::stopProxy() {
    if (!server_) return;
    LOG_INFO("停止 Claude 代理");
    // 审计 2026-09-23 [9]：先取消在途流式上游，再停 server。server_->stop()
    // 内部要 join httplib worker，而在途流的 worker 阻塞在 provider 的
    // readChunk 上——不取消就要等流自然结束（活动生成期无空闲超时，
    // UI「停止代理」按钮/退出流程会挂起，进程变幽灵）。
    cancelActiveStreams();
    server_->stop();
    server_.reset();
    router_.reset();
    // 停止即还原（§5.3）：settings.json 永不指向死代理
    syncRestore();
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

// ---------- 在途流式上游登记（审计 2026-09-23 [9]）----------

void ClaudeService::registerActiveStream(
    const std::shared_ptr<platform::http::StreamingResponse>& stream) {
    if (!stream) return;
    std::lock_guard<std::mutex> lock(streamsMutex_);
    // 顺手清掉已结束的条目（weak_ptr 过期 = provider/releaser 已放手），
    // 防止长期运行下 vector 单调增长
    std::erase_if(activeStreams_,
                  [](const std::weak_ptr<platform::http::StreamingResponse>& w) {
                      return w.expired();
                  });
    activeStreams_.emplace_back(stream);
    // stopProxy 已发起（cancelActiveStreams 跑完后 handler 才登记进来）：
    // 立即取消，消掉「取消与登记赛跑」的窗口，handler 快速走 Error 路径
    if (streamsStopping_) {
        stream->cancelWithReason("proxy stopping");
    }
}

void ClaudeService::cancelActiveStreams() {
    std::vector<std::shared_ptr<platform::http::StreamingResponse>> live;
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        streamsStopping_ = true;
        live.reserve(activeStreams_.size());
        for (auto& w : activeStreams_) {
            if (auto sp = w.lock()) live.push_back(std::move(sp));
        }
        activeStreams_.clear();
    }
    // 锁外逐个 cancel：只置标志 + notify_all（持 queueMutex），阻塞在
    // readChunk 的 worker 线程立即拿到 Error/"proxy stopping" 退出 provider
    // 循环 → httplib worker 收工 → Server::stop() 的 join 不再等流结束
    for (auto& sp : live) {
        sp->cancelWithReason("proxy stopping");
    }
}

// ---------- Claude Code settings 接管（Phase C）----------

void ClaudeService::setSettingsSyncPaths(const settings_sync::Paths& paths) {
    syncPaths_ = paths;
    lastSyncWarnings_.clear();
}

bool ClaudeService::isClaudeCodeManaged() const {
    return !syncPaths_.settingsFile.empty() &&
           settings_sync::hasPendingSnapshot(syncPaths_);
}

const std::vector<std::string>& ClaudeService::lastSyncWarnings() const {
    return lastSyncWarnings_;
}

void ClaudeService::recoverPendingSettingsRestore() {
    if (syncPaths_.settingsFile.empty()) return;
    auto r = settings_sync::recoverPendingSnapshot(syncPaths_);
    if (!r.ok) {
        LOG_WARN("Claude Code 接管崩溃补还原失败: " + r.error);
    }
}

void ClaudeService::syncTakeOver() {
    if (syncPaths_.settingsFile.empty()) return;
    // 无激活 profile：仍接管 BASE_URL/token（接入路径先就位，转发层会报
    // 503 提示用户先启用 profile）
    Profile active;
    auto a = getActiveProfile();
    if (a.isOk()) {
        active = std::move(a).value();
    }
    auto r = settings_sync::takeOver(active, proxyPort(), proxyAuthToken(),
                                     syncPaths_);
    lastSyncWarnings_ = std::move(r.warnings);
    if (!r.ok) {
        LOG_WARN("Claude Code settings 接管失败: " + r.error);
        return;
    }
    LOG_INFO("Claude Code settings 已接管: " +
             syncPaths_.settingsFile.string() +
             (lastSyncWarnings_.empty()
                  ? std::string("")
                  : "（含 " + std::to_string(lastSyncWarnings_.size()) +
                        " 条上层覆盖警告）"));
}

void ClaudeService::syncRestore() {
    if (syncPaths_.settingsFile.empty()) return;
    if (!settings_sync::hasPendingSnapshot(syncPaths_)) return;
    auto r = settings_sync::restore(syncPaths_);
    if (!r.ok) {
        LOG_WARN("Claude Code settings 还原失败: " + r.error);
        return;
    }
    lastSyncWarnings_.clear();
    LOG_INFO("Claude Code settings 已还原（settings.json 不再指向本地代理）");
}

// ---------- 代理凭据文件（三条启动路径共用）----------

void writeProxyTokenFile(const std::string& token, int port) {
    const auto path = core::paths::appDataDir() / "proxy_token.txt";
    std::ofstream f(path, std::ios::trunc);
    if (!f) {
        LOG_WARN("无法写入 proxy_token.txt: " + path.string());
        return;
    }
    f << "# DevDesk Claude 代理凭据（自动生成，请勿提交到版本控制）\n"
      << "port=" << port << "\n"
      << "token=" << token << "\n"
      << "\n"
      << "# 用法：\n"
      << "# curl -X POST http://127.0.0.1:" << port << "/v1/messages \\\n"
      << "#   -H \"Authorization: Bearer " << token << "\" \\\n"
      << "#   -H \"Content-Type: application/json\" \\\n"
      << "#   -d '{\"model\":\"claude-sonnet-4-5\",\"max_tokens\":64,"
         "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}'\n";
    f.close();
    LOG_INFO("代理凭据已写入: " + path.string());
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
