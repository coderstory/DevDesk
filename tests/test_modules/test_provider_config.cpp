// tests/test_modules/test_provider_config.cpp
// 供应商配置数据模型层：models_config / extra_env 序列化、导入解析、反向同步（settings.json → profile）、
// 高级设置 JSON 纯函数、failover 判据、[1M] 标记三纯函数、槽位表派生面一致性。
//
// 跨文件前提：
//   - 槽位是单一真相源：modelSlotTable() 声明什么，接管写入 / 导入识别 / 反向回填 / models_config 键 /
//     UI 文案就必须是什么。末尾几条用例遍历该表断言派生面，加一行槽位即自动进入覆盖。
//   - 反向同步三态语义落在 mergeReverseSyncEnv 而非 parse：键**出现** → 覆盖；键**缺席** → 清空
//     （空串约定）；由 sidecar 托管的键 → 不写也不清。
//   - 反向同步的认证字段三态与空值容错口径，
//     UI 侧据此不覆盖表单现值。
//   - 项目无历史包袱，schema 不做旧库升级，新列靠表默认值回落。
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

// 临时 db 文件名前缀（test_support::tempDbPath / TestDb 的第一实参）
constexpr const char* kDbPrefix = "devdesk_test_pconfig_";

} // namespace

// models_config 序列化往返（6 槽位全填）
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

// 空槽位：全空配置往返 + 空串/空对象解析
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

// 非法 JSON 容错
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

// models_config 读入槽位（空 / 非法 JSON → 全空槽位）
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

// extra_env 序列化往返 + 非字符串值拒绝
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

    // 值非字符串 → 拒绝（不做隐式转换，避免 "1" 与 1 的语义歧义）
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

// displayName 派生
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

// 别名映射
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
    // "true" → 0；空值 → 1（保守：无法确认时按关处理）
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

// 官方键判定源 = env_var_hints
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

// ClaudeService 读写往返 + model 列同步
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

// 新列默认值 + 空 JSON 读侧回落
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
            "api_key_check, model, is_active, "
            "created_at, updated_at) VALUES "
            "('默认供应商', 'https://old.example.com', 'sk-***abcd', "
            "'deadbeef', 'claude-3-5-haiku', 1, 0, 0)");
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
        // Profile::model 是活列、双向同步，没有 fallback 语义可断言
    }
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
    return true;
})

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

// 反向同步：映射表 / 守卫表 / 出现缺席语义

namespace {

bool hasEntry(const std::vector<std::string>& keys, const char* key) {
    for (const auto& k : keys) {
        if (k == key) return true;
    }
    return false;
}

} // namespace

// isLoopbackUrl
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

// 映射表 14 条
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

// sidecar 托管键 → 跳过（不写也不清）
REGISTER_TEST(provider_config_guard_sidecar_managed_key,
              "provider_config: sidecar 托管键跳过（reverse）", {
    using module::claude::parseEnvForReverseSync;
    auto sidecarManaged = parseEnvForReverseSync(
        R"({"ANTHROPIC_MODEL": "m-cc", "MY_KEY": "x"})", {"ANTHROPIC_MODEL"});
    DOCTEST_REQUIRE(sidecarManaged.ok);
    DOCTEST_CHECK(sidecarManaged.cfg.models.def.model.empty());  // 未被收编
    DOCTEST_CHECK_EQ(sidecarManaged.cfg.extraEnv.at("MY_KEY"), std::string("x"));
    DOCTEST_CHECK(hasEntry(sidecarManaged.skippedKeys, "ANTHROPIC_MODEL"));
    return true;
})

// loopback BASE_URL → base_url + token 联动跳过（本机 cc-switch 形态：127.0.0.1 +
// 非占位符的本地 token 不得被当上游 key 收编）
REGISTER_TEST(provider_config_guard_loopback_base_url,
              "provider_config: loopback BASE_URL 联动跳过（reverse）", {
    using module::claude::parseEnvForReverseSync;
    auto loopbackBaseUrl = parseEnvForReverseSync(
        R"({"ANTHROPIC_BASE_URL": "http://127.0.0.1:15721", "ANTHROPIC_AUTH_TOKEN": "local-token"})",
        {});
    DOCTEST_REQUIRE(loopbackBaseUrl.ok);
    DOCTEST_CHECK(loopbackBaseUrl.cfg.baseUrl.empty());
    DOCTEST_CHECK(loopbackBaseUrl.cfg.authToken.empty());
    DOCTEST_CHECK(hasEntry(loopbackBaseUrl.skippedKeys, "ANTHROPIC_BASE_URL"));
    DOCTEST_CHECK(hasEntry(loopbackBaseUrl.skippedKeys, "ANTHROPIC_AUTH_TOKEN"));
    DOCTEST_CHECK(hasEntry(loopbackBaseUrl.skippedKeys, "ANTHROPIC_API_KEY"));
    return true;
})

