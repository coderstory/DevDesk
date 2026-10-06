// tests/test_modules/test_claude_quota_dialog.cpp
// 余量查询弹窗的配置逻辑层（quota_dialog_config.{h,cpp}）单测：
//   ① 下拉选项与注册表一一对应；② 动态字段（火山两个 / 其余零个）；③ cc-switch
//   匹配与读取失败；④ 保存形态；⑤ 手填 secret 走 protect；⑥⑧ 取用侧凭据装配与覆盖方向。
// 跨文件前提（不变量 2）：选了 cc-switch 引用 → extra/secrets 一律无条件清空，
// 手填副本不共存。保存路径与取用路径遵守同一条，方向相反（存时清、取时赢）。
// **断言纪律**：字段集合断言钉死**具体 key 字面量**，绝不拿 fields() 当期望值
// （那是自证式断言——实现改了 fields() 用例跟着改，永远绿）。本文件里
// `{"accessKeyId","secretAccessKey"}` 是**手写字面量**，与被测对象无关。
#include "doctest.h"
#include "test_support.h"

#include "platform/credential_protection.h"
#include "quota/ccswitch_source.h"
#include "module_claude_proxy/profile_repository.h"
#include "quota/quota_credential.h"
#include "quota/quota_dialog_config.h"
#include "quota/quota_registry.h"
#include "quota/quota_store.h"

#include <sqlite3.h>

#include <cstdlib>
#include <map>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using namespace module::claude::quota;

constexpr const char* kDbPrefix = "devdesk_test_quota_dialog_";

// 假 credential Backend 的调用计数（Backend 是函数指针表 → 实现必须无捕获，
// 计数只能放文件作用域的静态量）。
int g_protectCalls = 0;
int g_unprotectCalls = 0;

// 假 cc-switch 列表（零磁盘 IO；不碰用户真实的 cc-switch 库）
CcSwitchProviderList fakeCcSwitch(std::vector<CcSwitchProviderInfo> items) {
    CcSwitchProviderList list;
    list.providers = std::move(items);
    list.status = CcSwitchSourceStatus::Ok;
    return list;
}

CcSwitchProviderInfo ccProvider(const std::string& id, const std::string& baseUrl,
                                const std::string& ak, const std::string& sk) {
    CcSwitchProviderInfo info;
    info.id = id;
    info.name = "cc-" + id;
    info.baseUrl = baseUrl;
    info.accessKeyId = ak;
    info.secretAccessKey = sk;
    return info;
}

// 指向真 cc-switch 读取器的环境变量（生产路径用；用例多数走注入缝）
void pointCcSwitchDbAt(const std::filesystem::path& p) {
    test_support::setEnvVar("CCSWITCH_DB_PATH", p.string());
}

// 逐个取字段 key（断言用；不假设顺序）
std::vector<std::string> fieldKeys(const QuotaDialogState& st) {
    std::vector<std::string> out;
    for (const QuotaDialogField& f : st.fields) out.push_back(f.spec.key);
    return out;
}

bool hasKey(const std::vector<std::string>& keys, const std::string& k) {
    for (const std::string& s : keys) {
        if (s == k) return true;
    }
    return false;
}

}  // namespace

// 下拉选项：索引 0 = 不配置，其后与注册表一一对应
// 期望值是**手写字面量**（不是 findProvider/kQuotaProviders 自己）——
// 用被测对象当期望值就是自证式断言，实现换 id 用例会跟着变，永远绿。
REGISTER_TEST(quota_dialog_provider_options,
              "quota_dialog: 下拉选项 = 不配置 + 四家注册表 id（顺序固定）", {
    const std::vector<QuotaProviderOption> opts = quotaProviderOptions();
    DOCTEST_REQUIRE(opts.size() == 5);
    DOCTEST_CHECK_EQ(opts[0].id, std::string(""));
    DOCTEST_CHECK_EQ(opts[0].displayName, std::string("不配置（关闭余量查询）"));
    DOCTEST_CHECK_EQ(opts[1].id, std::string("opencode_go"));
    DOCTEST_CHECK_EQ(opts[2].id, std::string("deepseek"));
    DOCTEST_CHECK_EQ(opts[3].id, std::string("minimax_cn"));
    DOCTEST_CHECK_EQ(opts[4].id, std::string("volcano"));

    // 反查索引对称
    DOCTEST_CHECK_EQ(quotaProviderOptionIndex(""), 0);
    DOCTEST_CHECK_EQ(quotaProviderOptionIndex("volcano"), 4);
    DOCTEST_CHECK_EQ(quotaProviderOptionIndex("deepseek"), 2);
    // 未知 id → 归「不配置」，不静默留一个假选中
    DOCTEST_CHECK_EQ(quotaProviderOptionIndex("no_such_provider"), 0);

    // 每一项都真的能被 findProvider 命中（工厂返 nullptr 的行不得进下拉）
    for (const QuotaProviderOption& o : opts) {
        if (o.id.empty()) continue;
        DOCTEST_CHECK(findProvider(o.id) != nullptr);
    }
    return true;
})

