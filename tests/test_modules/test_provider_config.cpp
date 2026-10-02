// tests/test_modules/test_provider_config.cpp
// 供应商完整配置数据模型层测试
// （spec docs/superpowers/specs/2026-09-22-claude-provider-config-design.md Phase A）
//
// 覆盖：
//   - models_config 6 槽位序列化往返 / 空槽位
//   - 旧单 model 字段兼容读取（fallback）
//   - extra_env 序列化往返 / 非法值拒绝
//   - 非法 JSON 容错（读路径不炸）
//   - displayName 派生（剥 [1M] 等标记）
//   - 别名映射（AUTO_COMPACT_ENABLED → DISABLE_AUTO_COMPACT 值取反 + 透传标记）
//   - ClaudeService 读写 API 往返 + 新列默认值（不做旧库升级：项目无历史包袱）
//   - 槽位单一真相源（审计 CPX-08）：表行驱动的派生面一致性（用例遍历
//     modelSlotTable()，加一行槽位即自动进入覆盖，无需改用例）
#include "doctest.h"
#include "test_support.h"

#include "claude_service.h"
#include "core/database.h"
#include "core/schema.h"
#include "core/sqlite_db.h"
#include "platform/keychain.h"
#include "env_var_hints.h"
#include "provider_config.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr const char* kTestKeychainService = "DevDeskTestProviderConfig";

// 构造全填的 6 槽位配置
module::claude::ModelsConfig fullConfig() {
    module::claude::ModelsConfig c;
    c.def.model = "fangzhou/glm-5.3[1M]";
    c.def.name = "fangzhou/glm-5.3";
    c.sonnet.model = "fangzhou/glm-5.3[1M]";
    c.opus.model = "fangzhou/glm-5.3[1M]";
    c.haiku.model = "fangzhou/glm-5.3";
    c.fable.model = "fangzhou/glm-5.3[1M]";
    c.subagent.model = "fangzhou/glm-5.3[1M]";
    return c;
}

bool slotEq(const module::claude::ModelSlot& a,
            const module::claude::ModelSlot& b) {
    return a.model == b.model && a.name == b.name;
}

// 临时 db 文件名前缀（共享工厂 test_support::tempDbPath / TestDb 的第一实参）
constexpr const char* kDbPrefix = "devdesk_test_pconfig_";

} // namespace

// ---- 1. models_config 序列化往返（6 槽位全填）----
REGISTER_TEST(provider_config_models_roundtrip,
              "provider_config: models_config 6 槽位序列化往返", {
    auto c = fullConfig();
    auto json = module::claude::serializeModelsConfig(c);
    DOCTEST_REQUIRE(json.isOk());

    auto back = module::claude::parseModelsConfig(json.value());
    DOCTEST_REQUIRE(back.isOk());
    auto& b = back.value();
    DOCTEST_CHECK(slotEq(b.def, c.def));
    DOCTEST_CHECK(slotEq(b.sonnet, c.sonnet));
    DOCTEST_CHECK(slotEq(b.opus, c.opus));
    DOCTEST_CHECK(slotEq(b.haiku, c.haiku));
    DOCTEST_CHECK(slotEq(b.fable, c.fable));
    DOCTEST_CHECK(slotEq(b.subagent, c.subagent));
    return true;
})

// ---- 2. 空槽位：全空配置往返 + 空串/空对象解析 ----
REGISTER_TEST(provider_config_empty_slots,
              "provider_config: 空槽位与空串解析", {
    module::claude::ModelsConfig c;  // 全空
    auto json = module::claude::serializeModelsConfig(c);
    DOCTEST_REQUIRE(json.isOk());
    auto back = module::claude::parseModelsConfig(json.value());
    DOCTEST_REQUIRE(back.isOk());
    DOCTEST_CHECK(slotEq(back.value().def, c.def));
    DOCTEST_CHECK(slotEq(back.value().subagent, c.subagent));

    // 空串 = 未配置 → 全空槽位
    auto fromEmpty = module::claude::parseModelsConfig("");
    DOCTEST_REQUIRE(fromEmpty.isOk());
    DOCTEST_CHECK(fromEmpty.value().def.model.empty());

    // "{}" 同样是全空
    auto fromBraces = module::claude::parseModelsConfig("{}");
    DOCTEST_REQUIRE(fromBraces.isOk());
    DOCTEST_CHECK(fromBraces.value().fable.model.empty());
    return true;
})

// ---- 3. 非法 JSON 容错 ----
REGISTER_TEST(provider_config_invalid_json_tolerance,
              "provider_config: 非法 JSON 容错", {
    // JSON 解析失败 → fail
    auto broken = module::claude::parseModelsConfig("{not json");
    DOCTEST_CHECK(broken.isFail());

    // 顶层非对象 → fail
    auto arr = module::claude::parseModelsConfig("[1,2]");
    DOCTEST_CHECK(arr.isFail());

    // 槽位缺失 / 非对象 / model 非字符串 → 容错为空，不 fail
    auto partial = module::claude::parseModelsConfig(
        R"({"default": {"model": "m-1"}, "sonnet": "bad", "opus": {"model": 123}})");
    DOCTEST_REQUIRE(partial.isOk());
    DOCTEST_CHECK_EQ(partial.value().def.model, std::string("m-1"));
    DOCTEST_CHECK(partial.value().sonnet.model.empty());  // 非对象槽位 → 空
    DOCTEST_CHECK(partial.value().opus.model.empty());    // 非字符串值 → 空
    DOCTEST_CHECK(partial.value().haiku.model.empty());   // 缺失槽位 → 空

    // extra_env 同理
    auto envBad = module::claude::parseExtraEnv("{oops");
    DOCTEST_CHECK(envBad.isFail());
    auto envNonObj = module::claude::parseExtraEnv("[]");
    DOCTEST_CHECK(envNonObj.isFail());
    auto envEmpty = module::claude::parseExtraEnv("");
    DOCTEST_REQUIRE(envEmpty.isOk());
    DOCTEST_CHECK(envEmpty.value().empty());
    return true;
})

// ---- 4. models_config 读入槽位（空 / 非法 JSON → 全空槽位）----
REGISTER_TEST(provider_config_effective_models_read,
              "provider_config: effectiveModelsConfig 读入槽位与容错", {
    using module::claude::effectiveModelsConfig;

    // 空串 → 全空槽位
    auto c1 = effectiveModelsConfig("");
    DOCTEST_CHECK(c1.def.model.empty());
    DOCTEST_CHECK(c1.sonnet.model.empty());

    // 非法 JSON → 容错为全空槽位（读路径不炸）
    auto c2 = effectiveModelsConfig("{broken");
    DOCTEST_CHECK(c2.def.model.empty());

    // 正常 JSON → 逐槽位落位
    auto c3 = effectiveModelsConfig(
        R"({"default": {"model": "m-default"}, "sonnet": {"model": "m-sonnet"}})");
    DOCTEST_CHECK_EQ(c3.def.model, std::string("m-default"));
    DOCTEST_CHECK_EQ(c3.sonnet.model, std::string("m-sonnet"));
    return true;
})

// ---- 5. extra_env 序列化往返 + 非字符串值拒绝 ----
REGISTER_TEST(provider_config_extra_env_roundtrip,
              "provider_config: extra_env 序列化往返与值校验", {
    std::map<std::string, std::string> env = {
        {"AUTO_COMPACT_ENABLED", "1"},
        {"DISABLE_AUTOUPDATER", "1"},
        {"CLAUDE_AUTOCOMPACT_PCT_OVERRIDE", "75"},
    };
    auto json = module::claude::serializeExtraEnv(env);
    DOCTEST_REQUIRE(json.isOk());

    auto back = module::claude::parseExtraEnv(json.value());
    DOCTEST_REQUIRE(back.isOk());
    DOCTEST_CHECK_EQ(back.value().size(), 3u);
    DOCTEST_CHECK_EQ(back.value()["AUTO_COMPACT_ENABLED"], std::string("1"));
    DOCTEST_CHECK_EQ(back.value()["CLAUDE_AUTOCOMPACT_PCT_OVERRIDE"],
                     std::string("75"));

    // 值非字符串 → 拒绝（不做隐式转换，避免 "1" vs 1 语义歧义）
    auto bad = module::claude::parseExtraEnv(R"({"FOO": 1})");
    DOCTEST_CHECK(bad.isFail());

    // 空表往返
    auto emptyJson = module::claude::serializeExtraEnv({});
    DOCTEST_REQUIRE(emptyJson.isOk());
    auto emptyBack = module::claude::parseExtraEnv(emptyJson.value());
    DOCTEST_REQUIRE(emptyBack.isOk());
    DOCTEST_CHECK(emptyBack.value().empty());
    return true;
})

// ---- 6. displayName 派生 ----
REGISTER_TEST(provider_config_derive_display_name,
              "provider_config: displayName 剥除 [1M]/[200K] 标记", {
    using module::claude::deriveDisplayName;
    DOCTEST_CHECK_EQ(deriveDisplayName("fangzhou/glm-5.3[1M]"),
                     std::string("fangzhou/glm-5.3"));
    DOCTEST_CHECK_EQ(deriveDisplayName("fangzhou/glm-5.3[200K]"),
                     std::string("fangzhou/glm-5.3"));
    DOCTEST_CHECK_EQ(deriveDisplayName("model[1m]"), std::string("model"));
    DOCTEST_CHECK_EQ(deriveDisplayName("claude-sonnet-4-5"),
                     std::string("claude-sonnet-4-5"));  // 无标记原样
    DOCTEST_CHECK_EQ(deriveDisplayName(""), std::string(""));
    DOCTEST_CHECK_EQ(deriveDisplayName("[1M]"), std::string("[1M]"));  // 整串即标记不剥
    return true;
})