// 占位 token → 跳过（保持 profile 原密钥）
REGISTER_TEST(provider_config_guard_placeholder_token,
              "provider_config: 占位 token 跳过（reverse）", {
    using module::claude::parseEnvForReverseSync;
    auto placeholderToken = parseEnvForReverseSync(
        R"({"ANTHROPIC_AUTH_TOKEN": "PROXY_MANAGED"})", {});
    DOCTEST_REQUIRE(placeholderToken.ok);
    DOCTEST_CHECK(placeholderToken.cfg.authToken.empty());
    DOCTEST_CHECK(hasEntry(placeholderToken.skippedKeys, "ANTHROPIC_AUTH_TOKEN"));
    return true;
})

// 空值 → 跳过（不拿空串覆盖，也不清空）；别名键同时登记归一后的键名
REGISTER_TEST(provider_config_guard_empty_value,
              "provider_config: 空值跳过（reverse）", {
    using module::claude::parseEnvForReverseSync;
    auto emptyValue = parseEnvForReverseSync(
        R"({"DISABLE_AUTOUPDATER": "", "AUTO_COMPACT_ENABLED": ""})", {});
    DOCTEST_REQUIRE(emptyValue.ok);
    DOCTEST_CHECK(emptyValue.cfg.extraEnv.empty());
    DOCTEST_CHECK(hasEntry(emptyValue.skippedKeys, "DISABLE_AUTOUPDATER"));
    DOCTEST_CHECK(hasEntry(emptyValue.skippedKeys, "AUTO_COMPACT_ENABLED"));
    DOCTEST_CHECK(hasEntry(emptyValue.skippedKeys, "DISABLE_AUTO_COMPACT"));
    return true;
})

// 键缺席（非托管字段不在 env 里）→ 清空 profile 对应值。语义落在 mergeReverseSyncEnv，
// 不在 parse
REGISTER_TEST(provider_config_guard_absent_clears,
              "provider_config: 缺席即清空（reverse）", {
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

// 非字符串 / 超长 → 跳过该键 + warning，不整体 fail
REGISTER_TEST(provider_config_guard_invalid_value,
              "provider_config: 值形态非法跳过（reverse）", {
    using module::claude::parseEnvForReverseSync;
    auto invalidValue = parseEnvForReverseSync(
        R"({"NUM_KEY": 1, "BOOL_KEY": true, "OK_KEY": "v"})", {});
    DOCTEST_REQUIRE(invalidValue.ok);
    DOCTEST_CHECK_EQ(invalidValue.cfg.extraEnv.size(), 1u);
    DOCTEST_CHECK(hasEntry(invalidValue.skippedKeys, "NUM_KEY"));
    DOCTEST_CHECK(hasEntry(invalidValue.skippedKeys, "BOOL_KEY"));
    DOCTEST_CHECK_EQ(invalidValue.cfg.warnings.size(), 2u);
    {
        const std::string big =
            "{ \"BIG_KEY\": \"" + std::string(4097, 'x') + "\" }";
        auto rb = parseEnvForReverseSync(big, {});
        DOCTEST_REQUIRE(rb.ok);
        DOCTEST_CHECK(rb.cfg.extraEnv.empty());
        DOCTEST_CHECK(hasEntry(rb.skippedKeys, "BIG_KEY"));
        DOCTEST_CHECK_EQ(rb.cfg.warnings.size(), 1u);
    }
    // 只有 env 本身非法才 ok=false（值形态非法是单键容错，不整体 fail）
    DOCTEST_CHECK(!parseEnvForReverseSync("{not json", {}).ok);
    DOCTEST_CHECK(!parseEnvForReverseSync("[1,2]", {}).ok);
    return true;
})
// 合并语义：出现覆盖 / 缺席清空 / 守卫不写也不清
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

    // settings.json：default 由 sidecar 托管（不清）；sonnet 出现覆盖
    //（_NAME 缺席 → 清）；opus 全缺席 → 清；haiku 仅 _NAME 出现（其 model 缺席
    // → 清）；MY_KEY 空值（不清）；OTHER 新增；DISABLE_AUTOUPDATER 缺席 → 删
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
    DOCTEST_CHECK_EQ(s.def.model, std::string("m-old"));  // sidecar 托管：不写也不清
    DOCTEST_CHECK_EQ(s.sonnet.model, std::string("s-new"));  // 出现覆盖
    DOCTEST_CHECK(s.sonnet.name.empty());                    // _NAME 缺席 → 清空
    DOCTEST_CHECK(s.opus.model.empty());                     // 缺席 → 清空
    DOCTEST_CHECK(s.opus.name.empty());
    DOCTEST_CHECK_EQ(s.haiku.name, std::string("haiku-name"));  // _NAME 出现覆盖
    DOCTEST_CHECK(s.haiku.model.empty());  // 同一槽位 model 键缺席 → 清空
    DOCTEST_CHECK(s.subagent.model.empty());

    auto ec = module::claude::parseExtraEnv(m.extraEnvJson);
    DOCTEST_REQUIRE(ec.isOk());
    DOCTEST_CHECK_EQ(ec.value().at("MY_KEY"), std::string("keep"));  // 空值：不清
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

// 副本命名
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

    // 100 次上限：占满 100 个候选 → 空串（调用方按命名失败处理）；退一个则仍可用
    Names full;
    full.push_back("X 副本");
    for (int i = 2; i <= 100; ++i) full.push_back("X 副本 " + std::to_string(i));
    DOCTEST_CHECK_EQ(makeCopyName("X", full), std::string(""));
    Names nearly(full.begin(), full.end() - 1);
    DOCTEST_CHECK_EQ(makeCopyName("X", nearly), std::string("X 副本 100"));
    return true;
})

