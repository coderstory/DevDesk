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
//   - ClaudeService 读写 API 往返 + 旧库升级（migration v3）
#include "doctest.h"

#include "claude_service.h"
#include "core/database.h"
#include "core/migrations.h"
#include "core/sqlite_db.h"
#include "platform/keychain.h"
#include "env_var_hints.h"
#include "provider_config.h"

#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>

namespace {

constexpr const char* kTestKeychainService = "DevDeskTestProviderConfig";

// REGISTER_TEST 宏实参不能有裸逗号（模板实参也算），统一用别名
using EnvMap = std::map<std::string, std::string>;

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

std::filesystem::path tempDbPath(const std::string& tag) {
    static int counter = 0;
    ++counter;
    auto name = "devdesk_test_pconfig_" + tag + "_" +
                std::to_string(counter) + ".db";
    return std::filesystem::temp_directory_path() / name;
}

// RAII：已 migrate 的临时数据库（复用 test_claude_service 模式）
struct TestDb {
    std::filesystem::path path;
    core::Database db;

    explicit TestDb(const std::string& tag) {
        path = tempDbPath(tag);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        auto r = core::Database::open(path);
        if (r.isFail()) {
            throw std::runtime_error("Database::open failed: " +
                                     r.errorMessage());
        }
        db = std::move(r.unsafeValue());
        auto m = db.migrate();
        if (m.isFail()) {
            throw std::runtime_error("migrate failed: " + m.errorMessage());
        }
    }

    ~TestDb() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path.string() + "-wal", ec);
        std::filesystem::remove(path.string() + "-shm", ec);
    }

    core::data::SqliteDb& handle() { return db.db(); }
};

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

// ---- 4. 旧单 model 字段兼容读取（§4.2 fallback）----
REGISTER_TEST(provider_config_legacy_model_fallback,
              "provider_config: 旧 model 列 fallback 到 default 槽位", {
    using module::claude::effectiveModelsConfig;

    // models_config 空 + 旧 model 非空 → default 槽位 = 旧值
    auto c1 = effectiveModelsConfig("", "claude-sonnet-4-5");
    DOCTEST_CHECK_EQ(c1.def.model, std::string("claude-sonnet-4-5"));
    DOCTEST_CHECK(c1.sonnet.model.empty());

    // models_config 非法 JSON → 容错为空 + fallback
    auto c2 = effectiveModelsConfig("{broken", "claude-opus-4-6");
    DOCTEST_CHECK_EQ(c2.def.model, std::string("claude-opus-4-6"));

    // default 槽位已配置 → 不吃 fallback，其余槽位保留
    auto c3 = effectiveModelsConfig(
        R"({"default": {"model": "m-default"}, "sonnet": {"model": "m-sonnet"}})",
        "legacy-model");
    DOCTEST_CHECK_EQ(c3.def.model, std::string("m-default"));
    DOCTEST_CHECK_EQ(c3.sonnet.model, std::string("m-sonnet"));

    // 两者都空 → 空
    auto c4 = effectiveModelsConfig("", "");
    DOCTEST_CHECK(c4.def.model.empty());
    return true;
})

// ---- 5. extra_env 序列化往返 + 非字符串值拒绝 ----
REGISTER_TEST(provider_config_extra_env_roundtrip,
              "provider_config: extra_env 序列化往返与值校验", {
    // ponytail: REGISTER_TEST 宏实参不能有裸逗号，map 用逐项赋值构造
    EnvMap env;
    env["AUTO_COMPACT_ENABLED"] = "1";
    env["DISABLE_AUTOUPDATER"] = "1";
    env["CLAUDE_AUTOCOMPACT_PCT_OVERRIDE"] = "75";
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
        EnvMap env;
        env["AUTO_COMPACT_ENABLED"] = "0";
        env["DISABLE_AUTOUPDATER"] = "1";                  // 官方已知键
        env["CLAUDE_AUTOCOMPACT_PCT_OVERRIDE"] = "75";     // 别名表外未知键
        auto r = applyEnvAliases(env);
        DOCTEST_CHECK_EQ(r.env.size(), 3u);
        DOCTEST_CHECK_EQ(r.env["DISABLE_AUTO_COMPACT"], std::string("1"));
        DOCTEST_CHECK_EQ(r.env["DISABLE_AUTOUPDATER"], std::string("1"));
        DOCTEST_CHECK_EQ(r.env["CLAUDE_AUTOCOMPACT_PCT_OVERRIDE"],
                         std::string("75"));  // 原样透传
        DOCTEST_CHECK_EQ(r.unrecognizedKeys.size(), 1u);
        DOCTEST_CHECK_EQ(r.unrecognizedKeys[0],
                         std::string("CLAUDE_AUTOCOMPACT_PCT_OVERRIDE"));
    }
    return true;
})