// ---- 7. 别名映射（§4.3 规则 6）----
REGISTER_TEST(provider_config_env_alias_mapping,
              "provider_config: AUTO_COMPACT_ENABLED 别名映射值取反 + 透传标记", {
    using module::claude::applyEnvAliases;

    // =1（开自动压缩）→ DISABLE_AUTO_COMPACT=0（不禁用）
    {
        auto r = applyEnvAliases({{"AUTO_COMPACT_ENABLED", "1"}});
        DOCTEST_CHECK_EQ(r.env.size(), 1u);
        DOCTEST_CHECK_EQ(r.env["DISABLE_AUTO_COMPACT"], std::string("0"));
        DOCTEST_CHECK(r.env.find("AUTO_COMPACT_ENABLED") == r.env.end());
        DOCTEST_CHECK_EQ(r.aliasMappedKeys.size(), 1u);
        DOCTEST_CHECK_EQ(r.aliasMappedKeys[0], std::string("AUTO_COMPACT_ENABLED"));
        DOCTEST_CHECK(r.unrecognizedKeys.empty());
    }
    // =0（关自动压缩）→ DISABLE_AUTO_COMPACT=1
    {
        auto r = applyEnvAliases({{"AUTO_COMPACT_ENABLED", "0"}});
        DOCTEST_CHECK_EQ(r.env["DISABLE_AUTO_COMPACT"], std::string("1"));
    }
    // "true" → 0；空值 → 1（保守：无法确认时不禁用/按关处理见头文件注释）
    {
        auto r = applyEnvAliases({{"AUTO_COMPACT_ENABLED", "true"}});
        DOCTEST_CHECK_EQ(r.env["DISABLE_AUTO_COMPACT"], std::string("0"));
    }

    // 混合：官方键透传无警告；别名表外未知键透传 + unrecognized 标记
    {
        std::map<std::string, std::string> env = {
            {"AUTO_COMPACT_ENABLED", "0"},
            {"DISABLE_AUTOUPDATER", "1"},              // 官方已知键
            {"NOT_A_REAL_VAR_XX", "75"},               // 别名表外、官方清单外未知键
        };
        auto r = applyEnvAliases(env);
        DOCTEST_CHECK_EQ(r.env.size(), 3u);
        DOCTEST_CHECK_EQ(r.env["DISABLE_AUTO_COMPACT"], std::string("1"));
        DOCTEST_CHECK_EQ(r.env["DISABLE_AUTOUPDATER"], std::string("1"));
        DOCTEST_CHECK_EQ(r.env["NOT_A_REAL_VAR_XX"],
                         std::string("75"));  // 原样透传
        DOCTEST_CHECK_EQ(r.unrecognizedKeys.size(), 1u);
        DOCTEST_CHECK_EQ(r.unrecognizedKeys[0], std::string("NOT_A_REAL_VAR_XX"));
    }
    return true;
})

// ---- 7b. 官方键判定源 = env_var_hints（审计 CPX-09）----
REGISTER_TEST(provider_config_official_keys_derived_from_hints,
              "provider_config: 官方键判定源自 env_var_hints 清单", {
    using module::claude::applyEnvAliases;

    // hints 收录的官方键（含 *_MODEL_NAME 变体）→ 不标未识别
    {
        auto r = applyEnvAliases({{"ANTHROPIC_DEFAULT_SONNET_MODEL_NAME", "x"}});
        DOCTEST_CHECK(r.unrecognizedKeys.empty());
    }
    // DevDesk 自造的 FABLE 扩展键（不在 hints）→ 仍不标未识别
    {
        std::map<std::string, std::string> env = {
            {"ANTHROPIC_DEFAULT_FABLE_MODEL", "m-fable"},
            {"ANTHROPIC_DEFAULT_FABLE_MODEL_NAME", "Fable 显示名"},
        };
        auto r = applyEnvAliases(env);
        DOCTEST_CHECK(r.unrecognizedKeys.empty());
    }
    // 真未知键 → 仍标记
    {
        auto r = applyEnvAliases({{"NOT_A_REAL_VAR_XX", "1"}});
        DOCTEST_REQUIRE_EQ(r.unrecognizedKeys.size(), 1u);
        DOCTEST_CHECK_EQ(r.unrecognizedKeys[0], std::string("NOT_A_REAL_VAR_XX"));
    }
    return true;
})

// ---- 8. ClaudeService 读写往返 + model 列同步 ----
REGISTER_TEST(provider_config_service_roundtrip,
              "claude_service: models_config/extra_env 读写往返", {
    test_support::TestDb tdb(kDbPrefix, "svc_roundtrip");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile("官方中转", "https://gw.example.com/anthropic",
                                     "sk-ind-test-0123456789abcdef",
                                     "claude-sonnet-4-5");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;

    // 新建 profile 两列默认空串
    {
        auto got = svc.getProfile(id);
        DOCTEST_REQUIRE(got.isOk());
        DOCTEST_CHECK(got.value().modelsConfigJson.empty());
        DOCTEST_CHECK(got.value().extraEnvJson.empty());
        DOCTEST_CHECK_EQ(got.value().model, std::string("claude-sonnet-4-5"));
    }

    // 写入完整配置（key 未变 → 不带 apiKeyPlain 的分支）
    module::claude::Profile p = created.value();
    auto cfgJson = module::claude::serializeModelsConfig(fullConfig());
    DOCTEST_REQUIRE(cfgJson.isOk());
    p.modelsConfigJson = cfgJson.value();
    p.extraEnvJson = R"({"DISABLE_AUTOUPDATER": "1"})";
    auto upd = svc.updateProfile(p);
    DOCTEST_REQUIRE(upd.isOk());

    // 回读：JSON 原文一致；model 列同步为 default 槽位
    {
        auto got = svc.getProfile(id);
        DOCTEST_REQUIRE(got.isOk());
        DOCTEST_CHECK_EQ(got.value().modelsConfigJson, cfgJson.value());
        DOCTEST_CHECK_EQ(got.value().extraEnvJson,
                         std::string(R"({"DISABLE_AUTOUPDATER": "1"})"));
        DOCTEST_CHECK_EQ(got.value().model,
                         std::string("fangzhou/glm-5.3[1M]"));
    }

    // 坏 JSON 落库被拒（信任边界校验）
    {
        module::claude::Profile bad = created.value();
        bad.modelsConfigJson = "{broken";
        auto r = svc.updateProfile(bad);
        DOCTEST_CHECK(r.isFail());
    }

    // 清理 keychain 条目
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(id));
    return true;
})

// ---- 9. 新列默认值 + 空 JSON 读侧回落（不做旧库升级，项目无历史包袱）----
REGISTER_TEST(provider_config_new_column_defaults,
              "claude_profiles: 新列默认值空串/0/'' 且空 models_config 回落 model 列", {
    auto path = test_support::tempDbPath(kDbPrefix, "defaults");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(r.value().migrate().isOk());
        auto& db = r.value().db();

        // 只写 v1 就有的列，其余走 schema 默认值
        auto ins = db.prepare(
            "INSERT INTO claude_profiles (name, base_url, api_key_hint, "
            "api_key_check, keychain_account, model, is_active, "
            "created_at, updated_at) VALUES "
            "('默认供应商', 'https://old.example.com', 'sk-***abcd', "
            "'deadbeef', 'claude:1', 'claude-3-5-haiku', 1, 0, 0)");
        DOCTEST_REQUIRE(ins.isOk());
        DOCTEST_REQUIRE(ins.value().step().isOk());

        module::claude::ClaudeService svc(db, kTestKeychainService);
        auto profiles = svc.listProfiles();
        DOCTEST_REQUIRE(profiles.isOk());
        DOCTEST_REQUIRE_EQ(profiles.value().size(), 1u);
        const auto& p = profiles.value()[0];
        DOCTEST_CHECK_EQ(p.name, std::string("默认供应商"));
        DOCTEST_CHECK_EQ(p.model, std::string("claude-3-5-haiku"));
        DOCTEST_CHECK(p.modelsConfigJson.empty());  // schema 默认 ''
        DOCTEST_CHECK(p.extraEnvJson.empty());      // schema 默认 ''
        DOCTEST_CHECK(p.dialectOverride.empty());   // schema 默认 ''
        DOCTEST_CHECK(p.extraSettingsJson.empty()); // schema 默认 ''
        // （merge 解：2 参 effectiveModelsConfig 的「default 槽位 = model 列」
        // 断言随 legacyModel 形参删除而作废——旧 model 列 fallback 已在
        // 旧数据兼容删除批拆除，Profile::model 是活列、双向同步）
    }
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
    return true;
})

// ---- parseImportedEnv（Phase D 导入解析，§6.1 + §7.4）----

// 用户真实模板（设计 §1）包进整段 settings.json（含非 env 顶层字段）。
// 注意 raw string 用自定义分隔符 json(...)json —— 模板里的 `)"` 序列
//（如 "Bash(ls)"）会提前终止默认 R"(...)"
static const char* kImportTemplate = R"json({
    "permissions": {"allow": ["Bash(ls)"]},
    "env": {
        "ANTHROPIC_AUTH_TOKEN": "sk_ind_test_abc",
        "ANTHROPIC_BASE_URL": "https://gw.example.com/fangzhou/anthropic",
        "ANTHROPIC_DEFAULT_FABLE_MODEL": "fangzhou/glm-5.3[1M]",
        "ANTHROPIC_DEFAULT_FABLE_MODEL_NAME": "fangzhou/glm-5.3",
        "ANTHROPIC_DEFAULT_HAIKU_MODEL": "fangzhou/glm-5.3",
        "ANTHROPIC_DEFAULT_OPUS_MODEL": "fangzhou/glm-5.3[1M]",
        "ANTHROPIC_DEFAULT_OPUS_MODEL_NAME": "fangzhou/glm-5.3",
        "ANTHROPIC_DEFAULT_SONNET_MODEL": "fangzhou/glm-5.3[1M]",
        "ANTHROPIC_DEFAULT_SONNET_MODEL_NAME": "fangzhou/glm-5.3",
        "ANTHROPIC_MODEL": "fangzhou/glm-5.3[1M]",
        "AUTO_COMPACT_ENABLED": "1",
        "NOT_A_REAL_VAR_XX": "75",
        "CLAUDE_CODE_AUTO_COMPACT_WINDOW": "800000",
        "CLAUDE_CODE_DISABLE_ADAPTIVE_THINKING": "1",
        "CLAUDE_CODE_DISABLE_EXPERIMENTAL_BETAS": "1",
        "CLAUDE_CODE_SUBAGENT_MODEL": "fangzhou/glm-5.3[1M]",
        "DISABLE_AUTOUPDATER": "1"
    }
})json";

