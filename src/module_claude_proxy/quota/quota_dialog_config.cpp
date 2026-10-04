// quota_dialog_config.cpp —— 弹窗配置逻辑层实现（契约见同名头文件）
#include "quota_dialog_config.h"

#include <algorithm>
#include <sstream>

#include "platform/json.h"
#include "quota_registry.h"

namespace module::claude::quota {

namespace {

namespace pj = platform::json;

// 「不配置」选项 id（空串）：注册表里没有空串行，空 id 天然不可能命中 findProvider。
const QuotaProviderOption kNoneOption{"", "不配置（关闭余量查询）"};

// cc-switch 候选 → 字段值：只认 ProviderField::key 逐字相等，不做大小写折叠 / 归一（那是「猜」，
// AK/SK 猜错会拿错凭据发签名请求）。火山两字段与 CcSwitchProviderInfo 同名同义；其余三家 fields() 为空，本函数不会被调到。
bool ccSwitchValueFor(const CcSwitchProviderInfo& info, const std::string& key,
                      std::string* out) {
    if (key == "accessKeyId") {
        *out = info.accessKeyId;
        return !out->empty();
    }
    if (key == "secretAccessKey") {
        *out = info.secretAccessKey;
        return !out->empty();
    }
    return false;
}

// extra/secrets 的 JSON 编码（单点）。空 map → 空串（不是 "{}"）：空串是「不吃额外字段」的既有语义。
core::Result<std::string> encodeFieldMap(const std::map<std::string, std::string>& m) {
    if (m.empty()) return core::Result<std::string>::ok(std::string());
    pj::JsonObject obj;
    for (const auto& kv : m) obj.set(kv.first, pj::makeString(kv.second));
    return pj::stringify(pj::JsonValue(obj), /*pretty=*/false);
}

// 字符串 → 前 N + 后 M + 中间点（maskSecret 的唯一实现）
std::string maskMiddle(const std::string& s, std::size_t head, std::size_t tail,
                       const char* dots) {
    if (s.empty()) return std::string();
    if (s.size() <= head + tail) return std::string(s.size(), '*');
    return s.substr(0, head) + dots + s.substr(s.size() - tail);
}

}  // namespace

std::vector<QuotaProviderOption> quotaProviderOptions() {
    std::vector<QuotaProviderOption> out;
    out.push_back(kNoneOption);   // 索引 0 恒为「不配置」
    const std::size_t tableLen = quotaProviderTableLength();
    for (std::size_t i = 0; i < tableLen; ++i) {
        const QuotaProviderEntry& entry = kQuotaProviders[i];
        if (entry.id == nullptr) break;   // 哨兵 = 表尾
        if (entry.create == nullptr) continue;   // 无效行不进下拉
        const QuotaProvider* provider = findProvider(entry.id);
        if (provider == nullptr) continue;       // 工厂返 nullptr 的行同样不进
        out.push_back(QuotaProviderOption{provider->id(), provider->displayName()});
    }
    return out;
}

int quotaProviderOptionIndex(const std::string& providerId) {
    if (providerId.empty()) return 0;
    const std::vector<QuotaProviderOption> opts = quotaProviderOptions();
    for (std::size_t i = 1; i < opts.size(); ++i) {
        if (opts[i].id == providerId) return static_cast<int>(i);
    }
    return 0;   // 未命中 → 落到「不配置」，不静默留假选中
}

void syncQuotaDialogFields(QuotaDialogState& st) {
    // 旧值按 key 带走（切走再切回来不丢用户填的东西）
    std::map<std::string, QuotaDialogField> old;
    for (const QuotaDialogField& f : st.fields) old[f.spec.key] = f;

    st.fields.clear();
    if (st.providerIdx <= 0) return;   // 「不配置」→ 无字段

    const std::vector<QuotaProviderOption> opts = quotaProviderOptions();
    if (static_cast<std::size_t>(st.providerIdx) >= opts.size()) {
        st.providerIdx = 0;           // 越界（表变小 / 状态写坏）→ 归「不配置」
        return;
    }
    const QuotaProvider* provider = findProvider(opts[static_cast<std::size_t>(st.providerIdx)].id);
    if (provider == nullptr) return;   // 注册表查不到 → 空字段（不编造）

    for (const ProviderField& spec : provider->fields()) {
        QuotaDialogField f;
        f.spec = spec;
        const auto it = old.find(spec.key);
        if (it != old.end()) {
            f.value = it->second.value;
            f.fromCcSwitch = it->second.fromCcSwitch;
            f.sourceLabel = it->second.sourceLabel;
        }
        if (f.sourceLabel.empty()) {
            f.sourceLabel = f.fromCcSwitch ? "来自 cc-switch" : "手动填写";
        }
        st.fields.push_back(std::move(f));
    }
}

void openQuotaDialog(QuotaDialogState& st, int64_t profileId,
                     const std::string& baseUrl, const std::string& apiKey,
                     const QuotaConfigRow* existing) {
    st = QuotaDialogState{};
    st.open = true;
    st.profileId = profileId;
    st.profileBaseUrl = baseUrl;
    st.profileApiKey = apiKey;

    // 已有配置优先（用户上次选过什么就还他什么）；没有才按 base_url 探测。
    std::string providerId;
    std::string ccswitchId;
    std::map<std::string, std::string> extra, secrets;
    if (existing != nullptr) {
        providerId = existing->providerId;
        ccswitchId = existing->ccswitchProviderId;
        // 读回手填值（按 fields() 过滤，未知键丢弃：换供应商后旧键名没意义）
        const QuotaProvider* provider = findProvider(providerId);
        if (provider != nullptr) {
            const auto read = [](const std::string& json,
                                 std::map<std::string, std::string>* dst) {
                if (json.empty()) return;
                auto parsed = pj::parse(json);
                if (parsed.isFail() || !parsed.value().isObject()) return;
                pj::JsonObject obj = parsed.value().asObject();
                for (const std::string& key : obj.keys()) {
                    pj::JsonValue v = obj.get(key);
                    if (v.isString()) (*dst)[key] = v.asString();
                }
            };
            read(existing->extraJson, &extra);
            read(existing->secretsJson, &secrets);
            for (const ProviderField& spec : provider->fields()) {
                const auto e = extra.find(spec.key);
                const auto s = secrets.find(spec.key);
                const std::string& val = (s != secrets.end()) ? s->second
                                       : (e != extra.end()) ? e->second
                                                            : std::string();
                if (val.empty()) continue;
                QuotaDialogField f;
                f.spec = spec;
                f.value = val;
                f.sourceLabel = "手动填写";
                st.fields.push_back(std::move(f));
            }
        }
    } else {
        const QuotaProvider* detected = detectByBaseUrl(baseUrl);
        if (detected != nullptr) providerId = detected->id();
    }
    st.providerIdx = quotaProviderOptionIndex(providerId);
    syncQuotaDialogFields(st);   // 会按 provider 重排并回填 sourceLabel

    if (!ccswitchId.empty()) {
        st.ccSwitchLoaded = true;
        auto list = listCcSwitchProviders();
        st.ccSwitch = list;
        // 定位候选下标（已消失 → -1 = 未选，文案层解释）
        st.ccSwitchIdx = -1;
        for (std::size_t i = 0; i < list.providers.size(); ++i) {
            if (list.providers[i].id == ccswitchId) {
                st.ccSwitchIdx = static_cast<int>(i);
                break;
            }
        }
        selectQuotaCcSwitch(st, st.ccSwitchIdx);
    }
}

void loadQuotaCcSwitch(QuotaDialogState& st) {
    if (st.ccSwitchLoaded) return;   // 每帧 compose 都可能调到，只读一次
    st.ccSwitchLoaded = true;
    st.ccSwitch = listCcSwitchProviders();
    // 默认按 base_url 自动匹配（host+path 标签边界，伪造后缀不命中）；未命中 = -1，用户可改选。
    st.ccSwitchIdx = -1;
    if (st.ccSwitch.status != CcSwitchSourceStatus::Ok) return;
    const std::string matchedId =
        matchCcSwitchProviderByBaseUrl(st.profileBaseUrl);
    if (matchedId.empty()) return;
    for (std::size_t i = 0; i < st.ccSwitch.providers.size(); ++i) {
        if (st.ccSwitch.providers[i].id == matchedId) {
            st.ccSwitchIdx = static_cast<int>(i);
            break;
        }
    }
    selectQuotaCcSwitch(st, st.ccSwitchIdx);
}

void selectQuotaCcSwitch(QuotaDialogState& st, int idx) {
    if (idx < 0 || static_cast<std::size_t>(idx) >= st.ccSwitch.providers.size()) {
        st.ccSwitchIdx = -1;
        // 取消 cc-switch 来源：值保留（用户可能只想去掉引用），标注回「手动填写」
        for (QuotaDialogField& f : st.fields) {
            f.fromCcSwitch = false;
            f.sourceLabel = "手动填写";
        }
        return;
    }
    st.ccSwitchIdx = idx;
    const CcSwitchProviderInfo& info = st.ccSwitch.providers[static_cast<std::size_t>(idx)];
    for (QuotaDialogField& f : st.fields) {
        std::string v;
        if (ccSwitchValueFor(info, f.spec.key, &v)) {
            f.value = v;
            f.fromCcSwitch = true;
            f.sourceLabel = "来自 cc-switch";
        }
    }
}

core::Result<QuotaConfigRow> buildQuotaConfigRow(const QuotaDialogState& st,
                                                 std::string* error) {
    const auto fail = [&](const std::string& msg) {
        if (error != nullptr) *error = msg;
        return core::Result<QuotaConfigRow>::fail(core::ErrorCode::ValidationError, msg);
    };
    const std::vector<QuotaProviderOption> opts = quotaProviderOptions();
    if (st.providerIdx < 0 || static_cast<std::size_t>(st.providerIdx) >= opts.size()) {
        return fail("未选择供应商");
    }
    QuotaConfigRow row;
    row.profileId = st.profileId;
    row.providerId = opts[static_cast<std::size_t>(st.providerIdx)].id;   // 「不配置」= 空串
    row.enabled = !row.providerId.empty();
    row.autoRefresh = true;
    row.intervalMinutes = 15;

    const bool hasCcSwitchRef =
        st.ccSwitchIdx >= 0 &&
        static_cast<std::size_t>(st.ccSwitchIdx) < st.ccSwitch.providers.size();
    if (hasCcSwitchRef) {
        row.ccswitchProviderId =
            st.ccSwitch.providers[static_cast<std::size_t>(st.ccSwitchIdx)].id;
    }

    // 手填路径：按 fields() 的 secret 标记分流到 extra / secrets。
    const QuotaProvider* provider = findProvider(row.providerId);
    const std::vector<ProviderField> spec =
        (provider != nullptr) ? provider->fields() : std::vector<ProviderField>{};
    std::map<std::string, std::string> extra, secrets;
    for (const QuotaDialogField& f : st.fields) {
        // 未知键（provider 换了 / 状态写坏）不落库
        const bool known = std::any_of(
            spec.begin(), spec.end(),
            [&](const ProviderField& s) { return s.key == f.spec.key; });
        if (!known) continue;
        if (f.value.empty()) continue;
        (f.spec.secret ? secrets : extra)[f.spec.key] = f.value;
    }
    auto encExtra = encodeFieldMap(extra);
    if (encExtra.isFail()) return fail("字段编码失败：" + encExtra.errorMessage());
    auto encSecrets = encodeFieldMap(secrets);
    if (encSecrets.isFail()) return fail("密钥编码失败：" + encSecrets.errorMessage());
    row.extraJson = encExtra.value();
    row.secretsJson = encSecrets.value();   // 明文；protect 由 QuotaStore::saveConfig 做

    // 不变量 2：引用非空 → extra / secrets 一律丢弃（在编码后清空，作用在真可能被填过的值上）。
    if (hasCcSwitchRef) {
        row.extraJson.clear();
        row.secretsJson.clear();
    }
    if (error != nullptr) error->clear();
    return core::Result<QuotaConfigRow>::ok(std::move(row));
}

ProviderConfig buildQuotaTestConfig(const QuotaDialogState& st) {
    ProviderConfig cfg;
    const std::vector<QuotaProviderOption> opts = quotaProviderOptions();
    if (st.providerIdx > 0 && static_cast<std::size_t>(st.providerIdx) < opts.size()) {
        cfg.providerId = opts[static_cast<std::size_t>(st.providerIdx)].id;
    }
    cfg.baseUrl = st.profileBaseUrl;
    cfg.apiKey = st.profileApiKey;
    // cc-switch 候选选中时以候选为准（测的正是「保存后会用的那份凭据」）
    if (st.ccSwitchIdx >= 0 &&
        static_cast<std::size_t>(st.ccSwitchIdx) < st.ccSwitch.providers.size()) {
        const CcSwitchProviderInfo& info =
            st.ccSwitch.providers[static_cast<std::size_t>(st.ccSwitchIdx)];
        for (const QuotaDialogField& f : st.fields) {
            std::string v;
            if (ccSwitchValueFor(info, f.spec.key, &v)) cfg.fields[f.spec.key] = v;
        }
    }
    for (const QuotaDialogField& f : st.fields) {
        if (!f.value.empty()) cfg.fields[f.spec.key] = f.value;
    }
    return cfg;
}

std::string maskSecret(const std::string& plaintext) {
    return maskMiddle(plaintext, 4, 2, "••••");
}

std::string quotaTestMessage(const QuotaResult& result) {
    if (result.error == QuotaError::None) {
        std::ostringstream oss;
        if (result.kind == QuotaKind::Balance) {
            oss << "查询成功：余额 " << result.balanceRemaining;
            if (!result.currency.empty()) oss << " " << result.currency;
        } else {
            oss << "查询成功：" << result.windows.size() << " 个配额窗口";
            if (!result.planName.empty()) oss << "（" << result.planName << "）";
        }
        return oss.str();
    }
    const std::string head = quotaErrorHeadline(result.error, result.errorMessage);
    // 兜底：万一将来加了枚举忘加文案，这里仍给用户看得到的东西（空文案 = 什么都没发生）。
    return head.empty() ? ("查询失败（错误码 " +
                          std::to_string(static_cast<int>(result.error)) + "）")
                        : head;
}

}  // namespace module::claude::quota