// 动态字段：火山两字段（含 secret），其余三家零字段
REGISTER_TEST(quota_dialog_dynamic_fields,
              "quota_dialog: 火山渲染 accessKeyId/secretAccessKey，其余三家零字段", {
    QuotaDialogState st;
    // 火山
    openQuotaDialog(st, 1, "https://ark.cn-beijing.volces.com/api/coding/v3", "sk-x",
                    nullptr);
    std::vector<std::string> keys = fieldKeys(st);
    DOCTEST_REQUIRE(keys.size() == 2);
    DOCTEST_CHECK(hasKey(keys, "accessKeyId"));
    DOCTEST_CHECK(hasKey(keys, "secretAccessKey"));
    // secret 标记：只有 secretAccessKey 是 secret
    DOCTEST_REQUIRE(st.fields.size() == 2);
    for (const QuotaDialogField& f : st.fields) {
        DOCTEST_CHECK_EQ(f.spec.secret, f.spec.key == "secretAccessKey");
    }
    // 默认选中：base_url 命中火山（探测生效）
    DOCTEST_CHECK_EQ(st.providerIdx, 4);

    // deepseek：零字段（沿用 profile 凭据）
    QuotaDialogState ds;
    openQuotaDialog(ds, 2, "https://api.deepseek.com", "sk-y", nullptr);
    DOCTEST_CHECK_EQ(ds.providerIdx, 2);
    DOCTEST_CHECK(fieldKeys(ds).empty());

    // minimax_cn：零字段
    QuotaDialogState mm;
    openQuotaDialog(mm, 3, "https://api.minimax.cn", "sk-z", nullptr);
    DOCTEST_CHECK_EQ(mm.providerIdx, 3);
    DOCTEST_CHECK(fieldKeys(mm).empty());

    // opencode_go：零字段
    QuotaDialogState og;
    openQuotaDialog(og, 4, "https://opencode.ai/zen/go", "sk-w", nullptr);
    DOCTEST_CHECK_EQ(og.providerIdx, 1);
    DOCTEST_CHECK(fieldKeys(og).empty());

    // 「不配置」→ 零字段
    QuotaDialogState none;
    openQuotaDialog(none, 5, "https://unmatched.example.com", "sk-v", nullptr);
    DOCTEST_CHECK_EQ(none.providerIdx, 0);
    DOCTEST_CHECK(fieldKeys(none).empty());
    return true;
})

// secret 脱敏回显：绘制层拿不到明文
REGISTER_TEST(quota_dialog_secret_mask,
              "quota_dialog: secret 字段脱敏形态不含明文，且只有 secretAccessKey 是 secret", {
    QuotaDialogState st;
    openQuotaDialog(st, 1, "https://ark.cn-beijing.volces.com/api/coding/v3", "sk-x",
                    nullptr);
    DOCTEST_REQUIRE(st.fields.size() == 2);
    for (QuotaDialogField& f : st.fields) {
        f.value = "AKLTtestaccesskeyid1234";
    }
    // 脱敏形态本身不含明文尾部
    const std::string shown = maskSecret(st.fields[0].value);
    DOCTEST_CHECK(shown.find("testaccesskeyid1234") == std::string::npos);
    DOCTEST_CHECK(shown.find("••••") != std::string::npos);
    // secret 字段的 key 判定（决定绘制层走「空输入框 + 脱敏回显」分支）
    int secretCount = 0;
    for (const QuotaDialogField& f : st.fields) {
        if (f.spec.secret) ++secretCount;
    }
    DOCTEST_CHECK_EQ(secretCount, 1);

    // maskSecret 边界：空 / 短 / 常规
    DOCTEST_CHECK_EQ(maskSecret(""), std::string(""));
    DOCTEST_CHECK_EQ(maskSecret("12345"), std::string("*****"));
    DOCTEST_CHECK_EQ(maskSecret("1234567890"), std::string("1234••••90"));
    return true;
})