REGISTER_TEST(provider_config_parse_import_full_settings,
              "parseImportedEnv: 整段 settings.json 形状全量分布", {
    auto r = module::claude::parseImportedEnv(kImportTemplate);
    DOCTEST_REQUIRE(r.isOk());
    const auto& c = r.value();
    DOCTEST_CHECK_EQ(c.baseUrl,
                     std::string("https://gw.example.com/fangzhou/anthropic"));
    DOCTEST_CHECK_EQ(c.authToken, std::string("sk_ind_test_abc"));
    // 6 槽位
    DOCTEST_CHECK_EQ(c.models.def.model, std::string("fangzhou/glm-5.3[1M]"));
    DOCTEST_CHECK_EQ(c.models.sonnet.model, std::string("fangzhou/glm-5.3[1M]"));
    DOCTEST_CHECK_EQ(c.models.sonnet.name, std::string("fangzhou/glm-5.3"));
    DOCTEST_CHECK_EQ(c.models.opus.model, std::string("fangzhou/glm-5.3[1M]"));
    DOCTEST_CHECK_EQ(c.models.opus.name, std::string("fangzhou/glm-5.3"));
    DOCTEST_CHECK_EQ(c.models.haiku.model, std::string("fangzhou/glm-5.3"));
    DOCTEST_CHECK(c.models.haiku.name.empty());   // 模板未给 → 派生兜底
    DOCTEST_CHECK_EQ(c.models.fable.model, std::string("fangzhou/glm-5.3[1M]"));
    DOCTEST_CHECK_EQ(c.models.fable.name, std::string("fangzhou/glm-5.3"));
    DOCTEST_CHECK_EQ(c.models.subagent.model,
                     std::string("fangzhou/glm-5.3[1M]"));
    DOCTEST_CHECK_EQ(c.recognizedSlots, 6);
    // 别名映射：AUTO_COMPACT_ENABLED=1 → DISABLE_AUTO_COMPACT=0
    DOCTEST_REQUIRE_EQ(c.aliasMappedKeys.size(), 1u);
    DOCTEST_CHECK_EQ(c.aliasMappedKeys[0], std::string("AUTO_COMPACT_ENABLED"));
    DOCTEST_CHECK_EQ(c.extraEnv.at("DISABLE_AUTO_COMPACT"), std::string("0"));
    // 官方键透传进 extraEnv
    DOCTEST_CHECK_EQ(c.extraEnv.at("CLAUDE_CODE_AUTO_COMPACT_WINDOW"),
                     std::string("800000"));
    DOCTEST_CHECK_EQ(c.extraEnv.at("DISABLE_AUTOUPDATER"), std::string("1"));
    DOCTEST_CHECK_EQ(c.extraEnv.at("CLAUDE_CODE_DISABLE_ADAPTIVE_THINKING"),
                     std::string("1"));
    DOCTEST_CHECK_EQ(c.extraEnv.at("CLAUDE_CODE_DISABLE_EXPERIMENTAL_BETAS"),
                     std::string("1"));
    // 非官方键警告角标透传
    DOCTEST_REQUIRE_EQ(c.unrecognizedKeys.size(), 1u);
    DOCTEST_CHECK_EQ(c.unrecognizedKeys[0],
                     std::string("NOT_A_REAL_VAR_XX"));
    DOCTEST_CHECK_EQ(c.extraEnv.at("NOT_A_REAL_VAR_XX"),
                     std::string("75"));
    DOCTEST_CHECK(c.warnings.empty());   // 无 API_KEY/AUTH_TOKEN 同现
    return true;
})

REGISTER_TEST(provider_config_parse_import_bare_env,
              "parseImportedEnv: 裸 env 对象形状", {
    const std::string json =
        "{\n"
        "  \"ANTHROPIC_BASE_URL\": \"https://relay.example.com\",\n"
        "  \"DISABLE_AUTOUPDATER\": \"1\"\n"
        "}";
    auto r = module::claude::parseImportedEnv(json);
    DOCTEST_REQUIRE(r.isOk());
    const auto& c = r.value();
    DOCTEST_CHECK_EQ(c.baseUrl, std::string("https://relay.example.com"));
    DOCTEST_CHECK(c.authToken.empty());
    DOCTEST_CHECK_EQ(c.recognizedSlots, 0);
    DOCTEST_REQUIRE_EQ(c.extraEnv.size(), 1u);
    DOCTEST_CHECK_EQ(c.extraEnv.at("DISABLE_AUTOUPDATER"), std::string("1"));
    DOCTEST_CHECK(c.unrecognizedKeys.empty());
    DOCTEST_CHECK(c.aliasMappedKeys.empty());
    return true;
})

REGISTER_TEST(provider_config_parse_import_rejects,
              "parseImportedEnv: 非法输入整体拒绝（不部分导入）", {
    using module::claude::parseImportedEnv;
    // 空白 / 非法 JSON / 顶层非对象 / 值非字符串 / 超键数 / 超长值
    //（键名格式校验 2026-09-24 放开：非标准键名可导入，见下方 loose_keys 用例）
    DOCTEST_CHECK(parseImportedEnv("   \n  ").isFail());
    DOCTEST_CHECK(parseImportedEnv("{ not json").isFail());
    DOCTEST_CHECK(parseImportedEnv("[1,2,3]").isFail());
    DOCTEST_CHECK(parseImportedEnv("{ \"FOO\": 1 }").isFail());          // 数字值
    DOCTEST_CHECK(parseImportedEnv("{ \"FOO\": true }").isFail());       // 布尔值

    DOCTEST_CHECK(parseImportedEnv(
        "{ \"env\": \"not-an-object\" }").isFail());                     // env 非对象
    // >64 键（65 个）
    {
        std::string json = "{";
        for (int i = 0; i < 65; ++i) {
            json += "\"K" + std::to_string(i) + "\": \"v\"";
            if (i != 64) json += ", ";
        }
        json += "}";
        DOCTEST_CHECK(parseImportedEnv(json).isFail());
    }
    // 单值 > 4KB
    {
        const std::string big(4097, 'x');
        DOCTEST_CHECK(parseImportedEnv(
            "{ \"BIG\": \"" + big + "\" }").isFail());
    }
    return true;
})

REGISTER_TEST(provider_config_parse_import_text_limit,
              "provider_config: parseImportedEnv 整体文本上限 1MB（超限整体 fail + 文案含上限，审计 L6）", {
    using module::claude::kMaxImportTextBytes;
    using module::claude::parseImportedEnv;

    // 恰好到上限仍是合法输入：在对象收尾 } 前用空白填充到上限（JSON 允许空白）
    const std::string prefix = "{ \"ANTHROPIC_BASE_URL\": \"http://127.0.0.1:9\"";
    const std::string suffix = " }";
    DOCTEST_REQUIRE(kMaxImportTextBytes > prefix.size() + suffix.size());
    const std::string atLimit =
        prefix +
        std::string(kMaxImportTextBytes - prefix.size() - suffix.size(), ' ') +
        suffix;
    DOCTEST_REQUIRE_EQ(atLimit.size(), kMaxImportTextBytes);
    auto ok = parseImportedEnv(atLimit);
    DOCTEST_REQUIRE(ok.isOk());
    DOCTEST_CHECK_EQ(ok.value().baseUrl, std::string("http://127.0.0.1:9"));

    // 超上限 → 整体 fail，文案含上限（不是 JSON 语法错误）
    auto over = parseImportedEnv(atLimit + " ");
    DOCTEST_REQUIRE(over.isFail());
    DOCTEST_CHECK(over.errorMessage().find("1MB") != std::string::npos);

    // 超长非法内容同样先被长度挡下（检查在 parse 之前，不进 JSON 解析）
    auto huge = parseImportedEnv(std::string(kMaxImportTextBytes + 1, 'x'));
    DOCTEST_REQUIRE(huge.isFail());
    DOCTEST_CHECK(huge.errorMessage().find("1MB") != std::string::npos);
    return true;
})

REGISTER_TEST(provider_config_parse_import_key_conflict,
              "parseImportedEnv: API_KEY 与 AUTH_TOKEN 同现取 AUTH_TOKEN", {
    const std::string both =
        "{ \"ANTHROPIC_API_KEY\": \"sk-key\", "
        "\"ANTHROPIC_AUTH_TOKEN\": \"sk-token\" }";
    auto r = module::claude::parseImportedEnv(both);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value().authToken, std::string("sk-token"));
    DOCTEST_REQUIRE_EQ(r.value().warnings.size(), 1u);
    DOCTEST_CHECK(r.value().warnings[0].find("AUTH_TOKEN") !=
                  std::string::npos);

    const std::string onlyKey = "{ \"ANTHROPIC_API_KEY\": \"sk-key\" }";
    auto r2 = module::claude::parseImportedEnv(onlyKey);
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK_EQ(r2.value().authToken, std::string("sk-key"));
    DOCTEST_CHECK(r2.value().warnings.empty());
    return true;
})

// ---- 认证字段三态（2026-09-27 profile-auth-field 设计 §3/§4/§5）----

REGISTER_TEST(provider_config_auth_field_text_roundtrip,
              "provider_config: 认证字段文本/枚举往返 + 未知/空串兜底 both", {
    using module::claude::AuthField;
    using module::claude::authFieldFromText;
    using module::claude::authFieldToText;
    DOCTEST_CHECK_EQ(authFieldFromText(authFieldToText(AuthField::Both)),
                     AuthField::Both);
    DOCTEST_CHECK_EQ(authFieldFromText(authFieldToText(AuthField::Bearer)),
                     AuthField::Bearer);
    DOCTEST_CHECK_EQ(authFieldFromText(authFieldToText(AuthField::ApiKey)),
                     AuthField::ApiKey);
    DOCTEST_CHECK_EQ(authFieldFromText("x"), AuthField::Both);
    DOCTEST_CHECK_EQ(authFieldFromText(""), AuthField::Both);
    // 规范值写侧输出（库内只应出现这三个）
    DOCTEST_CHECK_EQ(std::string(authFieldToText(AuthField::Both)),
                     std::string("both"));
    DOCTEST_CHECK_EQ(std::string(authFieldToText(AuthField::Bearer)),
                     std::string("bearer"));
    DOCTEST_CHECK_EQ(std::string(authFieldToText(AuthField::ApiKey)),
                     std::string("api_key"));
    return true;
})

