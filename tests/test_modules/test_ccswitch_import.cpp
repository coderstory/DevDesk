// tests/test_modules/test_ccswitch_import.cpp
// 从 cc-switch 导入 Profile：映射纯函数 + 只读读侧 —— 用例
//
// 被测：src/module_claude_proxy/ccswitch_import.{h,cpp}
//   ① 纯映射函数（mapCcSwitchProviderToProfile）：settings_config JSON → Profile 字段，
//      覆盖映射表全字段 + 边界（只 API_KEY 无 TOKEN / 缺 base_url / 无密钥跳过 /
//      双 key 未用值保留 / 同名冲突 / 非法 JSON / 空名称）。
//   ② 读侧（listCcSwitchImportRows）：**只读打开**临时库，筛选
//      app_type IN ('claude','claude-desktop')，其余 app_type 忽略。
//
// 夹具策略（同 test_claude_quota_ccswitch.cpp）：全部自建临时 sqlite 库
//（用 sqlite3 C API 建表插行），经 CCSWITCH_DB_PATH 指路；**绝不碰**用户真实的
// ~/.cc-switch/cc-switch.db。密钥一律假串（sk-ccswitch-test-*）。
//
// 密钥不出任何出口：夹具 / 断言里只有假串；映射失败的 skipReason 里也断言
// 不含 "sk-" 片段（确定不把 settings_config 原文拼进文案）。
#include "doctest.h"

#include "test_support.h"

#include "core/logging.h"
#include "module_claude_proxy/ccswitch_import.h"
#include "quota/ccswitch_source.h"

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace {

// ==================== 夹具：临时 cc-switch 库（同 ccswitch_source 测试）====================

// cc-switch 真实建表语句（列名逐字对齐；只填本侧要用的列，其余列照原样留着）。
const char* const kRealSchema = "CREATE TABLE providers ("
    "  id TEXT NOT NULL,"
    "  app_type TEXT NOT NULL,"
    "  name TEXT NOT NULL,"
    "  settings_config TEXT NOT NULL,"
    "  website_url TEXT,"
    "  category TEXT,"
    "  created_at INTEGER,"
    "  sort_index INTEGER,"
    "  notes TEXT,"
    "  icon TEXT,"
    "  icon_color TEXT,"
    "  meta TEXT NOT NULL DEFAULT '{}',"
    "  is_current BOOLEAN NOT NULL DEFAULT 0,"
    "  in_failover_queue BOOLEAN NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (id, app_type)"
    ");";

bool createCcSwitchDb(const std::filesystem::path& path, const std::string& schema,
                      const std::vector<std::string>& inserts) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.string().c_str(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    bool ok = true;
    const auto exec = [&](const std::string& sql) {
        char* err = nullptr;
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            ok = false;
            sqlite3_free(err);
        }
    };
    exec(schema);
    for (const std::string& insert : inserts) exec(insert);
    sqlite3_close(db);
    return ok;
}

std::string sqlQuote(const std::string& raw) {
    std::string out = "'";
    for (const char c : raw) {
        out.push_back(c);
        if (c == '\'') out.push_back('\'');
    }
    out.push_back('\'');
    return out;
}

std::string insertProvider(const std::string& id, const std::string& appType,
                           const std::string& name, const std::string& settingsConfig) {
    return "INSERT INTO providers (id, app_type, name, settings_config) VALUES (" +
           sqlQuote(id) + ", " + sqlQuote(appType) + ", " + sqlQuote(name) + ", " +
           sqlQuote(settingsConfig) + ");";
}

// 构造 settings_config 原文（env 全键形态）
std::string settingsConfigWith(
    const std::string& baseUrl, const std::string& token, const std::string& apiKey,
    const std::string& model, const std::string& extraEnvJson,   // 额外 env 段（可 ""）
    const std::string& extraTopLevelJson) {                      // 顶层非 env 段（可 ""）
    std::string env = "{\"ANTHROPIC_BASE_URL\":\"" + baseUrl + "\"";
    if (!token.empty()) env += ",\"ANTHROPIC_AUTH_TOKEN\":\"" + token + "\"";
    if (!apiKey.empty()) env += ",\"ANTHROPIC_API_KEY\":\"" + apiKey + "\"";
    if (!model.empty()) env += ",\"ANTHROPIC_MODEL\":\"" + model + "\"";
    if (!extraEnvJson.empty()) env += "," + extraEnvJson;
    env += "}";
    std::string out = "{\"env\":" + env;
    if (!extraTopLevelJson.empty()) out += "," + extraTopLevelJson;
    out += "}";
    return out;
}

