// src/module_claude_proxy/quota/quota_store.cpp
// 余量配置 / 结果两表的 CRUD + windows_json 单点编解码（契约见 quota_store.h）。
// 模式同 agent_store.cpp：db_.mutex() 锁内 prepare → bind → step → fetchAll，逐个检查并透传失败。
#include "quota_store.h"

#include <mutex>
#include <optional>
#include <utility>

#include <sqlite3.h>   // sqlite3_changes（DELETE 影响行数）

#include "platform/credential_protection.h"
#include "platform/json.h"

#include "quota_error_text.h"

namespace module::claude::quota {

using core::ErrorCode;

namespace {

namespace pj = platform::json;

// 收拢 bind 链的小 helper（Result<void> 没有 operator&&，链式写法编译不过）。
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
    "profile_id, provider_id, extra_json, secrets_json, ccswitch_provider_id, "
    "enabled, auto_refresh, interval_minutes";

constexpr const char* kResultColumns =
    "profile_id, kind, windows_json, balance_remaining, currency, plan_name, "
    "fetched_at, last_success_at, last_error, last_error_kind";

// 行 → 结构。任一列取不出值即 fail（不用 valueOr 兜默认值：会把「列名拼错」伪装成「值就是默认值」）。
core::Result<QuotaConfigRow> configRowFrom(const core::data::SqlRow& row) {
    QuotaConfigRow out;
    int64_t enabled = 0, autoRefresh = 0, interval = 0;
    if (auto e = rowInt(row, "profile_id", out.profileId)) return core::Result<QuotaConfigRow>::fail(e->first, e->second);
    if (auto e = rowStr(row, "provider_id", out.providerId)) return core::Result<QuotaConfigRow>::fail(e->first, e->second);
    if (auto e = rowStr(row, "extra_json", out.extraJson)) return core::Result<QuotaConfigRow>::fail(e->first, e->second);
    if (auto e = rowStr(row, "secrets_json", out.secretsJson)) return core::Result<QuotaConfigRow>::fail(e->first, e->second);
    if (auto e = rowStr(row, "ccswitch_provider_id", out.ccswitchProviderId)) return core::Result<QuotaConfigRow>::fail(e->first, e->second);
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
    int64_t fetchedAt = 0, lastSuccessAt = 0;
    if (auto e = rowInt(row, "profile_id", out.profileId)) return core::Result<QuotaResultRow>::fail(e->first, e->second);
    if (auto e = rowStr(row, "kind", kindText)) return core::Result<QuotaResultRow>::fail(e->first, e->second);
    if (auto e = rowStr(row, "windows_json", windowsJson)) return core::Result<QuotaResultRow>::fail(e->first, e->second);
    auto bd = row.getDouble("balance_remaining");
    if (bd.isFail()) return core::Result<QuotaResultRow>::fail(bd.errorCode(), bd.errorMessage());
    balance = bd.value();
    if (auto e = rowStr(row, "currency", out.currency)) return core::Result<QuotaResultRow>::fail(e->first, e->second);
    if (auto e = rowStr(row, "plan_name", out.planName)) return core::Result<QuotaResultRow>::fail(e->first, e->second);
    if (auto e = rowInt(row, "fetched_at", fetchedAt)) return core::Result<QuotaResultRow>::fail(e->first, e->second);
    if (auto e = rowInt(row, "last_success_at", lastSuccessAt)) return core::Result<QuotaResultRow>::fail(e->first, e->second);
    if (auto e = rowStr(row, "last_error", lastError)) return core::Result<QuotaResultRow>::fail(e->first, e->second);
    if (auto e = rowStr(row, "last_error_kind", lastErrorKindText)) return core::Result<QuotaResultRow>::fail(e->first, e->second);

    out.balanceRemaining = balance;
    out.fetchedAt = fetchedAt;
    out.lastSuccessAt = lastSuccessAt;
    out.lastError = std::move(lastError);
    out.lastErrorKind = quotaErrorFromName(lastErrorKindText);

    // kind 列空串 = 从未成功过（不变量 4）。不回落成 Tiered：会把「没查成」当成「查到了、窗口全 0」。
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

std::string encodeQuotaWindows(const std::vector<QuotaWindow>& windows) {
    // platform::json 没有对象数组 push 口径，故逐元素 stringify → 逗号拼接 → parse 一次自检；拼装出错当场返回 "[]"，绝不把坏串写进库。
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
        // 畸形 usedPercent → 丢该元素，不是采纳默认值：留默认 0.0 会被画成「几乎没用满」，正是本模块要杜绝的假 0%。
        // 越界同理（拒而非 clamp 后断言落在 [0,100]，那是恒真断言）。
        if (!pct.isNumber()) continue;              // 缺字段 / 类型错（含字符串 "62.5"）
        // 数字双分支读取：Int 与 Double 都映射成 T_Number，但 parse 的 Int 分支只填 intVal、dblVal 留 0，
        // 只读 asDouble() 会把整数百分比读成 0%（又一条假 0% 的路）。为 0 时回退 asInt；小数原样不截断。
        const double dbl = pct.asDouble();
        const double v = dbl != 0.0 ? dbl : static_cast<double>(pct.asInt());
        if (!(v >= 0.0 && v <= 100.0)) continue;   // 越界（250 / -5）→ 丢，不 clamp
        w.usedPercent = v;
        pj::JsonValue resets = obj.get("resetsAt");
        if (resets.isString()) {
            w.resetsAt = resets.asString();
        }
        out.push_back(std::move(w));
    }
    return core::Result<std::vector<QuotaWindow>>::ok(std::move(out));
}

const char* quotaErrorName(QuotaError error) {
    switch (error) {
        case QuotaError::None: return "none";
        case QuotaError::Auth: return "auth";
        case QuotaError::Entitlement: return "entitlement";
        case QuotaError::Shape: return "shape";
        case QuotaError::Transient: return "transient";
        case QuotaError::NotConfigured: return "not_configured";
        // 哨兵（取值个数，不是错误），不该被调用；真被调用说明上游造了越界值，按与下方 return 同一套保守兜底处理。
        case QuotaError::Count: break;
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
    auto row = configRowFrom(rows.value()[0]);
    if (row.isFail()) return row;
    // secrets_json 读侧解保护：与 api_key_encrypted 复用同一对 platform::credential 函数（POSIX 恒等，Windows = DPAPI 密文 hex）。
    // 解不开（换机器 / 换用户 / 列被篡改）→ fail(原错误码)，不静默回落空串——否则会伪装成「用户没配 secret」误导排查。
    auto secrets = platform::credential::unprotect(row.value().secretsJson);
    if (secrets.isFail()) {
        return core::Result<QuotaConfigRow>::fail(secrets.errorCode(),
                                                  secrets.errorMessage());
    }
    row.value().secretsJson = std::move(secrets.value());
    return row;
}

core::Result<void> QuotaStore::saveConfig(const QuotaConfigRow& row) {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    // secrets_json 写侧加保护：与 api_key_encrypted 复用同一对 platform::credential 函数（Windows = DPAPI 加密），与兄弟列保护级别一致。
    // 失败（DPAPI 出错）在建/改行之前失败，不留「行写好了但 secret 没加密」的中间态。
    auto stored = platform::credential::protect(row.secretsJson);
    if (stored.isFail()) {
        return core::Result<void>::fail(stored.errorCode(), stored.errorMessage());
    }
    auto st = db_.prepare(
        "INSERT OR REPLACE INTO profile_quota_config "
        "(profile_id, provider_id, extra_json, secrets_json, "
        " ccswitch_provider_id, enabled, auto_refresh, interval_minutes) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
    if (st.isFail()) return core::Result<void>::fail(st.errorCode(), st.errorMessage());
    auto& stmt = st.value();
    auto b = bindAll(stmt, row.profileId, row.providerId, row.extraJson,
                     stored.value(), row.ccswitchProviderId,
                     static_cast<int64_t>(row.enabled ? 1 : 0),
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
    // 「删到了没有」用 sqlite3_changes，不是 step() 的返回值（后者对 DELETE 恒为 false，与删没删到无关）；持锁覆盖 step + changes，计数不被并发插队。
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
    // 锁必须从第一行就持：「读回上次成功值 → 搬进新行 → 写回」是复合操作，锁只覆盖写就是非原子——
    // 手动刷出的新数据会被一次网络抖动无声覆盖回旧值 = 静默丢数据。db_.mutex() 是 recursive_mutex，重入安全。
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());

    // 韧性决策（不变量 3）：成功才写数据列；失败保留上次成功的值，只写错误两列。从未成功过时数据列保持初始值（windows 空 / kind 空）。
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
        // 数据时间只在成功时推进：Transient 失败后数据仍是 interval 之前的，推进会得到「15 分钟前的数字 + 刚刚查询过」的矛盾。定时器用 fetched_at。
        row.lastSuccessAt = fetchedAt;
    } else {
        // 失败：读回上次成功的行原样搬运。读不到 / 读失败都按「无旧值」处理，且不阻断错误落库（错误信息比旧值更重要）。
        auto prev = loadResult(profileId);
        if (prev.isOk()) {
            const QuotaResultRow& p = prev.value();
            row.hasEverSucceeded = p.hasEverSucceeded;
            row.kind = p.kind;
            row.windows = p.windows;
            row.balanceRemaining = p.balanceRemaining;
            row.currency = p.currency;
            row.planName = p.planName;
            row.lastSuccessAt = p.lastSuccessAt;   // 数据时间不动
        }
        row.lastError = result.errorMessage;
        row.lastErrorKind = result.error;
        // 代码保证 last_error 有错时恒非空：provider 给了空 errorMessage 就补 headline 文案（空文案 = UI 上什么都没发生）。
        // 这是兜底，读侧（quota_home_view.cpp）还会再兜一次，让旧行也显示得出话。
        if (row.lastError.empty()) {
            row.lastError = quotaErrorHeadline(result.error, std::string());
        }
    }

    auto st = db_.prepare(
        "INSERT OR REPLACE INTO profile_quota_result "
        "(profile_id, kind, windows_json, balance_remaining, currency, plan_name, "
        " fetched_at, last_success_at, last_error, last_error_kind) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (st.isFail()) return core::Result<void>::fail(st.errorCode(), st.errorMessage());
    auto& stmt = st.value();
    // kind 只在成功时落非空值；从未成功过写空串（没有「查到了、窗口全 0」的假行混进库里）。
    const std::string kindText =
        row.hasEverSucceeded ? std::string(quotaKindName(row.kind)) : std::string();
    const std::string windowsText =
        row.hasEverSucceeded ? encodeQuotaWindows(row.windows) : std::string();
    auto b = bindAll(stmt, row.profileId, kindText, windowsText,
                     row.balanceRemaining, row.currency, row.planName,
                     row.fetchedAt, row.lastSuccessAt, row.lastError,
                     std::string(quotaErrorName(row.lastErrorKind)));
    if (b.isFail()) return b;
    auto stepped = stmt.step();
    if (stepped.isFail()) {
        return core::Result<void>::fail(stepped.errorCode(), stepped.errorMessage());
    }
    return core::Result<void>::ok();
}

}  // namespace module::claude::quota
