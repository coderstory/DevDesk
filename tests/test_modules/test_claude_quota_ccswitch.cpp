// tests/test_modules/test_claude_quota_ccswitch.cpp
// H：从 cc-switch 只读读取供应商配置 —— 用例
//
// 被测：src/module_claude_proxy/quota/ccswitch_source.{h,cpp}
// 契约与字段语义出处见该头文件；字段名出处是 cc-switch（rev bfaaba16）
//   src-tauri/src/provider.rs:145  resolve_usage_credentials（Claude 分支）
//   src-tauri/src/provider.rs:268  UsageScript（JSON 键名）
//   src-tauri/src/provider.rs:444  ProviderMeta（usage_script 键名）
//   src-tauri/src/database/dao/providers.rs  providers 表列
//
// 夹具策略：**全部自建临时 sqlite 库**（用 sqlite3 C API 建表插行），不碰
// 用户真实的 ~/.cc-switch/cc-switch.db —— 那个库 cc-switch 应用正在写
//（实测 2026-10-03 00:15 还在变），拿它当夹具必然 flake，而且用例不该有
// 权限去假设它长什么样。
//
// 覆盖（对应任务书「必测」六条）：
//   ① 只读打开        —— 0444 库仍可读 + 内容/mtime/目录清单不变 + open flags
//   ② 正常读取        —— name / baseUrl / apiKey / usage_script 各字段
//   ③ 结构退化        —— meta 为 NULL / 非法 JSON / 缺列 → 不崩，name+baseUrl 仍在
//   ④ base_url 匹配   —— 命中 / 未命中 / 伪造后缀不命中（标签边界）
//   ⑤ 库不存在        —— 空列表 + 原因，且**不许把文件创建出来**
//   ⑥ 密钥不落任何出口 —— 日志文件与错误文案里都不含密钥明文
#include "doctest.h"

#include "test_support.h"

#include "core/logging.h"
#include "quota/ccswitch_source.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

using module::claude::quota::CcSwitchProviderInfo;
using module::claude::quota::CcSwitchProviderList;
using module::claude::quota::CcSwitchSourceStatus;

// ==================== 夹具：临时 cc-switch 库 ====================

// 建一个**真 cc-switch 形态**的最小库（列名逐字照抄 cc-switch 的
// CREATE TABLE providers，见 src-tauri/src/database/ 的 schema；只留本模块读的
// 列 + 一个多余列证明「列多列少都不影响」）。
// 返回 false = 夹具没建成（用例应 REQUIRE 它）。
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

// cc-switch 真实建表语句（列名逐字对齐；本模块只用 id / app_type / name /
// settings_config / meta 五列，其余列留着证明我们没依赖它们）。
const char* const kRealSchema =
    "CREATE TABLE providers ("
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

// 同上但 meta 不带 NOT NULL —— 用来造「meta 值是 NULL」这一形态（真实 cc-switch
// 库里 meta 恒非空；NULL 只会来自旧库 / 手工编辑过的库，仍必须能安全退化）。
std::string schemaWithNullableMeta() {
    std::string schema = kRealSchema;
    const std::string from = "meta TEXT NOT NULL DEFAULT '{}'";
    const std::string to = "meta TEXT";
    const std::size_t at = schema.find(from);
    if (at != std::string::npos) schema.replace(at, from.size(), to);
    return schema;
}

// 单引号转义（夹具里要嵌 JSON，里面有 ' 的只有测试串，做个最小防线）
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
                           const std::string& name, const std::string& settingsConfig,
                           const std::string& meta) {
    return "INSERT INTO providers (id, app_type, name, settings_config, meta) VALUES (" +
           sqlQuote(id) + ", " + sqlQuote(appType) + ", " + sqlQuote(name) + ", " +
           sqlQuote(settingsConfig) + ", " + sqlQuote(meta) + ");";
}

// 真实 cc-switch claude 供应商的 settings_config 形态（实测导出自本机库）：
// env 里 ANTHROPIC_BASE_URL + ANTHROPIC_AUTH_TOKEN（或 ANTHROPIC_API_KEY）。
std::string settingsConfig(const std::string& baseUrl, const std::string& keyName,
                           const std::string& keyValue) {
    return "{\"env\":{\"ANTHROPIC_BASE_URL\":\"" + baseUrl + "\",\"" + keyName + "\":\"" +
           keyValue + "\"}}";
}

// usage_script 形态（火山那条实测最全：accessKeyId + secretAccessKey +
// codingPlanProvider；DeepSeek 那条一个都没有）。
std::string usageMeta(const std::string& usageScriptJson) {
    return "{\"commonConfigEnabled\":true,\"usage_script\":" + usageScriptJson + "}";
}