// cc-switch 有匹配 → 字段自动填好
REGISTER_TEST(quota_dialog_ccswitch_match_fills_fields,
              "quota_dialog: cc-switch 按 base_url 匹配后自动填火山字段", {
    // 建真临时 cc-switch 库（不碰用户真实库）
    test_support::TempDir dir("quota_dialog_cc_");
    const std::filesystem::path dbPath = dir.file("cc-switch.db");
    {
        sqlite3* db = nullptr;
        DOCTEST_REQUIRE(sqlite3_open_v2(dbPath.string().c_str(), &db,
                                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                                         nullptr) == SQLITE_OK);
        char* err = nullptr;
        DOCTEST_REQUIRE(sqlite3_exec(
            db,
            "CREATE TABLE providers (id TEXT, app_type TEXT, name TEXT,"
            " settings_config TEXT, meta TEXT);",
            nullptr, nullptr, &err) == SQLITE_OK);
        sqlite3_free(err);
        err = nullptr;
        // 一条 base_url 与 profile 一致的火山候选
        const std::string cfg =
            R"({"env":{"ANTHROPIC_BASE_URL":"https://ark.cn-beijing.volces.com/api/coding/v3","ANTHROPIC_AUTH_TOKEN":"sk-cc"}})";
        const std::string meta =
            R"({"usage_script":{"accessKeyId":"AKFROMCC","secretAccessKey":"SKFROMCC"}})";
        const std::string sql =
            "INSERT INTO providers VALUES ('cc-vol','claude','火山', '" + cfg + "', '" +
            meta + "');";
        DOCTEST_REQUIRE(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK);
        sqlite3_free(err);
        sqlite3_close(db);
    }
    pointCcSwitchDbAt(dbPath);

    QuotaDialogState st;
    openQuotaDialog(st, 1, "https://ark.cn-beijing.volces.com/api/coding/v3", "sk-x",
                    nullptr);
    DOCTEST_REQUIRE(st.fields.size() == 2);
    loadQuotaCcSwitch(st);
    DOCTEST_CHECK_EQ(st.ccSwitch.status, CcSwitchSourceStatus::Ok);
    DOCTEST_REQUIRE(st.ccSwitchIdx == 0);
    DOCTEST_CHECK_EQ(st.ccSwitch.providers[0].id, std::string("cc-vol"));

    // 字段被自动填好 + 标「来自 cc-switch」
    for (const QuotaDialogField& f : st.fields) {
        DOCTEST_CHECK(f.fromCcSwitch);
        DOCTEST_CHECK_EQ(f.sourceLabel, std::string("来自 cc-switch"));
        DOCTEST_CHECK(f.value == "AKFROMCC" || f.value == "SKFROMCC");
    }
    // secret 字段的脱敏形态不含 cc-switch 那份明文
    for (const QuotaDialogField& f : st.fields) {
        if (!f.spec.secret) continue;
        DOCTEST_CHECK(maskSecret(f.value).find("SKFROMCC") == std::string::npos);
    }

    test_support::setEnvVar("CCSWITCH_DB_PATH", "");
    return true;
})

// cc-switch 读不到 → 明确文案 + 可手填，不崩
REGISTER_TEST(quota_dialog_ccswitch_unavailable,
              "quota_dialog: cc-switch 库不存在 → status != Ok、候选空、可手填", {
    test_support::TempDir dir("quota_dialog_none_");
    pointCcSwitchDbAt(dir.file("does-not-exist.db"));

    QuotaDialogState st;
    openQuotaDialog(st, 1, "https://ark.cn-beijing.volces.com/api/coding/v3", "sk-x",
                    nullptr);
    loadQuotaCcSwitch(st);
    DOCTEST_CHECK(st.ccSwitch.status != CcSwitchSourceStatus::Ok);
    DOCTEST_CHECK(st.ccSwitch.providers.empty());
    DOCTEST_CHECK(!st.ccSwitch.unavailableReason.empty());   // 面向用户的原因非空
    DOCTEST_CHECK_EQ(st.ccSwitchIdx, -1);

    // 字段表仍在（可手填）
    DOCTEST_REQUIRE(st.fields.size() == 2);
    st.fields[0].value = "AKMANUAL";
    st.fields[0].fromCcSwitch = false;
    st.fields[0].sourceLabel = "手动填写";
    std::string err;
    auto row = buildQuotaConfigRow(st, &err);
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK_EQ(row.value().ccswitchProviderId, std::string(""));
    DOCTEST_CHECK(row.value().secretsJson.find("SKFROMCC") == std::string::npos);

    test_support::setEnvVar("CCSWITCH_DB_PATH", "");
    return true;
})