// ---- 8. ClaudeService 读写往返 + model 列同步 ----
REGISTER_TEST(provider_config_service_roundtrip,
              "claude_service: models_config/extra_env 读写往返", {
    TestDb tdb("svc_roundtrip");
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

// ---- 9. 旧库升级（v2 → v3）：存量行零迁移，读出为空串 ----
REGISTER_TEST(provider_config_migration_upgrade,
              "migration v3: 旧库升级加列，存量行 models_config/extra_env 为空串", {
    auto path = tempDbPath("upgrade");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // 手工构造 v2 旧库（只跑前两条 migration），插入存量 profile 行
    {
        auto openR = core::data::SqliteDb::open(path);
        DOCTEST_REQUIRE(openR.isOk());
        auto& db = openR.value();
        std::vector<core::data::Migration> oldMigrations;
        oldMigrations.push_back(core::migrations::kCreateInitialTables);
        oldMigrations.push_back(core::migrations::kSeedDefaultTodoColumns);
        DOCTEST_REQUIRE(db.runMigrations(oldMigrations).isOk());

        auto ins = db.prepare(
            "INSERT INTO claude_profiles (name, base_url, api_key_hint, "
            "api_key_check, keychain_account, model, is_active, "
            "created_at, updated_at) VALUES "
            "('旧供应商', 'https://old.example.com', 'sk-***abcd', "
            "'deadbeef', 'claude:1', 'claude-3-5-haiku', 1, 0, 0)");
        DOCTEST_REQUIRE(ins.isOk());
        DOCTEST_REQUIRE(ins.value().step().isOk());
    }  // 句柄关闭（Windows 文件锁）

    // 重新打开 + 全量 migrate → v3 补两列
    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(r.value().migrate().isOk());
        auto& db = r.value().db();
        // v3 供应商配置迁移 + v4 todo kind 迁移（W9 批次并入后全量到 4）
        DOCTEST_CHECK_EQ(db.schemaVersion(), 4);

        module::claude::ClaudeService svc(db, kTestKeychainService);
        auto profiles = svc.listProfiles();
        DOCTEST_REQUIRE(profiles.isOk());
        DOCTEST_REQUIRE_EQ(profiles.value().size(), 1u);
        const auto& p = profiles.value()[0];
        DOCTEST_CHECK_EQ(p.name, std::string("旧供应商"));
        DOCTEST_CHECK_EQ(p.model, std::string("claude-3-5-haiku"));
        DOCTEST_CHECK(p.modelsConfigJson.empty());  // 存量行零迁移
        DOCTEST_CHECK(p.extraEnvJson.empty());

        // 旧数据经兼容读取 → default 槽位 = 旧 model
        auto eff = module::claude::effectiveModelsConfig(
            p.modelsConfigJson, p.model);
        DOCTEST_CHECK_EQ(eff.def.model, std::string("claude-3-5-haiku"));
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
        "CLAUDE_AUTOCOMPACT_PCT_OVERRIDE": "75",
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
                     std::string("CLAUDE_AUTOCOMPACT_PCT_OVERRIDE"));
    DOCTEST_CHECK_EQ(c.extraEnv.at("CLAUDE_AUTOCOMPACT_PCT_OVERRIDE"),
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
        "CLAUDE_AUTOCOMPACT_PCT_OVERRIDE": "75"
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
    DOCTEST_CHECK_EQ(c.extraEnv.at("CLAUDE_AUTOCOMPACT_PCT_OVERRIDE"),
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

// ---- 守卫表 G1–G6（§3.3）：每条守卫一个专测，编号与用例名一一对应
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
    auto m = mergeReverseSyncEnv(parsed.cfg, curModels, "", R"({"K": "v"})");
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

    auto m = mergeReverseSyncEnv(parsed.cfg, curModels, "", curExtra);
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

    // 无变化：env 与现值逐字段一致（含旧 model 列 fallback 等价——models_config
    // 空 + 旧列 m-1，env 给 m-1 → 不算变化，零写入）
    auto parsed = parseEnvForReverseSync(
        R"({"ANTHROPIC_MODEL": "m-1", "DISABLE_AUTOUPDATER": "1"})", {});
    DOCTEST_REQUIRE(parsed.ok);
    auto m = mergeReverseSyncEnv(parsed.cfg, "", "m-1",
                                 R"({"DISABLE_AUTOUPDATER": "1"})");
    DOCTEST_CHECK(!m.modelsChanged);
    DOCTEST_CHECK(!m.extraEnvChanged);
    DOCTEST_CHECK(m.modelsConfigJson.empty());  // 回传原文（空 = 未配置）
    DOCTEST_CHECK(m.extraEnvJson.find("DISABLE_AUTOUPDATER") != std::string::npos);

    // extra_env 全清 → 空串（本层「空串 = 未配置」约定）
    auto empty = parseEnvForReverseSync("{}", {});
    DOCTEST_REQUIRE(empty.ok);
    auto m2 = mergeReverseSyncEnv(empty.cfg, "", "", R"({"K": "v"})");
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