const char* kFakeToken = "sk-ccswitch-test-token";
const char* kFakeApiKey = "sk-ccswitch-test-apikey";

size_t findByName(const module::claude::CcSwitchImportRows& rows,
                  const std::string& name) {
    for (size_t i = 0; i < rows.rows.size(); ++i) {
        if (rows.rows[i].name == name) return i;
    }
    return rows.rows.size();
}

}  // namespace

using module::claude::CcSwitchImportMapping;
using module::claude::CcSwitchImportRows;
using module::claude::CcSwitchImportStatus;
using module::claude::mapCcSwitchProviderToProfile;
using module::claude::listCcSwitchImportRows;

// ==================== ① 纯映射：全字段 ====================

REGISTER_TEST(ccswitch_import_maps_all_fields,
              "ccswitch 导入：全字段映射（env 四保留键 + 额外 env + 顶层非 env + authField=both）", {
    const std::string json = settingsConfigWith(
        "https://api.deepseek.com/anthropic", kFakeToken, kFakeApiKey,
        "deepseek-flash[1M]",
        "\"ANTHROPIC_DEFAULT_SONNET_MODEL\":\"m\","
        "\"CLAUDE_CODE_SUBAGENT_MODEL\":\"s\"",
        "\"theme\":\"custom:ember-light\",\"statusLine\":{\"type\":\"command\","
        "\"command\":\"echo hi\"},\"enabledPlugins\":{\"x\":true}");
    const CcSwitchImportMapping m =
        mapCcSwitchProviderToProfile("DeepSeek", json);
    DOCTEST_REQUIRE(m.ok);
    DOCTEST_CHECK_EQ(m.skipReason, std::string(""));
    DOCTEST_CHECK_EQ(m.name, std::string("DeepSeek"));
    DOCTEST_CHECK_EQ(m.baseUrl, std::string("https://api.deepseek.com/anthropic"));
    DOCTEST_CHECK_EQ(m.apiKey, std::string(kFakeToken));   // TOKEN 优先
    DOCTEST_CHECK_EQ(m.model, std::string("deepseek-flash[1M]"));
    DOCTEST_CHECK_EQ(m.authField, std::string("both"));     // 两键都有
    // extraEnvJson：四保留键之外的全部键值，一个不丢。例外（审计批 ②）：
    // 双 key 并存时未用作 apiKey 的 ANTHROPIC_API_KEY 保留进表（本例 TOKEN
    // 已用作 apiKey，API_KEY 是被保留的那个）。
    DOCTEST_CHECK_NE(m.extraEnvJson, std::string(""));
    DOCTEST_CHECK(m.extraEnvJson.find("ANTHROPIC_DEFAULT_SONNET_MODEL") !=
                  std::string::npos);
    DOCTEST_CHECK(m.extraEnvJson.find("CLAUDE_CODE_SUBAGENT_MODEL") !=
                  std::string::npos);
    DOCTEST_CHECK(m.extraEnvJson.find("ANTHROPIC_BASE_URL") == std::string::npos);
    DOCTEST_CHECK(m.extraEnvJson.find("ANTHROPIC_AUTH_TOKEN") == std::string::npos);
    DOCTEST_CHECK(m.extraEnvJson.find("ANTHROPIC_API_KEY") != std::string::npos);
    DOCTEST_CHECK(m.extraEnvJson.find(kFakeApiKey) != std::string::npos);
    DOCTEST_CHECK(m.extraEnvJson.find("ANTHROPIC_MODEL") == std::string::npos);
    // extraSettingsJson：env 块外全部顶层属性，一个不丢
    DOCTEST_CHECK_NE(m.extraSettingsJson, std::string(""));
    DOCTEST_CHECK(m.extraSettingsJson.find("\"theme\"") != std::string::npos);
    DOCTEST_CHECK(m.extraSettingsJson.find("custom:ember-light") != std::string::npos);
    DOCTEST_CHECK(m.extraSettingsJson.find("\"statusLine\"") != std::string::npos);
    DOCTEST_CHECK(m.extraSettingsJson.find("echo hi") != std::string::npos);
    DOCTEST_CHECK(m.extraSettingsJson.find("\"enabledPlugins\"") != std::string::npos);
    // extraSettingsJson 里不许残留 env 块
    DOCTEST_CHECK(m.extraSettingsJson.find("\"env\"") == std::string::npos);
    return true;
})

// ==================== ① 边界：只 API_KEY 无 TOKEN ====================