// 在临时目录里造一个只含一个 DeepSeek 供应商的库，并把 CCSWITCH_DB_PATH 指过去。
// 返回库路径（调用方用 TempDir 持有生命周期）。
std::string deepSeekOnlyDb(const std::filesystem::path& dir) {
    const std::filesystem::path db = dir / "cc-switch.db";
    if (!createCcSwitchDb(db, kRealSchema,
                          {insertProvider("id-deepseek", "claude", "DeepSeek",
                                          settingsConfig("https://api.deepseek.com/anthropic",
                                                         "ANTHROPIC_AUTH_TOKEN",
                                                         "sk-ccswitch-sentinel-key"),
                                          usageMeta("{\"enabled\":true,\"templateType\":"
                                                    "\"balance\"}"))})) {
        return {};
    }
    return db.string();
}

// ==================== 小工具 ====================

std::string readWholeFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// 内容指纹（不引第三方 hash：库只有几十 KB，FNV-1a 足够判「有没有被改过」）
std::uint64_t fnv1a(const std::string& data) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

// 目录下的文件名清单（排序，判「有没有多出 -wal / -shm / -journal 一类的尸块」）
std::vector<std::string> dirEntries(const std::filesystem::path& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::int64_t mtimeTicks(const std::filesystem::path& file) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(file, ec);
    if (ec) return -1;
    return static_cast<std::int64_t>(t.time_since_epoch().count());
}

std::size_t findByName(const CcSwitchProviderList& list, const std::string& name) {
    for (std::size_t i = 0; i < list.providers.size(); ++i) {
        if (list.providers[i].name == name) return i;
    }
    return list.providers.size();
}

}  // namespace

// ==================== ② 正常读取：各字段 ====================

REGISTER_TEST(claude_quota_ccswitch_reads_all_fields,
              "claude_quota_ccswitch: 读出 name/baseUrl/apiKey/codingPlanProvider/"
              "accessKeyId/secretAccessKey", {
    const test_support::TempDir dir("ccswitch_read");
    const std::filesystem::path db = dir.file("cc-switch.db");
    // 四行 = 用户本机 cc-switch 里真实存在的四条（含火山那条的两个控制面凭据）
    const bool built = createCcSwitchDb(
        db, kRealSchema,
        {insertProvider("id-minimax", "claude", "MiniMax",
                        settingsConfig("https://api.minimax.cn/anthropic",
                                       "ANTHROPIC_AUTH_TOKEN", "sk-minimax-secret"),
                        usageMeta("{\"enabled\":true,\"codingPlanProvider\":\"minimax\","
                                  "\"autoQueryInterval\":5,\"templateType\":\"token_plan\"}")),
         insertProvider("id-volcano", "claude", "火山 Coding Plan",
                        settingsConfig("https://ark.cn-beijing.volces.com/api/coding",
                                       "ANTHROPIC_AUTH_TOKEN", "sk-volcano-secret"),
                        usageMeta("{\"enabled\":true,\"codingPlanProvider\":\"volcengine\","
                                  "\"accessKeyId\":\"AKLT-volcano-ak\","
                                  "\"secretAccessKey\":\"volcano-sk\"}")),
         insertProvider(
             // OpenCode Go 的真实形态：key 落在 ANTHROPIC_API_KEY（不是 AUTH_TOKEN）
             "id-opencode", "claude", "OpenCode Go",
             settingsConfig("https://opencode.ai/zen/go", "ANTHROPIC_API_KEY", "sk-oc-secret"),
             usageMeta("{\"enabled\":true,\"codingPlanProvider\":\"opencode_go\"}")),
         insertProvider("id-deepseek", "claude", "DeepSeek",
                        settingsConfig("https://api.deepseek.com/anthropic",
                                       "ANTHROPIC_AUTH_TOKEN", "sk-deepseek-secret"),
                        usageMeta("{\"enabled\":true,\"templateType\":\"balance\"}"))});
    DOCTEST_REQUIRE(built);
    const test_support::ScopedEnv env("CCSWITCH_DB_PATH", db.string());

    const CcSwitchProviderList list = module::claude::quota::listCcSwitchProviders();
    DOCTEST_CHECK(list.status == CcSwitchSourceStatus::Ok);
    DOCTEST_CHECK_EQ(list.unavailableReason, std::string(""));
    DOCTEST_REQUIRE_EQ(list.providers.size(), static_cast<std::size_t>(4));

    const std::size_t minimax = findByName(list, "MiniMax");
    DOCTEST_REQUIRE(minimax < list.providers.size());
    DOCTEST_CHECK_EQ(list.providers[minimax].id, std::string("id-minimax"));
    DOCTEST_CHECK_EQ(list.providers[minimax].baseUrl,
                     std::string("https://api.minimax.cn/anthropic"));
    DOCTEST_CHECK_EQ(list.providers[minimax].apiKey, std::string("sk-minimax-secret"));
    DOCTEST_CHECK_EQ(list.providers[minimax].codingPlanProvider, std::string("minimax"));

    // OpenCode Go：ANTHROPIC_API_KEY 兜底（cc-switch first_non_empty 的第二档）
    const std::size_t oc = findByName(list, "OpenCode Go");
    DOCTEST_REQUIRE(oc < list.providers.size());
    DOCTEST_CHECK_EQ(list.providers[oc].apiKey, std::string("sk-oc-secret"));
    DOCTEST_CHECK_EQ(list.providers[oc].baseUrl, std::string("https://opencode.ai/zen/go"));
    DOCTEST_CHECK_EQ(list.providers[oc].codingPlanProvider, std::string("opencode_go"));

    // 火山：推理 key 与**控制面 OpenAPI** 的 AK/SK 是两套凭据，都要能取到
    const std::size_t volcano = findByName(list, "火山 Coding Plan");
    DOCTEST_REQUIRE(volcano < list.providers.size());
    DOCTEST_CHECK_EQ(list.providers[volcano].apiKey, std::string("sk-volcano-secret"));
    DOCTEST_CHECK_EQ(list.providers[volcano].accessKeyId, std::string("AKLT-volcano-ak"));
    DOCTEST_CHECK_EQ(list.providers[volcano].secretAccessKey, std::string("volcano-sk"));
    DOCTEST_CHECK_EQ(list.providers[volcano].codingPlanProvider, std::string("volcengine"));

    // DeepSeek：usage_script 里没有 codingPlanProvider / AK —— 空串，不是崩溃
    const std::size_t deepseek = findByName(list, "DeepSeek");
    DOCTEST_REQUIRE(deepseek < list.providers.size());
    DOCTEST_CHECK_EQ(list.providers[deepseek].codingPlanProvider, std::string(""));
    DOCTEST_CHECK_EQ(list.providers[deepseek].accessKeyId, std::string(""));
    DOCTEST_CHECK_EQ(list.providers[deepseek].secretAccessKey, std::string(""));
    DOCTEST_CHECK_EQ(list.providers[deepseek].apiKey, std::string("sk-deepseek-secret"));
    return true;
})