// 保存：选了 cc-switch → 只落引用，secrets_json 不含明文 AK
REGISTER_TEST(quota_dialog_saves_ccswitch_reference_without_secrets,
              "quota_dialog: 选 cc-switch 后保存只落引用，手填副本被清空", {
    test_support::TempDir dir("quota_dialog_save_");
    pointCcSwitchDbAt(dir.file("does-not-exist.db"));

    QuotaDialogState st;
    openQuotaDialog(st, 7, "https://ark.cn-beijing.volces.com/api/coding/v3", "sk-x",
                    nullptr);
    DOCTEST_REQUIRE(st.fields.size() == 2);
    // 注入假 cc-switch 列表（不走磁盘）
    st.ccSwitchLoaded = true;
    st.ccSwitch = fakeCcSwitch({ccProvider("cc-x", "https://ark.cn-beijing.volces.com/api/coding/v3",
                                           "AK-SHOULD-NOT-PERSIST", "SK-SHOULD-NOT-PERSIST")});
    st.ccSwitchIdx = 0;
    selectQuotaCcSwitch(st, 0);
    // 顺手塞一份手填副本（模拟「先手填后改选 cc-switch」的历史）
    st.fields[0].value = "AKMANUALCOPY";

    std::string err;
    auto row = buildQuotaConfigRow(st, &err);
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK_EQ(row.value().profileId, static_cast<int64_t>(7));
    DOCTEST_CHECK_EQ(row.value().providerId, std::string("volcano"));
    DOCTEST_CHECK_EQ(row.value().ccswitchProviderId, std::string("cc-x"));
    DOCTEST_CHECK(row.value().enabled);
    // 不变量 2 的执行点：引用非空 → extra/secrets 一律空
    DOCTEST_CHECK_EQ(row.value().extraJson, std::string(""));
    DOCTEST_CHECK_EQ(row.value().secretsJson, std::string(""));
    DOCTEST_CHECK(row.value().secretsJson.find("AK-SHOULD-NOT-PERSIST") == std::string::npos);
    DOCTEST_CHECK(row.value().secretsJson.find("SK-SHOULD-NOT-PERSIST") == std::string::npos);
    DOCTEST_CHECK(row.value().secretsJson.find("AKMANUALCOPY") == std::string::npos);

    test_support::setEnvVar("CCSWITCH_DB_PATH", "");
    return true;
})

// 「不配置」→ providerId 空串 + enabled=false
REGISTER_TEST(quota_dialog_save_none_provider,
              "quota_dialog: 选「不配置」保存出空 provider_id 且 enabled=false", {
    QuotaDialogState st;
    openQuotaDialog(st, 9, "https://unmatched.example.com", "sk-x", nullptr);
    DOCTEST_CHECK_EQ(st.providerIdx, 0);
    std::string err;
    auto row = buildQuotaConfigRow(st, &err);
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK_EQ(row.value().providerId, std::string(""));
    DOCTEST_CHECK_EQ(row.value().ccswitchProviderId, std::string(""));
    DOCTEST_CHECK(!row.value().enabled);
    return true;
})