REGISTER_TEST(ccswitch_import_api_key_only,
              "ccswitch 导入：只 ANTHROPIC_API_KEY 无 TOKEN → apiKey 兜底 + authField=api_key", {
    const std::string json = settingsConfigWith(
        "https://opencode.ai/zen/go", "", kFakeApiKey, "",
        std::string(), std::string());
    const CcSwitchImportMapping m =
        mapCcSwitchProviderToProfile("OpenCode Go", json);
    DOCTEST_REQUIRE(m.ok);
    DOCTEST_CHECK_EQ(m.apiKey, std::string(kFakeApiKey));
    DOCTEST_CHECK_EQ(m.authField, std::string("api_key"));
    DOCTEST_CHECK_EQ(m.model, std::string(""));     // 缺模型 → 空串
    DOCTEST_CHECK_EQ(m.extraEnvJson, std::string(""));
    DOCTEST_CHECK_EQ(m.extraSettingsJson, std::string(""));
    return true;
})

// ==================== ① 边界：只 TOKEN 无 API_KEY ====================

REGISTER_TEST(ccswitch_import_token_only,
              "ccswitch 导入：只 ANTHROPIC_AUTH_TOKEN → authField=bearer", {
    const std::string json = settingsConfigWith(
        "https://ark.cn-beijing.volces.com/api/coding", kFakeToken, "", "",
        std::string(), std::string());
    const CcSwitchImportMapping m =
        mapCcSwitchProviderToProfile("火山", json);
    DOCTEST_REQUIRE(m.ok);
    DOCTEST_CHECK_EQ(m.apiKey, std::string(kFakeToken));
    DOCTEST_CHECK_EQ(m.authField, std::string("bearer"));
    return true;
})

// ==================== ① 边界：缺 base_url → 跳过 ====================

REGISTER_TEST(ccswitch_import_missing_base_url,
              "ccswitch 导入：无 env.ANTHROPIC_BASE_URL → 跳过并报告「缺 base_url」", {
    const std::string json =
        "{\"env\":{\"ANTHROPIC_AUTH_TOKEN\":\"sk-ccswitch-test-token\"}}";
    const CcSwitchImportMapping m =
        mapCcSwitchProviderToProfile("NoUrl", json);
    DOCTEST_CHECK_FALSE(m.ok);
    DOCTEST_CHECK(m.skipReason.find("缺 base_url") != std::string::npos);
    // skipReason 不得携带密钥明文（假串也不许进文案）
    DOCTEST_CHECK(m.skipReason.find("sk-") == std::string::npos);
    return true;
})

// ==================== ① 边界：无密钥 → 跳过（缺密钥），不落「失败」 ====================
// 审计批 ①（2026-10-03）：无 AUTH_TOKEN 且无 API_KEY 的条目是「源缺必填字段」
// 的确定性不可导入，与「缺 base_url → 跳过」同类；此前按映射建行会被
// createProfile 的「apiKey 不能为空」（profile_repository.cpp:379）拒进
// 「失败」桶——失败应留给可重试/系统错误。

REGISTER_TEST(ccswitch_import_missing_key_skips,
              "ccswitch 导入：无 TOKEN 无 KEY → 跳过「缺密钥」（skip 非 fail）", {
    // 有 base_url、无任何密钥 → 确定性跳过
    const std::string json = settingsConfigWith(
        "https://api.example.com", "", "", "", std::string(), std::string());
    const CcSwitchImportMapping m =
        mapCcSwitchProviderToProfile("NoKey", json);
    DOCTEST_CHECK_FALSE(m.ok);
    DOCTEST_CHECK(m.skipReason.find("缺密钥") != std::string::npos);
    // skipReason 不得携带密钥明文（防御性约定，本例本来就没有）
    DOCTEST_CHECK(m.skipReason.find("sk-") == std::string::npos);
    // 判定顺序钉死：base_url 与密钥都缺时，先报「缺 base_url」（既有顺序）
    const std::string neither = settingsConfigWith(
        "", "", "", "", std::string(), std::string());
    const CcSwitchImportMapping m2 =
        mapCcSwitchProviderToProfile("Neither", neither);
    DOCTEST_CHECK_FALSE(m2.ok);
    DOCTEST_CHECK(m2.skipReason.find("缺 base_url") != std::string::npos);
    return true;
})

