// ProfileRepository 实现：策略尾（停代理 / 接管 / 激活时机）留在门面，本类只做 DB 段
#include "profile_repository.h"

#include "core/app_config.h"
#include "core/logging.h"
#include "core/time.h"
#include "platform/credential_protection.h"
#include "platform/keychain.h"
#include "provider_config.h"

#include <openssl/sha.h>

#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace module::claude {

namespace {

// 取当前 unix 秒
using core::time::nowUnix;

std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string stripTrailingSlash(const std::string& s) {
    if (!s.empty() && s.back() == '/') return s.substr(0, s.size() - 1);
    return s;
}

// 全量 SHA256 hex（「密钥是否变更」判据）
std::string sha256Hex(const std::string& s) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), digest);

    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(digest[i]);
    }
    return oss.str();
}

// UI / 导出用的脱敏形态：前 6 + "***" + 尾 4（太短则全 *）；导出链只许当占位值，绝不用明文
std::string makeApiKeyHint(const std::string& apiKey) {
    if (apiKey.size() <= 10) {
        return std::string(apiKey.size(), '*');
    }
    std::string hint;
    hint.reserve(apiKey.size());
    hint.append(apiKey.substr(0, 6));
    hint.append("***");
    hint.append(apiKey.substr(apiKey.size() - 4));
    return hint;
}

// 凭据三列的唯一写口（段一 / 补偿共用；空 stored = 清空三列）；hint / check 由调用方传入，保证三列同批更新不漂移
core::Result<void> writeCredentialColumns(core::data::SqliteDb& db,
                                          int64_t profileId,
                                          const std::string& stored,
                                          const std::string& hint,
                                          const std::string& check) {
    auto prep = db.prepare(
        "UPDATE claude_profiles SET api_key_encrypted = ?, api_key_hint = ?, "
        "api_key_check = ?, updated_at = ? WHERE id = ?");
    if (prep.isFail()) {
        return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    stmt.bind(1, stored);
    stmt.bind(2, hint);
    stmt.bind(3, check);
    stmt.bind(4, static_cast<int64_t>(nowUnix()));
    stmt.bind(5, profileId);
    auto stepR = stmt.step();
    if (stepR.isFail()) {
        return core::Result<void>::fail(stepR.errorCode(), stepR.errorMessage());
    }
    return core::Result<void>::ok();
}

// 读该行的存储形态（不存在 / 读失败 → 空串 = 没有密钥）
std::string readStoredCredential(core::data::SqliteDb& db, int64_t profileId) {
    auto prep = db.prepare(
        "SELECT api_key_encrypted FROM claude_profiles WHERE id = ?");
    if (prep.isFail()) return std::string();
    auto& stmt = prep.value();
    if (stmt.bind(1, profileId).isFail()) return std::string();
    auto stepR = stmt.step();
    if (stepR.isFail() || !stepR.value()) return std::string();
    auto row = stmt.currentRow();
    if (row.isFail()) return std::string();
    auto v = row.value().getString("api_key_encrypted");
    return v.isOk() ? v.value() : std::string();
}

} // namespace

// 两段写助手

TwoPhaseKeyWrite::TwoPhaseKeyWrite(core::data::SqliteDb& db, int64_t profileId,
                                   std::string oldStored, std::string oldPlain)
    : db_(db),
      profileId_(profileId),
      oldStored_(std::move(oldStored)),
      oldPlain_(std::move(oldPlain)) {}

TwoPhaseKeyWrite TwoPhaseKeyWrite::capture(core::data::SqliteDb& db,
                                           int64_t profileId) {
    std::string stored = readStoredCredential(db, profileId);
    std::string plain;
    // 明文只为「密钥是否已回到旧值」的诊断比较而留；解不开就留空（回写走原样 stored 字节，不需要明文）
    if (!stored.empty()) {
        auto p = platform::credential::unprotect(stored);
        if (p.isOk()) plain = p.value();
    }
    return TwoPhaseKeyWrite(db, profileId, std::move(stored), std::move(plain));
}