// 手填 secret 落库必过 protect（注入假 Backend 断言形态 ≠ 明文）
// POSIX 上真实 protect 是恒等函数，所以在 macOS 上「原样返回」——用注入的假
// Backend 才能证明**调用确实发生了**（而不是碰巧明文落库）。
REGISTER_TEST(quota_dialog_manual_secret_goes_through_protect,
              "quota_dialog: 手填 secret 落库走 protect（注入 Backend 验证调用）", {
    test_support::TestDb dbEnv(kDbPrefix, "");
    QuotaStore store(dbEnv.handle());

    // 假 Backend：protect = **hex 编码**（真变换，明文在落库形态里逐字节消失），
    // unprotect = hex 解码。选 hex 而不是「加前缀」是有意的：加前缀会把明文
    // 原样留在里面，「落库形态不含明文」这条断言就成了恒真的假守卫 —— 而它正是
    // 本条用例要守的东西。
    // Backend 是**函数指针表**，所以两个函数必须无捕获（`[hexEncode]{...}`
    // 这种带捕获的 lambda 转换不成函数指针）。
    g_protectCalls = 0;
    g_unprotectCalls = 0;
    platform::credential::Backend counting{
        [](const std::string& in) -> core::Result<std::string> {
            ++g_protectCalls;
            if (in.empty()) return core::Result<std::string>::ok(std::string());
            static const char* kHex = "0123456789abcdef";
            std::string out;
            out.reserve(in.size() * 2);
            for (unsigned char c : in) {
                out.push_back(kHex[c >> 4]);
                out.push_back(kHex[c & 0x0f]);
            }
            return core::Result<std::string>::ok(out);
        },
        [](const std::string& in) -> core::Result<std::string> {
            ++g_unprotectCalls;
            if (in.empty()) return core::Result<std::string>::ok(std::string());
            if (in.size() % 2 != 0) {
                return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                                       "hex 长度不是偶数");
            }
            const auto val = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            std::string out;
            out.reserve(in.size() / 2);
            for (std::size_t i = 0; i < in.size(); i += 2) {
                const int hi = val(in[i]);
                const int lo = val(in[i + 1]);
                if (hi < 0 || lo < 0) {
                    return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                                           "非 hex 字符");
                }
                out.push_back(static_cast<char>(hi * 16 + lo));
            }
            return core::Result<std::string>::ok(out);
        }};
    const platform::credential::Backend* prev =
        platform::credential::setBackendForTest(&counting);

    QuotaConfigRow row;
    row.profileId = 42;
    row.providerId = "volcano";
    row.secretsJson = R"({"secretAccessKey":"SK-PLAINTEXT-FIXTURE"})";
    row.enabled = true;
    DOCTEST_REQUIRE(store.saveConfig(row).isOk());
    DOCTEST_REQUIRE(g_protectCalls == 1);

    // 直接读原始列（绕开 loadConfig 的 unprotect）——证明落库形态不是明文
    auto stq = dbEnv.handle().prepare(
        "SELECT secrets_json, ccswitch_provider_id FROM profile_quota_config "
        "WHERE profile_id = 42");
    DOCTEST_REQUIRE(stq.isOk());
    auto rows = stq.value().fetchAll();
    DOCTEST_REQUIRE(rows.isOk());
    DOCTEST_REQUIRE(rows.value().size() == 1);
    auto raw = rows.value()[0].getString("secrets_json");
    DOCTEST_REQUIRE(raw.isOk());
    DOCTEST_CHECK(raw.value().find("SK-PLAINTEXT-FIXTURE") == std::string::npos);
    DOCTEST_CHECK(raw.value().find("secretAccessKey") == std::string::npos);
    DOCTEST_CHECK(!raw.value().empty());
    auto ccs = rows.value()[0].getString("ccswitch_provider_id");
    DOCTEST_REQUIRE(ccs.isOk());
    DOCTEST_CHECK_EQ(ccs.value(), std::string(""));   // 新列可读且默认空

    // 读回经 unprotect 还原明文（对称）
    auto loaded = store.loadConfig(42);
    DOCTEST_REQUIRE(loaded.isOk());
    DOCTEST_CHECK(loaded.value().secretsJson.find("SK-PLAINTEXT-FIXTURE") != std::string::npos);
    DOCTEST_REQUIRE(g_unprotectCalls >= 1);

    platform::credential::setBackendForTest(prev);
    return true;
})

