// src/module_claude_proxy/quota/quota_store.cpp
// 余量配置 / 结果两表的 CRUD + windows_json 单点编解码（契约见 quota_store.h）。
//
// 统一模式与 module_agent/agent_store.cpp 一致：db_.mutex() 锁内
// prepare → bind → step → fetchAll，逐个检查并透传失败，不静默跳过。
#include "quota_store.h"

#include <mutex>
#include <utility>

#include <sqlite3.h>   // sqlite3_changes（DELETE 影响行数）

#include "platform/json.h"

namespace module::claude::quota {

using core::ErrorCode;

namespace {

namespace pj = platform::json;

// 全参数非 optional 的语句用这个小 helper 收拢 bind 链（agent_store.cpp 同款：
// Result<void> 没有 operator&&，链式写法编译不过）。
template <typename... Args>
core::Result<void> bindAll(core::data::Statement& s, Args&&... args) {
    int index = 1;
    core::Result<void> r = core::Result<void>::ok();
    auto one = [&](auto&& v) {
        if (r.isOk()) {
            r = s.bind(index++, std::forward<decltype(v)>(v));
        }
    };
    (one(std::forward<Args>(args)), ...);
    return r;
}

using RowError = std::pair<ErrorCode, std::string>;

std::optional<RowError> rowInt(const core::data::SqlRow& row, const char* col,
                               int64_t& out) {
    auto v = row.getInt64(col);
    if (v.isFail()) return RowError{v.errorCode(), v.errorMessage()};
    out = v.value();
    return std::nullopt;
}

std::optional<RowError> rowStr(const core::data::SqlRow& row, const char* col,
                               std::string& out) {
    auto v = row.getString(col);
    if (v.isFail()) return RowError{v.errorCode(), v.errorMessage()};
    out = std::move(v.value());
    return std::nullopt;
}

// 两张表的 SELECT 列清单各自一份常量：列名与取值散成两份就是「改一处漏一处」。
constexpr const char* kConfigColumns =
    "profile_id, provider_id, extra_json, secrets_json, enabled, "
    "auto_refresh, interval_minutes";

constexpr const char* kResultColumns =
    "profile_id, kind, windows_json, balance_remaining, currency, plan_name, "
    "fetched_at, last_error, last_error_kind";

// 行 → 结构。任一列取不出值即 fail（不用 valueOr 兜默认值：那会把「列名拼错」
// 伪装成「值就是默认值」，SQL 改动时无声退化）。
core::Result<QuotaConfigRow> configRowFrom(const core::data::SqlRow& row) {
    QuotaConfigRow out;
    int64_t enabled = 0, autoRefresh = 0, interval = 0;
    if (auto e = rowInt(row, "profile_id", out.profileId)) return core::Result<QuotaConfigRow>::fail(e->first, e->second);
    if (auto e = rowStr(row, "provider_id", out.providerId)) return core::Result<QuotaConfigRow>::fail(e->first, e->second);
    if (auto e = rowStr(row, "extra_json", out.extraJson)) return core::Result<QuotaConfigRow>::fail(e->first, e->second);
    if (auto e = rowStr(row, "secrets_json", out.secretsJson)) return core::Result<QuotaConfigRow>::fail(e->first, e->second);
    if (auto e = rowInt(row, "enabled", enabled)) return core::Result<QuotaConfigRow>::fail(e->first, e->second);
    if (auto e = rowInt(row, "auto_refresh", autoRefresh)) return core::Result<QuotaConfigRow>::fail(e->first, e->second);
    if (auto e = rowInt(row, "interval_minutes", interval)) return core::Result<QuotaConfigRow>::fail(e->first, e->second);
    out.enabled = enabled != 0;
    out.autoRefresh = autoRefresh != 0;
    out.intervalMinutes = static_cast<int>(interval);
    return core::Result<QuotaConfigRow>::ok(std::move(out));
}

core::Result<QuotaResultRow> resultRowFrom(const core::data::SqlRow& row) {
    QuotaResultRow out;
    std::string kindText, windowsJson, lastError, lastErrorKindText;
    double balance = 0.0;
    int64_t fetchedAt = 0;
    if (auto e = rowInt(row, "profile_id", out.profileId)) return core::Result<QuotaResultRow>::fail(e->first, e->second);
    if (auto e = rowStr(row, "kind", kindText)) return core::Result<QuotaResultRow>::fail(e->first, e->second);
    if (auto e = rowStr(row, "windows_json", windowsJson)) return core::Result<QuotaResultRow>::fail(e->first, e->second);
    auto bd = row.getDouble("balance_remaining");
    if (bd.isFail()) return core::Result<QuotaResultRow>::fail(bd.errorCode(), bd.errorMessage());
    balance = bd.value();
    if (auto e = rowStr(row, "currency", out.currency)) return core::Result<QuotaResultRow>::fail(e->first, e->second);
    if (auto e = rowStr(row, "plan_name", out.planName)) return core::Result<QuotaResultRow>::fail(e->first, e->second);
    if (auto e = rowInt(row, "fetched_at", fetchedAt)) return core::Result<QuotaResultRow>::fail(e->first, e->second);
    if (auto e = rowStr(row, "last_error", lastError)) return core::Result<QuotaResultRow>::fail(e->first, e->second);
    if (auto e = rowStr(row, "last_error_kind", lastErrorKindText)) return core::Result<QuotaResultRow>::fail(e->first, e->second);

    out.balanceRemaining = balance;
    out.fetchedAt = fetchedAt;
    out.lastError = std::move(lastError);
    out.lastErrorKind = quotaErrorFromName(lastErrorKindText);

    // kind 列空串 = 从未成功过（不变量 4）。不回落成 Tiered：那会让 G7 把
    // 「没查成」当成「查到了、五个窗口全是 0」。
    if (!kindText.empty()) {
        out.hasEverSucceeded = true;
        out.kind = (kindText == "balance") ? QuotaKind::Balance : QuotaKind::Tiered;
        auto decoded = decodeQuotaWindows(windowsJson);
        if (decoded.isFail()) {
            return core::Result<QuotaResultRow>::fail(decoded.errorCode(),
                                                      decoded.errorMessage());
        }
        out.windows = std::move(decoded.value());
    }
    return core::Result<QuotaResultRow>::ok(std::move(out));
}

}  // namespace

// ============ windows_json 编解码（单点）============

std::string encodeQuotaWindows(const std::vector<QuotaWindow>& windows) {
    // platform::json **没有对象数组的 push 口径**（json.h:107-109 的 makeArray
    // 是空桩），故按 agent_loop.cpp joinBlockArray / llm_client.cpp
    // parseJoinedArray 同款路数组装：逐元素 stringify → 逗号拼接 → parse 一次
    // 自检。拼装层出错当场返回 "[]"，绝不把坏串写进库。
    std::string text = "[";
    for (size_t i = 0; i < windows.size(); ++i) {
        pj::JsonObject obj;
        obj.set("key", pj::makeString(windows[i].key));
        obj.set("usedPercent", pj::makeDouble(windows[i].usedPercent));
        obj.set("resetsAt", pj::makeString(windows[i].resetsAt));
        auto piece = pj::stringify(pj::JsonValue(obj));
        if (piece.isFail()) return std::string("[]");
        if (i > 0) text += ",";
        text += piece.value();
    }
    text += "]";
    auto selfCheck = pj::parse(text);
    if (selfCheck.isFail() || !selfCheck.value().isArray()) return std::string("[]");
    return text;
}

core::Result<std::vector<QuotaWindow>> decodeQuotaWindows(const std::string& json) {
    std::vector<QuotaWindow> out;
    if (json.empty()) {
        return core::Result<std::vector<QuotaWindow>>::ok(std::move(out));
    }
    auto parsed = pj::parse(json);
    if (parsed.isFail()) {
        // 容错回落空列表（展示面不因一行坏 JSON 崩）；已写过的 kind 不受影响。
        return core::Result<std::vector<QuotaWindow>>::ok(std::move(out));
    }
    pj::JsonValue v = parsed.value();
    if (!v.isArray()) {
        return core::Result<std::vector<QuotaWindow>>::ok(std::move(out));
    }
    for (int i = 0; i < v.size(); ++i) {
        pj::JsonValue item = v[i];
        if (!item.isObject()) continue;   // 元素非对象 → 跳过，不整表丢弃
        pj::JsonObject obj = item.asObject();
        QuotaWindow w;
        pj::JsonValue keyVal = obj.get("key");
        w.key = keyVal.isString() ? keyVal.asString() : std::string();
        if (w.key.empty()) continue;      // 无规范键的窗口无法呈现 → 跳过
        pj::JsonValue pct = obj.get("usedPercent");
        if (pct.isNumber()) {
            w.usedPercent = pct.asDouble();
        }
        pj::JsonValue resets = obj.get("resetsAt");
        if (resets.isString()) {
            w.resetsAt = resets.asString();
        }
        out.push_back(std::move(w));
    }
    return core::Result<std::vector<QuotaWindow>>::ok(std::move(out));
}

// ============ 枚举 ↔ 列文本 ============

const char* quotaErrorName(QuotaError error) {
    switch (error) {
        case QuotaError::None: return "none";
        case QuotaError::Auth: return "auth";
        case QuotaError::Entitlement: return "entitlement";
        case QuotaError::Shape: return "shape";
        case QuotaError::Transient: return "transient";
        case QuotaError::NotConfigured: return "not_configured";
    }
    return "transient";   // 枚举外的值不静默变成「无错」
}

QuotaError quotaErrorFromName(const std::string& name) {
    if (name.empty() || name == "none") return QuotaError::None;
    if (name == "auth") return QuotaError::Auth;
    if (name == "entitlement") return QuotaError::Entitlement;
    if (name == "shape") return QuotaError::Shape;
    if (name == "transient") return QuotaError::Transient;
    if (name == "not_configured") return QuotaError::NotConfigured;
    // 认不出的文本 → Transient（不回落 None：把失败读成成功是假象）
    return QuotaError::Transient;
}

const char* quotaKindName(QuotaKind kind) {
    return kind == QuotaKind::Balance ? "balance" : "tiered";
}

// ============ profile_quota_config ============

core::Result<QuotaConfigRow> QuotaStore::loadConfig(int64_t profileId) {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare(std::string("SELECT ") + kConfigColumns +
                          " FROM profile_quota_config WHERE profile_id = ?");
    if (st.isFail()) {
        return core::Result<QuotaConfigRow>::fail(st.errorCode(), st.errorMessage());
    }
    auto& stmt = st.value();
    auto b = stmt.bind(1, profileId);
    if (b.isFail()) {
        return core::Result<QuotaConfigRow>::fail(b.errorCode(), b.errorMessage());
    }
    auto rows = stmt.fetchAll();
    if (rows.isFail()) {
        return core::Result<QuotaConfigRow>::fail(rows.errorCode(), rows.errorMessage());
    }
    if (rows.value().empty()) {
        return core::Result<QuotaConfigRow>::fail(ErrorCode::NotFound,
                                                  "该 profile 未配置余量查询");
    }
    return configRowFrom(rows.value()[0]);
}

core::Result<void> QuotaStore::saveConfig(const QuotaConfigRow& row) {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare(
        "INSERT OR REPLACE INTO profile_quota_config "
        "(profile_id, provider_id, extra_json, secrets_json, enabled, "
        " auto_refresh, interval_minutes) VALUES (?, ?, ?, ?, ?, ?, ?)");
    if (st.isFail()) return core::Result<void>::fail(st.errorCode(), st.errorMessage());
    auto& stmt = st.value();
    auto b = bindAll(stmt, row.profileId, row.providerId, row.extraJson,
                     row.secretsJson, static_cast<int64_t>(row.enabled ? 1 : 0),
                     static_cast<int64_t>(row.autoRefresh ? 1 : 0),
                     static_cast<int64_t>(row.intervalMinutes));
    if (b.isFail()) return b;
    auto stepped = stmt.step();
    if (stepped.isFail()) {
        return core::Result<void>::fail(stepped.errorCode(), stepped.errorMessage());
    }
    return core::Result<void>::ok();
}

core::Result<void> QuotaStore::deleteConfig(int64_t profileId) {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare("DELETE FROM profile_quota_config WHERE profile_id = ?");
    if (st.isFail()) return core::Result<void>::fail(st.errorCode(), st.errorMessage());
    auto& stmt = st.value();
    auto b = stmt.bind(1, profileId);
    if (b.isFail()) return core::Result<void>::fail(b.errorCode(), b.errorMessage());
    auto stepped = stmt.step();
    if (stepped.isFail()) {
        return core::Result<void>::fail(stepped.errorCode(), stepped.errorMessage());
    }
    // 「删到了没有」用 sqlite3_changes，**不是** step() 的返回值——后者只是
    // 「这一步有没有取到一行」（SQLITE_ROW），对 DELETE 恒为 false，跟删没删
    // 到无关（agent_store.cpp:229-235 同款口径：持锁覆盖 step + changes，
    // 计数不会被并发语句插队）。
    if (sqlite3_changes(db_.handle()) == 0) {
        return core::Result<void>::fail(ErrorCode::NotFound,
                                        "该 profile 未配置余量查询");
    }
    return core::Result<void>::ok();
}

core::Result<std::vector<QuotaConfigRow>> QuotaStore::listConfigs() {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare(std::string("SELECT ") + kConfigColumns +
                          " FROM profile_quota_config ORDER BY profile_id");
    if (st.isFail()) {
        return core::Result<std::vector<QuotaConfigRow>>::fail(st.errorCode(),
                                                               st.errorMessage());
    }
    auto rows = st.value().fetchAll();
    if (rows.isFail()) {
        return core::Result<std::vector<QuotaConfigRow>>::fail(rows.errorCode(),
                                                               rows.errorMessage());
    }
    std::vector<QuotaConfigRow> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto mapped = configRowFrom(row);
        if (mapped.isFail()) {
            return core::Result<std::vector<QuotaConfigRow>>::fail(mapped.errorCode(),
                                                                   mapped.errorMessage());
        }
        out.push_back(std::move(mapped.value()));
    }
    return core::Result<std::vector<QuotaConfigRow>>::ok(std::move(out));
}

