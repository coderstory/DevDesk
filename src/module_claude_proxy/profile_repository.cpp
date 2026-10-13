// ProfileRepository 实现：策略尾（停代理 / 接管 / 激活时机）留在门面，本类只做 DB 段
#include "profile_repository.h"

#include "core/logging.h"
#include "core/time.h"
#include "platform/credential_protection.h"
#include "platform/keychain.h"
#include "provider_config.h"
#include "upstream_dialect.h"

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

// 方言覆盖文本规范化：命中值域 → 规范文本；空串 / 未知 → 空串（回落 host 推断），写读两侧各调一次
std::string normalizeDialectOverride(const std::string& text) {
    const auto kind = dialectKindFromText(text);
    return kind ? std::string(dialectKindToText(*kind)) : std::string();
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

// 凭据列探测（Keychain 残留清理护栏）：判据 = 列可读；列可读但值为空 = 库只是没存密钥，由调用处再判「至少一行有密钥」
struct CredentialColumnProbe {
    bool readable = false;     // 新列可读 = 库是本形态建的
    int64_t rows = 0;          // claude_profiles 行数
    int64_t rowsWithKey = 0;   // 凭据列非空的行数
};

CredentialColumnProbe probeCredentialColumn(core::data::SqliteDb& db) {
    CredentialColumnProbe out;
    auto prep = db.prepare("SELECT api_key_encrypted FROM claude_profiles");
    if (prep.isFail()) return out;   // 旧库：no such column → readable 保持 false
    auto rows = prep.value().fetchAll();
    if (rows.isFail()) return out;
    for (const auto& row : rows.value()) {
        auto v = row.getString("api_key_encrypted");
        if (v.isFail()) return out;   // 逐行读不到 = 形态不对 → 同样按未迁移处理
        ++out.rows;
        if (!v.value().empty()) ++out.rowsWithKey;
    }
    out.readable = true;
    return out;
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
    auto extraSettings = row.getString("extra_settings_json");
    auto sharedCfg = row.getInt64("shared_config_id");
    auto authField = row.getString("auth_field");
    auto dialectOverride = row.getString("dialect_override");
    auto fallbackProfileId = row.getInt64("fallback_profile_id");
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
    if (extraSettings.isOk()) p.extraSettingsJson = extraSettings.value();
    if (sharedCfg.isOk()) p.sharedConfigId = sharedCfg.value();
    // 读侧规范化：未知/空值一律兜底 both
    if (authField.isOk()) {
        p.authField = authFieldToText(authFieldFromText(authField.value()));
    }
    // 读侧规范化：未知/空值一律按「未覆盖」读回空串（同 authField 口径）
    if (dialectOverride.isOk()) {
        p.dialectOverride = normalizeDialectOverride(dialectOverride.value());
    }
    if (fallbackProfileId.isOk()) p.fallbackProfileId = fallbackProfileId.value();
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
        "model, models_config, extra_env, extra_settings_json, "
        "shared_config_id, auth_field, dialect_override, fallback_profile_id, is_active, "
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
        "model, models_config, extra_env, extra_settings_json, "
        "shared_config_id, auth_field, dialect_override, fallback_profile_id, is_active, "
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
                                                       int64_t sharedConfigId,
                                                       const std::string& authField,
                                                       const std::string& dialectOverride,
                                                       int64_t fallbackProfileId,
                                                       const std::string& extraSettingsJson) {
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
    const std::string cleanedDialectOverride =
        normalizeDialectOverride(dialectOverride);
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
        " model, shared_config_id, auth_field, dialect_override, "
        " fallback_profile_id, extra_settings_json, is_active, created_at, "
        " updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?) RETURNING id");
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
    stmt.bind(7, sharedConfigId);    // 建时即绑定（0 = 不绑定）
    stmt.bind(8, cleanedAuthField);  // 认证字段（写侧已规范化）
    stmt.bind(9, cleanedDialectOverride);  // 方言覆盖（写侧已规范化，空串 = 未覆盖）
    stmt.bind(10, fallbackProfileId);      // 备用（0 = 无；duplicateProfile 携带）
    stmt.bind(11, extraSettingsJson);      // 高级设置 JSON 原文（空串 = 未配置）
    stmt.bind(12, static_cast<int64_t>(now));
    stmt.bind(13, static_cast<int64_t>(now));

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
    out.sharedConfigId = sharedConfigId;
    out.authField = cleanedAuthField;   // 必须带上：二段补写以本返回值为基
    out.dialectOverride = cleanedDialectOverride;   // 同上
    out.fallbackProfileId = fallbackProfileId;
    out.extraSettingsJson = extraSettingsJson;      // 同上
    out.isActive = false;
    out.createdAt = now;
    out.updatedAt = now;
    // apiKeyPlain 留空（不返回明文）
    return core::Result<Profile>::ok(std::move(out));
}