// 「测试连接」的凭据装配：cc-switch 引用覆盖落库副本
REGISTER_TEST(quota_dialog_test_config_uses_ccswitch,
              "quota_dialog: 测试连接的凭据来自 cc-switch 引用（不落库的也生效）", {
    test_support::TempDir dir("quota_dialog_cfg_");
    pointCcSwitchDbAt(dir.file("does-not-exist.db"));

    QuotaDialogState st;
    openQuotaDialog(st, 1, "https://ark.cn-beijing.volces.com/api/coding/v3", "sk-x",
                    nullptr);
    st.ccSwitchLoaded = true;
    st.ccSwitch = fakeCcSwitch({ccProvider("cc-y", "https://ark.cn-beijing.volces.com/api/coding/v3",
                                           "AKLIVE", "SKLIVE")});
    st.ccSwitchIdx = 0;
    selectQuotaCcSwitch(st, 0);
    // 用户又手改了一个字段（保存时会被清空，但「测试连接」要能看到）
    st.fields[0].value = "AKOVERRIDDEN";

    const ProviderConfig cfg = buildQuotaTestConfig(st);
    DOCTEST_CHECK_EQ(cfg.providerId, std::string("volcano"));
    DOCTEST_CHECK_EQ(cfg.baseUrl,
                     std::string("https://ark.cn-beijing.volces.com/api/coding/v3"));
    DOCTEST_CHECK_EQ(cfg.apiKey, std::string("sk-x"));
    auto sk = cfg.fields.find("secretAccessKey");
    DOCTEST_REQUIRE(sk != cfg.fields.end());
    DOCTEST_CHECK_EQ(sk->second, std::string("SKLIVE"));

    test_support::setEnvVar("CCSWITCH_DB_PATH", "");
    return true;
})

// 错误文案归一：五类 QuotaError 都有中文，且**永不返回空串**
REGISTER_TEST(quota_dialog_error_headline,
              "quota_dialog: QuotaError 五类都有中文文案且不返回空串", {
    DOCTEST_CHECK(quotaErrorHeadline(QuotaError::Auth, "").find("密钥") != std::string::npos);
    DOCTEST_CHECK(quotaErrorHeadline(QuotaError::Entitlement, "").find("套餐") != std::string::npos);
    DOCTEST_CHECK(quotaErrorHeadline(QuotaError::Shape, "").find("响应") != std::string::npos);
    DOCTEST_CHECK(quotaErrorHeadline(QuotaError::Transient, "").find("网络") != std::string::npos);
    DOCTEST_CHECK(quotaErrorHeadline(QuotaError::NotConfigured, "").find("配置") != std::string::npos);
    // None → 空（成功没有错误文案）
    DOCTEST_CHECK_EQ(quotaErrorHeadline(QuotaError::None, "x"), std::string(""));

    // 测试连接结果文案：成功给摘要，失败带头
    QuotaResult ok;
    ok.kind = QuotaKind::Balance;
    ok.balanceRemaining = 12.5;
    ok.currency = "CNY";
    DOCTEST_CHECK(quotaTestMessage(ok).find("12.5") != std::string::npos);
    DOCTEST_CHECK(quotaTestMessage(ok).find("CNY") != std::string::npos);

    QuotaResult bad;
    bad.error = QuotaError::Entitlement;
    bad.errorMessage = "套餐已过期";
    const std::string msg = quotaTestMessage(bad);
    DOCTEST_CHECK(msg.find("套餐") != std::string::npos);
    DOCTEST_CHECK(msg.find("套餐已过期") != std::string::npos);
    return true;
})
// 凭据缝：cc-switch 引用**真的**在 fetch 时生效（零网络）
// 假 provider 把收到的 ProviderConfig 原样记下来，供用例断言字段来源。
namespace {

class CapturingProvider final : public QuotaProvider {
public:
    std::string id() const override { return "volcano"; }
    std::string displayName() const override { return "捕获供应商"; }
    std::vector<ProviderField> fields() const override {
        return {{"accessKeyId", "AK", false},
                {"secretAccessKey", "SK", true}};
    }
    QuotaResult fetch(const ProviderConfig& cfg, const HttpSendFn& http) const override {
        (void)http;   // **不发任何请求**（零真实网络）
        got = true;
        lastBaseUrl = cfg.baseUrl;
        lastApiKey = cfg.apiKey;
        lastFields = cfg.fields;
        QuotaResult r;
        r.error = QuotaError::None;
        return r;
    }
    mutable bool got = false;
    mutable std::string lastBaseUrl;
    mutable std::string lastApiKey;
    mutable std::map<std::string, std::string> lastFields;
};

}  // namespace