REGISTER_TEST(claude_quota_ccswitch_key_fallback_order,
              "claude_quota_ccswitch: apiKey 取第一个**非空**键（空串占位要跳过）", {
    const test_support::TempDir dir("ccswitch_fallback");
    const std::filesystem::path db = dir.file("cc-switch.db");
    // cc-switch 的预设会把 ANTHROPIC_AUTH_TOKEN 种成「存在但空串」的占位，真 key
    // 在 ANTHROPIC_API_KEY 里。JS 的 `a || b` 会跳过空串，只按「键存在」挑会挑错。
    const bool built = createCcSwitchDb(
        db, kRealSchema,
        {insertProvider("id-empty-first", "claude", "空占位",
                        "{\"env\":{\"ANTHROPIC_BASE_URL\":\"https://a.example.com\","
                        "\"ANTHROPIC_AUTH_TOKEN\":\"\",\"ANTHROPIC_API_KEY\":\"sk-real\"}}",
                        "{}"),
         // 一个 key 都没有 → 空串，不是崩溃
         insertProvider("id-no-key", "claude", "没 key",
                        "{\"env\":{\"ANTHROPIC_BASE_URL\":\"https://b.example.com\"}}", "{}")});
    DOCTEST_REQUIRE(built);
    const test_support::ScopedEnv env("CCSWITCH_DB_PATH", db.string());

    const CcSwitchProviderList list = module::claude::quota::listCcSwitchProviders();
    DOCTEST_REQUIRE_EQ(list.providers.size(), static_cast<std::size_t>(2));
    const std::size_t a = findByName(list, "空占位");
    DOCTEST_REQUIRE(a < list.providers.size());
    DOCTEST_CHECK_EQ(list.providers[a].apiKey, std::string("sk-real"));
    const std::size_t b = findByName(list, "没 key");
    DOCTEST_REQUIRE(b < list.providers.size());
    DOCTEST_CHECK_EQ(list.providers[b].apiKey, std::string(""));
    // base_url 尾斜杠要去掉（cc-switch :262 trim_end_matches('/')）
    DOCTEST_CHECK_EQ(list.providers[b].baseUrl, std::string("https://b.example.com"));
    return true;
})

