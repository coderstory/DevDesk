// src/module_claude_proxy/profile_repository.cpp
// ProfileRepository 实现：claude_profiles 行 + SharedConfig 片段 + Keychain
// 条目的唯一持久化入口（R10-A2 自 claude_service.cpp 抽出，方法体逐字搬移
// 不改语义；含编排尾巴的 5 方法按 R10 spec §2.3 段级拆分执行——策略尾
// 停代理 / 接管时机 / 激活时机判断留在门面 claude_service.cpp）
#include "profile_repository.h"

#include "core/logging.h"
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

// 方言覆盖文本规范化（审计 CPX-13，同 authField 的写侧口径）：命中值域 → 规范
// 文本；空串 / 未知 / 拼写错误 → 空串（未覆盖 = 回落 base_url 的 host 推断）。
// 写侧与读侧各调一次，保证库里与内存里都只有两个规范值之一或空串
std::string normalizeDialectOverride(const std::string& text) {
    const auto kind = dialectKindFromText(text);
    return kind ? std::string(dialectKindToText(*kind)) : std::string();
}

} // namespace

// ---------- 静态工具 ----------

std::string ProfileRepository::sha256HexPrefix(const std::string& s) {
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

std::string ProfileRepository::makeApiKeyHint(const std::string& apiKey) {
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

// ---------- 两段写助手（CPX-07）----------

TwoPhaseKeyWrite::TwoPhaseKeyWrite(std::string keychainService,
                                   std::string account, std::string oldKey)
    : service_(std::move(keychainService)),
      account_(std::move(account)),
      oldKey_(std::move(oldKey)) {}

TwoPhaseKeyWrite TwoPhaseKeyWrite::capture(const std::string& keychainService,
                                           const std::string& account) {
    auto old = platform::keychain::loadSecret(keychainService, account);
    return TwoPhaseKeyWrite(keychainService, account,
                            old.isOk() ? old.value() : std::string());
}

core::Result<void> TwoPhaseKeyWrite::writeKey(const std::string& newKey) {
    auto r = newKey.empty()
        ? platform::keychain::deleteSecret(service_, account_)
        : platform::keychain::storeSecret(service_, account_, newKey);
    if (r.isFail()) {
        // 段一失败也可能已破坏旧值 → 尽力回写。CP-11 后 Windows 侧 storeSecret
        // 走 core::atomicWriteFile（tmp 全量写成功才做覆盖式替换，原有「trunc 写
        // 留半截文件」通道已消除），但替换阶段（remove 后 rename）失败仍会丢旧值；
        // POSIX 兜底仍是 ofstream 原地覆盖写、macOS 是先删后加——三者都不承诺
        // 失败时旧值完好，故回写照做
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
        compensate();   // 段二失败 → Keychain 回到旧值
    }
    return r;
}

void TwoPhaseKeyWrite::compensate() {
    if (!keyTouched_) return;   // 没改过（或已回写）→ 无需动作
    auto r = oldKey_.empty()
        ? platform::keychain::deleteSecret(service_, account_)
        : platform::keychain::storeSecret(service_, account_, oldKey_);
    if (r.isFail()) {
        // 保留 keyTouched_：Keychain 还没回到旧值（密钥可能不可用）
        LOG_ERROR("两段写补偿：回写 Keychain 旧值失败（account=" + account_ +
                  "）: " + r.errorMessage());
        return;
    }
    keyTouched_ = false;
}

// ---------- 构造 / 行解析 ----------

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
    auto acct = row.getString("keychain_account");
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
    if (acct.isOk()) p.keychainAccount = acct.value();
    if (model.isOk()) p.model = model.value();
    if (modelsCfg.isOk()) p.modelsConfigJson = modelsCfg.value();
    if (extraEnv.isOk()) p.extraEnvJson = extraEnv.value();
    if (extraSettings.isOk()) p.extraSettingsJson = extraSettings.value();
    if (sharedCfg.isOk()) p.sharedConfigId = sharedCfg.value();
    // 读侧规范化：未知/空值一律兜底 both（设计 D-8 双保险之一）
    if (authField.isOk()) {
        p.authField = authFieldToText(authFieldFromText(authField.value()));
    }
    // 读侧规范化：未知/空值一律按「未覆盖」读回空串（同 authField 口径，CPX-13）
    if (dialectOverride.isOk()) {
        p.dialectOverride = normalizeDialectOverride(dialectOverride.value());
    }
    if (fallbackProfileId.isOk()) p.fallbackProfileId = fallbackProfileId.value();
    if (active.isOk()) p.isActive = (active.value() != 0);
    if (created.isOk()) p.createdAt = static_cast<std::time_t>(created.value());
    if (updated.isOk()) p.updatedAt = static_cast<std::time_t>(updated.value());

    p.apiKeyPlain.clear();   // 默认永远为空
    return core::Result<Profile>::ok(std::move(p));
}

// ---------- Profile CRUD（R10-A2：策略尾在门面）----------

core::Result<std::vector<Profile>> ProfileRepository::listProfiles() {
    auto prep = db_.prepare(
        "SELECT id, name, base_url, api_key_hint, api_key_check, "
        "keychain_account, model, models_config, extra_env, extra_settings_json, "
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
        "keychain_account, model, models_config, extra_env, extra_settings_json, "
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
    const std::string cleanedAuthField =
        authFieldToText(authFieldFromText(authField));   // 写侧规范化（D-8）
    // 写侧规范化（CPX-13）：非法值 → 空串（未覆盖）
    const std::string cleanedDialectOverride =
        normalizeDialectOverride(dialectOverride);
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
    stmt.bind(5, std::string(""));   // 插入后用 UPDATE 回填真实 account
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

    // 把 INSERT ... RETURNING 步进到 DONE 释放连接上的 in-progress 状态：
    // 语句停在结果行时 COMMIT 会被 SQLite 拒绝（"cannot commit transaction -
    // SQL statements in progress"）——门面首建自动激活路径里 setActiveProfile
    // 的事务在本连接上会直接撞上它（产品审计 2026-09-23 [6] 实测触发）。
    // DONE 后语句自动 reset，不再阻塞任何事务；失败极罕见且行已写入，只 WARN
    auto drain = stmt.step();
    if (drain.isFail()) {
        LOG_WARN("INSERT RETURNING 收尾 step 失败（行已写入，继续）: " +
                 drain.errorMessage());
    }

    // 两段写（CPX-07）：段一 Keychain（account 由自增 id 生成，旧值必为空 =
    // 该条目尚不存在）→ 段二 DB 回填 keychain_account。任一段失败都回滚本次
    // INSERT 的行（恢复「没建过」的原状），Keychain 侧由助手按旧值回写
    // （旧值为空 = 删除条目，不留半截密钥）。
    const std::string account = keychainAccountFor(newId);
    TwoPhaseKeyWrite keyTx(keychainService_, account, std::string());
    // 回滚本次 INSERT；回滚本身再失败会留下无密钥的幽灵 profile 行，
    // 至少留 ERROR 痕迹可诊断（审计 2026-09-23 [25]）
    const auto rollbackInsertedRow = [&]() {
        auto delPrep = db_.prepare("DELETE FROM claude_profiles WHERE id = ?");
        if (delPrep.isOk()) {
            auto& delStmt = delPrep.value();
            delStmt.bind(1, newId);
            auto delStep = delStmt.step();
            if (delStep.isFail()) {
                LOG_ERROR("createProfile 失败后的回滚 DELETE 也失败，存在悬空 "
                          "profile 行 id=" + std::to_string(newId) + ": " +
                          delStep.errorMessage());
            }
        } else {
            LOG_ERROR("createProfile 失败后的回滚 DELETE prepare 失败，存在悬空 "
                      "profile 行 id=" + std::to_string(newId) + ": " +
                      delPrep.errorMessage());
        }
    };

    auto kcStore = keyTx.writeKey(apiKey);
    if (kcStore.isFail()) {
        rollbackInsertedRow();
        return core::Result<Profile>::fail(kcStore.errorCode(),
            "Keychain 存储失败: " + kcStore.errorMessage());
    }

    // 段二：回填 keychain_account
    auto backfill = keyTx.writeDb([&]() -> core::Result<void> {
        auto updPrep = db_.prepare(
            "UPDATE claude_profiles SET keychain_account = ? WHERE id = ?");
        if (updPrep.isFail()) {
            return core::Result<void>::fail(updPrep.errorCode(),
                                            updPrep.errorMessage());
        }
        auto& updStmt = updPrep.value();
        updStmt.bind(1, account);
        updStmt.bind(2, newId);
        auto uStep = updStmt.step();
        if (uStep.isFail()) {
            return core::Result<void>::fail(uStep.errorCode(),
                                            uStep.errorMessage());
        }
        return core::Result<void>::ok();
    });
    if (backfill.isFail()) {
        // 行缺 keychain_account = 密钥不可达，与段一失败同等对待 → 一并回滚
        rollbackInsertedRow();
        return core::Result<Profile>::fail(backfill.errorCode(),
                                           backfill.errorMessage());
    }

    LOG_INFO("已创建 Claude profile id=" + std::to_string(newId) +
             " name=" + cleanedName);

    // 返回完整 Profile（R10-A2：首建自动激活策略在门面，本类 is_active 恒
    // false——INSERT 字面 0；激活与否由门面按 activateIfNoneActive 回填）
    Profile out;
    out.id = newId;
    out.name = cleanedName;
    out.baseUrl = cleanedUrl;
    out.apiKeyHint = hint;
    out.apiKeyCheck = check;
    out.keychainAccount = account;
    out.model = cleanedModel;
    out.sharedConfigId = sharedConfigId;
    out.authField = cleanedAuthField;   // 必须带上：二段补写以本返回值为基
    out.dialectOverride = cleanedDialectOverride;   // 同上（CPX-13）
    out.fallbackProfileId = fallbackProfileId;
    out.extraSettingsJson = extraSettingsJson;      // 同上（复制即配置口径）
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
    // 设计 §3.7 校验：拒自引用（fallback_profile_id == 自身 = 纯浪费一跳）。
    // 与「A→B / B→A 的一跳环」是两回事——环无害（备用各指对方），不拦
    if (profile.fallbackProfileId == profile.id) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "备用 Profile 不能是自身");
    }

    // 查旧 profile（用于回滚 Keychain；oldProfile 出参供门面的 re-takeover
    // 策略尾判断「片段绑定是否变化 / 旧行是否激活」）
    auto old = getProfile(profile.id);
    if (old.isFail()) {
        return core::Result<void>::fail(old.errorCode(), old.errorMessage());
    }
    if (oldProfile) *oldProfile = old.value();

    const std::string cleanedUrl = stripTrailingSlash(trim(profile.baseUrl));
    const std::string cleanedName = trim(profile.name);
    const std::time_t now = nowUnix();
    const std::string cleanedAuthField =
        authFieldToText(authFieldFromText(profile.authField));   // 写侧规范化（D-8）
    // 写侧规范化（CPX-13）：非法值 → 空串（未覆盖）
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
        // 两段写（CPX-07）：段一 Keychain（旧值 = 当前密钥，先捕获）→ 段二 DB。
        // 段二失败时按旧值回写 Keychain——原实现没有补偿，留下「密钥已换、
        // api_key_hint / api_key_check 没换」的不一致态，事后无从判断。
        TwoPhaseKeyWrite keyTx = TwoPhaseKeyWrite::capture(
            keychainService_, old.value().keychainAccount);
        auto kcStore = keyTx.writeKey(profile.apiKeyPlain);
        if (kcStore.isFail()) {
            return core::Result<void>::fail(kcStore.errorCode(),
                "Keychain 更新失败: " + kcStore.errorMessage());
        }

        auto stepR = keyTx.writeDb([&]() -> core::Result<void> {
            auto prep = db_.prepare(
                "UPDATE claude_profiles SET "
                "name = ?, base_url = ?, api_key_hint = ?, api_key_check = ?, "
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
            stmt.bind(3, newHint);
            stmt.bind(4, newCheck);
            stmt.bind(5, cleanedModel);
            stmt.bind(6, profile.modelsConfigJson);
            stmt.bind(7, profile.extraEnvJson);
            stmt.bind(8, profile.extraSettingsJson);
            stmt.bind(9, profile.sharedConfigId);
            stmt.bind(10, cleanedAuthField);
            stmt.bind(11, cleanedDialectOverride);
            stmt.bind(12, profile.fallbackProfileId);
            stmt.bind(13, static_cast<int64_t>(now));
            stmt.bind(14, profile.id);
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
        // 只更新 name / url / model / 片段绑定 / 备用
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

    // R10-A2 段级拆分：re-takeover 策略尾
    //（片段绑定变化 && 旧行激活 && 代理运行 → 立即重写 live）留门面——
    // 此刻旧行经 oldProfile 出参回填给门面判断，本类不编排接管
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

    // 两段写（CPX-07）：段一清 Keychain（旧值先捕获）→ 段二删 SQLite 行。
    // 顺序不能反：先删行会让 Keychain 失败时留下永久孤儿密钥。段二失败时按
    // 旧值回写 Keychain（原实现无补偿：密钥已删、行还在，用户重试时密钥已丢）。
    TwoPhaseKeyWrite keyTx = TwoPhaseKeyWrite::capture(
        keychainService_, p.value().keychainAccount);
    auto kcDel = keyTx.writeKey(std::string());   // 空串 = 删除该条目
    if (kcDel.isFail()) {
        LOG_ERROR("清理 Keychain 失败，中止删除 profile id=" +
                  std::to_string(id) + ": " + kcDel.errorMessage());
        return core::Result<void>::fail(
            kcDel.errorCode(),
            "清理 Keychain 失败，profile 未删除: " + kcDel.errorMessage());
    }

    // 段二：删 SQLite 行（FK CASCADE 会级联清空该 profile 的调用日志）。
    // 同事务先解绑「备用指向它」的 profile（设计 §3.7：不留悬空 fallback id，
    // 否则其它 profile 的 failover 静默落到已删 profile → resolveFallbackProfile
    // 容错 nullopt 也是「不换家」，但库里留着悬空 id 难看且删除可重试时重复）
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

    // R10-A2 段级拆分：停代理策略尾
    //（删的是激活 profile 且代理运行中 → stopProxy，其内部 syncRestore 还原
    // ~/.claude/settings.json，settings.json 不能继续指向无激活 profile 的代理）
    // 留门面——wasActive 经出参回填给门面判断，本类不编排代理生命周期
    return core::Result<void>::ok();
}

core::Result<int> ProfileRepository::cleanupOrphanKeychainEntries() {
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

core::Result<std::string> ProfileRepository::loadApiKey(int64_t profileId) {
    auto p = getProfile(profileId);
    if (p.isFail()) {
        return core::Result<std::string>::fail(p.errorCode(), p.errorMessage());
    }
    return platform::keychain::loadSecret(keychainService_,
                                          p.value().keychainAccount);
}

// ---------- 激活 ----------

core::Result<Profile> ProfileRepository::getActiveProfile() {
    auto prep = db_.prepare(
        "SELECT id, name, base_url, api_key_hint, api_key_check, "
        "keychain_account, model, models_config, extra_env, extra_settings_json, "
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
        // 产品审计 2026-09-23 [6]：错误 message 补可操作指引——503 的 JSON
        // body 原样透传此文案（proxy_router 的 writeNoProfileError 是唯一出口，
        // CPX-10 后缓冲 / 流式两条路径共用它、不再内联副本），用户在 Claude Code
        // 侧即可看到去 DevDesk 启用的路径，不用再猜原因
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
    // R10-A2：setActiveProfile 的 DB 段（is_active 全局唯一）。调用方（门面
    // setActiveProfile）负责前置校验（id / 存在性）与捕获编排
    //（syncCaptureActiveProfile 必须先于本事务发生，否则会把自己刚写进去的
    // 托管值当用户值捕获）。本方法只做「清零 → 置位」
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

// ---------- Profile 复制（设计 §4.2/§4.3）----------

core::Result<Profile> ProfileRepository::duplicateProfile(int64_t srcId) {
    auto srcR = getProfile(srcId);
    if (srcR.isFail()) {
        return core::Result<Profile>::fail(srcR.errorCode(),
                                          srcR.errorMessage());
    }
    const Profile src = std::move(srcR).value();

    // 副本命名（§4.2）：name + " 副本"，重名递增避让既有 UNIQUE 约束；
    // 100 次仍冲突 → 返回 fail 且不建行（不空转死循环）
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

    // 源 key 缺失（Keychain 孤儿）→ fail：不建无 key 的副本
    //（与 createProfile 的「apiKey 必填」约束一致）
    auto keyR = loadApiKey(srcId);
    if (keyR.isFail() || keyR.value().empty()) {
        return core::Result<Profile>::fail(core::ErrorCode::InvalidApiKey,
            "源 Profile 的密钥缺失，无法复制");
    }

    // 一行 SQLite + 一条 Keychain（account 自动为 claude:<新 id>，与源不同）；
    // 副本不激活（S-4）→ 不触发 setActiveProfile、也不触发反向同步。
    // 片段绑定在 createProfile 处一并带上（设计 §2.6：它属于配置）——若留到
    // 二段补写才设，二段条件（models/extra_env 非空）不成立时就会丢掉绑定；
    // fallback_profile_id 同样（设计 §3.7：备用属于配置，复制 = 拿到同一套）；
    // extra_settings_json 同样经 createProfile 尾参携带（advanced-json-editor
    // 设计 §3.3：与 dialect_override「复制即配置」同款口径）
    const std::string defModel =
        effectiveModelsConfig(src.modelsConfigJson, src.model).def.model;
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

    // 二段补写 models_config / extra_env / extra_settings_json（同 UI 新建流程：
    // createProfile 只带得走 default 模型名）。门槛条件（审计 F-3 必修）：仅
    // JSON profile（两个 env 字段均空、仅 extraSettingsJson 非空）复制不得因
    // 门槛不成立而静默丢字段
    if (!src.modelsConfigJson.empty() || !src.extraEnvJson.empty() ||
        !src.extraSettingsJson.empty()) {
        Profile upd = out;
        upd.modelsConfigJson = src.modelsConfigJson;
        upd.extraEnvJson = src.extraEnvJson;
        upd.extraSettingsJson = src.extraSettingsJson;
        upd.model = defModel;
        auto u = updateProfile(upd);
        if (u.isFail()) {
            // 回滚（§4.3）：deleteProfile 先清 Keychain 再删行
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

// ---------- 共享配置片段（设计 §2.1/§2.4/§2.6）----------

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
    // 信任边界：不把坏 JSON 落库（读侧另有容错）。（v11 过渡注：片段内容自
    // advanced-json-editor §8 起为高级属性 JSON 原文，本处校验随 §8.1 切换到
    // validateExtraSettingsJson 同一套规则；过渡期 env 域 UI 仍经此写）
    if (!extraSettingsJson.empty()) {
        auto ee = parseExtraEnv(extraSettingsJson);
        if (ee.isFail()) {
            return core::Result<SharedConfig>::fail(ee.errorCode(),
                "共享配置片段 JSON 非法: " + ee.errorMessage());
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

    // 步进到 DONE 释放连接上的 in-progress 状态（同 createProfile 的 [7]：
    // 语句停在结果行时，随后在同一连接上开事务会被 SQLite 拒绝）
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
        auto ee = parseExtraEnv(config.extraSettingsJson);
        if (ee.isFail()) {
            return core::Result<void>::fail(ee.errorCode(),
                "共享配置片段 JSON 非法: " + ee.errorMessage());
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

    // R10-A2 段级拆分：re-takeover 策略尾（改了就生效——代理在跑且激活 profile
    // 绑定了它 → 立即重写 settings.json，否则 live 会留旧片段值到下次切换）留
    // 门面，本类不编排接管
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

    // 删除前先看激活 profile 是否绑定它：只有它的键在 live 里（片段值由绑定它
    // 的那个 profile 的接管写入），故只有这种情况删完需要重写 settings.json
    //（activeBound 出参供门面 re-takeover 策略尾判断）
    auto active = getActiveProfile();
    const bool activeBoundVal = active.isOk() && active.value().sharedConfigId == id;
    if (activeBound) *activeBound = activeBoundVal;

    // 同事务解绑 + 删行（设计 §2.6）：不留悬空 shared_config_id；不做级联删
    // profile（删片段 = 交回控制权，不是丢配置）
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

    // R10-A2 段级拆分：re-takeover 策略尾（解绑当场生效——代理在跑且激活
    // profile 绑定了它 → 重写 live（片段键撤走、与片段同名的 profile 自身值
    // 恢复生效））留门面，本类不编排接管
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

core::Result<std::vector<std::string>> ProfileRepository::sharedEnvKeysFor(
    int64_t profileId) {
    auto p = getProfile(profileId);
    if (p.isFail()) {
        return core::Result<std::vector<std::string>>::fail(p.errorCode(),
                                                            p.errorMessage());
    }
    std::vector<std::string> keys;
    const std::string json = sharedExtraSettingsJsonFor(p.value().sharedConfigId);
    if (json.empty()) {
        return core::Result<std::vector<std::string>>::ok(std::move(keys));
    }
    // 键集口径与写侧一致（applyEnvAliases 归一后）：live 里的片段键就是这个形状
    auto parsed = parseExtraEnv(json);
    if (parsed.isOk()) {
        for (const auto& [k, v] : applyEnvAliases(parsed.value()).env) {
            keys.push_back(k);
        }
    }
    // JSON 非法 → 空集（读路径容错：宁可少挡几个键，不让整次切换失败）
    return core::Result<std::vector<std::string>>::ok(std::move(keys));
}

} // namespace module::claude