REGISTER_TEST(provider_config_parse_import_auth_field_evidence,
              "parseImportedEnv: 键名证据四态（api_key/bearer/both/无）", {
    using module::claude::parseImportedEnv;
    // 只有 ANTHROPIC_API_KEY → api_key
    auto onlyKey = parseImportedEnv("{ \"ANTHROPIC_API_KEY\": \"k\" }");
    DOCTEST_REQUIRE(onlyKey.isOk());
    DOCTEST_CHECK_EQ(onlyKey.value().authFieldFromEnv, std::string("api_key"));
    // 只有 ANTHROPIC_AUTH_TOKEN → bearer
    auto onlyToken = parseImportedEnv("{ \"ANTHROPIC_AUTH_TOKEN\": \"k\" }");
    DOCTEST_REQUIRE(onlyToken.isOk());
    DOCTEST_CHECK_EQ(onlyToken.value().authFieldFromEnv, std::string("bearer"));
    // 两键同现 → both（保守取超集）
    auto both = parseImportedEnv(
        "{ \"ANTHROPIC_API_KEY\": \"k\", \"ANTHROPIC_AUTH_TOKEN\": \"k\" }");
    DOCTEST_REQUIRE(both.isOk());
    DOCTEST_CHECK_EQ(both.value().authFieldFromEnv, std::string("both"));
    // 都没出现 → 空串（无证据，UI 不覆盖表单现值）
    auto none = parseImportedEnv("{ \"ANTHROPIC_BASE_URL\": \"https://x\" }");
    DOCTEST_REQUIRE(none.isOk());
    DOCTEST_CHECK(none.value().authFieldFromEnv.empty());
    return true;
})

REGISTER_TEST(provider_config_parse_import_loose_keys,
              "parseImportedEnv: 非标准键名（小写/连字符/数字开头）也导入（2026-09-24 放开）", {
    using module::claude::parseImportedEnv;
    auto r = parseImportedEnv("{ \"foo-bar\": \"1\", \"1FOO\": \"2\" }");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_REQUIRE_EQ(r.value().extraEnv.size(), 2u);
    DOCTEST_CHECK_EQ(r.value().extraEnv.count("foo-bar"), 1u);
    DOCTEST_CHECK_EQ(r.value().extraEnv.count("1FOO"), 1u);
    DOCTEST_CHECK_EQ(r.value().extraEnv.at("foo-bar"), std::string("1"));
    return true;
})

REGISTER_TEST(claude_env_var_hints,
              "envVarHint: 官方清单识别（代表变量）+ 未知返回 null", {
    using module::claude::envVarHint;
    DOCTEST_CHECK(envVarHint("ANTHROPIC_BASE_URL") != nullptr);
    DOCTEST_CHECK(envVarHint("ANTHROPIC_AUTH_TOKEN") != nullptr);
    DOCTEST_CHECK(envVarHint("CLAUDE_CODE_EFFORT_LEVEL") != nullptr);
    DOCTEST_CHECK(envVarHint("MAX_THINKING_TOKENS") != nullptr);
    DOCTEST_CHECK(envVarHint("MCP_TIMEOUT") != nullptr);
    const char* h = envVarHint("ANTHROPIC_BASE_URL");
    DOCTEST_CHECK(std::string(h).find("端点") != std::string::npos);
    DOCTEST_CHECK(envVarHint("NOT_A_REAL_VAR_XX") == nullptr);
    DOCTEST_CHECK(envVarHint("") == nullptr);
    return true;
})

// ---- 反向同步（设计 §3.2 映射表 / §3.3 守卫表 / §3.4 出现缺席语义）----

namespace {

bool hasEntry(const std::vector<std::string>& keys, const char* key) {
    for (const auto& k : keys) {
        if (k == key) return true;
    }
    return false;
}

} // namespace

// ---- isLoopbackUrl（G2）----
REGISTER_TEST(provider_config_reverse_loopback_url,
              "provider_config: isLoopbackUrl 数字段/大小写/方括号（reverse loopback）", {
    using module::claude::isLoopbackUrl;
    DOCTEST_CHECK(isLoopbackUrl("http://127.0.0.1:8888"));
    DOCTEST_CHECK(isLoopbackUrl("http://127.5.5.5"));  // 127/8 全段
    DOCTEST_CHECK(isLoopbackUrl("LocalHost"));         // 大小写
    DOCTEST_CHECK(isLoopbackUrl("http://LocalHost:15721"));
    DOCTEST_CHECK(isLoopbackUrl("[::1]:8888"));
    DOCTEST_CHECK(isLoopbackUrl("https://[::1]/v1"));
    DOCTEST_CHECK(isLoopbackUrl("http://127.0.0.1:15721/v1/messages"));  // 带路径
    DOCTEST_CHECK(!isLoopbackUrl(""));
    DOCTEST_CHECK(!isLoopbackUrl("https://api.anthropic.com"));
    DOCTEST_CHECK(!isLoopbackUrl("http://192.168.1.1:8080"));
    DOCTEST_CHECK(!isLoopbackUrl("http://10.0.0.1"));
    DOCTEST_CHECK(!isLoopbackUrl("http://127.0.0.1.evil.com"));  // 不做前缀误判
    DOCTEST_CHECK(!isLoopbackUrl("http://127.0.0.1evil"));       // 非数字段
    DOCTEST_CHECK(!isLoopbackUrl("http://127.1"));               // 短写不认
    DOCTEST_CHECK(!isLoopbackUrl("http://[::2]:8888"));
    return true;
})

// ---- 映射表 14 条（§3.2）----
REGISTER_TEST(provider_config_reverse_parse_mapping,
              "provider_config: parseEnvForReverseSync 映射表 14 条（reverse）", {
    const std::string env = R"json({
        "ANTHROPIC_BASE_URL": "https://gw.example.com/anthropic",
        "ANTHROPIC_AUTH_TOKEN": "sk-token",
        "ANTHROPIC_MODEL": "m-def",
        "ANTHROPIC_DEFAULT_SONNET_MODEL": "m-sonnet",
        "ANTHROPIC_DEFAULT_SONNET_MODEL_NAME": "Sonnet 显示名",
        "ANTHROPIC_DEFAULT_OPUS_MODEL": "m-opus",
        "ANTHROPIC_DEFAULT_OPUS_MODEL_NAME": "Opus 显示名",
        "ANTHROPIC_DEFAULT_HAIKU_MODEL": "m-haiku",
        "ANTHROPIC_DEFAULT_FABLE_MODEL": "m-fable",
        "ANTHROPIC_DEFAULT_FABLE_MODEL_NAME": "Fable 显示名",
        "CLAUDE_CODE_SUBAGENT_MODEL": "m-sub",
        "AUTO_COMPACT_ENABLED": "1",
        "DISABLE_AUTOUPDATER": "1",
        "NOT_A_REAL_VAR_XX": "75"
    })json";
    auto r = module::claude::parseEnvForReverseSync(env, {});
    DOCTEST_REQUIRE(r.ok);
    const auto& c = r.cfg;
    DOCTEST_CHECK_EQ(c.baseUrl, std::string("https://gw.example.com/anthropic"));
    DOCTEST_CHECK_EQ(c.authToken, std::string("sk-token"));
    DOCTEST_CHECK_EQ(c.models.def.model, std::string("m-def"));
    DOCTEST_CHECK_EQ(c.models.sonnet.model, std::string("m-sonnet"));
    DOCTEST_CHECK_EQ(c.models.sonnet.name, std::string("Sonnet 显示名"));
    DOCTEST_CHECK_EQ(c.models.opus.model, std::string("m-opus"));
    DOCTEST_CHECK_EQ(c.models.opus.name, std::string("Opus 显示名"));
    DOCTEST_CHECK_EQ(c.models.haiku.model, std::string("m-haiku"));
    DOCTEST_CHECK(c.models.haiku.name.empty());
    DOCTEST_CHECK_EQ(c.models.fable.model, std::string("m-fable"));
    DOCTEST_CHECK_EQ(c.models.fable.name, std::string("Fable 显示名"));
    DOCTEST_CHECK_EQ(c.models.subagent.model, std::string("m-sub"));
    DOCTEST_CHECK_EQ(c.recognizedSlots, 6);
    // 别名归一复用 kEnvAliases：AUTO_COMPACT_ENABLED=1 → DISABLE_AUTO_COMPACT=0
    DOCTEST_REQUIRE_EQ(c.extraEnv.count("DISABLE_AUTO_COMPACT"), 1u);
    DOCTEST_CHECK_EQ(c.extraEnv.at("DISABLE_AUTO_COMPACT"), std::string("0"));
    DOCTEST_CHECK_EQ(c.extraEnv.at("DISABLE_AUTOUPDATER"), std::string("1"));
    DOCTEST_CHECK_EQ(c.extraEnv.at("NOT_A_REAL_VAR_XX"),
                     std::string("75"));
    DOCTEST_CHECK_EQ(c.unrecognizedKeys.size(), 1u);  // 未识别键照收，不阻断
    DOCTEST_CHECK(r.skippedKeys.empty());             // 无守卫命中

    // API_KEY 仅当 AUTH_TOKEN 缺席时生效；同现取 AUTH_TOKEN + warning
    auto keyOnly = module::claude::parseEnvForReverseSync(
        R"({"ANTHROPIC_API_KEY": "sk-key"})", {});
    DOCTEST_REQUIRE(keyOnly.ok);
    DOCTEST_CHECK_EQ(keyOnly.cfg.authToken, std::string("sk-key"));
    DOCTEST_CHECK(keyOnly.cfg.warnings.empty());

    auto both = module::claude::parseEnvForReverseSync(
        R"({"ANTHROPIC_API_KEY": "sk-key", "ANTHROPIC_AUTH_TOKEN": "sk-token"})",
        {});
    DOCTEST_REQUIRE(both.ok);
    DOCTEST_CHECK_EQ(both.cfg.authToken, std::string("sk-token"));
    DOCTEST_REQUIRE_EQ(both.cfg.warnings.size(), 1u);
    DOCTEST_CHECK(both.cfg.warnings[0].find("AUTH_TOKEN") != std::string::npos);
    return true;
})

// ---- 守卫表 G1–G6（§3.3；G10 随 advanced-json-editor §8.4 片段 env 拆除）：
// ---- 每条守卫一个专测，编号与用例名一一对应
//（provider_config.h 的守卫表注释列出同样的编号 ↔ 用例名映射）----

// G1 sidecar 托管键 → 跳过（不写也不清）
REGISTER_TEST(provider_config_guard_G1_sidecar_managed_key,
              "provider_config: 守卫 G1 sidecar 托管键跳过（reverse）", {
    using module::claude::parseEnvForReverseSync;
    auto g1 = parseEnvForReverseSync(
        R"({"ANTHROPIC_MODEL": "m-cc", "MY_KEY": "x"})", {"ANTHROPIC_MODEL"});
    DOCTEST_REQUIRE(g1.ok);
    DOCTEST_CHECK(g1.cfg.models.def.model.empty());  // 未被收编
    DOCTEST_CHECK_EQ(g1.cfg.extraEnv.at("MY_KEY"), std::string("x"));
    DOCTEST_CHECK(hasEntry(g1.skippedKeys, "ANTHROPIC_MODEL"));
    return true;
})