// 以下三条遍历 modelSlotTable() 断言派生面：新增槽位（表加一行）无需改用例即自动进入覆盖，
// 多写、少写、串槽三种漏改都会红。勿改成手抄键名清单——那会让新槽位静默漏测。

// 表 → 托管键全集 == 接管实际写出的键集合
REGISTER_TEST(provider_config_slot_table_managed_keys,
              "provider_config: 槽位表派生托管键与接管写入（slot table）", {
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

REGISTER_TEST(provider_config_slot_table_json_and_labels,
              "provider_config: 槽位表 JSON 键与 UI 文案（slot table）", {
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
    // 外观取样钉死：详情面板 / 表单页两套文案，防止收敛派生时误改外观
    const auto table = modelSlotTable();
    DOCTEST_REQUIRE(!table.empty());
    DOCTEST_CHECK_EQ(std::string(table.front().label), std::string("默认"));
    DOCTEST_CHECK_EQ(std::string(table.front().formLabel), std::string("默认 (MODEL)"));
    DOCTEST_CHECK_EQ(std::string(table.back().label), std::string("子代理"));
    DOCTEST_CHECK_EQ(std::string(table.back().formLabel), std::string("Subagent"));
    return true;
})

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

REGISTER_TEST(provider_config_one_m_marker_pure,
              "provider_config: [1M] 标记 判定/剥离/设置（大小写不敏感 + 空 base）", {
    using module::claude::hasOneMMarker;
    using module::claude::stripOneMMarker;
    using module::claude::setOneMMarker;

    // 判定：大小写不敏感 + 容忍尾空白；非尾部 / 空串不算
    DOCTEST_CHECK(hasOneMMarker("claude-opus-5-5[1M]"));
    DOCTEST_CHECK(hasOneMMarker("claude-opus-5-5[1m]"));
    DOCTEST_CHECK(hasOneMMarker("claude-opus-5-5 [1M]  "));
    DOCTEST_CHECK_FALSE(hasOneMMarker("claude-opus-5-5"));
    DOCTEST_CHECK_FALSE(hasOneMMarker(""));
    DOCTEST_CHECK_FALSE(hasOneMMarker("[1M]-prefix"));
    // 整串即标记：判定为真（endsWith 语义），剥离后是空串
    DOCTEST_CHECK(hasOneMMarker("[1M]"));

    // 剥离：命中才动；未命中原样返回（含大小写混合）
    DOCTEST_CHECK_EQ(stripOneMMarker("m-sonnet[1M]"), std::string("m-sonnet"));
    DOCTEST_CHECK_EQ(stripOneMMarker("m-sonnet[1m]"), std::string("m-sonnet"));
    DOCTEST_CHECK_EQ(stripOneMMarker("m-sonnet [1M] "), std::string("m-sonnet"));
    DOCTEST_CHECK_EQ(stripOneMMarker("m-sonnet"), std::string("m-sonnet"));
    DOCTEST_CHECK_EQ(stripOneMMarker(""), std::string(""));
    DOCTEST_CHECK_EQ(stripOneMMarker("[1M]"), std::string(""));

    // 设置：off = 纯 base；on = base + 规范大写标记（重复拼接防护：已含标记时再 on 仍只有一个
    // [1M]）；空 base 恒返回空串
    DOCTEST_CHECK_EQ(setOneMMarker("m-sonnet", true), std::string("m-sonnet[1M]"));
    DOCTEST_CHECK_EQ(setOneMMarker("m-sonnet", false), std::string("m-sonnet"));
    DOCTEST_CHECK_EQ(setOneMMarker("m-sonnet[1m]", true), std::string("m-sonnet[1M]"));
    DOCTEST_CHECK_EQ(setOneMMarker("m-sonnet[1M]", true), std::string("m-sonnet[1M]"));
    DOCTEST_CHECK_EQ(setOneMMarker("m-sonnet[1M]", false), std::string("m-sonnet"));
    DOCTEST_CHECK_EQ(setOneMMarker("  m-sonnet  ", true), std::string("m-sonnet[1M]"));
    // 空 base（空串 / 纯空白 / 整串即标记）→ 不允许带标记
    DOCTEST_CHECK_EQ(setOneMMarker("", true), std::string(""));
    DOCTEST_CHECK_EQ(setOneMMarker("   ", true), std::string(""));
    DOCTEST_CHECK_EQ(setOneMMarker("[1M]", true), std::string(""));
    return true;
})

// 槽位 supportsOneM 列：逐槽位钉死取值。sonnet / opus / fable / subagent = true，haiku = false
// （小快模型无 1M 变体）；def 无对应外部参考（ANTHROPIC_MODEL 是独立主模型输入框）→ 按主模型语义 true。
// 不要「保守」取值：给 true 的槽位改 false 会让用户选得到却用不了。
// 反向断言：新增槽位必须同时进本用例的清单，否则红。
REGISTER_TEST(provider_config_one_m_slot_support_table,
              "provider_config: 槽位 supportsOneM 列（六槽位取值逐个钉死）", {
    const auto table = module::claude::modelSlotTable();
    DOCTEST_REQUIRE_EQ(table.size(), module::claude::kModelSlotCount);
    DOCTEST_REQUIRE_EQ(table.size(), static_cast<size_t>(6));   // 表长硬钉
    struct Expected {
        const char* key;
        bool oneM;
    };
    const Expected expected[] = {
        {"default", true},    // ANTHROPIC_MODEL（主模型）
        {"sonnet", true},
        {"opus", true},
        {"haiku", false},     // 小快模型，无 1M 变体（cc-switch: false）
        {"fable", true},
        {"subagent", true},   // cc-switch: true —— 勿改回「保守 false」
    };
    DOCTEST_REQUIRE_EQ(sizeof(expected) / sizeof(expected[0]),
                       static_cast<size_t>(6));
    for (const auto& e : expected) {
        const module::claude::ModelSlotMeta* hit = nullptr;
        for (const auto& m : table) {
            if (std::string(m.jsonKey) == e.key) {
                hit = &m;
                break;
            }
        }
        DOCTEST_REQUIRE_MESSAGE(hit != nullptr,
                                std::string("槽位表缺行: ") + e.key);
        DOCTEST_CHECK_MESSAGE(
            hit->supportsOneM == e.oneM,
            std::string("槽位 ") + e.key + " 的 supportsOneM 应为 " +
                (e.oneM ? "true" : "false") + "（照 cc-switch 取值，改错即红）");
    }
    // 反向：表里每一行都必须被上面的清单钉到
    for (const auto& m : table) {
        bool listed = false;
        for (const auto& e : expected) {
            if (std::string(m.jsonKey) == e.key) {
                listed = true;
                break;
            }
        }
        DOCTEST_CHECK_MESSAGE(listed,
                              std::string("槽位未进本用例（新增槽位请同步）: ") +
                                  m.jsonKey);
    }
    return true;
})

// 保存前归一（UI 写入口调用）：支持槽位标记规范化成大写 [1M]；不支持槽位强制剥离；无标记 /
// 空槽位原样不动（displayName 派生不因标记变化而变）
REGISTER_TEST(provider_config_one_m_normalize,
              "provider_config: 保存前归一（不支持槽位强制剥离 + 大小写规范化）", {
    using module::claude::ModelsConfig;
    using module::claude::normalizeOneMMarkers;
    using module::claude::modelSlotTable;

    ModelsConfig c;
    c.def.model = "m-def[1m]";        // 支持 → 规范化（大小写）
    c.sonnet.model = "m-sonnet";      // 支持、无标记 → 不动
    c.opus.model = "";                // 空槽位 → 不动
    c.haiku.model = "m-haiku[1M]";    // **唯一不支持槽位** → 强制剥离
    c.fable.model = "m-fable [1m] ";  // 支持 → 规范化成 base+[1M]
    c.subagent.model = "m-sub[1m]";   // 支持（照 cc-switch）→ 规范化
    normalizeOneMMarkers(c);
    DOCTEST_CHECK_EQ(c.def.model, std::string("m-def[1M]"));
    DOCTEST_CHECK_EQ(c.sonnet.model, std::string("m-sonnet"));
    DOCTEST_CHECK_EQ(c.opus.model, std::string(""));
    DOCTEST_CHECK_EQ(c.haiku.model, std::string("m-haiku"));
    DOCTEST_CHECK_EQ(c.fable.model, std::string("m-fable[1M]"));
    DOCTEST_CHECK_EQ(c.subagent.model, std::string("m-sub[1M]"));

    // 幂等：再归一一次结果不变（保存重入 / 多次写不累积）
    const ModelsConfig once = c;
    normalizeOneMMarkers(c);
    for (const auto& m : modelSlotTable()) {
        DOCTEST_CHECK_EQ((c.*m.slot).model, (once.*m.slot).model);
    }

    // 显示名（派生）不因标记变化而变：归一前后 deriveDisplayName 同值
    DOCTEST_CHECK_EQ(module::claude::deriveDisplayName("m-def[1M]"),
                     module::claude::deriveDisplayName("m-def"));
    return true;
})

REGISTER_TEST(provider_config_reverse_sync_normalizes_one_m_markers,
              "provider_config: 反向同步归一 1M 标记（不支持槽位的手写标记不落库）", {
    // 反向同步（settings.json → profile）也必须归一：用户手写 [1M] 到不支持 1M 的槽位
    //（haiku 是全表唯一 supportsOneM=false）时，标记若照落库，下次打开表单的开关状态就会与落库值不一致。
    using module::claude::mergeReverseSyncEnv;
    using module::claude::parseEnvForReverseSync;
    using module::claude::parseModelsConfig;

    // 现值（已归一：haiku 无标记、def 带规范标记）
    const std::string curModels = R"json({
        "default": {"model": "m-old[1M]", "name": ""},
        "haiku":   {"model": "m-haiku-old", "name": ""}
    })json";
    // settings.json 侧：用户给不支持 1M 的 haiku 手写了 [1m]（小写也不放过）
    auto parsed = parseEnvForReverseSync(
        R"({"ANTHROPIC_MODEL": "m-new[1m]", "ANTHROPIC_DEFAULT_HAIKU_MODEL": "m-haiku-new[1m]"})",
        {});
    DOCTEST_REQUIRE(parsed.ok);

    const auto m = mergeReverseSyncEnv(parsed.cfg, curModels, "{}");
    auto mc = parseModelsConfig(m.modelsConfigJson);
    DOCTEST_REQUIRE(mc.isOk());
    // 支持 1M 的 default：保留 + 规范化大小写
    DOCTEST_CHECK_EQ(mc.value().def.model, std::string("m-new[1M]"));
    // **不支持 1M 的 haiku：标记被剥掉**
    DOCTEST_CHECK_EQ(mc.value().haiku.model, std::string("m-haiku-new"));

    // 反向对照：若现值本身就与归一后的结果一致，判为「无变化」，不产生写库
    const auto same = mergeReverseSyncEnv(parsed.cfg, m.modelsConfigJson, "{}");
    DOCTEST_CHECK_FALSE(same.modelsChanged);
    return true;
})