core::Result<void> TwoPhaseKeyWrite::writeKey(const std::string& newKey) {
    std::string stored;
    std::string hint;
    std::string check;
    if (!newKey.empty()) {
        auto prot = platform::credential::protect(newKey);
        if (prot.isFail()) {
            // 段一在落任何字节之前就失败：旧值完好，无需补偿
            return core::Result<void>::fail(prot.errorCode(),
                                            prot.errorMessage());
        }
        stored = prot.value();
        hint = makeApiKeyHint(newKey);
        check = sha256Hex(newKey);
    }

    auto r = writeCredentialColumns(db_, profileId_, stored, hint, check);
    if (r.isFail()) {
        // 段一落库失败也可能已动过旧值 → 尽力按存储形态原样字节回写
        keyTouched_ = true;
        compensate();
        return r;
    }
    keyTouched_ = true;
    return r;
}

core::Result<void> TwoPhaseKeyWrite::writeDb(
    const std::function<core::Result<void>()>& dbWrite) {
    auto r = dbWrite();
    if (r.isFail()) {
        compensate();   // 段二失败 → 凭据三列回到旧值
    }
    return r;
}

void TwoPhaseKeyWrite::compensate() {
    if (!keyTouched_) return;   // 没改过（或已回写）→ 无需动作
    const std::string hint = oldPlain_.empty() ? std::string()
                                              : makeApiKeyHint(oldPlain_);
    const std::string check = oldPlain_.empty() ? std::string()
                                               : sha256Hex(oldPlain_);
    auto r = writeCredentialColumns(db_, profileId_, oldStored_, hint, check);
    if (r.isFail()) {
        // 保留 keyTouched_：三列还没回到旧值（密钥可能不可用）
        LOG_ERROR("两段写补偿：回写凭据列旧值失败（profile id=" +
                  std::to_string(profileId_) + "）: " + r.errorMessage());
        return;
    }
    keyTouched_ = false;
}

ProfileRepository::ProfileRepository(core::data::SqliteDb& db,
                                     std::string keychainService)
    : db_(db), keychainService_(std::move(keychainService)) {}

core::Result<Profile> ProfileRepository::rowToProfile(
    const core::data::SqlRow& row) const {
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
    auto model = row.getString("model");
    auto modelsCfg = row.getString("models_config");
    auto extraEnv = row.getString("extra_env");
    auto authField = row.getString("auth_field");
    auto active = row.getInt64("is_active");
    auto created = row.getInt64("created_at");
    auto updated = row.getInt64("updated_at");

    if (name.isOk()) p.name = name.value();
    if (url.isOk()) p.baseUrl = url.value();
    if (hint.isOk()) p.apiKeyHint = hint.value();
    if (check.isOk()) p.apiKeyCheck = check.value();
    if (model.isOk()) p.model = model.value();
    if (modelsCfg.isOk()) p.modelsConfigJson = modelsCfg.value();
    if (extraEnv.isOk()) p.extraEnvJson = extraEnv.value();
    // 读侧规范化：未知/空值一律兜底 both
    if (authField.isOk()) {
        p.authField = authFieldToText(authFieldFromText(authField.value()));
    }
    if (active.isOk()) p.isActive = (active.value() != 0);
    if (created.isOk()) p.createdAt = static_cast<std::time_t>(created.value());
    if (updated.isOk()) p.updatedAt = static_cast<std::time_t>(updated.value());

    p.apiKeyPlain.clear();
    return core::Result<Profile>::ok(std::move(p));
}

// Profile CRUD

