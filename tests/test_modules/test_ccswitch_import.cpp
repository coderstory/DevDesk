// tests/test_modules/test_ccswitch_import.cpp
// 从 cc-switch 导入 Profile：纯映射 + 只读读侧 + 编排层三层。
// 跨文件前提：
//   - 夹具全部自建临时 sqlite 库（sqlite3 C API 建表插行），经 CCSWITCH_DB_PATH
//     指路，绝不碰用户真实的 ~/.cc-switch/cc-switch.db。密钥一律假串。
//   - 读侧只读打开该库，按 app_type ∈ ('claude','claude-desktop') 筛选；其余 app_type
//     忽略；库不存在时返回 NotFound，绝不静默空列表。
//   - skip 语义（缺 base_url / 缺密钥 / 同名）与 fail 语义（可重试/系统错误）
//     分开：源缺必填字段属确定性不可导入，不进失败桶。
//   - 密钥不出任何出口：夹具 / 断言里只有假串；skipReason 与 toast 文案里都断言
//     不含 "sk-"（确定不把 settings_config 原文拼进文案）。
#include "doctest.h"

#include "test_support.h"

#include "core/logging.h"
#include "module_claude_proxy/ccswitch_import.h"
#include "module_claude_proxy/claude_ui_ccswitch.h"   // 编排层入口（透出 claude_service.h）

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

// cc-switch 真实建表语句（列名逐字对齐）
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
    // extraEnvJson：四保留键之外的全部键值，一个不丢；双 key 并存时未用作 apiKey 的
    // ANTHROPIC_API_KEY 保留进表（本例 TOKEN 已用作 apiKey，API_KEY 是被保留的那个）
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

// 无 TOKEN 且无 API_KEY 的条目属「源缺必填字段」的确定性不可导入，与缺 base_url
// 同类；若按映射建行，会被 createProfile 的「apiKey 不能为空」拒进「失败」桶，
// 而失败桶留给可重试/系统错误
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

// 双 key（TOKEN+API_KEY）时 apiKey 取 TOKEN，未用作 apiKey 的那个 key 保留进
// extraEnvJson，不丢值。运行期安全由既有守卫保证：直连 buildDirectEnv 清另一密钥键、
// 代理 UpstreamDialect::applyHeaders 无条件丢弃客户端凭据
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
    // 两行恰是 claude 与 claude-desktop；settings_config 原文原样读回
    const size_t cd = findByName(rows, "Claude Desktop");
    DOCTEST_REQUIRE(cd < rows.rows.size());
    DOCTEST_CHECK_EQ(rows.rows[cd].appType, std::string("claude-desktop"));
    DOCTEST_CHECK(rows.rows[cd].settingsConfig.find("ANTHROPIC_BASE_URL") !=
                  std::string::npos);
    return true;
})

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

// 编排层 confirmCcSwitchImport（勾选过滤 → 带 existingNames 重映射 → createProfile
// → 失败收集 → 二段补写 → 结果报告 → 标脏/toast）**直接驱动自由函数**：它不依赖
// 窗口（入参 = 状态结构体 + existingProfiles + hooks 回调），ClaudeService 经
// setGlobal 注入真服务 + 临时库，无 headless ui、无生产测试缝。
// 本节只测编排层独有的聚合行为：多结果混合计数、失败不中断、报告结构与顺序、
// 标脏时机、二段写半失败的事后状态、无服务早退。映射层的 skip 判定本身已在上方
// 用例覆盖，不重复。

namespace {

// RAII：临时库 + 注册全局 ClaudeService。析构必须解除注册，防悬垂污染后续用例；
// test_main 的 resetCaseGlobals 是兜底，不是本夹具的替代
struct CcSwitchConfirmEnv : test_support::TestDb {
    std::unique_ptr<module::claude::ClaudeService> svc;