// G2 loopback BASE_URL → base_url + token 联动跳过
//（本机 cc-switch 形态：127.0.0.1 + 非占位符的本地 token 不得被当上游 key 收编）
REGISTER_TEST(provider_config_guard_G2_loopback_base_url,
              "provider_config: 守卫 G2 loopback BASE_URL 联动跳过（reverse）", {
    using module::claude::parseEnvForReverseSync;
    auto g2 = parseEnvForReverseSync(
        R"({"ANTHROPIC_BASE_URL": "http://127.0.0.1:15721", "ANTHROPIC_AUTH_TOKEN": "local-token"})",
        {});
    DOCTEST_REQUIRE(g2.ok);
    DOCTEST_CHECK(g2.cfg.baseUrl.empty());
    DOCTEST_CHECK(g2.cfg.authToken.empty());
    DOCTEST_CHECK(hasEntry(g2.skippedKeys, "ANTHROPIC_BASE_URL"));
    DOCTEST_CHECK(hasEntry(g2.skippedKeys, "ANTHROPIC_AUTH_TOKEN"));
    DOCTEST_CHECK(hasEntry(g2.skippedKeys, "ANTHROPIC_API_KEY"));
    return true;
})

// G3 占位 token → 跳过（保持 profile 原密钥）
REGISTER_TEST(provider_config_guard_G3_placeholder_token,
              "provider_config: 守卫 G3 占位 token 跳过（reverse）", {
    using module::claude::parseEnvForReverseSync;
    auto g3 = parseEnvForReverseSync(
        R"({"ANTHROPIC_AUTH_TOKEN": "PROXY_MANAGED"})", {});
    DOCTEST_REQUIRE(g3.ok);
    DOCTEST_CHECK(g3.cfg.authToken.empty());
    DOCTEST_CHECK(hasEntry(g3.skippedKeys, "ANTHROPIC_AUTH_TOKEN"));
    return true;
})

// G4 空值 → 跳过（不拿空串覆盖，也不清空）；别名键同时登记归一后的键名
REGISTER_TEST(provider_config_guard_G4_empty_value,
              "provider_config: 守卫 G4 空值跳过（reverse）", {
    using module::claude::parseEnvForReverseSync;
    auto g4 = parseEnvForReverseSync(
        R"({"DISABLE_AUTOUPDATER": "", "AUTO_COMPACT_ENABLED": ""})", {});
    DOCTEST_REQUIRE(g4.ok);
    DOCTEST_CHECK(g4.cfg.extraEnv.empty());
    DOCTEST_CHECK(hasEntry(g4.skippedKeys, "DISABLE_AUTOUPDATER"));
    DOCTEST_CHECK(hasEntry(g4.skippedKeys, "AUTO_COMPACT_ENABLED"));
    DOCTEST_CHECK(hasEntry(g4.skippedKeys, "DISABLE_AUTO_COMPACT"));
    return true;
})

// G5 键缺席（非守卫托管字段不在 env 里）→ 清空 profile 对应值
//（语义落在 mergeReverseSyncEnv，不在 parse）
REGISTER_TEST(provider_config_guard_G5_absent_clears,
              "provider_config: 守卫 G5 缺席即清空（reverse）", {
    using module::claude::mergeReverseSyncEnv;
    using module::claude::parseEnvForReverseSync;
    using module::claude::parseModelsConfig;

    const std::string curModels = R"json({
        "default": {"model": "m-old", "name": ""},
        "sonnet":  {"model": "s-old", "name": "s-name"}
    })json";
    // env 空 → 全部非守卫托管字段缺席 → 清空（models 槽位 + extra_env）
    auto parsed = parseEnvForReverseSync("{}", {});
    DOCTEST_REQUIRE(parsed.ok);
    auto m = mergeReverseSyncEnv(parsed.cfg, curModels, R"({"K": "v"})");
    DOCTEST_CHECK(m.modelsChanged);
    DOCTEST_CHECK(m.extraEnvChanged);

    auto mc = parseModelsConfig(m.modelsConfigJson);
    DOCTEST_REQUIRE(mc.isOk());
    DOCTEST_CHECK(mc.value().def.model.empty());  // 缺席 → 清空
    DOCTEST_CHECK(mc.value().sonnet.model.empty());
    DOCTEST_CHECK(mc.value().sonnet.name.empty());
    DOCTEST_CHECK(m.extraEnvJson.empty());  // extra_env 缺席 → 清空（空串约定）
    return true;
})

// G6 非字符串 / 超长 → 跳过该键 + warning，不整体 fail
REGISTER_TEST(provider_config_guard_G6_invalid_value,
              "provider_config: 守卫 G6 值形态非法跳过（reverse）", {
    using module::claude::parseEnvForReverseSync;
    auto g6 = parseEnvForReverseSync(
        R"({"NUM_KEY": 1, "BOOL_KEY": true, "OK_KEY": "v"})", {});
    DOCTEST_REQUIRE(g6.ok);
    DOCTEST_CHECK_EQ(g6.cfg.extraEnv.size(), 1u);
    DOCTEST_CHECK(hasEntry(g6.skippedKeys, "NUM_KEY"));
    DOCTEST_CHECK(hasEntry(g6.skippedKeys, "BOOL_KEY"));
    DOCTEST_CHECK_EQ(g6.cfg.warnings.size(), 2u);
    {
        const std::string big =
            "{ \"BIG_KEY\": \"" + std::string(4097, 'x') + "\" }";
        auto rb = parseEnvForReverseSync(big, {});
        DOCTEST_REQUIRE(rb.ok);
        DOCTEST_CHECK(rb.cfg.extraEnv.empty());
        DOCTEST_CHECK(hasEntry(rb.skippedKeys, "BIG_KEY"));
        DOCTEST_CHECK_EQ(rb.cfg.warnings.size(), 1u);
    }
    // 只有 env 本身非法才 ok=false（与 G6 的单键容错分界）
    DOCTEST_CHECK(!parseEnvForReverseSync("{not json", {}).ok);
    DOCTEST_CHECK(!parseEnvForReverseSync("[1,2]", {}).ok);
    return true;
})


// ---- 合并语义（§3.4 出现覆盖 / 缺席清空 / 守卫不写也不清）----
REGISTER_TEST(provider_config_reverse_merge_clear_keep_override,
              "provider_config: mergeReverseSyncEnv 缺席清空/守卫跳过/出现覆盖（reverse）", {
    using module::claude::mergeReverseSyncEnv;
    using module::claude::parseEnvForReverseSync;

    // 现值：3 个槽位 + 2 个 extra 键
    const std::string curModels = R"json({
        "default": {"model": "m-old", "name": ""},
        "sonnet":  {"model": "s-old", "name": "old-name"},
        "opus":    {"model": "o-old", "name": "o-name"}
    })json";
    const std::string curExtra =
        R"json({"DISABLE_AUTOUPDATER": "1", "MY_KEY": "keep"})json";

    // settings.json：default 由 sidecar 托管（G1，不清）；sonnet 出现覆盖
    //（_NAME 缺席 → 清）；opus 全缺席 → 清；haiku 仅 _NAME 出现（其 model 缺席
    // → 清）；MY_KEY 空值（G4，不清）；OTHER 新增；DISABLE_AUTOUPDATER 缺席 → 删
    const std::string env = R"json({
        "ANTHROPIC_MODEL": "m-cc",
        "ANTHROPIC_DEFAULT_SONNET_MODEL": "s-new",
        "ANTHROPIC_DEFAULT_HAIKU_MODEL_NAME": "haiku-name",
        "MY_KEY": "",
        "OTHER": "new"
    })json";
    auto parsed = parseEnvForReverseSync(env, {"ANTHROPIC_MODEL"});
    DOCTEST_REQUIRE(parsed.ok);

    auto m = mergeReverseSyncEnv(parsed.cfg, curModels, curExtra);
    DOCTEST_CHECK(m.modelsChanged);
    DOCTEST_CHECK(m.extraEnvChanged);

    auto mc = module::claude::parseModelsConfig(m.modelsConfigJson);
    DOCTEST_REQUIRE(mc.isOk());
    const auto& s = mc.value();
    DOCTEST_CHECK_EQ(s.def.model, std::string("m-old"));  // G1：不写也不清
    DOCTEST_CHECK_EQ(s.sonnet.model, std::string("s-new"));  // 出现覆盖
    DOCTEST_CHECK(s.sonnet.name.empty());                    // _NAME 缺席 → 清空
    DOCTEST_CHECK(s.opus.model.empty());                     // 缺席 → 清空
    DOCTEST_CHECK(s.opus.name.empty());
    DOCTEST_CHECK_EQ(s.haiku.name, std::string("haiku-name"));  // _NAME 出现覆盖
    DOCTEST_CHECK(s.haiku.model.empty());  // 同一槽位 model 键缺席 → 清空
    DOCTEST_CHECK(s.subagent.model.empty());

    auto ec = module::claude::parseExtraEnv(m.extraEnvJson);
    DOCTEST_REQUIRE(ec.isOk());
    DOCTEST_CHECK_EQ(ec.value().at("MY_KEY"), std::string("keep"));  // G4：不清
    DOCTEST_CHECK_EQ(ec.value().at("OTHER"), std::string("new"));    // 新增
    DOCTEST_CHECK(ec.value().find("DISABLE_AUTOUPDATER") == ec.value().end());
    DOCTEST_CHECK_EQ(ec.value().size(), 2u);
    return true;
})

REGISTER_TEST(provider_config_reverse_merge_no_change,
              "provider_config: mergeReverseSyncEnv 无变化不落库 + 空表约定（reverse）", {
    using module::claude::mergeReverseSyncEnv;
    using module::claude::parseEnvForReverseSync;

    // 无变化：env 与现值逐字段一致（models_config 现值 default 槽位已是 m-1，
    // env 给 m-1 → 不算变化，零写入）
    auto parsed = parseEnvForReverseSync(
        R"({"ANTHROPIC_MODEL": "m-1", "DISABLE_AUTOUPDATER": "1"})", {});
    DOCTEST_REQUIRE(parsed.ok);
    auto m = mergeReverseSyncEnv(parsed.cfg,
                                 R"({"default": {"model": "m-1"}})",
                                 R"({"DISABLE_AUTOUPDATER": "1"})");
    DOCTEST_CHECK(!m.modelsChanged);
    DOCTEST_CHECK(!m.extraEnvChanged);
    DOCTEST_CHECK_EQ(m.modelsConfigJson,
                     std::string(R"({"default": {"model": "m-1"}})"));  // 回传原文
    DOCTEST_CHECK(m.extraEnvJson.find("DISABLE_AUTOUPDATER") != std::string::npos);

    // extra_env 全清 → 空串（本层「空串 = 未配置」约定）
    auto empty = parseEnvForReverseSync("{}", {});
    DOCTEST_REQUIRE(empty.ok);
    auto m2 = mergeReverseSyncEnv(empty.cfg, "", R"({"K": "v"})");
    DOCTEST_CHECK(!m2.modelsChanged);   // 两侧都空 → 不算变化
    DOCTEST_CHECK(m2.extraEnvChanged);  // 缺席 → 清空
    DOCTEST_CHECK(m2.extraEnvJson.empty());
    return true;
})