// ==================== ① 边界：双 key 并存 → 未用作 apiKey 的值保留进 extraEnv ====================
// 审计批 ②（2026-10-03，M4）：TOKEN+API_KEY 双 key 时 apiKey 取 TOKEN，
// API_KEY 的值原本整条丢（保留键不进 extraEnv、又不进 apiKey）。修法：
// 未用作 apiKey 的那个 key 保留进 extraEnvJson，不丢值。运行期安全由既有
// 守卫保证：直连 buildDirectEnv 清另一密钥键（provider_config.cpp:717-732）、
// 代理 UpstreamDialect::applyHeaders 无条件丢弃客户端凭据（upstream_dialect.cpp:142-155）。

REGISTER_TEST(ccswitch_import_dual_key_preserves_unused,
              "ccswitch 导入：双 key → apiKey 取 TOKEN，未用的 API_KEY 值保留进 extraEnv 不丢", {
    const std::string json = settingsConfigWith(
        "https://api.deepseek.com/anthropic", kFakeToken, kFakeApiKey,
        "", std::string(), std::string());
    const CcSwitchImportMapping m =
        mapCcSwitchProviderToProfile("DualKey", json);
    DOCTEST_REQUIRE(m.ok);
    DOCTEST_CHECK_EQ(m.apiKey, std::string(kFakeToken));   // TOKEN 优先
    DOCTEST_CHECK_EQ(m.authField, std::string("both"));
    // 未用作 apiKey 的 ANTHROPIC_API_KEY 连键带值保留进 extraEnvJson
    DOCTEST_CHECK(m.extraEnvJson.find("ANTHROPIC_API_KEY") != std::string::npos);
    DOCTEST_CHECK(m.extraEnvJson.find(kFakeApiKey) != std::string::npos);
    // 已用作 apiKey 的 ANTHROPIC_AUTH_TOKEN 不重复进 extraEnv
    DOCTEST_CHECK(m.extraEnvJson.find("ANTHROPIC_AUTH_TOKEN") == std::string::npos);
    return true;
})

// ==================== ① 边界：同名冲突 → 跳过（不覆盖） ====================

REGISTER_TEST(ccswitch_import_name_conflict,
              "ccswitch 导入：DevDesk 已有同名 profile → 跳过，不覆盖", {
    const std::string json = settingsConfigWith(
        "https://api.deepseek.com/anthropic", kFakeToken, "", "",
        std::string(), std::string());
    const CcSwitchImportMapping m = mapCcSwitchProviderToProfile(
        "DeepSeek", json, /*existingNames=*/{"DeepSeek", "另一条"});
    DOCTEST_CHECK_FALSE(m.ok);
    DOCTEST_CHECK(m.skipReason.find("同名") != std::string::npos);
    // 同名判据是「精确相等」：大小写不同不算冲突（与 DB 大小写敏感的 UNIQUE 一致）
    const CcSwitchImportMapping m2 = mapCcSwitchProviderToProfile(
        "deepseek", json, /*existingNames=*/{"DeepSeek"});
    DOCTEST_CHECK(m2.ok);
    return true;
})

// ==================== ① 边界：非法 JSON / 空名称 ====================

REGISTER_TEST(ccswitch_import_bad_inputs,
              "ccswitch 导入：非法 JSON / 顶层非对象 / 空名称 → 跳过", {
    {
        const CcSwitchImportMapping m =
            mapCcSwitchProviderToProfile("Bad", "{broken json");
        DOCTEST_CHECK_FALSE(m.ok);
        DOCTEST_CHECK(m.skipReason.find("JSON") != std::string::npos);
    }
    {
        const CcSwitchImportMapping m =
            mapCcSwitchProviderToProfile("Bad", "[1,2,3]");
        DOCTEST_CHECK_FALSE(m.ok);
        DOCTEST_CHECK(m.skipReason.find("JSON") != std::string::npos);
    }
    {
        const std::string json = settingsConfigWith(
            "https://api.deepseek.com/anthropic", kFakeToken, "", "",
            std::string(), std::string());
        const CcSwitchImportMapping m =
            mapCcSwitchProviderToProfile("", json);
        DOCTEST_CHECK_FALSE(m.ok);
        DOCTEST_CHECK(m.skipReason.find("名称") != std::string::npos);
    }
    return true;
})

// ==================== ① 边界：env 缺失 / 非对象 → 缺 base_url 跳过 ====================