    CcSwitchConfirmEnv()
        : test_support::TestDb("devdesk_test_ccswitch_confirm_", "") {
        svc = std::make_unique<module::claude::ClaudeService>(
            db->db(), "DevDeskTestCcSwitchConfirm");
        module::claude::ClaudeService::setGlobal(svc.get());
    }
    ~CcSwitchConfirmEnv() {
        module::claude::ClaudeService::setGlobal(nullptr);
    }
};

// hooks 捕获器：markDataDirty 调用次数 + toast 三元组逐次记录
struct HookRecorder {
    int dirtyCalls = 0;
    std::vector<std::string> toastTitles;
    std::vector<std::string> toastMsgs;
    std::vector<bool> toastErrors;

    module::claude::CcSwitchImportDialogHooks hooks() {
        return module::claude::CcSwitchImportDialogHooks{
            [this] { ++dirtyCalls; },
            [this](const std::string& title, const std::string& msg, bool err) {
                toastTitles.push_back(title);
                toastMsgs.push_back(msg);
                toastErrors.push_back(err);
            }};
    }
};

// 一行候选：name + settings_config 原文（settingsConfigWith 产）
struct Candidate {
    std::string name;
    std::string settingsConfig;
};

// 装配弹窗状态。rows_/mappings_/selected_ 三向量必须同长对齐（claude_ui_state.h
// 声明的不变式）；confirm 不读 mappings_（确认时带 existingNames 重映射），这里
// 按 open 语义填齐
module::claude::CcSwitchImportDialogState makeConfirmState(
    const std::vector<Candidate>& candidates,
    const std::vector<bool>& selected) {
    module::claude::CcSwitchImportDialogState st;
    st.open_ = true;
    st.loaded_ = true;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const Candidate& c = candidates[i];
        st.rows_.push_back(
            {"id-" + std::to_string(i), "claude", c.name, c.settingsConfig});
        st.mappings_.push_back(
            mapCcSwitchProviderToProfile(c.name, c.settingsConfig));
        st.selected_.push_back(selected.empty() ? true : selected[i]);
    }
    return st;
}

// 按名取 profile（找不到返回 nullptr）
const module::claude::Profile* findProfile(
    const std::vector<module::claude::Profile>& profiles,
    const std::string& name) {
    for (const module::claude::Profile& p : profiles) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

}  // namespace