REGISTER_TEST(claude_quota_ccswitch_only_claude_app_type,
              "claude_quota_ccswitch: 只取 Claude 侧（codex/gemini 行不进结果）", {
    const test_support::TempDir dir("ccswitch_apptype");
    const std::filesystem::path db = dir.file("cc-switch.db");
    const bool built = createCcSwitchDb(
        db, kRealSchema,
        {insertProvider("id-claude", "claude", "Claude 侧",
                        settingsConfig("https://api.deepseek.com/anthropic",
                                       "ANTHROPIC_AUTH_TOKEN", "sk-a"),
                        "{}"),
         insertProvider("id-codex", "codex", "Codex 侧",
                        settingsConfig("https://api.deepseek.com/anthropic",
                                       "ANTHROPIC_AUTH_TOKEN", "sk-b"),
                        "{}"),
         insertProvider("id-gemini", "gemini", "Gemini 侧",
                        settingsConfig("https://api.deepseek.com/anthropic",
                                       "ANTHROPIC_AUTH_TOKEN", "sk-c"),
                        "{}")});
    DOCTEST_REQUIRE(built);
    const test_support::ScopedEnv env("CCSWITCH_DB_PATH", db.string());

    const CcSwitchProviderList list = module::claude::quota::listCcSwitchProviders();
    DOCTEST_REQUIRE_EQ(list.providers.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(list.providers[0].id, std::string("id-claude"));
    // 匹配也只可能在 Claude 侧命中
    DOCTEST_CHECK_EQ(module::claude::quota::matchCcSwitchProviderByBaseUrl(
                         "https://api.deepseek.com/anthropic"),
                     std::string("id-claude"));
    return true;
})

// ==================== ③ schema 演进 / 结构退化 ====================

REGISTER_TEST(claude_quota_ccswitch_degrades_on_broken_meta,
              "claude_quota_ccswitch: meta 为 NULL/非法 JSON/缺 usage_script → 仍读出 "
              "name+baseUrl，不崩", {
    const test_support::TempDir dir("ccswitch_degrade");
    const std::filesystem::path db = dir.file("cc-switch.db");
    const std::string goodSettings =
        settingsConfig("https://api.deepseek.com/anthropic", "ANTHROPIC_AUTH_TOKEN", "sk-x");
    const bool built = createCcSwitchDb(
        db, schemaWithNullableMeta(),
        {insertProvider("id-null-meta", "claude", "meta 是 NULL", goodSettings, "{}"),
         // 手工把 meta 置 NULL（列声明是 NOT NULL，夹具期先建后改，模拟旧库/异库形态）
         "UPDATE providers SET meta = NULL WHERE id = 'id-null-meta';",
         insertProvider("id-bad-json", "claude", "meta 不是 JSON", goodSettings,
                        "这不是 json {{{"),
         insertProvider("id-no-script", "claude", "没有 usage_script", goodSettings,
                        "{\"commonConfigEnabled\":false}"),
         insertProvider("id-script-not-object", "claude", "usage_script 是字符串",
                        goodSettings, "{\"usage_script\":\"nope\"}"),
         insertProvider("id-bad-settings", "claude", "settings_config 不是 JSON", "{坏了",
                        usageMeta("{\"codingPlanProvider\":\"volcengine\"}")),
         insertProvider("id-env-not-object", "claude", "env 是数组",
                        "{\"env\":[1,2,3]}", "{\"usage_script\":{\"apiKey\":\"sk-usage\"}}")});
    DOCTEST_REQUIRE(built);
    const test_support::ScopedEnv env("CCSWITCH_DB_PATH", db.string());

    const CcSwitchProviderList list = module::claude::quota::listCcSwitchProviders();
    DOCTEST_CHECK(list.status == CcSwitchSourceStatus::Ok);
    DOCTEST_REQUIRE_EQ(list.providers.size(), static_cast<std::size_t>(6));

    // 六行全部：name 与 baseUrl 该有的都有；坏的字段退化成空串而不是崩
    const std::size_t nullMeta = findByName(list, "meta 是 NULL");
    DOCTEST_REQUIRE(nullMeta < list.providers.size());
    DOCTEST_CHECK_EQ(list.providers[nullMeta].baseUrl,
                     std::string("https://api.deepseek.com/anthropic"));
    DOCTEST_CHECK_EQ(list.providers[nullMeta].apiKey, std::string("sk-x"));
    DOCTEST_CHECK_EQ(list.providers[nullMeta].codingPlanProvider, std::string(""));

    const std::size_t badJson = findByName(list, "meta 不是 JSON");
    DOCTEST_REQUIRE(badJson < list.providers.size());
    DOCTEST_CHECK_EQ(list.providers[badJson].name, std::string("meta 不是 JSON"));
    DOCTEST_CHECK_EQ(list.providers[badJson].apiKey, std::string("sk-x"));

    const std::size_t noScript = findByName(list, "没有 usage_script");
    DOCTEST_REQUIRE(noScript < list.providers.size());
    DOCTEST_CHECK_EQ(list.providers[noScript].baseUrl,
                     std::string("https://api.deepseek.com/anthropic"));
    DOCTEST_CHECK_EQ(list.providers[noScript].accessKeyId, std::string(""));

    // usage_script 不是对象 → 所有 usage 字段空，但 name/baseUrl 照读
    const std::size_t notObject = findByName(list, "usage_script 是字符串");
    DOCTEST_REQUIRE(notObject < list.providers.size());
    DOCTEST_CHECK_EQ(list.providers[notObject].codingPlanProvider, std::string(""));
    DOCTEST_CHECK_EQ(list.providers[notObject].baseUrl,
                     std::string("https://api.deepseek.com/anthropic"));

    const std::size_t badSettings = findByName(list, "settings_config 不是 JSON");
    DOCTEST_REQUIRE(badSettings < list.providers.size());
    DOCTEST_CHECK_EQ(list.providers[badSettings].name,
                     std::string("settings_config 不是 JSON"));
    DOCTEST_CHECK_EQ(list.providers[badSettings].baseUrl, std::string(""));
    DOCTEST_CHECK_EQ(list.providers[badSettings].apiKey, std::string(""));
    // meta 那半仍然有效 → 退化是**逐字段**的，不是整行丢弃
    DOCTEST_CHECK_EQ(list.providers[badSettings].codingPlanProvider,
                     std::string("volcengine"));

    const std::size_t envArray = findByName(list, "env 是数组");
    DOCTEST_REQUIRE(envArray < list.providers.size());
    DOCTEST_CHECK_EQ(list.providers[envArray].baseUrl, std::string(""));
    DOCTEST_CHECK_EQ(list.providers[envArray].codingPlanProvider, std::string(""));
    return true;
})

REGISTER_TEST(claude_quota_ccswitch_degrades_on_missing_columns,
              "claude_quota_ccswitch: providers 表少了 meta/settings_config 列 → 仍读出 "
              "id/name，不崩", {
    const test_support::TempDir dir("ccswitch_nocol");
    const std::filesystem::path db = dir.file("cc-switch.db");
    // 只有 id/app_type/name 三列（模拟 cc-switch 未来把列改名 / 拆表）
    const bool built = createCcSwitchDb(
        db, "CREATE TABLE providers (id TEXT NOT NULL, app_type TEXT NOT NULL, name TEXT NOT NULL);",
        {"INSERT INTO providers (id, app_type, name) VALUES ('id-a','claude','只有名字');"});
    DOCTEST_REQUIRE(built);
    const test_support::ScopedEnv env("CCSWITCH_DB_PATH", db.string());

    const CcSwitchProviderList list = module::claude::quota::listCcSwitchProviders();
    DOCTEST_CHECK(list.status == CcSwitchSourceStatus::Ok);
    DOCTEST_REQUIRE_EQ(list.providers.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(list.providers[0].id, std::string("id-a"));
    DOCTEST_CHECK_EQ(list.providers[0].name, std::string("只有名字"));
    DOCTEST_CHECK_EQ(list.providers[0].baseUrl, std::string(""));
    DOCTEST_CHECK_EQ(list.providers[0].apiKey, std::string(""));
    return true;
})

REGISTER_TEST(claude_quota_ccswitch_missing_table_is_reported,
              "claude_quota_ccswitch: providers 表不存在 / 缺 id 列 → Schema 状态 + 原因", {
    const test_support::TempDir dir("ccswitch_notable");
    const std::filesystem::path noTable = dir.file("no-table.db");
    const std::filesystem::path noId = dir.file("no-id.db");
    DOCTEST_REQUIRE(createCcSwitchDb(noTable, "CREATE TABLE other (x INTEGER);", {}));
    DOCTEST_REQUIRE(createCcSwitchDb(
        noId, "CREATE TABLE providers (app_type TEXT, name TEXT);",
        {"INSERT INTO providers (app_type, name) VALUES ('claude','没有 id 列');"}));

    {
        const test_support::ScopedEnv env("CCSWITCH_DB_PATH", noTable.string());
        const CcSwitchProviderList list = module::claude::quota::listCcSwitchProviders();
        DOCTEST_CHECK(list.status == CcSwitchSourceStatus::Schema);
        DOCTEST_CHECK(list.providers.empty());
        DOCTEST_CHECK_FALSE(list.unavailableReason.empty());
    }
    {
        const test_support::ScopedEnv env("CCSWITCH_DB_PATH", noId.string());
        const CcSwitchProviderList list = module::claude::quota::listCcSwitchProviders();
        DOCTEST_CHECK(list.status == CcSwitchSourceStatus::Schema);
        DOCTEST_CHECK(list.providers.empty());
        DOCTEST_CHECK_FALSE(list.unavailableReason.empty());
    }
    return true;
})

// ==================== ④ base_url 自动匹配 ====================

REGISTER_TEST(claude_quota_ccswitch_match_base_url,
              "claude_quota_ccswitch: base_url 匹配 命中/未命中/伪造后缀不命中", {
    const test_support::TempDir dir("ccswitch_match");
    const std::filesystem::path db = dir.file("cc-switch.db");
    const bool built = createCcSwitchDb(
        db, kRealSchema,
        {insertProvider("id-deepseek", "claude", "DeepSeek",
                        settingsConfig("https://api.deepseek.com/anthropic",
                                       "ANTHROPIC_AUTH_TOKEN", "sk-d"),
                        "{}"),
         insertProvider("id-minimax", "claude", "MiniMax",
                        settingsConfig("https://api.minimax.cn/anthropic",
                                       "ANTHROPIC_AUTH_TOKEN", "sk-m"),
                        "{}"),
         insertProvider("id-empty-base", "claude", "没 base_url",
                        "{\"env\":{\"ANTHROPIC_AUTH_TOKEN\":\"sk-e\"}}", "{}")});
    DOCTEST_REQUIRE(built);
    const test_support::ScopedEnv env("CCSWITCH_DB_PATH", db.string());
    const auto match = [](const std::string& url) {
        return module::claude::quota::matchCcSwitchProviderByBaseUrl(url);
    };

    // 命中：整串相同 / 尾斜杠 / 大小写 / 带 query
    DOCTEST_CHECK_EQ(match("https://api.deepseek.com/anthropic"), std::string("id-deepseek"));
    DOCTEST_CHECK_EQ(match("https://api.deepseek.com/anthropic/"), std::string("id-deepseek"));
    DOCTEST_CHECK_EQ(match("HTTPS://API.DeepSeek.COM/anthropic"), std::string("id-deepseek"));
    DOCTEST_CHECK_EQ(match("https://api.deepseek.com/anthropic?x=1"), std::string("id-deepseek"));
    DOCTEST_CHECK_EQ(match("https://api.minimax.cn/anthropic"), std::string("id-minimax"));

    // 未命中：别的 host / 别的 path / 没 path / 空串
    DOCTEST_CHECK_EQ(match("https://api.other.com/anthropic"), std::string(""));
    DOCTEST_CHECK_EQ(match("https://api.deepseek.com/v1"), std::string(""));
    DOCTEST_CHECK_EQ(match("https://api.deepseek.com"), std::string(""));
    DOCTEST_CHECK_EQ(match(""), std::string(""));
    DOCTEST_CHECK_EQ(match("不是 url"), std::string(""));

    // 伪造后缀（任务书点名的两条）：裸 contains 会全放进来，标签边界必须挡住
    DOCTEST_CHECK_EQ(match("https://api.deepseek.com.evil.com/anthropic"), std::string(""));
    DOCTEST_CHECK_EQ(match("https://notapi.deepseek.com/anthropic"), std::string(""));
    DOCTEST_CHECK_EQ(match("https://xapi.deepseek.com/anthropic"), std::string(""));
    DOCTEST_CHECK_EQ(match("https://api.deepseek.com.evil.com"), std::string(""));
    // 端口 / userinfo 不该改变判定
    DOCTEST_CHECK_EQ(match("https://api.deepseek.com:443/anthropic"), std::string("id-deepseek"));
    DOCTEST_CHECK_EQ(match("https://user:pw@api.deepseek.com/anthropic"),
                     std::string("id-deepseek"));
    // 有 provider 但 base_url 为空 → 永远不命中它
    DOCTEST_CHECK_EQ(match("https:///"), std::string(""));
    return true;
})

REGISTER_TEST(claude_quota_ccswitch_match_when_source_unavailable,
              "claude_quota_ccswitch: 来源不可用时匹配返回空串（不是崩溃）", {
    const test_support::TempDir dir("ccswitch_match_bad");
    const test_support::ScopedEnv env("CCSWITCH_DB_PATH", dir.file("缺的库.db").string());
    DOCTEST_CHECK_EQ(module::claude::quota::matchCcSwitchProviderByBaseUrl(
                         "https://api.deepseek.com/anthropic"),
                     std::string(""));
    return true;
})

// ==================== ⑤ 库不存在 / 打不开 ====================

REGISTER_TEST(claude_quota_ccswitch_missing_db_is_reported_and_not_created,
              "claude_quota_ccswitch: 库不存在 → 空列表+NotFound，且**不许创建文件**", {
    const test_support::TempDir dir("ccswitch_missing");
    const std::filesystem::path db = dir.file("根本没有这个库.db");
    const test_support::ScopedEnv env("CCSWITCH_DB_PATH", db.string());
    DOCTEST_REQUIRE(!std::filesystem::exists(db));

    const CcSwitchProviderList list = module::claude::quota::listCcSwitchProviders();
    DOCTEST_CHECK(list.status == CcSwitchSourceStatus::NotFound);
    DOCTEST_CHECK(list.providers.empty());
    DOCTEST_CHECK_FALSE(list.unavailableReason.empty());
    // SQLITE_OPEN_CREATE 的实现在这里会把库**建出来**——等于在用户盘上留垃圾
    DOCTEST_CHECK_FALSE(std::filesystem::exists(db));
    // 连 -wal / -shm / -journal 都不许留
    DOCTEST_CHECK(dirEntries(dir.path).empty());  // 连 -wal/-shm/-journal 都没留下
    return true;
})

REGISTER_TEST(claude_quota_ccswitch_garbage_file_is_reported,
              "claude_quota_ccswitch: 非 SQLite 文件 → Unreadable + 原因，不崩", {
    const test_support::TempDir dir("ccswitch_garbage");
    const std::filesystem::path db = dir.file("garbage.db");
    dir.write(db, "this is not a sqlite database, it is a poem about embroidery\n");
    const test_support::ScopedEnv env("CCSWITCH_DB_PATH", db.string());

    const CcSwitchProviderList list = module::claude::quota::listCcSwitchProviders();
    DOCTEST_CHECK(list.providers.empty());
    DOCTEST_CHECK(list.status == CcSwitchSourceStatus::Unreadable ||
                  list.status == CcSwitchSourceStatus::Schema);
    DOCTEST_CHECK_FALSE(list.unavailableReason.empty());
    return true;
})

REGISTER_TEST(claude_quota_ccswitch_default_path_under_home,
              "claude_quota_ccswitch: 未设环境变量 → ~/.cc-switch/cc-switch.db", {
    // 只断言路径形态：不读真实库（那是用户的文件，且有应用在写）
    const test_support::TempDir dir("ccswitch_home");
    const test_support::ScopedEnv env("CCSWITCH_DB_PATH", std::string(""));
    const std::string path = module::claude::quota::ccSwitchDbPath();
    DOCTEST_CHECK(path.find(".cc-switch") != std::string::npos);
    DOCTEST_CHECK(path.find("cc-switch.db") != std::string::npos);
    DOCTEST_CHECK(path.find("/.cc-switch/") != std::string::npos);
    // 覆盖值原样生效
    const test_support::ScopedEnv env2("CCSWITCH_DB_PATH", dir.file("x.db").string());
    DOCTEST_CHECK_EQ(module::claude::quota::ccSwitchDbPath(), dir.file("x.db").string());
    return true;
})

// ==================== ① 只读打开 ====================

REGISTER_TEST(claude_quota_ccswitch_opens_readonly,
              "claude_quota_ccswitch: 只读打开（open flags 含 READONLY、不含 "
              "READWRITE/CREATE）+ 0444 库可读且内容/mtime/目录清单不变", {
    // (a) open flags —— SQLite 在 EACCES 时会**静默降级**成只读连接（实测：
    //     0444 的库上 SQLITE_OPEN_READWRITE 照样打开成功），所以「文件权限 +
    //     内容没变」这一对观察区分不出两种打开方式，flag 必须直接断言。
    DOCTEST_CHECK((module::claude::quota::kCcSwitchOpenFlags & SQLITE_OPEN_READONLY) != 0);
    DOCTEST_CHECK((module::claude::quota::kCcSwitchOpenFlags & SQLITE_OPEN_READWRITE) == 0);
    DOCTEST_CHECK((module::claude::quota::kCcSwitchOpenFlags & SQLITE_OPEN_CREATE) == 0);

    // (b) 0444 的库仍读得出来（真只读打开不需要写权限）
    const test_support::TempDir dir("ccswitch_ro");
    const std::string dbPath = deepSeekOnlyDb(dir.path);
    DOCTEST_REQUIRE(!dbPath.empty());
    const std::filesystem::path db(dbPath);

    const std::string before = readWholeFile(db);
    const std::uint64_t beforeHash = fnv1a(before);
    const std::int64_t beforeMtime = mtimeTicks(db);
    const std::vector<std::string> beforeEntries = dirEntries(dir.path);

    std::error_code ec;
    std::filesystem::permissions(db, std::filesystem::perms::owner_read |
                                         std::filesystem::perms::group_read |
                                         std::filesystem::perms::others_read,
                                 std::filesystem::perm_options::replace, ec);
    DOCTEST_REQUIRE(!ec);

    {
        const test_support::ScopedEnv env("CCSWITCH_DB_PATH", dbPath);
        const CcSwitchProviderList list = module::claude::quota::listCcSwitchProviders();
        DOCTEST_REQUIRE_EQ(list.providers.size(), static_cast<std::size_t>(1));
        DOCTEST_CHECK_EQ(list.providers[0].apiKey, std::string("sk-ccswitch-sentinel-key"));
        // 再读一次（多轮读取也不许留下痕迹）
        DOCTEST_CHECK_EQ(module::claude::quota::listCcSwitchProviders().providers.size(),
                         static_cast<std::size_t>(1));
    }

    DOCTEST_CHECK_EQ(fnv1a(readWholeFile(db)), beforeHash);   // 内容一字未动
    DOCTEST_CHECK_EQ(mtimeTicks(db), beforeMtime);            // mtime 未动
    DOCTEST_CHECK_EQ(dirEntries(dir.path), beforeEntries);    // 没多出 -wal/-shm/-journal

    std::filesystem::permissions(db, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
    return true;
})

// ==================== ⑥ 密钥不落任何出口 ====================

REGISTER_TEST(claude_quota_ccswitch_secrets_never_logged_or_in_reason,
              "claude_quota_ccswitch: 密钥不出现在日志文件/错误文案里", {
    const test_support::TempDir dir("ccswitch_nolog");
    const std::string sentinel = "sk-SENTINEL-MUST-NOT-LEAK-0123456789";
    const std::string sentinelAk = "AKLT-SENTINEL-AK-MUST-NOT-LEAK";
    const std::string sentinelSk = "SENTINEL-SK-MUST-NOT-LEAK";

    const std::filesystem::path good = dir.file("good.db");
    DOCTEST_REQUIRE(createCcSwitchDb(
        good, kRealSchema,
        {insertProvider("id-secret", "claude", "有密钥的",
                        settingsConfig("https://api.deepseek.com/anthropic",
                                       "ANTHROPIC_AUTH_TOKEN", sentinel),
                        usageMeta("{\"enabled\":true,\"codingPlanProvider\":\"volcengine\","
                                  "\"accessKeyId\":\"" + sentinelAk + "\","
                                  "\"secretAccessKey\":\"" + sentinelSk + "\"}"))}));

    const std::filesystem::path logFile = dir.file("ccswitch-test.log");
    core::log::setLevel(core::log::Level::Trace);
    core::log::addFileOutput(logFile);

    // (a) 成功路径：读一遍（把能触发的日志全触发）
    {
        const test_support::ScopedEnv env("CCSWITCH_DB_PATH", good.string());
        const CcSwitchProviderList list = module::claude::quota::listCcSwitchProviders();
        DOCTEST_REQUIRE_EQ(list.providers.size(), static_cast<std::size_t>(1));
        DOCTEST_CHECK_EQ(list.providers[0].apiKey, sentinel);
        DOCTEST_CHECK_EQ(list.providers[0].secretAccessKey, sentinelSk);
        DOCTEST_CHECK_EQ(module::claude::quota::matchCcSwitchProviderByBaseUrl(
                             "https://api.deepseek.com/anthropic"),
                         std::string("id-secret"));
        DOCTEST_CHECK_EQ(module::claude::quota::matchCcSwitchProviderByBaseUrl(
                             "https://api.deepseek.com.evil.com/anthropic"),
                         std::string(""));
    }

    // (b) 失败路径：把「含密钥的库」当坏库用（表被删）→ 错误文案里也不许带密钥
    const std::filesystem::path broken = dir.file("broken.db");
    DOCTEST_REQUIRE(createCcSwitchDb(
        broken, kRealSchema,
        {insertProvider("id-secret", "claude", "有密钥的",
                        settingsConfig("https://api.deepseek.com/anthropic",
                                       "ANTHROPIC_AUTH_TOKEN", sentinel),
                        usageMeta("{\"secretAccessKey\":\"" + sentinelSk + "\"}")),
         "DROP TABLE providers;"}));
    std::string reason;
    {
        const test_support::ScopedEnv env("CCSWITCH_DB_PATH", broken.string());
        const CcSwitchProviderList list = module::claude::quota::listCcSwitchProviders();
        DOCTEST_CHECK_FALSE(list.unavailableReason.empty());
        reason = list.unavailableReason;
    }
    {
        const test_support::ScopedEnv env("CCSWITCH_DB_PATH", dir.file("缺.db").string());
        const CcSwitchProviderList list = module::claude::quota::listCcSwitchProviders();
        reason += " " + list.unavailableReason;
    }

    DOCTEST_CHECK(reason.find(sentinel) == std::string::npos);
    DOCTEST_CHECK(reason.find(sentinelAk) == std::string::npos);
    DOCTEST_CHECK(reason.find(sentinelSk) == std::string::npos);

    const std::string log = readWholeFile(logFile);
    DOCTEST_CHECK(log.find(sentinel) == std::string::npos);
    DOCTEST_CHECK(log.find(sentinelAk) == std::string::npos);
    DOCTEST_CHECK(log.find(sentinelSk) == std::string::npos);
    return true;
})

REGISTER_TEST(claude_quota_ccswitch_has_no_write_path_at_all,
              "claude_quota_ccswitch: 反复读取不产生任何写副作用（行数/内容恒定）", {
    const test_support::TempDir dir("ccswitch_nowrite");
    const std::filesystem::path db = dir.file("cc-switch.db");
    DOCTEST_REQUIRE(createCcSwitchDb(
        db, kRealSchema,
        {insertProvider("id-a", "claude", "A",
                        settingsConfig("https://api.deepseek.com/anthropic",
                                       "ANTHROPIC_AUTH_TOKEN", "sk-a"),
                        "{}")}));
    const test_support::ScopedEnv env("CCSWITCH_DB_PATH", db.string());
    const std::string before = readWholeFile(db);
    for (int i = 0; i < 3; ++i) {
        (void)module::claude::quota::listCcSwitchProviders();
        (void)module::claude::quota::matchCcSwitchProviderByBaseUrl(
            "https://api.deepseek.com/anthropic");
    }
    DOCTEST_CHECK_EQ(readWholeFile(db), before);
    return true;
})
