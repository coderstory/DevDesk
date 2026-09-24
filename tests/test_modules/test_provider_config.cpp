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

