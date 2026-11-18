// tests/test_modules/test_credential_storage.cpp
// 凭据落库回归网：api_key_encrypted 列往返一致 + 导出面不泄露凭据形态。
// 跨文件前提：
//   - 凭据的唯一源是 claude_profiles.api_key_encrypted 列（need305），
//     整条创建/更新路径只动这一列。
#include "doctest.h"
#include "test_support.h"

#include "claude_service.h"
#include "claude_ui_logic.h"
#include "core/database.h"
#include "core/sqlite_db.h"
#include "platform/credential_protection.h"
#include "platform/json.h"

#include <string>

namespace {

constexpr const char* kCredPrefix = "devdesk_test_cred_";

} // namespace

REGISTER_TEST(credential_stored_in_db_column_roundtrip,
              "credential: 凭据落在 api_key_encrypted 列（往返一致）+ 全量 SHA256", {
    test_support::TestDb tdb(kCredPrefix, "roundtrip");
    module::claude::ClaudeService svc(tdb.handle());

    const std::string key = "sk-ant-roundtrip-0123456789abcdef";
    auto created = svc.createProfile("往返", "https://r.example.com", key, "m");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;

    auto back = svc.loadApiKey(id);
    DOCTEST_REQUIRE(back.isOk());
    DOCTEST_CHECK_EQ(back.value(), key);

    // 列里存的即明文本身（全平台口径，保密靠文件权限 / NTFS ACL）
    auto prep = tdb.handle().prepare(
        "SELECT api_key_encrypted, api_key_hint, api_key_check FROM "
        "claude_profiles WHERE id = ?");
    DOCTEST_REQUIRE(prep.isOk());
    auto& stmt = prep.value();
    DOCTEST_REQUIRE(stmt.bind(1, id).isOk());
    auto step = stmt.step();
    DOCTEST_REQUIRE(step.isOk());
    DOCTEST_REQUIRE(step.value());
    auto row = stmt.currentRow();
    DOCTEST_REQUIRE(row.isOk());
    const std::string stored =
        row.value().getString("api_key_encrypted").value();
    const std::string hint = row.value().getString("api_key_hint").value();
    const std::string check = row.value().getString("api_key_check").value();

    // 全平台明文口径：protect 是确定性透传，列里就是明文本身（stored == protect(key)）。
    // 往返一致性由上方 loadApiKey(id)==key 证明。
    DOCTEST_CHECK_EQ(stored, platform::credential::protect(key).value());
    // hint 脱敏（前 6 + *** + 尾 4），check 全量 64 hex
    DOCTEST_CHECK(hint.find("***") != std::string::npos);
    DOCTEST_CHECK(hint.find(key) == std::string::npos);
    DOCTEST_CHECK_EQ(check.size(), static_cast<size_t>(64));
    return true;
})

REGISTER_TEST(credential_export_json_contains_no_secret,
              "credential: 导出 JSON 不含凭据（明文 / 摘要 / 存储形态一律不得出现）", {
    test_support::TestDb tdb(kCredPrefix, "export");
    module::claude::ClaudeService svc(tdb.handle());

    const std::string key = "sk-ant-EXPORT-MUST-NOT-LEAK-0123456789";
    auto created = svc.createProfile("导出源", "https://e.example.com", key, "m");
    DOCTEST_REQUIRE(created.isOk());
    const module::claude::Profile& p = created.value();

    // 生产口径：keyPlaceholder 只传 hint（脱敏形态）
    const std::string json =
        module::claude::buildProfileExportJson(p,
                                               /*keyPlaceholder=*/p.apiKeyHint);
    DOCTEST_REQUIRE(!json.empty());

    DOCTEST_CHECK(json.find(key) == std::string::npos);
    // 摘要（api_key_check）与存储形态（api_key_encrypted）同样不得出现——前者是密钥
    // 可爆破的判据，后者在加密平台是密文、在明文平台就是明文
    DOCTEST_CHECK(json.find(p.apiKeyCheck) == std::string::npos);
    const std::string stored =
        platform::credential::protect(key).value();
    if (!stored.empty() && !p.apiKeyHint.empty()) {
        DOCTEST_CHECK(json.find(p.apiKeyHint) != std::string::npos);  // 占位符真在
        DOCTEST_CHECK(json.find(stored) == std::string::npos);
    }

    // 反向对照：把明文当占位值传进去，导出里就出现明文。这条钉住上面不是恒真的空断言，
    // 同时标出生产路径绝不能这么传 keyPlaceholder
    const std::string leaky =
        module::claude::buildProfileExportJson(p, key);
    DOCTEST_CHECK(leaky.find(key) != std::string::npos);

    // 顶层形状只有 {"env": {...}}；坏 extra_env 整体忽略但托管键照写，
    // 凭据仍是占位（坏输入不能把明文/摘要带进导出）
    module::claude::Profile bad = p;
    bad.extraEnvJson = "{not-json";
    const std::string badJson =
        module::claude::buildProfileExportJson(bad, p.apiKeyHint);
    auto parsed = platform::json::parse(badJson);
    DOCTEST_REQUIRE(parsed.isOk());
    DOCTEST_REQUIRE(parsed.value().isObject());
    auto root = parsed.value().asObject();
    DOCTEST_CHECK_EQ(root.keys().size(), static_cast<size_t>(1));
    DOCTEST_CHECK(root.has("env"));
    auto envObj = root.get("env").asObject();
    DOCTEST_CHECK(envObj.has("ANTHROPIC_BASE_URL"));
    DOCTEST_CHECK(envObj.has("ANTHROPIC_AUTH_TOKEN"));
    DOCTEST_CHECK_EQ(envObj.get("ANTHROPIC_AUTH_TOKEN").asString(),
                     p.apiKeyHint);
    DOCTEST_CHECK(badJson.find(key) == std::string::npos);
    return true;
})