// ============ profile_quota_result ============

core::Result<QuotaResultRow> QuotaStore::loadResult(int64_t profileId) {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare(std::string("SELECT ") + kResultColumns +
                          " FROM profile_quota_result WHERE profile_id = ?");
    if (st.isFail()) {
        return core::Result<QuotaResultRow>::fail(st.errorCode(), st.errorMessage());
    }
    auto& stmt = st.value();
    auto b = stmt.bind(1, profileId);
    if (b.isFail()) {
        return core::Result<QuotaResultRow>::fail(b.errorCode(), b.errorMessage());
    }
    auto rows = stmt.fetchAll();
    if (rows.isFail()) {
        return core::Result<QuotaResultRow>::fail(rows.errorCode(), rows.errorMessage());
    }
    if (rows.value().empty()) {
        return core::Result<QuotaResultRow>::fail(ErrorCode::NotFound,
                                                  "该 profile 尚无余量结果");
    }
    return resultRowFrom(rows.value()[0]);
}

core::Result<void> QuotaStore::deleteResult(int64_t profileId) {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare("DELETE FROM profile_quota_result WHERE profile_id = ?");
    if (st.isFail()) return core::Result<void>::fail(st.errorCode(), st.errorMessage());
    auto& stmt = st.value();
    auto b = stmt.bind(1, profileId);
    if (b.isFail()) return core::Result<void>::fail(b.errorCode(), b.errorMessage());
    auto stepped = stmt.step();
    if (stepped.isFail()) {
        return core::Result<void>::fail(stepped.errorCode(), stepped.errorMessage());
    }
    if (sqlite3_changes(db_.handle()) == 0) {   // 同 deleteConfig：changes 才是删没删到
        return core::Result<void>::fail(ErrorCode::NotFound, "该 profile 尚无余量结果");
    }
    return core::Result<void>::ok();
}