REGISTER_TEST(quota_dialog_ccswitch_credential_overlay,
              "quota_dialog: fetch 时 cc-switch 引用覆盖落库副本（凭据缝真的生效）", {
    using module::claude::ProfileRepository;
    test_support::TestDb dbEnv(kDbPrefix, "");
    QuotaStore store(dbEnv.handle());

    // profile + 密钥（走真仓储，落进 api_key_encrypted）
    ProfileRepository profiles(dbEnv.handle(), "DevDeskTestQuotaDialog");
    auto created = profiles.createProfile("火山", "https://ark.cn-beijing.volces.com/api/coding/v3",
                                          "sk-ant-profile-key", "m");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t pid = created.value().id;

    // 落一行**只存 cc-switch 引用**的配置（extra/secrets 按不变量 2 为空）
    QuotaConfigRow row;
    row.profileId = pid;
    row.providerId = "volcano";
    row.ccswitchProviderId = "cc-42";
    row.enabled = true;
    DOCTEST_REQUIRE(store.saveConfig(row).isOk());

    CapturingProvider provider;
    ProfileCredentialSource creds(
        profiles, store, [] { return fakeCcSwitch({ccProvider("cc-42", "https://ark.cn-beijing.volces.com/api/coding/v3",
                                                              "AK-FROM-CCSWITCH", "SK-FROM-CCSWITCH")}); });

    // 空 HttpSendFn：真 provider 会因空缝报错；本条断言的是**送进 fetch 的凭据**，
    // 用捕获型假 provider 读回来，不发请求。
    QuotaResult r = creds.resolveAndFetch(pid, provider, HttpSendFn());
    DOCTEST_CHECK(provider.got);
    DOCTEST_CHECK_EQ(provider.lastApiKey, std::string("sk-ant-profile-key"));
    auto sk = provider.lastFields.find("secretAccessKey");
    DOCTEST_REQUIRE(sk != provider.lastFields.end());
    DOCTEST_CHECK_EQ(sk->second, std::string("SK-FROM-CCSWITCH"));
    (void)r;

    // 引用悬空（cc-switch 里删了这条）→ NotConfigured，**不**静默回落
    ProfileCredentialSource missing(
        profiles, store, [] { return fakeCcSwitch({}); });
    QuotaResult r2 = missing.resolveAndFetch(pid, provider, HttpSendFn());
    DOCTEST_CHECK(r2.error == QuotaError::NotConfigured);
    DOCTEST_CHECK(r2.errorMessage.find("cc-42") != std::string::npos);
    DOCTEST_CHECK(r2.errorMessage.find("找不到") != std::string::npos);
    return true;
})