// ---- 副本命名（§4.2）----
REGISTER_TEST(provider_config_copyname,
              "provider_config: makeCopyName 副本命名与避让（copyname）", {
    using module::claude::makeCopyName;
    using Names = std::vector<std::string>;

    DOCTEST_CHECK_EQ(makeCopyName("官方中转", {}), std::string("官方中转 副本"));
    DOCTEST_CHECK_EQ(makeCopyName("官方中转", Names({"官方中转 副本"})),
                     std::string("官方中转 副本 2"));
    DOCTEST_CHECK_EQ(
        makeCopyName("官方中转", Names({"官方中转 副本", "官方中转 副本 2"})),
        std::string("官方中转 副本 3"));
    DOCTEST_CHECK_EQ(makeCopyName("", {}), std::string(" 副本"));  // 空名

    // 100 次上限：占满 100 个候选 → 空串（调用方按命名失败处理）；退一个可用
    Names full;
    full.push_back("X 副本");
    for (int i = 2; i <= 100; ++i) full.push_back("X 副本 " + std::to_string(i));
    DOCTEST_CHECK_EQ(makeCopyName("X", full), std::string(""));
    Names nearly(full.begin(), full.end() - 1);
    DOCTEST_CHECK_EQ(makeCopyName("X", nearly), std::string("X 副本 100"));
    return true;
})

// ============================================================================
// 槽位单一真相源（审计 CPX-08）
//
// 这三个用例全部**遍历 modelSlotTable()** 断言派生面：新增一个槽位（表加一行）
// 后无需改用例即自动进入覆盖——表行声明什么，接管写入 / 导入识别 / 反向回填 /
// models_config 键 / UI 文案就必须是什么，多写、少写、串槽三种漏改都会红。
// ============================================================================

// 表 → 托管键全集 + 接管实际写入（「遍历表得到的键集合 == 接管里实际写出的键集合」）
REGISTER_TEST(provider_config_slot_table_managed_keys,
              "provider_config: 槽位表派生托管键与接管写入（slot table CPX-08）", {
    using namespace module::claude;
    const auto table = modelSlotTable();
    DOCTEST_CHECK_EQ(table.size(), kModelSlotCount);   // 表长 == 派生的槽位数

    // 表行元数据自洽：键名/文案齐备，声明的 env 键不重复
    std::vector<std::string> declared;
    for (const auto& m : table) {
        DOCTEST_REQUIRE(m.jsonKey != nullptr);
        DOCTEST_REQUIRE(m.envKey != nullptr);
        DOCTEST_REQUIRE(m.label != nullptr);
        DOCTEST_REQUIRE(m.formLabel != nullptr);
        DOCTEST_CHECK(!std::string(m.envKey).empty());
        DOCTEST_CHECK(!std::string(m.label).empty());
        DOCTEST_CHECK(!std::string(m.formLabel).empty());
        declared.push_back(m.envKey);
        if (m.nameKey != nullptr) declared.push_back(m.nameKey);
    }
    std::sort(declared.begin(), declared.end());
    DOCTEST_CHECK(std::adjacent_find(declared.begin(), declared.end()) == declared.end());

    // 托管键全集 == 2 个固定键 ∪ 表声明键（计数 + 逐键命中 → 集合相等）
    const auto managed = managedEnvKeyNames();
    DOCTEST_CHECK_EQ(managed.size(), 2u + declared.size());
    DOCTEST_CHECK(hasEntry(managed, "ANTHROPIC_BASE_URL"));
    DOCTEST_CHECK(hasEntry(managed, "ANTHROPIC_AUTH_TOKEN"));
    for (const auto& k : declared) DOCTEST_CHECK(hasEntry(managed, k.c_str()));

    // 逐行：只填该槽位 → 接管写出的槽位键恰好 = 该行声明（多写/少写都红）
    for (const auto& m : table) {
        ModelsConfig mc;
        (mc.*m.slot).model = std::string("m-") + m.jsonKey;
        (mc.*m.slot).name = std::string("n-") + m.jsonKey;
        auto text = serializeModelsConfig(mc);
        DOCTEST_REQUIRE(text.isOk());
        const auto env = buildProxyEnv(text.value(), "", 8888);
        std::vector<std::string> written;
        for (const auto& kv : env) {
            if (kv.first == "ANTHROPIC_BASE_URL" ||
                kv.first == "ANTHROPIC_AUTH_TOKEN") {
                continue;
            }
            written.push_back(kv.first);
        }
        DOCTEST_REQUIRE_EQ(written.size(), m.nameKey != nullptr ? 2u : 1u);
        DOCTEST_CHECK_EQ(written[0], std::string(m.envKey));
        DOCTEST_CHECK_EQ(env.at(m.envKey), std::string("m-") + m.jsonKey);
        if (m.nameKey != nullptr) {
            DOCTEST_CHECK_EQ(written[1], std::string(m.nameKey));
            DOCTEST_CHECK_EQ(env.at(m.nameKey), std::string("n-") + m.jsonKey);
        }
        for (const auto& k : written) DOCTEST_CHECK(hasEntry(managed, k.c_str()));
    }

    // 全体槽位填满 → 接管写出的槽位键集合 == 表声明键集合（两条路径同集合）
    {
        ModelsConfig all;
        for (const auto& m : table) {
            (all.*m.slot).model = std::string("m-") + m.jsonKey;
            (all.*m.slot).name = std::string("n-") + m.jsonKey;
        }
        auto text = serializeModelsConfig(all);
        DOCTEST_REQUIRE(text.isOk());
        const auto env = buildProxyEnv(text.value(), "", 8888);
        std::vector<std::string> written;
        for (const auto& kv : env) {
            if (kv.first == "ANTHROPIC_BASE_URL" ||
                kv.first == "ANTHROPIC_AUTH_TOKEN") {
                continue;
            }
            written.push_back(kv.first);
        }
        std::sort(written.begin(), written.end());
        DOCTEST_CHECK_EQ(written, declared);
    }
    return true;
})

// 表 → 导入识别 + 反向回填（逐行：该行的 env 键 → 恰好落到该槽位）
REGISTER_TEST(provider_config_slot_table_import_and_reverse,
              "provider_config: 槽位表派生导入识别与反向回填（slot table CPX-08）", {
    using namespace module::claude;
    for (const auto& m : modelSlotTable()) {
        std::string json = std::string("{\"") + m.envKey + "\": \"m-x\"";
        if (m.nameKey != nullptr) {
            json += std::string(", \"") + m.nameKey + "\": \"n-x\"";
        }
        json += "}";

        // 粘贴导入：只认该槽位；槽位键不落 extra_env
        auto imp = parseImportedEnv(json);
        DOCTEST_REQUIRE(imp.isOk());
        DOCTEST_CHECK_EQ(imp.value().recognizedSlots, 1);
        DOCTEST_CHECK_EQ((imp.value().models.*m.slot).model, std::string("m-x"));
        DOCTEST_CHECK(imp.value().extraEnv.empty());
        if (m.nameKey != nullptr) {
            DOCTEST_CHECK_EQ((imp.value().models.*m.slot).name, std::string("n-x"));
        }

        // 反向同步：同输入落同一槽位，合并结果能原样读回
        auto rp = parseEnvForReverseSync(json, {});
        DOCTEST_REQUIRE(rp.ok);
        DOCTEST_CHECK_EQ(rp.cfg.recognizedSlots, 1);
        DOCTEST_CHECK_EQ((rp.cfg.models.*m.slot).model, std::string("m-x"));
        auto merged = mergeReverseSyncEnv(rp.cfg, "", "");
        DOCTEST_CHECK(merged.modelsChanged);
        auto back = parseModelsConfig(merged.modelsConfigJson);
        DOCTEST_REQUIRE(back.isOk());
        DOCTEST_CHECK_EQ((back.value().*m.slot).model, std::string("m-x"));
    }
    return true;
})

// 表 → models_config JSON 键 + UI 文案（UI 遍历派生的前提：文案齐备且互异）
REGISTER_TEST(provider_config_slot_table_json_and_labels,
              "provider_config: 槽位表 JSON 键与 UI 文案（slot table CPX-08）", {
    using namespace module::claude;
    std::vector<std::string> labels, formLabels, jsonKeys;
    for (const auto& m : modelSlotTable()) {
        // 单槽位填充 → 序列化含该行 JSON 键、反序列化回同一槽位、其余槽位仍空
        ModelsConfig mc;
        (mc.*m.slot).model = "m-x";
        auto text = serializeModelsConfig(mc);
        DOCTEST_REQUIRE(text.isOk());
        DOCTEST_CHECK(text.value().find(std::string("\"") + m.jsonKey + "\"") !=
                      std::string::npos);
        auto back = parseModelsConfig(text.value());
        DOCTEST_REQUIRE(back.isOk());
        DOCTEST_CHECK_EQ((back.value().*m.slot).model, std::string("m-x"));
        int filled = 0;
        for (const auto& n : modelSlotTable()) {
            if (!(back.value().*n.slot).model.empty()) ++filled;
        }
        DOCTEST_CHECK_EQ(filled, 1);   // JSON 键不串槽

        labels.push_back(m.label);
        formLabels.push_back(m.formLabel);
        jsonKeys.push_back(m.jsonKey);
    }
    // 两套 UI 文案 + JSON 键各自互异（同屏 N 行不重名）
    for (const auto* v : {&labels, &formLabels, &jsonKeys}) {
        auto sorted = *v;
        std::sort(sorted.begin(), sorted.end());
        DOCTEST_CHECK(std::unique(sorted.begin(), sorted.end()) == sorted.end());
    }
    // 既有 UI 外观取样（详情面板 / 表单页两套文案），防收敛时误改外观
    const auto table = modelSlotTable();
    DOCTEST_REQUIRE(!table.empty());
    DOCTEST_CHECK_EQ(std::string(table.front().label), std::string("默认"));
    DOCTEST_CHECK_EQ(std::string(table.front().formLabel), std::string("默认 (MODEL)"));
    DOCTEST_CHECK_EQ(std::string(table.back().label), std::string("子代理"));
    DOCTEST_CHECK_EQ(std::string(table.back().formLabel), std::string("Subagent"));
    return true;
})