core::Result<std::vector<QuotaResultRow>> QuotaStore::listResults() {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare(std::string("SELECT ") + kResultColumns +
                          " FROM profile_quota_result ORDER BY profile_id");
    if (st.isFail()) {
        return core::Result<std::vector<QuotaResultRow>>::fail(st.errorCode(),
                                                               st.errorMessage());
    }
    auto rows = st.value().fetchAll();
    if (rows.isFail()) {
        return core::Result<std::vector<QuotaResultRow>>::fail(rows.errorCode(),
                                                               rows.errorMessage());
    }
    std::vector<QuotaResultRow> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto mapped = resultRowFrom(row);
        if (mapped.isFail()) {
            return core::Result<std::vector<QuotaResultRow>>::fail(mapped.errorCode(),
                                                                   mapped.errorMessage());
        }
        out.push_back(std::move(mapped.value()));
    }
    return core::Result<std::vector<QuotaResultRow>>::ok(std::move(out));
}

core::Result<void> QuotaStore::applyFetchResult(int64_t profileId,
                                                const QuotaResult& result,
                                                int64_t fetchedAt) {
    // 韧性决策（不变量 3）：成功才写数据列；失败保留上次成功的值，只写错误两列。
    // 从未成功过时数据列保持初始值（windows 空 / balance 0 / kind 空），
    // 于是「失败且从未成功」在库里就是**可分辨**的一行，G7 不画 0% 条。
    QuotaResultRow row;
    row.profileId = profileId;
    row.fetchedAt = fetchedAt;
    if (result.error == QuotaError::None) {
        row.hasEverSucceeded = true;
        row.kind = result.kind;
        row.windows = result.windows;
        row.balanceRemaining = result.balanceRemaining;
        row.currency = result.currency;
        row.planName = result.planName;
        row.lastError.clear();
        row.lastErrorKind = QuotaError::None;
    } else {
        // 失败：先读回上次成功的那一行，把数据列**原样**搬过来。
        // 读不到行（真·从未成功过）或读失败都按「无旧值」处理 —— 读失败时
        // **不**阻断错误落库：错误信息比旧值更重要，不能因为读失败就丢掉。
        auto prev = loadResult(profileId);
        if (prev.isOk()) {
            const QuotaResultRow& p = prev.value();
            row.hasEverSucceeded = p.hasEverSucceeded;
            row.kind = p.kind;
            row.windows = p.windows;
            row.balanceRemaining = p.balanceRemaining;
            row.currency = p.currency;
            row.planName = p.planName;
        }
        row.lastError = result.errorMessage;
        row.lastErrorKind = result.error;
    }

    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare(
        "INSERT OR REPLACE INTO profile_quota_result "
        "(profile_id, kind, windows_json, balance_remaining, currency, plan_name, "
        " fetched_at, last_error, last_error_kind) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (st.isFail()) return core::Result<void>::fail(st.errorCode(), st.errorMessage());
    auto& stmt = st.value();
    // kind 只在成功时落非空值；从未成功过就写空串（这正是「不写假 0%」的形态：
    // 没有一条「查到了、窗口全 0」的假行混进库里）。
    const std::string kindText =
        row.hasEverSucceeded ? std::string(quotaKindName(row.kind)) : std::string();
    const std::string windowsText =
        row.hasEverSucceeded ? encodeQuotaWindows(row.windows) : std::string();
    auto b = bindAll(stmt, row.profileId, kindText, windowsText,
                     row.balanceRemaining, row.currency, row.planName,
                     row.fetchedAt, row.lastError,
                     std::string(quotaErrorName(row.lastErrorKind)));
    if (b.isFail()) return b;
    auto stepped = stmt.step();
    if (stepped.isFail()) {
        return core::Result<void>::fail(stepped.errorCode(), stepped.errorMessage());
    }
    return core::Result<void>::ok();
}

}  // namespace module::claude::quota