// **取用侧覆盖方向**
//
// 同文件里 quota_dialog_ccswitch_credential_overlay 只钉住了 **secretAccessKey**
// 一个字段，而且它的库行 extra/secrets 是**空的**（不变量 2 的正常形态）——
// 于是它证明的是「cc-switch 的值填进了空字段」，**不是**「cc-switch 赢过库里的
// 手填副本」。实测：把 accessKeyId 的覆盖改成 `if (false)`（cc-switch 完全不覆盖）
// → 整个 quota 套件**全绿**。
//
// 本条钉的正是那个方向：**库里手填了同名值**（外部改库 / 历史遗留 / 直接手改
// sqlite 都可能造成这种状态），cc-switch 的值仍然必须赢。变异靶点：
//   · `if (!matched->accessKeyId.empty())` → `if (false)`     → accessKeyId 红
//   · `if (!matched->secretAccessKey.empty())` → `if (false)` → secretAccessKey 红
//   · 整个 if 块删掉                                          → 两个字段都红
// 零真实网络（假 provider 捕获 cfg.fields）。
REGISTER_TEST(quota_dialog_ccswitch_wins_over_db_copy_on_fetch,
              "quota_dialog: 取用侧 cc-switch 覆盖赢过库里的手填副本（两个字段各钉一次）", {
    using module::claude::ProfileRepository;
    test_support::TestDb dbEnv(kDbPrefix, "");
    QuotaStore store(dbEnv.handle());

    ProfileRepository profiles(dbEnv.handle(), "DevDeskTestQuotaCcwins");
    auto created = profiles.createProfile("火山", "https://ark.cn-beijing.volces.com/api/coding/v3",
                                          "sk-ant-profile-key", "m");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t pid = created.value().id;

    // 库里**故意留着**手填副本：cc-switch 引用与手填值共存。正常保存路径
    //   不会产生这种行（不变量 2 无条件清空），但 quota_credential.cpp 的
    //   注释明确承诺「万一外部改了库留了副本，覆盖方向仍是 cc-switch 赢」——
    //   这里让那句承诺变成会红的断言。别把这两行"顺手清掉"以求与保存路径自洽。
    QuotaConfigRow row;
    row.profileId = pid;
    row.providerId = "volcano";
    row.ccswitchProviderId = "cc-77";
    row.enabled = true;
    row.extraJson = R"({"accessKeyId":"AK-MANUAL-IN-DB"})";
    row.secretsJson = R"({"secretAccessKey":"SK-MANUAL-IN-DB"})";
    DOCTEST_REQUIRE(store.saveConfig(row).isOk());

    CapturingProvider provider;
    ProfileCredentialSource creds(
        profiles, store, [] { return fakeCcSwitch({ccProvider("cc-77",
                                                              "https://ark.cn-beijing.volces.com/api/coding/v3",
                                                              "AK-FROM-CCSWITCH",
                                                              "SK-FROM-CCSWITCH")}); });
    creds.resolveAndFetch(pid, provider, HttpSendFn());
    DOCTEST_REQUIRE(provider.got);

    // 方向钉死：拿到的是 **cc-switch 的值**，不是库里手填的那份。
    const auto ak = provider.lastFields.find("accessKeyId");
    DOCTEST_REQUIRE(ak != provider.lastFields.end());
    DOCTEST_CHECK_MESSAGE(ak->second == std::string("AK-FROM-CCSWITCH"),
                          "accessKeyId 用的是库里的手填副本「" + ak->second +
                              "」而不是 cc-switch 的值 —— 「cc-switch 赢」的方向反了");
    DOCTEST_CHECK_MESSAGE(ak->second.find("AK-MANUAL-IN-DB") == std::string::npos,
                          "accessKeyId 仍是库里的手填副本：cc-switch 覆盖完全没发生");

    const auto sk = provider.lastFields.find("secretAccessKey");
    DOCTEST_REQUIRE(sk != provider.lastFields.end());
    DOCTEST_CHECK_MESSAGE(sk->second == std::string("SK-FROM-CCSWITCH"),
                          "secretAccessKey 用的是库里的手填副本「" + sk->second +
                              "」而不是 cc-switch 的值 —— 「cc-switch 赢」的方向反了");
    DOCTEST_CHECK_MESSAGE(sk->second.find("SK-MANUAL-IN-DB") == std::string::npos,
                          "secretAccessKey 仍是库里的手填副本：cc-switch 覆盖完全没发生");

    // profile 自身的密钥（api_key_encrypted 解密来的）**不受** cc-switch 覆盖影响
    // ——覆盖范围只有 AK/SK 两个字段。这条同时钉住「覆盖不是无差别的整体替换」。
    DOCTEST_CHECK_EQ(provider.lastApiKey, std::string("sk-ant-profile-key"));
    return true;
})

// 不变量 2 的反向：选了 cc-switch → 手填副本**无条件**被清空
// 变异靶点：把 buildQuotaConfigRow 里的「清空 extra/secrets」删掉，本条即红。
REGISTER_TEST(quota_dialog_ccswitch_wins_over_manual,
              "quota_dialog: 选了 cc-switch 则手填副本无条件丢弃（不共存）", {
    QuotaDialogState st;
    openQuotaDialog(st, 11, "https://ark.cn-beijing.volces.com/api/coding/v3", "sk-x",
                    nullptr);
    DOCTEST_REQUIRE(st.fields.size() == 2);
    st.ccSwitchLoaded = true;
    st.ccSwitch = fakeCcSwitch({ccProvider("cc-z", "https://ark.cn-beijing.volces.com/api/coding/v3",
                                           "AKCC", "SKCC")});
    // **先手填**（两字段都填上），再选 cc-switch
    st.fields[0].value = "AK-TYPED-BY-HAND";
    st.fields[1].value = "SK-TYPED-BY-HAND";
    st.ccSwitchIdx = 0;
    selectQuotaCcSwitch(st, 0);
    std::string err;
    auto out = buildQuotaConfigRow(st, &err);
    DOCTEST_REQUIRE(out.isOk());
    DOCTEST_CHECK_EQ(out.value().ccswitchProviderId, std::string("cc-z"));
    DOCTEST_CHECK_EQ(out.value().extraJson, std::string(""));
    DOCTEST_CHECK_EQ(out.value().secretsJson, std::string(""));
    // 手填的明文一个字都不许出现在落库行里
    DOCTEST_CHECK(out.value().secretsJson.find("SK-TYPED-BY-HAND") == std::string::npos);
    DOCTEST_CHECK(out.value().extraJson.find("AK-TYPED-BY-HAND") == std::string::npos);
    return true;
})