// ---- failover 换家判据（设计 §3.8；纯函数，判据表逐行锚定）----
// 换 → true / 不换 → false，与设计表的 category 一一对应：
//   传输失败（statusCode==0）                换
//   401 / 403（换 key 才可能变好）            换
//   429（限流）                             换
//   5xx 含 500/502/503/504/529（上游故障）    换
//   400/405/406/413/414/415/422/501（负列表） 不换
//   其余 4xx（404/408 等，负列表之外都试）     换
//   errorKind=="no_active_profile"          不换（DevDesk 自身 503 兜底防御）
REGISTER_TEST(provider_config_failover_switch_categories,
              "shouldFailoverTo: 换家类别逐行（传输/401/403/429/5xx/其余4xx）", {
    // 传输失败：无响应头 / DNS / TLS / 连接 / 超时 = statusCode==0
    //（我们这侧 = 502 upstream_unreachable；errorKind 只是日志口径）
    DOCTEST_CHECK(module::claude::shouldFailoverTo(0, "upstream_unreachable"));
    // 401 / 403：只有换 key 才可能变好
    DOCTEST_CHECK(module::claude::shouldFailoverTo(401, ""));
    DOCTEST_CHECK(module::claude::shouldFailoverTo(403, ""));
    // 429：上游限流
    DOCTEST_CHECK(module::claude::shouldFailoverTo(429, ""));
    // 5xx：上游侧故障（含 500/502/503/504/529 过载）——503 空 errorKind 时
    // 属上游 5xx 档（换）；带 no_active_profile 才归「不换」（见下一条用例）
    for (const int sc : {500, 502, 503, 504, 529}) {
        DOCTEST_CHECK(module::claude::shouldFailoverTo(sc, ""));
    }
    // 其余 4xx（负列表之外都试）：404 可能是「该网关没有这个模型」，换家能救
    DOCTEST_CHECK(module::claude::shouldFailoverTo(404, ""));
    DOCTEST_CHECK(module::claude::shouldFailoverTo(408, ""));
    return true;
})

REGISTER_TEST(provider_config_failover_no_switch_categories,
              "shouldFailoverTo: 不换家类别逐行（负列表 / no_active_profile 兜底）", {
    // 负列表（请求自身非法——换家同结果，白烧一次请求）
    for (const int sc : {400, 405, 406, 413, 414, 415, 422, 501}) {
        DOCTEST_CHECK_FALSE(module::claude::shouldFailoverTo(sc, ""));
    }
    // DevDesk 自身 503 no_active_profile → 不换（writeNoProfileError 在转发
    // **前**直接回，判定点理论收不到；本行是防御——本地配置错误不换家）
    DOCTEST_CHECK_FALSE(
        module::claude::shouldFailoverTo(503, "no_active_profile"));
    // errorKind 兜底优先于状态码：即使换家档状态码带上 no_active_profile 也不换
    DOCTEST_CHECK_FALSE(
        module::claude::shouldFailoverTo(500, "no_active_profile"));
    DOCTEST_CHECK_FALSE(
        module::claude::shouldFailoverTo(0, "no_active_profile"));
    // 2xx/3xx 到不了判定点（调用方只在失败时判），保守不换
    DOCTEST_CHECK_FALSE(module::claude::shouldFailoverTo(200, ""));
    DOCTEST_CHECK_FALSE(module::claude::shouldFailoverTo(302, ""));
    return true;
})


// ===== 高级设置 JSON 纯函数（2026-09-30 advanced-json-editor 设计 §4.3，无头）=====

// ---- 生效合并：片段赢 / 解绑回落 / env 键跳过 + 警告 / 非法按空 / 双空 ----
REGISTER_TEST(provider_config_extra_settings_effective,
              "provider_config: effectiveExtraSettingsJson 片段赢/解绑回落/env 键跳过/非法按空/双空", {
    using module::claude::effectiveExtraSettingsJson;
    std::vector<std::string> warnings;

    // 片段赢（同键片段值胜；各自独有键都在；输出 pretty 规范形）
    {
        const std::string eff = effectiveExtraSettingsJson(
            R"({"model": "profile-model", "statusLine": "profile-s"})",
            R"({"model": "fragment-model", "hooks": {}})", &warnings);
        DOCTEST_REQUIRE(!eff.empty());
        auto parsed = platform::json::parse(eff);
        DOCTEST_REQUIRE(parsed.isOk());
        auto obj = parsed.value().asObject();
        DOCTEST_REQUIRE(obj.has("model"));
        DOCTEST_CHECK_EQ(obj.get("model").asString(), std::string("fragment-model"));
        DOCTEST_CHECK_EQ(obj.get("statusLine").asString(), std::string("profile-s"));
        DOCTEST_CHECK(obj.has("hooks"));
        DOCTEST_CHECK(eff.find("\n  \"") != std::string::npos);   // pretty 形
        DOCTEST_CHECK(warnings.empty());
    }
    warnings.clear();
    // 解绑回落（fragmentJson="" → profile 值原样语义等值；round-trip 后键值不变）
    {
        const std::string eff = effectiveExtraSettingsJson(
            R"({"model": "profile-model"})", "", &warnings);
        auto parsed = platform::json::parse(eff);
        DOCTEST_REQUIRE(parsed.isOk());
        DOCTEST_CHECK_EQ(parsed.value().asObject().get("model").asString(),
                         std::string("profile-model"));
        DOCTEST_CHECK(warnings.empty());
    }
    warnings.clear();
    // 任一侧 "env" 键跳过 + warnings 记录（D-5 读侧兜底）；另一侧照用
    {
        const std::string eff = effectiveExtraSettingsJson(
            R"({"env": {"A": "1"}, "permissions": {"p": 1}})",
            R"({"env": {"B": "2"}, "model": "m"})", &warnings);
        auto parsed = platform::json::parse(eff);
        DOCTEST_REQUIRE(parsed.isOk());
        auto obj = parsed.value().asObject();
        DOCTEST_CHECK_FALSE(obj.has("env"));
        DOCTEST_CHECK(obj.has("permissions"));
        DOCTEST_CHECK_EQ(obj.get("model").asString(), std::string("m"));
        DOCTEST_CHECK_EQ(warnings.size(), static_cast<size_t>(2));   // 两侧各一条
    }
    warnings.clear();
    // 非法一侧按空处理（warnings 记录），另一侧照用
    {
        const std::string eff = effectiveExtraSettingsJson(
            "{broken", R"({"model": "m"})", &warnings);
        auto parsed = platform::json::parse(eff);
        DOCTEST_REQUIRE(parsed.isOk());
        DOCTEST_CHECK_EQ(parsed.value().asObject().get("model").asString(),
                         std::string("m"));
        DOCTEST_CHECK_EQ(warnings.size(), static_cast<size_t>(1));
    }
    // 顶层非对象一侧同样按空处理
    {
        std::vector<std::string> w2;
        const std::string eff = effectiveExtraSettingsJson(
            "[1,2]", R"({"model": "m"})", &w2);
        DOCTEST_CHECK_EQ(w2.size(), static_cast<size_t>(1));
        auto parsed = platform::json::parse(eff);
        DOCTEST_REQUIRE(parsed.isOk());
        DOCTEST_CHECK(parsed.value().asObject().has("model"));
    }
    // 双空返回空串
    {
        std::vector<std::string> w3;
        DOCTEST_CHECK(effectiveExtraSettingsJson("", "", &w3).empty());
        DOCTEST_CHECK(w3.empty());
    }
    return true;
})