REGISTER_TEST(ccswitch_confirm_all_success,
              "ccswitch 编排：全部成功 2 条 → 2/0/0 + 行序报告 + 标脏一次 + toast「导入成功」+ 二段写落库 + 首条自动激活", {
    CcSwitchConfirmEnv env;
    HookRecorder rec;
    // Alpha 带额外 env + 顶层设置（触发二段补写 updateProfile）；Beta 纯净
    const std::vector<Candidate> candidates = {
        {"Alpha", settingsConfigWith("https://a.example.com", kFakeToken, "", "model-a",
                                     "\"ANTHROPIC_DEFAULT_SONNET_MODEL\":\"m\"",
                                     "\"theme\":\"dark\"")},
        {"Beta", settingsConfigWith("https://b.example.com", kFakeToken, "", "",
                                    std::string(), std::string())},
    };
    module::claude::CcSwitchImportDialogState st =
        makeConfirmState(candidates, {});

    module::claude::confirmCcSwitchImport(st, /*existingProfiles=*/{},
                                          rec.hooks());

    DOCTEST_CHECK_EQ(st.resultSuccess_, 2);
    DOCTEST_CHECK_EQ(st.resultSkipped_, 0);
    DOCTEST_CHECK_EQ(st.resultFailed_, 0);
    DOCTEST_REQUIRE_EQ(st.resultLines_.size(), static_cast<std::size_t>(2));
    DOCTEST_CHECK_EQ(st.resultLines_[0], std::string("成功：Alpha"));
    DOCTEST_CHECK_EQ(st.resultLines_[1], std::string("成功：Beta"));  // 行序 = 候选序
    // 标脏无条件一次（claude_ui_ccswitch.h 口径：0 成功也刷新列表）
    DOCTEST_CHECK_EQ(rec.dirtyCalls, 1);
    // toast：无失败无跳过 → 「导入成功」档、非错误
    DOCTEST_REQUIRE_EQ(rec.toastTitles.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(rec.toastTitles[0], std::string("导入成功"));
    DOCTEST_CHECK_EQ(rec.toastMsgs[0], std::string("已导入 2 条 Profile"));
    DOCTEST_CHECK_FALSE(rec.toastErrors[0]);
    DOCTEST_CHECK(rec.toastMsgs[0].find("sk-") == std::string::npos);
    // Alpha 的 extraEnv/extraSettings 落上 = 二段补写真跑过：createProfile 不写
    // extra_env 列，只有 updateProfile 补写
    auto listR = env.svc->listProfiles();
    DOCTEST_REQUIRE(listR.isOk());
    const std::vector<module::claude::Profile> profiles =
        std::move(listR).value();
    DOCTEST_REQUIRE_EQ(profiles.size(), static_cast<std::size_t>(2));
    const module::claude::Profile* alpha = findProfile(profiles, "Alpha");
    const module::claude::Profile* beta = findProfile(profiles, "Beta");
    DOCTEST_REQUIRE(alpha != nullptr);
    DOCTEST_REQUIRE(beta != nullptr);
    DOCTEST_CHECK(alpha->extraEnvJson.find("ANTHROPIC_DEFAULT_SONNET_MODEL") !=
                  std::string::npos);
    DOCTEST_CHECK(alpha->extraSettingsJson.find("\"theme\"") != std::string::npos);
    DOCTEST_CHECK_EQ(beta->extraEnvJson, std::string(""));
    // 编排传 activateIfNoneActive=true：首条自动激活、第二条不抢占
    DOCTEST_CHECK(alpha->isActive);
    DOCTEST_CHECK_FALSE(beta->isActive);
    return true;
})

REGISTER_TEST(ccswitch_confirm_mixed_counts_and_no_abort,
              "ccswitch 编排：竞态失败→成功→同名跳过混合 → 1/1/1 计数、失败原因进报告、失败不中断后续", {
    CcSwitchConfirmEnv env;
    HookRecorder rec;
    // 预置 Base（进 existing 名单 → 确认时同名跳过）
    auto baseR = env.svc->createProfile(
        "Base", "https://base.example.com", kFakeToken, "", /*activate=*/true,
        /*sharedConfigId=*/0, "bearer", "", /*fallbackProfileId=*/0, "");
    DOCTEST_REQUIRE(baseR.isOk());
    // 真实 UNIQUE 竞态路径：UI 确认回调在点击时读一次 listProfiles；读到之后、
    // confirm 之前别的窗口建了 Ghost → confirm 拿着过期名单进来，映射放过
    // Ghost、INSERT 撞 UNIQUE → 落「失败」桶（失败桶留给可重试/系统错误，
    // UNIQUE 竞态正属此类）
    auto existingR = env.svc->listProfiles();
    DOCTEST_REQUIRE(existingR.isOk());
    std::vector<module::claude::Profile> existing = std::move(existingR).value();
    auto ghostR = env.svc->createProfile(
        "Ghost", "https://ghost.example.com", kFakeToken, "", true, 0, "bearer",
        "", 0, "");
    DOCTEST_REQUIRE(ghostR.isOk());

    // 行序故意失败在前：Ghost 失败后 Fresh 仍要被处理（失败不中断）
    const std::vector<Candidate> candidates = {
        {"Ghost", settingsConfigWith("https://ghost.example.com", kFakeToken,
                                     "", "", std::string(), std::string())},
        {"Fresh", settingsConfigWith("https://fresh.example.com", kFakeToken,
                                     "", "", std::string(), std::string())},
        {"Base", settingsConfigWith("https://base.example.com", kFakeToken, "",
                                    "", std::string(), std::string())},
    };
    module::claude::CcSwitchImportDialogState st =
        makeConfirmState(candidates, {});

    module::claude::confirmCcSwitchImport(st, existing, rec.hooks());

    DOCTEST_CHECK_EQ(st.resultSuccess_, 1);
    DOCTEST_CHECK_EQ(st.resultSkipped_, 1);
    DOCTEST_CHECK_EQ(st.resultFailed_, 1);
    DOCTEST_REQUIRE_EQ(st.resultLines_.size(), static_cast<std::size_t>(3));
    // 失败原因进报告（createProfile 的 UNIQUE 文案），不带密钥
    DOCTEST_CHECK_EQ(st.resultLines_[0].find("失败：Ghost（"),
                     static_cast<std::size_t>(0));
    DOCTEST_CHECK(st.resultLines_[0].find("已存在") != std::string::npos);
    DOCTEST_CHECK(st.resultLines_[0].find("sk-") == std::string::npos);
    // 失败之后的成功条目照常处理（不中断）
    DOCTEST_CHECK_EQ(st.resultLines_[1], std::string("成功：Fresh"));
    DOCTEST_CHECK_EQ(st.resultLines_[2].find("跳过：Base（"),
                     static_cast<std::size_t>(0));
    DOCTEST_CHECK(st.resultLines_[2].find("同名") != std::string::npos);
    DOCTEST_CHECK_EQ(rec.dirtyCalls, 1);   // 部分成功也标脏（列表要出新条目）
    // 有失败 → toast 错误档 + 三计数 + 总数
    DOCTEST_REQUIRE_EQ(rec.toastTitles.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(rec.toastTitles[0], std::string("导入完成（有失败）"));
    DOCTEST_CHECK_EQ(rec.toastMsgs[0],
                     std::string("成功 1 / 跳过 1 / 失败 1（共 3 条）"));
    DOCTEST_CHECK(rec.toastErrors[0]);
    // DB：Fresh 真落库（成功 = 真写了）；Ghost/Base 各一条（预置）
    auto listR = env.svc->listProfiles();
    DOCTEST_REQUIRE(listR.isOk());
    const std::vector<module::claude::Profile> profiles =
        std::move(listR).value();
    DOCTEST_REQUIRE_EQ(profiles.size(), static_cast<std::size_t>(3));
    DOCTEST_REQUIRE(findProfile(profiles, "Fresh") != nullptr);
    return true;
})

REGISTER_TEST(ccswitch_confirm_unselected_excluded,
              "ccswitch 编排：未勾选条目不进结果报告（计数/行数/DB 三处一致）", {
    CcSwitchConfirmEnv env;
    HookRecorder rec;
    const std::vector<Candidate> candidates = {
        {"Picked", settingsConfigWith("https://p.example.com", kFakeToken, "",
                                      "", std::string(), std::string())},
        {"Dropped", settingsConfigWith("https://d.example.com", kFakeToken, "",
                                       "", std::string(), std::string())},
    };
    module::claude::CcSwitchImportDialogState st =
        makeConfirmState(candidates, {true, false});

    module::claude::confirmCcSwitchImport(st, {}, rec.hooks());

    DOCTEST_CHECK_EQ(st.resultSuccess_, 1);
    DOCTEST_CHECK_EQ(st.resultSkipped_, 0);
    DOCTEST_CHECK_EQ(st.resultFailed_, 0);
    DOCTEST_REQUIRE_EQ(st.resultLines_.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(st.resultLines_[0], std::string("成功：Picked"));
    DOCTEST_CHECK_EQ(rec.dirtyCalls, 1);
    DOCTEST_REQUIRE_EQ(rec.toastTitles.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(rec.toastMsgs[0], std::string("已导入 1 条 Profile"));
    // DB：只落勾选那条
    auto listR = env.svc->listProfiles();
    DOCTEST_REQUIRE(listR.isOk());
    const std::vector<module::claude::Profile> profiles =
        std::move(listR).value();
    DOCTEST_REQUIRE_EQ(profiles.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(profiles[0].name, std::string("Picked"));
    return true;
})

REGISTER_TEST(ccswitch_confirm_empty_selection_current_behavior,
              "ccswitch 编排：空勾选确认 → 0/0/0 + 无结果行 + 仍标脏 + toast「已导入 0 条」", {
    CcSwitchConfirmEnv env;
    HookRecorder rec;
    const std::vector<Candidate> candidates = {
        {"A", settingsConfigWith("https://a.example.com", kFakeToken, "", "",
                                 std::string(), std::string())},
        {"B", settingsConfigWith("https://b.example.com", kFakeToken, "", "",
                                 std::string(), std::string())},
    };
    module::claude::CcSwitchImportDialogState st =
        makeConfirmState(candidates, {false, false});

    module::claude::confirmCcSwitchImport(st, {}, rec.hooks());

    // 现行行为：0 勾选也走「导入成功 已导入 0 条」，措辞轻误导。裁决改文案前钉住
    // 现状——无意的行为漂移（改静默/改报错）会在此红
    DOCTEST_CHECK_EQ(st.resultSuccess_, 0);
    DOCTEST_CHECK_EQ(st.resultSkipped_, 0);
    DOCTEST_CHECK_EQ(st.resultFailed_, 0);
    DOCTEST_CHECK(st.resultLines_.empty());
    DOCTEST_CHECK_EQ(rec.dirtyCalls, 1);   // markDataDirty 无条件
    DOCTEST_REQUIRE_EQ(rec.toastTitles.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(rec.toastTitles[0], std::string("导入成功"));
    DOCTEST_CHECK_EQ(rec.toastMsgs[0], std::string("已导入 0 条 Profile"));
    DOCTEST_CHECK_FALSE(rec.toastErrors[0]);
    // DB 零写入
    auto listR = env.svc->listProfiles();
    DOCTEST_REQUIRE(listR.isOk());
    DOCTEST_CHECK(std::move(listR).value().empty());
    return true;
})

REGISTER_TEST(ccswitch_confirm_null_service,
              "ccswitch 编排：ClaudeService 未注册 → 勾选数全落失败桶 + 错误 toast + 不标脏", {
    // 出厂态 = 未注册。test_main 每例后重置是兜底，此处显式再清一次
    module::claude::ClaudeService::setGlobal(nullptr);
    HookRecorder rec;
    const std::vector<Candidate> candidates = {
        {"A", settingsConfigWith("https://a.example.com", kFakeToken, "", "",
                                 std::string(), std::string())},
        {"B", settingsConfigWith("https://b.example.com", kFakeToken, "", "",
                                 std::string(), std::string())},
    };
    module::claude::CcSwitchImportDialogState st =
        makeConfirmState(candidates, {});

    module::claude::confirmCcSwitchImport(st, {}, rec.hooks());

    DOCTEST_REQUIRE_EQ(st.resultLines_.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(st.resultLines_[0],
                     std::string("服务未就绪（ClaudeService 未注册）"));
    DOCTEST_CHECK_EQ(st.resultFailed_, 2);   // = 勾选数
    DOCTEST_CHECK_EQ(st.resultSuccess_, 0);
    DOCTEST_CHECK_EQ(st.resultSkipped_, 0);
    DOCTEST_CHECK_EQ(rec.dirtyCalls, 0);     // 早退：标脏不调
    DOCTEST_REQUIRE_EQ(rec.toastTitles.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(rec.toastTitles[0], std::string("导入失败"));
    DOCTEST_CHECK_EQ(rec.toastMsgs[0], std::string("ClaudeService 未就绪"));
    DOCTEST_CHECK(rec.toastErrors[0]);
    return true;
})

REGISTER_TEST(ccswitch_confirm_env_write_half_fail,
              "ccswitch 编排：已建但 env 写入失败 → 落失败桶、半写行如实留在库、同批重名兜底跳过", {
    CcSwitchConfirmEnv env;
    HookRecorder rec;
    // 预置激活 profile：编排在 activateIfNoneActive=true 下建行，已有激活时
    // createProfile 不做激活 UPDATE（首建自动激活才 UPDATE），故障注入就只打
    // 二段补写的 updateProfile
    auto anchorR = env.svc->createProfile(
        "Anchor", "https://anchor.example.com", kFakeToken, "", true, 0,
        "bearer", "", 0, "");
    DOCTEST_REQUIRE(anchorR.isOk());
    // 故障注入（零生产缝）：BEFORE UPDATE 触发器 RAISE(ABORT)，做 DB 级错误
    //（磁盘满/锁死一类的确定性替身）；INSERT 不受影响 → 行建得成、补写必败
    auto trig = env.db->db().exec(
        "CREATE TRIGGER ccswitch_test_block_update "
        "BEFORE UPDATE ON claude_profiles "
        "BEGIN SELECT RAISE(ABORT, 'test: update blocked'); END;");
    DOCTEST_REQUIRE(trig.isOk());

    // 同名两行成立的前提：cc-switch 的 PK 是 (id, app_type)，不同 id 允许同名。
    // 行 1 建行成功但补写失败 → 失败 + 名字进 existingNames（行已建，同批后续重名
    // 会撞它）；行 2 撞同批兜底 → 跳过「同名」而非再去撞 UNIQUE
    const std::string halfJson = settingsConfigWith(
        "https://half.example.com", kFakeToken, "", "",
        "\"ANTHROPIC_DEFAULT_SONNET_MODEL\":\"m\"", "\"theme\":\"dark\"");
    const std::vector<Candidate> candidates = {{"EnvCarrier", halfJson},
                                               {"EnvCarrier", halfJson}};
    module::claude::CcSwitchImportDialogState st =
        makeConfirmState(candidates, {});

    module::claude::confirmCcSwitchImport(st, {}, rec.hooks());

    DOCTEST_CHECK_EQ(st.resultSuccess_, 0);
    DOCTEST_CHECK_EQ(st.resultSkipped_, 1);
    DOCTEST_CHECK_EQ(st.resultFailed_, 1);
    DOCTEST_REQUIRE_EQ(st.resultLines_.size(), static_cast<std::size_t>(2));
    // 「已建，但 env 写入失败」如实进失败桶（不静默、不谎报成功）
    DOCTEST_CHECK(st.resultLines_[0].find("失败：EnvCarrier（已建，但 env 写入失败") !=
                  std::string::npos);
    // 同批重名兜底：失败分支也 push existingNames → 第二行跳过而非再失败
    DOCTEST_CHECK_EQ(st.resultLines_[1].find("跳过：EnvCarrier（"),
                     static_cast<std::size_t>(0));
    DOCTEST_CHECK(st.resultLines_[1].find("同名") != std::string::npos);
    DOCTEST_REQUIRE_EQ(rec.toastTitles.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK(rec.toastErrors[0]);
    DOCTEST_CHECK_EQ(rec.toastMsgs[0],
                     std::string("成功 0 / 跳过 1 / 失败 1（共 2 条）"));
    // DB 事后状态如实：EnvCarrier 行存在（createProfile 已 INSERT 成功）但
    // extra_env 空（补写失败）——「已建但 env 缺」的中间态可被用户看到并补救
    auto listR = env.svc->listProfiles();
    DOCTEST_REQUIRE(listR.isOk());
    const std::vector<module::claude::Profile> profiles =
        std::move(listR).value();
    DOCTEST_REQUIRE_EQ(profiles.size(),
                       static_cast<std::size_t>(2));   // Anchor + EnvCarrier
    const module::claude::Profile* half = findProfile(profiles, "EnvCarrier");
    DOCTEST_REQUIRE(half != nullptr);
    DOCTEST_CHECK_EQ(half->extraEnvJson, std::string(""));
    return true;
})