core::Result<std::vector<Profile>> ProfileRepository::listProfiles() {
    auto prep = db_.prepare(
        "SELECT id, name, base_url, api_key_hint, api_key_check, "
        "model, models_config, extra_env, "
        "auth_field, is_active, "
        "created_at, updated_at "
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

core::Result<Profile> ProfileRepository::getProfile(int64_t id) {
    auto prep = db_.prepare(
        "SELECT id, name, base_url, api_key_hint, api_key_check, "
        "model, models_config, extra_env, "
        "auth_field, is_active, "
        "created_at, updated_at "
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

core::Result<Profile> ProfileRepository::createProfile(const std::string& name,
                                                       const std::string& baseUrl,
                                                       const std::string& apiKey,
                                                       const std::string& model,
                                                       const std::string& authField) {
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
    const std::string cleanedAuthField =
        authFieldToText(authFieldFromText(authField));   // 写侧规范化
    const std::string hint = makeApiKeyHint(apiKey);
    const std::string check = sha256Hex(apiKey);

    // 凭据先过平台保护（Windows = DPAPI；macOS/POSIX = 原样），放在 INSERT 之前：保护失败一字节不落，不留半截凭据
    auto protectedKey = platform::credential::protect(apiKey);
    if (protectedKey.isFail()) {
        return core::Result<Profile>::fail(protectedKey.errorCode(),
            "API Key 保护失败（未建行）: " + protectedKey.errorMessage());
    }

    // 单条 INSERT ... RETURNING id 原子取回 rowid（两条语句间不持锁会被并发 INSERT 穿插改写 last_insert_rowid）
    auto prep = db_.prepare(
        "INSERT INTO claude_profiles "
        "(name, base_url, api_key_hint, api_key_check, api_key_encrypted, "
        " model, auth_field, "
        " is_active, created_at, "
        " updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, 0, ?, ?) RETURNING id");
    if (prep.isFail()) {
        return core::Result<Profile>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    stmt.bind(1, cleanedName);
    stmt.bind(2, cleanedUrl);
    stmt.bind(3, hint);
    stmt.bind(4, check);
    stmt.bind(5, protectedKey.value());  // 凭据本体（分平台形态）
    stmt.bind(6, cleanedModel);
    stmt.bind(7, cleanedAuthField);  // 认证字段（写侧已规范化）
    stmt.bind(8, static_cast<int64_t>(now));
    stmt.bind(9, static_cast<int64_t>(now));

    auto stepR = stmt.step();
    if (stepR.isFail()) {
        std::string msg = stepR.errorMessage();
        if (msg.find("UNIQUE") != std::string::npos) {
            return core::Result<Profile>::fail(
                core::ErrorCode::ProfileAlreadyExists,
                "Profile name 已存在: " + cleanedName);
        }
        return core::Result<Profile>::fail(stepR.errorCode(), msg);
    }
    if (!stepR.value()) {
        // RETURNING 理论上必返回一行；防御式处理
        return core::Result<Profile>::fail(core::ErrorCode::Internal,
            "INSERT ... RETURNING id 未返回行");
    }

    auto idRow = stmt.currentRow();
    if (idRow.isFail()) {
        return core::Result<Profile>::fail(idRow.errorCode(), idRow.errorMessage());
    }
    auto idVal = idRow.value().getInt64("id");
    if (idVal.isFail()) {
        return core::Result<Profile>::fail(idVal.errorCode(), idVal.errorMessage());
    }
    const int64_t newId = idVal.value();

    // 把 RETURNING 步进到 DONE 释放连接 in-progress 状态（语句停在结果行时 COMMIT 被 SQLite 拒绝）；失败极罕见且行已写入，只 WARN
    auto drain = stmt.step();
    if (drain.isFail()) {
        LOG_WARN("INSERT RETURNING 收尾 step 失败（行已写入，继续）: " +
                 drain.errorMessage());
    }

    LOG_INFO("已创建 Claude profile id=" + std::to_string(newId) +
             " name=" + cleanedName);

    Profile out;
    out.id = newId;
    out.name = cleanedName;
    out.baseUrl = cleanedUrl;
    out.apiKeyHint = hint;
    out.apiKeyCheck = check;
    out.model = cleanedModel;
    out.authField = cleanedAuthField;   // 必须带上：二段补写以本返回值为基
    out.isActive = false;
    out.createdAt = now;
    out.updatedAt = now;
    // apiKeyPlain 留空（不返回明文）
    return core::Result<Profile>::ok(std::move(out));
}

core::Result<void> ProfileRepository::updateProfile(const Profile& profile) {
    if (profile.id == 0) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "profile.id 不能为 0");
    }
    if (trim(profile.name).empty()) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "name 不能为空");
    }
    // 前置存在性校验（行不存在 → NotFound，避免 UPDATE 静默 0 行）
    auto old = getProfile(profile.id);
    if (old.isFail()) {
        return core::Result<void>::fail(old.errorCode(), old.errorMessage());
    }

    const std::string cleanedUrl = stripTrailingSlash(trim(profile.baseUrl));
    const std::string cleanedName = trim(profile.name);
    const std::time_t now = nowUnix();
    const std::string cleanedAuthField =
        authFieldToText(authFieldFromText(profile.authField));   // 写侧规范化

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

    // model 列与 default 槽位保持一致（双向同步两字段成本低于省一列）
    std::string cleanedModel = trim(profile.model);
    if (!profile.modelsConfigJson.empty()) {
        auto mc = parseModelsConfig(profile.modelsConfigJson);
        if (mc.isOk() && !mc.value().def.model.empty()) {
            cleanedModel = mc.value().def.model;
        }
    }

    if (!profile.apiKeyPlain.empty()) {
        // 两段写：段一凭据三列 → 段二其余元数据；段二失败按旧值回写三列
        TwoPhaseKeyWrite keyTx = TwoPhaseKeyWrite::capture(db_, profile.id);
        auto keyWrite = keyTx.writeKey(profile.apiKeyPlain);
        if (keyWrite.isFail()) {
            return core::Result<void>::fail(keyWrite.errorCode(),
                "保存 API Key 失败: " + keyWrite.errorMessage());
        }

        auto stepR = keyTx.writeDb([&]() -> core::Result<void> {
            auto prep = db_.prepare(
                "UPDATE claude_profiles SET "
                "name = ?, base_url = ?, "
                "model = ?, models_config = ?, extra_env = ?, "
                "auth_field = ?, "
                "updated_at = ? "
                "WHERE id = ?");
            if (prep.isFail()) {
                return core::Result<void>::fail(prep.errorCode(),
                                                prep.errorMessage());
            }
            auto& stmt = prep.value();
            stmt.bind(1, cleanedName);
            stmt.bind(2, cleanedUrl);
            stmt.bind(3, cleanedModel);
            stmt.bind(4, profile.modelsConfigJson);
            stmt.bind(5, profile.extraEnvJson);
            stmt.bind(6, cleanedAuthField);
            stmt.bind(7, static_cast<int64_t>(now));
            stmt.bind(8, profile.id);
            auto step = stmt.step();
            if (step.isFail()) {
                std::string msg = step.errorMessage();
                if (msg.find("UNIQUE") != std::string::npos) {
                    return core::Result<void>::fail(
                        core::ErrorCode::ProfileAlreadyExists,
                        "Profile name 已存在: " + cleanedName);
                }
                return core::Result<void>::fail(step.errorCode(), msg);
            }
            return core::Result<void>::ok();
        });
        if (stepR.isFail()) {
            return core::Result<void>::fail(stepR.errorCode(),
                                            stepR.errorMessage());
        }
    } else {
        auto prep = db_.prepare(
            "UPDATE claude_profiles SET "
            "name = ?, base_url = ?, model = ?, models_config = ?, "
            "extra_env = ?, "
            "auth_field = ?, "
            "updated_at = ? WHERE id = ?");
        if (prep.isFail()) {
            return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
        }
        auto& stmt = prep.value();
        stmt.bind(1, cleanedName);
        stmt.bind(2, cleanedUrl);
        stmt.bind(3, cleanedModel);
        stmt.bind(4, profile.modelsConfigJson);
        stmt.bind(5, profile.extraEnvJson);
        stmt.bind(6, cleanedAuthField);
        stmt.bind(7, static_cast<int64_t>(now));
        stmt.bind(8, profile.id);
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

core::Result<void> ProfileRepository::deleteProfile(int64_t id,
                                                    bool* wasActive) {
    if (id == 0) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "id 不能为 0");
    }

    auto p = getProfile(id);
    if (p.isFail()) {
        return core::Result<void>::fail(p.errorCode(), p.errorMessage());
    }
    const bool wasActiveVal = p.value().isActive;
    if (wasActive) *wasActive = wasActiveVal;

    // 两段写：段一清凭据三列（旧值先捕获）→ 段二删 SQLite 行；顺序不能反——先删行则失败路径无从补偿
    TwoPhaseKeyWrite keyTx = TwoPhaseKeyWrite::capture(db_, id);
    auto keyClear = keyTx.writeKey(std::string());   // 空串 = 清空三列
    if (keyClear.isFail()) {
        LOG_ERROR("清理凭据失败，中止删除 profile id=" +
                  std::to_string(id) + ": " + keyClear.errorMessage());
        return core::Result<void>::fail(
            keyClear.errorCode(),
            "清理保存的 API Key 失败，profile 未删除: " + keyClear.errorMessage());
    }

    // 段二：删行（FK CASCADE 级联清调用日志）
    auto delRow = keyTx.writeDb([&]() -> core::Result<void> {
        auto prep = db_.prepare("DELETE FROM claude_profiles WHERE id = ?");
        if (prep.isFail()) {
            return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
        }
        auto& stmt = prep.value();
        stmt.bind(1, id);
        auto stepR = stmt.step();
        if (stepR.isFail()) {
            return core::Result<void>::fail(stepR.errorCode(),
                                            stepR.errorMessage());
        }
        return core::Result<void>::ok();
    });
    if (delRow.isFail()) {
        return core::Result<void>::fail(delRow.errorCode(), delRow.errorMessage());
    }

    LOG_INFO("已删除 Claude profile id=" + std::to_string(id));

    // 停代理策略尾（删的是激活 profile 且代理运行中 → stopProxy 还原 settings.json）留门面，wasActive 经出参回填
    return core::Result<void>::ok();
}

core::Result<int> ProfileRepository::backfillCredentialsFromKeychain() {
    // 前提：旧 keychain_account 列还在旧库中——新库无此行，跳过不报错
    {
        auto prep = db_.prepare("SELECT keychain_account FROM claude_profiles LIMIT 0");
        if (prep.isFail()) {
            // 列不存在 → 新库，无旧凭据需回填
            LOG_INFO("凭据回填跳过：keychain_account 列不存在（新库或已清理）");
            return core::Result<int>::ok(0);
        }
    }

    // 一次性门：这是 2026-10-14 之前旧库的迁移路径，只允许首次启动尝试一次。
    // 钥匙串授权在 ad-hoc 签名下每次重装都会失效（macOS 弹「请输入钥匙串密码」），
    // 每次启动重试 = 用户永远清不掉该框（2026-10-18 实证）。失败也置位：旧条目
    // 仍在钥匙串里，用户可手动重配，不再自动重试。
    constexpr const char* kBackfillDoneKey = "claude.keychain_backfill_done";
    auto done = core::appconfig::get(db_, kBackfillDoneKey);
    if (done.isOk() && done.value() == "1") {
        LOG_DEBUG("凭据回填跳过：一次性迁移已执行过");
        return core::Result<int>::ok(0);
    }

    // 查待回填行：api_key_encrypted 仍为空但 keychain_account 有值
    auto prep = db_.prepare(
        "SELECT id, keychain_account FROM claude_profiles "
        "WHERE api_key_encrypted = '' AND keychain_account IS NOT NULL "
        "AND keychain_account != ''");
    if (prep.isFail()) {
        return core::Result<int>::fail(prep.errorCode(),
                                       "凭据回填查询失败: " + prep.errorMessage());
    }
    auto rows = prep.value().fetchAll();
    if (rows.isFail()) {
        return core::Result<int>::fail(rows.errorCode(),
                                       "凭据回填取行失败: " + rows.errorMessage());
    }

    int backfilled = 0;
    for (const auto& row : rows.value()) {
        auto idV = row.getInt64("id");
        auto accountV = row.getString("keychain_account");
        if (idV.isFail() || accountV.isFail()) continue;
        const int64_t id = idV.value();
        const std::string account = accountV.value();

        // 读旧 Keychain 凭据（DPAPI 密文文件）。keychainService_ 是按数据根派生的新命名空间
        // （"DevDesk-<hash>"）；Windows 旧版实际用纯 "DevDesk" 存（keychain/DevDesk/claude%3A<id>.key），
        // 派生空间 miss 时必须回退旧名——否则老用户的密钥永远「找不到」，凭据静默丢失（2026-10-14 实证）。
        auto secretR = platform::keychain::loadSecret(keychainService_, account);
        std::string primaryError;
        if (secretR.isFail()) {
            // 回退旧名是 Windows 旧版形态；但**不能丢掉派生空间的原始错误**——
            // 否则「授权被拒」会被旧名的 not found 覆盖，日志再次说谎
            //（2026-10-18 实证：派生空间 errSecAuthFailed 被旧名 NotFound 掩盖）。
            primaryError = secretR.errorMessage();
            secretR = platform::keychain::loadSecret("DevDesk", account);
        }
        if (secretR.isFail()) {
            LOG_WARN("凭据回填：id=" + std::to_string(id) +
                     " Keychain 读取失败（跳过）: 派生空间=" + primaryError +
                     "；旧名=" + secretR.errorMessage());
            continue;
        }
        if (secretR.value().empty()) {
            LOG_WARN("凭据回填：id=" + std::to_string(id) +
                     " Keychain 凭据为空串（跳过）");
            continue;
        }

        // 经当前平台 protect 入库
        auto protectedR = platform::credential::protect(secretR.value());
        if (protectedR.isFail()) {
            LOG_WARN("凭据回填：id=" + std::to_string(id) +
                     " 凭据保护失败（跳过）: " + protectedR.errorMessage());
            continue;
        }
        const std::string hint = makeApiKeyHint(secretR.value());
        const std::string check = sha256Hex(secretR.value());

        auto up = db_.prepare(
            "UPDATE claude_profiles SET api_key_encrypted = ?, api_key_hint = ?, "
            "api_key_check = ?, keychain_account = NULL, updated_at = ? WHERE id = ?");
        if (up.isFail()) {
            LOG_WARN("凭据回填：id=" + std::to_string(id) +
                     " UPDATE 准备失败（跳过）: " + up.errorMessage());
            continue;
        }
        auto& stmt = up.value();
        stmt.bind(1, protectedR.value());
        stmt.bind(2, hint);
        stmt.bind(3, check);
        stmt.bind(4, static_cast<int64_t>(nowUnix()));
        stmt.bind(5, id);
        auto stepR = stmt.step();
        if (stepR.isFail()) {
            LOG_WARN("凭据回填：id=" + std::to_string(id) +
                     " UPDATE 执行失败（跳过）: " + stepR.errorMessage());
            continue;
        }
        ++backfilled;
        LOG_INFO("凭据回填：id=" + std::to_string(id) + " 已从 Keychain 迁入数据库");
    }

    if (backfilled > 0) {
        LOG_INFO("凭据回填完成：共回填 " + std::to_string(backfilled) + " 个 profile");
    }
    // 无论成败都置位（一次性语义）。写入失败只记日志：下次启动会再试一次，
    // 属可接受退化，不会变成每次启动必然重试。
    auto mark = core::appconfig::set(db_, kBackfillDoneKey, "1");
    if (mark.isFail()) {
        LOG_WARN("凭据回填：一次性标记写入失败 - " + mark.errorMessage());
    }
    return core::Result<int>::ok(backfilled);
}

core::Result<std::string> ProfileRepository::loadApiKey(int64_t profileId) {
    auto p = getProfile(profileId);
    if (p.isFail()) {
        return core::Result<std::string>::fail(p.errorCode(), p.errorMessage());
    }
    const std::string stored = readStoredCredential(db_, profileId);
    if (stored.empty()) {
        // 「没有密钥」与「有但解不开」必须可分辨（不静默回落空串——那会把不可用伪装成没配过）
        return core::Result<std::string>::fail(
            core::ErrorCode::NotFound,
            "该 Profile 没有保存 API Key（列空）");
    }
    auto plain = platform::credential::unprotect(stored);
    if (plain.isOk()) return plain;

    // 跨平台迁移兜底：macOS/POSIX 直接存明文，库里被双机同步搬到 Windows 后
    // hex 与 DPAPI 都解不开（用户侧表现 = 切换 Profile 时读 token 报错）。
    // 明文形态可识别 → 就地升级成本机 DPAPI 密文（是升级，不降级为明文）。
    if (!platform::credential::storesPlaintext() &&
        platform::credential::looksLikePlaintextCredential(stored)) {
        auto prot = platform::credential::protect(stored);
        if (prot.isOk()) {
            auto w = writeCredentialColumns(db_, profileId, prot.value(),
                                            makeApiKeyHint(stored),
                                            sha256Hex(stored));
            if (w.isFail()) {
                LOG_WARN("凭据迁移：profile " + std::to_string(profileId) +
                         " 已读出明文 Key，但改写密文失败（下次读取重试）: " +
                         w.errorMessage());
            } else {
                LOG_INFO("凭据迁移：profile " + std::to_string(profileId) +
                         " 的明文 Key 已升级为本机 DPAPI 密文");
            }
            return core::Result<std::string>::ok(stored);
        }
    }
    return plain;
}

core::Result<Profile> ProfileRepository::getActiveProfile() {
    auto prep = db_.prepare(
        "SELECT id, name, base_url, api_key_hint, api_key_check, "
        "model, models_config, extra_env, "
        "auth_field, is_active, "
        "created_at, updated_at "
        "FROM claude_profiles WHERE is_active = 1 LIMIT 1");
    if (prep.isFail()) {
        return core::Result<Profile>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto stepR = prep.value().step();
    if (stepR.isFail()) {
        return core::Result<Profile>::fail(stepR.errorCode(), stepR.errorMessage());
    }
    if (!stepR.value()) {
        // 错误 message 带可操作指引——503 的 JSON body 原样透传此文案（proxy_router 唯一出口）
        return core::Result<Profile>::fail(core::ErrorCode::NoActiveProfile,
            "没有激活的 Claude profile，请在 DevDesk 的 Claude 页右键选中 "
            "Profile 并选择『设为当前』");
    }
    auto row = prep.value().currentRow();
    if (row.isFail()) {
        return core::Result<Profile>::fail(row.errorCode(), row.errorMessage());
    }
    return rowToProfile(row.value());
}

core::Result<void> ProfileRepository::activateProfile(int64_t id) {
    // setActiveProfile 的 DB 段（前置校验与反向同步编排在门面）
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

// Profile 复制

core::Result<Profile> ProfileRepository::duplicateProfile(int64_t srcId) {
    auto srcR = getProfile(srcId);
    if (srcR.isFail()) {
        return core::Result<Profile>::fail(srcR.errorCode(),
                                          srcR.errorMessage());
    }
    const Profile src = std::move(srcR).value();

    // 副本命名：name + " 副本"，重名递增避让 UNIQUE 约束；100 次仍冲突 → fail 且不建行
    auto listR = listProfiles();
    if (listR.isFail()) {
        return core::Result<Profile>::fail(listR.errorCode(),
                                           listR.errorMessage());
    }
    std::vector<std::string> names;
    names.reserve(listR.value().size());
    for (const auto& p : listR.value()) names.push_back(p.name);
    const std::string copyName = makeCopyName(src.name, names);
    if (copyName.empty()) {
        return core::Result<Profile>::fail(
            core::ErrorCode::ProfileAlreadyExists,
            "副本命名连续 100 次冲突，请先重命名或删除部分 Profile");
    }

    // 源 key 缺失 → fail：不建无 key 的副本。列空 →「密钥缺失」+ 恢复提示；读失败 → 原样透传错误码（读不了 ≠ 没有）
    auto keyR = loadApiKey(srcId);
    if (keyR.isFail()) {
        if (keyR.errorCode() == core::ErrorCode::NotFound) {
            return core::Result<Profile>::fail(
                core::ErrorCode::InvalidApiKey,
                "源 Profile 的密钥缺失（该 Profile 未保存 API Key），无法建副本；"
                "编辑该 Profile 重新填一次 API Key 即可");
        }
        return core::Result<Profile>::fail(
            keyR.errorCode(),
            "读取源 Profile 密钥失败：" + keyR.errorMessage());
    }
    if (keyR.value().empty()) {
        // protect 理论上不会把空串存成非空，但真发生了要如实报
        return core::Result<Profile>::fail(
            core::ErrorCode::InvalidApiKey,
            "源 Profile 的密钥为空串，无法建副本；"
            "编辑该 Profile 重新填一次 API Key 即可");
    }

    // 凭据三列随 createProfile 一并写入；副本不激活
    const std::string defModel =
        effectiveModelsConfig(src.modelsConfigJson).def.model;
    auto createdR = createProfile(copyName, src.baseUrl, keyR.value(), defModel,
                                  src.authField);
    if (createdR.isFail()) {
        return core::Result<Profile>::fail(createdR.errorCode(),
                                           createdR.errorMessage());
    }
    const int64_t newId = createdR.value().id;
    Profile out = createdR.value();

    // 二段补写 models_config / extra_env（createProfile 只带得走 default 模型名）；不得静默丢字段
    if (!src.modelsConfigJson.empty() || !src.extraEnvJson.empty()) {
        Profile upd = out;
        upd.modelsConfigJson = src.modelsConfigJson;
        upd.extraEnvJson = src.extraEnvJson;
        upd.model = defModel;
        auto u = updateProfile(upd);
        if (u.isFail()) {
            // 回滚：deleteProfile 先清凭据再删行
            auto rb = deleteProfile(newId);
            if (rb.isOk()) {
                return core::Result<Profile>::fail(
                    u.errorCode(), "复制失败（已回滚）: " + u.errorMessage());
            }
            LOG_ERROR("复制回滚失败，存在不完整副本 id=" + std::to_string(newId) +
                      ": " + rb.errorMessage());
            return core::Result<Profile>::fail(u.errorCode(),
                "复制失败: " + u.errorMessage() + "；已创建不完整副本 id=" +
                std::to_string(newId) + "，请手动删除");
        }
        out = std::move(upd);
    }

    LOG_INFO("已复制 Claude profile id=" + std::to_string(newId) +
             " name=" + copyName + "（源 id=" + std::to_string(srcId) +
             "，副本未激活）");
    return core::Result<Profile>::ok(std::move(out));
}

} // namespace module::claude