// ---- 文档并入：写入键集 / model 抑制 / env 键跳过 / 非法零写入 ----
REGISTER_TEST(provider_config_extra_settings_apply,
              "provider_config: applyExtraSettingsToSettingsDoc 写入键集/model 抑制/env 跳过/顶层非对象", {
    using module::claude::applyExtraSettingsToSettingsDoc;
    using platform::json::JsonValue;

    // 写入键集正确；"model"（非空字符串）触发抑制：env 中 ANTHROPIC_MODEL 被移除、
    // ANTHROPIC_DEFAULT_SONNET_MODEL 保留（其余槽位键无顶层对映键，照常）
    {
        platform::json::JsonObject envObj;
        envObj.set("ANTHROPIC_MODEL", platform::json::makeString("slot-model"));
        envObj.set("ANTHROPIC_DEFAULT_SONNET_MODEL",
                   platform::json::makeString("slot-sonnet"));
        envObj.set("MY_KEY", platform::json::makeString("v"));
        platform::json::JsonObject root;
        root.set("env", JsonValue(std::move(envObj)));
        root.set("live", platform::json::makeString("old"));
        // D-3 夹具：live 侧预置与 JSON 同名的 statusLine（异值）——没有同名键
        // 时「JSON 赢过 live」的断言恒真（假绿）
        root.set("statusLine", platform::json::makeString("live-s"));

        auto r = applyExtraSettingsToSettingsDoc(
            root, R"({"model": "json-model", "statusLine": "on"})");
        DOCTEST_CHECK(r.modelSuppressed);
        DOCTEST_REQUIRE_EQ(r.writtenKeys.size(), static_cast<size_t>(2));
        DOCTEST_CHECK_EQ(r.writtenKeys[0], std::string("model"));
        DOCTEST_CHECK_EQ(r.writtenKeys[1], std::string("statusLine"));
        DOCTEST_CHECK(r.warnings.empty());
        // D-3：JSON 赢过 live 既有同名非 env 键——statusLine 取 JSON 值 "on"，
        // live 原值 "live-s" 被压过；JSON 没有的 "live" 键零触碰
        DOCTEST_CHECK_EQ(root.get("live").asString(), std::string("old"));
        DOCTEST_CHECK_EQ(root.get("model").asString(), std::string("json-model"));
        DOCTEST_CHECK_EQ(root.get("statusLine").asString(), std::string("on"));
        // 抑制：ANTHROPIC_MODEL 移除、SONNET 保留
        auto envAfter = root.get("env").asObject();
        DOCTEST_CHECK_FALSE(envAfter.has("ANTHROPIC_MODEL"));
        DOCTEST_CHECK(envAfter.has("ANTHROPIC_DEFAULT_SONNET_MODEL"));
        DOCTEST_CHECK(envAfter.has("MY_KEY"));
    }
    // "model" 非字符串 / 空串 → 不抑制（D-4：非空字符串才触发）
    {
        platform::json::JsonObject envObj;
        envObj.set("ANTHROPIC_MODEL", platform::json::makeString("slot-model"));
        platform::json::JsonObject root;
        root.set("env", JsonValue(std::move(envObj)));
        auto r1 = applyExtraSettingsToSettingsDoc(root, R"({"model": ""})");
        DOCTEST_CHECK_FALSE(r1.modelSuppressed);
        DOCTEST_CHECK(root.get("env").asObject().has("ANTHROPIC_MODEL"));
        auto r2 = applyExtraSettingsToSettingsDoc(root, R"({"model": 42})");
        DOCTEST_CHECK_FALSE(r2.modelSuppressed);
        DOCTEST_CHECK(root.get("env").asObject().has("ANTHROPIC_MODEL"));
    }
    // "env" 键跳过 + WARN；写入键集不含它
    {
        platform::json::JsonObject root;
        root.set("env", JsonValue(platform::json::makeObject()));
        auto r = applyExtraSettingsToSettingsDoc(
            root, R"({"env": {"A": "1"}, "hooks": {}})");
        DOCTEST_REQUIRE_EQ(r.writtenKeys.size(), static_cast<size_t>(1));
        DOCTEST_CHECK_EQ(r.writtenKeys[0], std::string("hooks"));
        DOCTEST_REQUIRE_EQ(r.warnings.size(), static_cast<size_t>(1));
        DOCTEST_CHECK(r.warnings[0].find("env") != std::string::npos);
    }
    // 非法 / 超限 / 顶层非对象输入 → warnings 不写
    {
        platform::json::JsonObject root;
        root.set("env", JsonValue(platform::json::makeObject()));
        auto r1 = applyExtraSettingsToSettingsDoc(root, "{not-json");
        DOCTEST_CHECK(r1.writtenKeys.empty());
        DOCTEST_CHECK_EQ(r1.warnings.size(), static_cast<size_t>(1));
        auto r2 = applyExtraSettingsToSettingsDoc(root, "[1]");
        DOCTEST_CHECK(r2.writtenKeys.empty());
        DOCTEST_CHECK_EQ(r2.warnings.size(), static_cast<size_t>(1));
        std::string big(1 * 1024 * 1024 + 1, 'x');
        big[0] = '{';
        big[big.size() - 1] = '}';
        auto r3 = applyExtraSettingsToSettingsDoc(root, big);
        DOCTEST_CHECK(r3.writtenKeys.empty());
        DOCTEST_CHECK_EQ(r3.warnings.size(), static_cast<size_t>(1));
        DOCTEST_CHECK(r3.warnings[0].find("1MB") != std::string::npos);
    }
    // 空串 → 零写入零警告（未配置）
    {
        platform::json::JsonObject root;
        auto r = applyExtraSettingsToSettingsDoc(root, "");
        DOCTEST_CHECK(r.writtenKeys.empty());
        DOCTEST_CHECK_FALSE(r.modelSuppressed);
        DOCTEST_CHECK(r.warnings.empty());
    }
    return true;
})

// ---- 校验五态：空串 OK / 非法 / 顶层数组 / 超 1MB / 含 "env" ----
REGISTER_TEST(provider_config_extra_settings_validate,
              "provider_config: validateExtraSettingsJson 空串/非法/顶层数组/超限/env 键五态", {
    using module::claude::validateExtraSettingsJson;
    using module::claude::kMaxExtraSettingsBytes;

    DOCTEST_CHECK_EQ(kMaxExtraSettingsBytes, static_cast<size_t>(1u * 1024u * 1024u));
    DOCTEST_CHECK(validateExtraSettingsJson("").isOk());
    DOCTEST_CHECK(validateExtraSettingsJson("   \n\t ").isOk());   // 空白 = 未配置
    DOCTEST_CHECK(validateExtraSettingsJson(R"({"model": "m"})").isOk());

    auto e1 = validateExtraSettingsJson("{not-json");
    DOCTEST_REQUIRE(e1.isFail());
    DOCTEST_CHECK(e1.errorMessage().find("解析失败") != std::string::npos);

    auto e2 = validateExtraSettingsJson("[1,2]");
    DOCTEST_REQUIRE(e2.isFail());
    DOCTEST_CHECK(e2.errorMessage().find("顶层必须是 JSON 对象") != std::string::npos);

    std::string big(1 * 1024 * 1024 + 1, 'x');
    big[0] = '{';
    big[big.size() - 1] = '}';
    auto e3 = validateExtraSettingsJson(big);
    DOCTEST_REQUIRE(e3.isFail());
    DOCTEST_CHECK(e3.errorMessage().find("1MB") != std::string::npos);

    auto e4 = validateExtraSettingsJson(R"({"env": {"A": "1"}})");
    DOCTEST_REQUIRE(e4.isFail());
    DOCTEST_CHECK(e4.errorMessage().find("不能包含 env 键") != std::string::npos);
    return true;
})

// ---- F-10：def 槽位 envKey 单一来源自槽位表派生（不手抄键名）----
REGISTER_TEST(provider_config_model_slot_def_env_key,
              "provider_config: modelSlotDefEnvKey/isModelSlotEnvKey 自槽位表派生（F-10）", {
    using module::claude::modelSlotDefEnvKey;
    using module::claude::isModelSlotEnvKey;
    // def 行的 envKey 必须与槽位表一致（对照表项，不对照手抄常量）
    const char* expected = nullptr;
    for (const auto& m : module::claude::modelSlotTable()) {
        if (std::string(m.jsonKey) == "default") expected = m.envKey;
    }
    DOCTEST_REQUIRE(expected != nullptr);
    DOCTEST_CHECK_EQ(std::string(modelSlotDefEnvKey()), std::string(expected));
    // 槽位键判定：envKey 命中 / *_NAME 键命中 / 非槽位键不命中
    DOCTEST_CHECK(isModelSlotEnvKey(modelSlotDefEnvKey()));
    bool hasNameKey = false;
    for (const auto& m : module::claude::modelSlotTable()) {
        if (m.nameKey != nullptr) {
            hasNameKey = true;
            DOCTEST_CHECK(isModelSlotEnvKey(m.nameKey));
        }
    }
    DOCTEST_CHECK(hasNameKey);
    DOCTEST_CHECK_FALSE(isModelSlotEnvKey("ANTHROPIC_BASE_URL"));
    DOCTEST_CHECK_FALSE(isModelSlotEnvKey("MY_CUSTOM_KEY"));
    return true;
})

// ---- batch-C C-4：nonEnvTopLevelJson（settings.json 原文 → 非 env 顶层属性）----
// 抽取规则边界（评审 minor：补测试）：空/纯空白 / 超输入上限 / 非法 JSON /
// 顶层非对象 / 除 env 外无键 → **静默**空串（无 warning 通道，与
// validateExtraSettingsJson「超限按未配置」同口径，导入不产生超限值）；
// 正常路径 = pretty 规范形 + env 排除 + 键序字典序（与 effectiveExtraSettingsJson
// 同一条序列化通道）。
REGISTER_TEST(provider_config_non_env_top_level_json,
              "provider_config: nonEnvTopLevelJson 抽取规则与边界（空/非对象/无键/超限静默）", {
    using module::claude::nonEnvTopLevelJson;

    // 正常：非 env 顶层属性全取 + env 排除 + pretty + 字典序
    {
        const std::string j = nonEnvTopLevelJson(R"({
            "statusLine": {"type": "command"},
            "env": {"ANTHROPIC_BASE_URL": "https://x"},
            "model": "m-1",
            "permissions": {"allow": ["Bash"]}
        })");
        DOCTEST_REQUIRE(!j.empty());
        DOCTEST_CHECK(j.find("\n  \"") != std::string::npos);   // pretty 形
        auto parsed = platform::json::parse(j);
        DOCTEST_REQUIRE(parsed.isOk());
        auto obj = parsed.value().asObject();
        DOCTEST_CHECK(obj.has("model"));
        DOCTEST_CHECK(obj.has("permissions"));
        DOCTEST_CHECK(obj.has("statusLine"));
        DOCTEST_CHECK_FALSE(obj.has("env"));
        DOCTEST_REQUIRE_EQ(obj.keys().size(), static_cast<size_t>(3));
        DOCTEST_CHECK_EQ(obj.keys()[0], std::string("model"));
        DOCTEST_CHECK_EQ(obj.keys()[1], std::string("permissions"));
        DOCTEST_CHECK_EQ(obj.keys()[2], std::string("statusLine"));
    }
    // 空 / 纯空白 → 空串
    DOCTEST_CHECK(nonEnvTopLevelJson("").empty());
    DOCTEST_CHECK(nonEnvTopLevelJson("   \n\t ").empty());
    // 非法 JSON / 顶层非对象（数组、标量）→ 空串（读路径容错，不炸）
    DOCTEST_CHECK(nonEnvTopLevelJson("{broken").empty());
    DOCTEST_CHECK(nonEnvTopLevelJson("[1,2,3]").empty());
    DOCTEST_CHECK(nonEnvTopLevelJson("\"just-a-string\"").empty());
    // 只有 env（含 env = 空对象）→ 空串（= 未配置，与 extraSettingsJson 语义一致）
    DOCTEST_CHECK(nonEnvTopLevelJson(R"({"env": {"A": "1"}})").empty());
    DOCTEST_CHECK(nonEnvTopLevelJson(R"({"env": {}})").empty());
    // 输入超上限（kMaxImportTextBytes = 1MB）→ 静默空串（挡在解析之前）
    {
        const std::string huge = "{\"model\":\"" +
            std::string(module::claude::kMaxImportTextBytes, 'x') + "\"}";
        DOCTEST_CHECK(huge.size() > module::claude::kMaxImportTextBytes);
        DOCTEST_CHECK(nonEnvTopLevelJson(huge).empty());
    }
    // 输出超上限（kMaxExtraSettingsBytes = 1MB）：compact 输入 < 1MB、pretty 后
    // 超限 → 静默空串（不产超限值；不设 warning 通道）
    {
        std::string big = "{\"big\":[";
        big.reserve(500000);
        for (int i = 0; i < 200000; ++i) {
            if (i > 0) big += ',';
            big += '1';
        }
        big += "]}";
        DOCTEST_REQUIRE(big.size() <= module::claude::kMaxImportTextBytes);
        DOCTEST_CHECK(nonEnvTopLevelJson(big).empty());
    }
    return true;
})