core::Result<void> ProfileRepository::updateProfile(const Profile& profile,
                                                    Profile* oldProfile) {
    if (profile.id == 0) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "profile.id 不能为 0");
    }
    if (trim(profile.name).empty()) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "name 不能为空");
    }
    // 拒自引用（== 自身 = 纯浪费一跳）；A→B / B→A 的一跳环无害，不拦
    if (profile.fallbackProfileId == profile.id) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "备用 Profile 不能是自身");
    }

    // 查旧 profile；oldProfile 出参供门面的 re-takeover 策略尾判断
    auto old = getProfile(profile.id);
    if (old.isFail()) {
        return core::Result<void>::fail(old.errorCode(), old.errorMessage());
    }
    if (oldProfile) *oldProfile = old.value();

    const std::string cleanedUrl = stripTrailingSlash(trim(profile.baseUrl));
    const std::string cleanedName = trim(profile.name);
    const std::time_t now = nowUnix();
    const std::string cleanedAuthField =
        authFieldToText(authFieldFromText(profile.authField));   // 写侧规范化
    const std::string cleanedDialectOverride =
        normalizeDialectOverride(profile.dialectOverride);

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
                "model = ?, models_config = ?, extra_env = ?, extra_settings_json = ?, "
                "shared_config_id = ?, "
                "auth_field = ?, dialect_override = ?, fallback_profile_id = ?, "
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
            stmt.bind(6, profile.extraSettingsJson);
            stmt.bind(7, profile.sharedConfigId);
            stmt.bind(8, cleanedAuthField);
            stmt.bind(9, cleanedDialectOverride);
            stmt.bind(10, profile.fallbackProfileId);
            stmt.bind(11, static_cast<int64_t>(now));
            stmt.bind(12, profile.id);
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
            "extra_env = ?, extra_settings_json = ?, shared_config_id = ?, "
            "auth_field = ?, "
            "dialect_override = ?, fallback_profile_id = ?, "
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
        stmt.bind(6, profile.extraSettingsJson);
        stmt.bind(7, profile.sharedConfigId);
        stmt.bind(8, cleanedAuthField);
        stmt.bind(9, cleanedDialectOverride);
        stmt.bind(10, profile.fallbackProfileId);
        stmt.bind(11, static_cast<int64_t>(now));
        stmt.bind(12, profile.id);
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

    // re-takeover 策略尾（片段绑定变化 && 旧行激活 && 代理运行 → 立即重写 live）留门面（经 oldProfile 出参判断）
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

    // 段二：同事务先解绑「备用指向它」的 profile（不留悬空 fallback id），再删行（FK CASCADE 级联清调用日志）
    auto delRow = keyTx.writeDb([&]() -> core::Result<void> {
        auto ub = db_.prepare(
            "UPDATE claude_profiles SET fallback_profile_id = 0, "
            "updated_at = ? WHERE fallback_profile_id = ?");
        if (ub.isFail()) {
            return core::Result<void>::fail(ub.errorCode(), ub.errorMessage());
        }
        auto& ubStmt = ub.value();
        ubStmt.bind(1, static_cast<int64_t>(nowUnix()));
        ubStmt.bind(2, id);
        auto ubStep = ubStmt.step();
        if (ubStep.isFail()) {
            return core::Result<void>::fail(ubStep.errorCode(),
                                            ubStep.errorMessage());
        }

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

core::Result<int> ProfileRepository::cleanupOrphanKeychainEntries() {
    // 一次性的旧 Keychain 残留清理：本命名空间下剩余 claude:* 条目一律清掉，白名单之外的 account 保留不删不计数
    auto profilesR = listProfiles();
    if (profilesR.isFail()) {
        return core::Result<int>::fail(profilesR.errorCode(),
                                       profilesR.errorMessage());
    }

    // 误删护栏（优先于一切）：数据根零 profile = 实例还没建过东西，而 Keychain 命名空间隔离弱于数据根 → 整体跳过
    if (profilesR.value().empty()) {
        LOG_WARN("跳过清理：当前数据根无 profile，防误删（keychain service=" +
                 keychainService_ + " 的条目一律保留）");
        return core::Result<int>::ok(0);
    }

    // 护栏③：光有 profile 不代表凭据已落库。列可读但没有任何一行存有密钥时同样整体跳过——宁可不清理也不能误删
    const CredentialColumnProbe probe = probeCredentialColumn(db_);
    if (!probe.readable) {
        LOG_WARN("跳过清理：库中没有 api_key_encrypted 列（旧形态库，凭据未迁入库），"
                 "Keychain 条目一律保留，防误删（keychain service=" +
                 keychainService_ + "）");
        return core::Result<int>::ok(0);
    }
    if (probe.rowsWithKey == 0) {
        LOG_WARN("跳过清理：库里 " + std::to_string(probe.rows) +
                 " 个 profile 都没有存过密钥（凭据未落库），Keychain 条目一律保留，"
                 "防误删（keychain service=" + keychainService_ + "）");
        return core::Result<int>::ok(0);
    }

    // keep 传空是有意选择：走到这里已证明库是本形态且至少一行存着凭据
    auto delR = platform::keychain::deleteSecretsNotIn(keychainService_, {});
    if (delR.isFail()) {
        return core::Result<int>::fail(delR.errorCode(), delR.errorMessage());
    }
    const int removed = static_cast<int>(delR.value().size());
    if (removed > 0) {
        LOG_INFO("已清理 " + std::to_string(removed) +
                 " 个旧 Keychain 残留密钥（凭据已迁至数据库，Keychain 不再是来源）");
    }
    return core::Result<int>::ok(removed);
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
    return platform::credential::unprotect(stored);
}

core::Result<Profile> ProfileRepository::getActiveProfile() {
    auto prep = db_.prepare(
        "SELECT id, name, base_url, api_key_hint, api_key_check, "
        "model, models_config, extra_env, extra_settings_json, "
        "shared_config_id, auth_field, dialect_override, fallback_profile_id, is_active, "
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
            "没有激活的 Claude profile，请在 DevDesk 的 Claude 页选中 Profile "
            "并点击「启用」");
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

    // 凭据三列随 createProfile 一并写入；副本不激活。片段绑定 / fallback / extra_settings 经尾参带上（留到二段补写才设，门槛不成立时会丢）
    const std::string defModel =
        effectiveModelsConfig(src.modelsConfigJson).def.model;
    auto createdR = createProfile(copyName, src.baseUrl, keyR.value(), defModel,
                                  src.sharedConfigId, src.authField,
                                  src.dialectOverride, src.fallbackProfileId,
                                  src.extraSettingsJson);
    if (createdR.isFail()) {
        return core::Result<Profile>::fail(createdR.errorCode(),
                                           createdR.errorMessage());
    }
    const int64_t newId = createdR.value().id;
    Profile out = createdR.value();

    // 二段补写 models_config / extra_env / extra_settings_json（createProfile 只带得走 default 模型名）；不得静默丢字段
    if (!src.modelsConfigJson.empty() || !src.extraEnvJson.empty() ||
        !src.extraSettingsJson.empty()) {
        Profile upd = out;
        upd.modelsConfigJson = src.modelsConfigJson;
        upd.extraEnvJson = src.extraEnvJson;
        upd.extraSettingsJson = src.extraSettingsJson;
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

// 共享配置片段

core::Result<SharedConfig> ProfileRepository::rowToSharedConfig(
    const core::data::SqlRow& row) const {
    SharedConfig c;
    auto id = row.getInt64("id");
    if (id.isFail()) {
        return core::Result<SharedConfig>::fail(id.errorCode(), id.errorMessage());
    }
    c.id = id.value();
    auto name = row.getString("name");
    auto settings = row.getString("extra_settings_json");
    auto created = row.getInt64("created_at");
    auto updated = row.getInt64("updated_at");
    if (name.isOk()) c.name = name.value();
    if (settings.isOk()) c.extraSettingsJson = settings.value();
    if (created.isOk()) c.createdAt = static_cast<std::time_t>(created.value());
    if (updated.isOk()) c.updatedAt = static_cast<std::time_t>(updated.value());
    return core::Result<SharedConfig>::ok(std::move(c));
}

core::Result<std::vector<SharedConfig>> ProfileRepository::listSharedConfigs() {
    auto prep = db_.prepare(
        "SELECT id, name, extra_settings_json, created_at, updated_at "
        "FROM claude_shared_configs ORDER BY name ASC");
    if (prep.isFail()) {
        return core::Result<std::vector<SharedConfig>>::fail(
            prep.errorCode(), prep.errorMessage());
    }
    auto rows = prep.value().fetchAll();
    if (rows.isFail()) {
        return core::Result<std::vector<SharedConfig>>::fail(
            rows.errorCode(), rows.errorMessage());
    }
    std::vector<SharedConfig> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto c = rowToSharedConfig(row);
        if (c.isFail()) {
            return core::Result<std::vector<SharedConfig>>::fail(
                c.errorCode(), c.errorMessage());
        }
        out.push_back(std::move(c).value());
    }
    return core::Result<std::vector<SharedConfig>>::ok(std::move(out));
}

core::Result<SharedConfig> ProfileRepository::createSharedConfig(
    const std::string& name, const std::string& extraSettingsJson) {
    const std::string cleanedName = trim(name);
    if (cleanedName.empty()) {
        return core::Result<SharedConfig>::fail(core::ErrorCode::ValidationError,
            "共享配置片段名不能为空");
    }
    // 信任边界：不把坏 JSON 落库（读侧另有容错）
    if (!extraSettingsJson.empty()) {
        auto v = validateExtraSettingsJson(extraSettingsJson);
        if (v.isFail()) {
            return core::Result<SharedConfig>::fail(v.errorCode(),
                "共享配置片段 JSON 非法: " + v.errorMessage());
        }
    }

    const std::time_t now = nowUnix();
    auto prep = db_.prepare(
        "INSERT INTO claude_shared_configs (name, extra_settings_json, "
        "created_at, updated_at) VALUES (?, ?, ?, ?) RETURNING id");
    if (prep.isFail()) {
        return core::Result<SharedConfig>::fail(prep.errorCode(),
                                               prep.errorMessage());
    }
    auto& stmt = prep.value();
    stmt.bind(1, cleanedName);
    stmt.bind(2, extraSettingsJson);
    stmt.bind(3, static_cast<int64_t>(now));
    stmt.bind(4, static_cast<int64_t>(now));
    auto stepR = stmt.step();
    if (stepR.isFail()) {
        std::string msg = stepR.errorMessage();
        if (msg.find("UNIQUE") != std::string::npos) {
            return core::Result<SharedConfig>::fail(core::ErrorCode::AlreadyExists,
                "共享配置片段名已存在: " + cleanedName);
        }
        return core::Result<SharedConfig>::fail(stepR.errorCode(), msg);
    }
    if (!stepR.value()) {
        return core::Result<SharedConfig>::fail(core::ErrorCode::Internal,
            "INSERT ... RETURNING id 未返回行");
    }
    auto idRow = stmt.currentRow();
    if (idRow.isFail()) {
        return core::Result<SharedConfig>::fail(idRow.errorCode(),
                                                idRow.errorMessage());
    }
    auto idVal = idRow.value().getInt64("id");
    if (idVal.isFail()) {
        return core::Result<SharedConfig>::fail(idVal.errorCode(),
                                                idVal.errorMessage());
    }
    const int64_t newId = idVal.value();

    // 步进到 DONE 释放连接 in-progress 状态（同 createProfile：语句停在结果行时开事务会被 SQLite 拒绝）
    auto drain = stmt.step();
    if (drain.isFail()) {
        LOG_WARN("共享片段 INSERT RETURNING 收尾 step 失败（行已写入，继续）: " +
                 drain.errorMessage());
    }

    LOG_INFO("已创建共享配置片段 id=" + std::to_string(newId) +
             " name=" + cleanedName);
    SharedConfig out;
    out.id = newId;
    out.name = cleanedName;
    out.extraSettingsJson = extraSettingsJson;
    out.createdAt = now;
    out.updatedAt = now;
    return core::Result<SharedConfig>::ok(std::move(out));
}

core::Result<void> ProfileRepository::requireSharedConfigExists(int64_t id) const {
    auto prep = db_.prepare("SELECT id FROM claude_shared_configs WHERE id = ?");
    if (prep.isFail()) {
        return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    stmt.bind(1, id);
    auto stepR = stmt.step();
    if (stepR.isFail()) {
        return core::Result<void>::fail(stepR.errorCode(), stepR.errorMessage());
    }
    if (!stepR.value()) {
        return core::Result<void>::fail(core::ErrorCode::NotFound,
            "共享配置片段不存在: id=" + std::to_string(id));
    }
    return core::Result<void>::ok();
}

core::Result<void> ProfileRepository::updateSharedConfig(
    const SharedConfig& config) {
    if (config.id == 0) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "共享配置片段 id 不能为 0");
    }
    const std::string cleanedName = trim(config.name);
    if (cleanedName.empty()) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "共享配置片段名不能为空");
    }
    if (!config.extraSettingsJson.empty()) {
        auto v = validateExtraSettingsJson(config.extraSettingsJson);
        if (v.isFail()) {
            return core::Result<void>::fail(v.errorCode(),
                "共享配置片段 JSON 非法: " + v.errorMessage());
        }
    }
    auto exists = requireSharedConfigExists(config.id);
    if (exists.isFail()) {
        return exists;
    }

    auto prep = db_.prepare(
        "UPDATE claude_shared_configs SET name = ?, extra_settings_json = ?, "
        "updated_at = ? WHERE id = ?");
    if (prep.isFail()) {
        return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    stmt.bind(1, cleanedName);
    stmt.bind(2, config.extraSettingsJson);
    stmt.bind(3, static_cast<int64_t>(nowUnix()));
    stmt.bind(4, config.id);
    auto stepR = stmt.step();
    if (stepR.isFail()) {
        std::string msg = stepR.errorMessage();
        if (msg.find("UNIQUE") != std::string::npos) {
            return core::Result<void>::fail(core::ErrorCode::AlreadyExists,
                "共享配置片段名已存在: " + cleanedName);
        }
        return core::Result<void>::fail(stepR.errorCode(), msg);
    }
    LOG_INFO("已更新共享配置片段 id=" + std::to_string(config.id));

    // re-takeover 策略尾（改了就生效——代理在跑且激活 profile 绑定了它 → 立即重写 settings.json）留门面
    return core::Result<void>::ok();
}

core::Result<void> ProfileRepository::deleteSharedConfig(int64_t id,
                                                         bool* activeBound) {
    if (id == 0) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "id 不能为 0");
    }
    auto exists = requireSharedConfigExists(id);
    if (exists.isFail()) {
        return exists;
    }

    // 删除前先看激活 profile 是否绑定它：只有它的键在 live 里，删完才需要重写 settings.json（activeBound 出参供门面判断）
    auto active = getActiveProfile();
    const bool activeBoundVal = active.isOk() && active.value().sharedConfigId == id;
    if (activeBound) *activeBound = activeBoundVal;

    // 同事务解绑 + 删行：不留悬空 shared_config_id；不级联删 profile（删片段 = 交回控制权，不是丢配置）
    auto tx = db_.transaction([this, id]() -> core::Result<void> {
        auto u = db_.prepare(
            "UPDATE claude_profiles SET shared_config_id = 0, updated_at = ? "
            "WHERE shared_config_id = ?");
        if (u.isFail()) {
            return core::Result<void>::fail(u.errorCode(), u.errorMessage());
        }
        auto& ust = u.value();
        ust.bind(1, static_cast<int64_t>(nowUnix()));
        ust.bind(2, id);
        auto us = ust.step();
        if (us.isFail()) {
            return core::Result<void>::fail(us.errorCode(), us.errorMessage());
        }

        auto d = db_.prepare("DELETE FROM claude_shared_configs WHERE id = ?");
        if (d.isFail()) {
            return core::Result<void>::fail(d.errorCode(), d.errorMessage());
        }
        auto& dst = d.value();
        dst.bind(1, id);
        auto ds = dst.step();
        if (ds.isFail()) {
            return core::Result<void>::fail(ds.errorCode(), ds.errorMessage());
        }
        return core::Result<void>::ok();
    });
    if (tx.isFail()) {
        return tx;
    }

    LOG_INFO("已删除共享配置片段 id=" + std::to_string(id) +
             (activeBoundVal ? "（激活 profile 已解绑）" : ""));

    // re-takeover 策略尾（解绑当场生效）留门面
    return core::Result<void>::ok();
}

std::string ProfileRepository::sharedExtraSettingsJsonFor(
    int64_t sharedConfigId) const {
    if (sharedConfigId == 0) return {};  // 未绑定
    auto prep = db_.prepare(
        "SELECT extra_settings_json FROM claude_shared_configs WHERE id = ?");
    if (prep.isFail()) {
        LOG_WARN("读共享配置片段失败（按未绑定处理）: " + prep.errorMessage());
        return {};
    }
    auto& stmt = prep.value();
    stmt.bind(1, sharedConfigId);
    auto stepR = stmt.step();
    if (stepR.isFail()) {
        LOG_WARN("读共享配置片段失败（按未绑定处理）: " + stepR.errorMessage());
        return {};
    }
    if (!stepR.value()) return {};  // 片段已被删 → 悬空 id 视作未绑定
    auto row = stmt.currentRow();
    if (row.isFail()) return {};
    auto txt = row.value().getString("extra_settings_json");
    return txt.isOk() ? txt.value() : std::string();
}

} // namespace module::claude
