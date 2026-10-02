// quota_dialog_config.cpp —— 弹窗配置逻辑层实现（契约见同名头文件）
#include "quota_dialog_config.h"

#include <algorithm>
#include <sstream>

#include "platform/json.h"
#include "quota_registry.h"

namespace module::claude::quota {

namespace {

namespace pj = platform::json;

// 「不配置」选项的固定 id（空串）。**不是**某个真实 provider 的 id ——
// 注册表里没有空串行（quota_registry.cpp 的哨兵 id == nullptr 是另一回事），
// 所以空 id 天然「配了也不可能命中 findProvider」。
const QuotaProviderOption kNoneOption{"", "不配置（关闭余量查询）"};

// cc-switch 候选 → 字段值的映射。**只认 ProviderField::key 逐字相等**，
// 不做大小写折叠 / 下划线-连字符归一 —— 那是「猜」，而 AK/SK 猜错的后果是
// 拿错误的凭据发签名请求（403 且用户看不懂）。
//
// 火山两字段（accessKeyId / secretAccessKey）在 cc-switch 的
// CcSwitchProviderInfo 里同名同义（ccswitch_source.h:56）；其余三家
// fields() 为空，本函数一次都不会被调到。
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

// extra/secrets 的 JSON 编码（**单点**，两侧都经它）。
// 空 map → 空串（不是 "{}"）：空串在库里读回来是「该供应商不吃额外字段」的
// 既有语义（quota_store.h 与 schema 注释都按空串表达）。
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

// ============ 供应商下拉选项 ============

std::vector<QuotaProviderOption> quotaProviderOptions() {
    std::vector<QuotaProviderOption> out;
    out.push_back(kNoneOption);   // 索引 0 恒为「不配置」（不变量 1）
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
    return 0;   // 未命中（含写错的 id）→ 落到「不配置」，不静默留一个假选中
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
        // 读回的手填值（键名按 provider 的 fields() 过滤，未知键丢弃：
        // 换了供应商后旧键名不再有意义，留着只会被 provider 自己忽略）
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
        // 定位候选下标（库已删/条目已消失 → -1 = 未选，文案层负责解释）
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

// ============ cc-switch ============

void loadQuotaCcSwitch(QuotaDialogState& st) {
    if (st.ccSwitchLoaded) return;   // 每帧 compose 都可能调到，只读一次
    st.ccSwitchLoaded = true;
    st.ccSwitch = listCcSwitchProviders();
    // 默认按 base_url 自动匹配（matchCcSwitchProviderByBaseUrl 是 host+path
    // 标签边界匹配，伪造后缀不命中）；未命中 = -1，用户可改选。
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

// ============ 保存 ============

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
    row.secretsJson = encSecrets.value();   // **明文**；protect 由 QuotaStore::saveConfig 做

    // ---- 不变量 2：**代码保证** cc-switch 优先 ----
    // 引用非空 → 上面算出来的 extra / secrets 一律丢弃（不是「绘制层不该
    // 同时填」的自觉）。放在**编码之后**而不是早返回，是刻意的：
    //   · 早返回版本里这两行 `clear()` 是**死代码**（此刻本来就是空串）——
    //     实测过：只删这两行、保留早返回，全量 1220 例照绿不变（变异 4 的
    //     教训：等价变异不等于被守住）。早返回本身也确实是「安全」的，但
    //     它靠的是「结构上空」，不是「这里显式清空」。
    //   · 现在 clear 作用在**真的可能被填过的值**上：删掉它，红。
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

// ============ 展示 ============

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
    // 兜底：headline 表覆盖了全部枚举值，但**万一**将来加了枚举而忘了加文案，
    // 这里仍要给用户看得到的东西 —— 空文案会被画成「什么都没发生」。
    return head.empty() ? ("查询失败（错误码 " +
                          std::to_string(static_cast<int>(result.error)) + "）")
                        : head;
}

}  // namespace module::claude::quota