REGISTER_TEST(ccswitch_import_env_missing,
              "ccswitch 导入：settings_config 无 env 块 / env 非对象 → 缺 base_url 跳过", {
    {
        const CcSwitchImportMapping m =
            mapCcSwitchProviderToProfile("NoEnv", "{\"model\":\"m\"}");
        DOCTEST_CHECK_FALSE(m.ok);
        DOCTEST_CHECK(m.skipReason.find("缺 base_url") != std::string::npos);
    }
    {
        const CcSwitchImportMapping m =
            mapCcSwitchProviderToProfile("EnvArr", "{\"env\":[1,2]}");
        DOCTEST_CHECK_FALSE(m.ok);
        DOCTEST_CHECK(m.skipReason.find("缺 base_url") != std::string::npos);
    }
    return true;
})

// ==================== ② 读侧：app_type 筛选（claude + claude-desktop） ====================

REGISTER_TEST(ccswitch_import_reads_only_claude_types,
              "ccswitch 读侧：只取 app_type∈claude/claude-desktop，codex/gemini 忽略", {
    const test_support::TempDir dir("ccswitch_import");
    const std::filesystem::path db = dir.file("cc-switch.db");
    const bool built = createCcSwitchDb(
        db, kRealSchema,
        {insertProvider("id-c", "claude", "Claude Provider",
                        settingsConfigWith("https://api.anthropic.com", kFakeToken, "", "",
                                           std::string(), std::string())),
         insertProvider("id-cd", "claude-desktop", "Claude Desktop",
                        settingsConfigWith("https://desktop.example.com", kFakeToken, "", "",
                                           std::string(), std::string())),
         insertProvider("id-codex", "codex", "Codex",
                        settingsConfigWith("https://codex.example.com", kFakeToken, "", "",
                                           std::string(), std::string())),
         insertProvider("id-gemini", "gemini", "Gemini",
                        settingsConfigWith("https://gemini.example.com", kFakeToken, "", "",
                                           std::string(), std::string()))});
    DOCTEST_REQUIRE(built);
    const test_support::ScopedEnv env("CCSWITCH_DB_PATH", db.string());

    const CcSwitchImportRows rows = listCcSwitchImportRows();
    DOCTEST_CHECK(rows.status == CcSwitchImportStatus::Ok);
    DOCTEST_CHECK_EQ(rows.unavailableReason, std::string(""));
    DOCTEST_REQUIRE_EQ(rows.rows.size(), static_cast<std::size_t>(2));
    // 两行恰是 claude 与 claude-desktop；settings_config **原文**原样读回
    const size_t cd = findByName(rows, "Claude Desktop");
    DOCTEST_REQUIRE(cd < rows.rows.size());
    DOCTEST_CHECK_EQ(rows.rows[cd].appType, std::string("claude-desktop"));
    DOCTEST_CHECK(rows.rows[cd].settingsConfig.find("ANTHROPIC_BASE_URL") !=
                  std::string::npos);
    return true;
})

// ==================== ② 读侧：库不存在 → NotFound（绝不静默空列表） ====================

REGISTER_TEST(ccswitch_import_missing_db,
              "ccswitch 读侧：库不存在 → NotFound + 中文原因，绝不静默空列表", {
    const test_support::TempDir dir("ccswitch_import_miss");
    const test_support::ScopedEnv env("CCSWITCH_DB_PATH",
                                      (dir.file("no-such.db")).string());
    const CcSwitchImportRows rows = listCcSwitchImportRows();
    DOCTEST_CHECK(rows.status == CcSwitchImportStatus::NotFound);
    DOCTEST_REQUIRE_EQ(rows.rows.size(), static_cast<std::size_t>(0));
    DOCTEST_CHECK(rows.unavailableReason.find("未找到 cc-switch 数据库") !=
                  std::string::npos);
    return true;
})

// ==================== ② 读侧：settings_config 原文原样读回 ====================

REGISTER_TEST(ccswitch_import_reads_raw_config,
              "ccswitch 读侧：settings_config 原文原样读回（不脱密不裁剪）", {
    const test_support::TempDir dir("ccswitch_import_ro");
    const std::filesystem::path db = dir.file("cc-switch.db");
    const std::string json = settingsConfigWith(
        "https://api.deepseek.com/anthropic", kFakeToken, "", "",
        std::string(), std::string());
    const bool built = createCcSwitchDb(
        db, kRealSchema, {insertProvider("id-c", "claude", "Ro", json)});
    DOCTEST_REQUIRE(built);
    const test_support::ScopedEnv env("CCSWITCH_DB_PATH", db.string());

    const CcSwitchImportRows rows = listCcSwitchImportRows();
    DOCTEST_CHECK(rows.status == CcSwitchImportStatus::Ok);
    DOCTEST_REQUIRE_EQ(rows.rows.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(rows.rows[0].settingsConfig, json);
    return true;
})