// tests/test_modules/test_credential_storage.cpp
// 凭据落库回归网：api_key_encrypted 列往返一致 + 旧 Keychain 残留清理的白名单与空库护栏。
// 跨文件前提：
//   - 凭据的唯一源是 claude_profiles.api_key_encrypted 列，Keychain 已不是凭据源，
//     整条创建/更新路径不得产生任何 Keychain 条目。
//   - cleanupOrphanKeychainEntries 只在「该列可读且至少一行存过密钥」时才删旧条目，
//     且只删 claude: 前缀。两种「空」都要跳过：列不可读（旧库）、列可读但无一行存过密钥。
//   - 全程装内存假 Keychain 后端，不碰真实系统钥匙串。
#include "doctest.h"
#include "test_support.h"

#include "claude_service.h"
#include "claude_ui_logic.h"
#include "core/database.h"
#include "core/sqlite_db.h"
#include "platform/credential_protection.h"
#include "platform/keychain.h"

#include <map>
#include <string>
#include <vector>

namespace {

constexpr const char* kCredPrefix = "devdesk_test_cred_";
constexpr const char* kSvc = "DevDeskTestCredentialStorage";

// 内存假 Keychain 后端（结构自足，不依赖 test_claude_service 的那份）
struct FakeKc {
    std::map<std::string, std::string> data;
    static std::string key(const std::string& service, const std::string& account) {
        return service + '\x1f' + account;
    }
    void put(const std::string& s, const std::string& a, const std::string& v) {
        data[key(s, a)] = v;
    }
    bool has(const std::string& s, const std::string& a) const {
        return data.count(key(s, a)) != 0;
    }
    size_t size() const { return data.size(); }
};

FakeKc* g_kc = nullptr;

core::Result<void> kcStore(const std::string& s, const std::string& a,
                           const std::string& v) {
    g_kc->put(s, a, v);
    return core::Result<void>::ok();
}
core::Result<std::string> kcLoad(const std::string& s, const std::string& a) {
    auto it = g_kc->data.find(FakeKc::key(s, a));
    if (it == g_kc->data.end()) {
        return core::Result<std::string>::fail(core::ErrorCode::NotFound,
                                               "Keychain item not found");
    }
    return core::Result<std::string>::ok(it->second);
}
core::Result<void> kcDelete(const std::string& s, const std::string& a) {
    g_kc->data.erase(FakeKc::key(s, a));
    return core::Result<void>::ok();
}
core::Result<std::vector<std::string>> kcDeleteNotIn(
    const std::string& service, const std::vector<std::string>& keepAccounts) {
    std::vector<std::string> removed;
    const std::string prefix = service + '\x1f';
    for (auto it = g_kc->data.begin(); it != g_kc->data.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            const std::string account = it->first.substr(prefix.size());
            bool keep = false;
            for (const auto& k : keepAccounts) {
                if (k == account) { keep = true; break; }
            }
            // claude: 之外的 account 一律保留、不计数（deletableOrphanAccount 三平台统一判据）
            if (!keep && platform::keychain::deletableOrphanAccount(account)) {
                removed.push_back(account);
                it = g_kc->data.erase(it);
                continue;
            }
        }
        ++it;
    }
    return core::Result<std::vector<std::string>>::ok(std::move(removed));
}

struct ScopedFakeKc {
    FakeKc fake;
    platform::keychain::Backend table;
    const platform::keychain::Backend* prev;
    ScopedFakeKc() {
        g_kc = &fake;
        table = platform::keychain::Backend{&kcStore, &kcLoad, &kcDelete,
                                            &kcDeleteNotIn};
        prev = platform::keychain::setBackendForTest(&table);
    }
    ~ScopedFakeKc() {
        platform::keychain::setBackendForTest(prev);
        g_kc = nullptr;
    }
    ScopedFakeKc(const ScopedFakeKc&) = delete;
    ScopedFakeKc& operator=(const ScopedFakeKc&) = delete;
};

} // namespace

REGISTER_TEST(credential_stored_in_db_column_roundtrip,
              "credential: 凭据落在 api_key_encrypted 列（往返一致）+ 全量 SHA256", {
    test_support::TestDb tdb(kCredPrefix, "roundtrip");
    module::claude::ClaudeService svc(tdb.handle(), kSvc);

    const std::string key = "sk-ant-roundtrip-0123456789abcdef";
    auto created = svc.createProfile("往返", "https://r.example.com", key, "m");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;

    auto back = svc.loadApiKey(id);
    DOCTEST_REQUIRE(back.isOk());
    DOCTEST_CHECK_EQ(back.value(), key);

    // 列里确实是**保护后**的形态（本平台契约：明文平台 = 明文；加密平台 = 密文）
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

    // 明文平台（macOS/POSIX）：protect 是确定性透传，stored == protect(key) 成立，可以比密文。
    // 加密平台（Windows DPAPI）每次 CryptProtectData 含随机 salt/IV，同一 key 两次 protect 产不同
    // 密文，比相等无意义——改验明文不出现在列里。往返一致性由上方 loadApiKey(id)==key 证明。
    if (platform::credential::storesPlaintext()) {
        DOCTEST_CHECK_EQ(stored, platform::credential::protect(key).value());
    }
    if (!platform::credential::storesPlaintext()) {
        DOCTEST_CHECK(stored.find(key) == std::string::npos);   // 加密平台不得明文
    }
    // hint 脱敏（前 6 + *** + 尾 4），check 全量 64 hex
    DOCTEST_CHECK(hint.find("***") != std::string::npos);
    DOCTEST_CHECK(hint.find(key) == std::string::npos);
    DOCTEST_CHECK_EQ(check.size(), static_cast<size_t>(64));
    return true;
})

REGISTER_TEST(credential_legacy_keychain_not_read_nor_written,
              "credential: 旧 Keychain 既不读也不写（迁库后 Keychain 非凭据源）", {
    test_support::TestDb tdb(kCredPrefix, "legacy");
    ScopedFakeKc kc;
    module::claude::ClaudeService svc(tdb.handle(), kSvc);

    const std::string dbKey = "sk-ant-db-authoritative-01";
    auto created = svc.createProfile("甲", "https://a.example.com", dbKey, "m");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;

    DOCTEST_CHECK_EQ(kc.fake.size(), static_cast<size_t>(0));

    // 旧 account 约定是 claude:<id>。往里塞一个**不同的值**：若实现仍读 Keychain，
    // loadApiKey 会返回这个陈旧值而不是库里那个
    kc.fake.put(kSvc, "claude:" + std::to_string(id), "sk-ant-STALE-keychain-99");
    auto key = svc.loadApiKey(id);
    DOCTEST_REQUIRE(key.isOk());
    DOCTEST_CHECK_EQ(key.value(), dbKey);

    // 更新密钥同样只动库：Keychain 里仍只有那条陈旧值，条目数不增
    module::claude::Profile p = created.value();
    p.apiKeyPlain = "sk-ant-db-authoritative-02";
    DOCTEST_REQUIRE(svc.updateProfile(p).isOk());
    DOCTEST_CHECK_EQ(kc.fake.size(), static_cast<size_t>(1));
    auto changed = svc.loadApiKey(id);
    DOCTEST_REQUIRE(changed.isOk());
    DOCTEST_CHECK_EQ(changed.value(), std::string("sk-ant-db-authoritative-02"));
    return true;
})

REGISTER_TEST(credential_residue_cleanup_guard_no_profiles_deletes_nothing,
              "credential: 数据根无 profile 时旧 Keychain 清理一个都不删（防误删）", {
    test_support::TestDb tdb(kCredPrefix, "guard");   // 空库：零 profile
    ScopedFakeKc kc;
    module::claude::ClaudeService svc(tdb.handle(), kSvc);

    kc.fake.put(kSvc, "claude:1", "user-real-key-1");
    kc.fake.put(kSvc, "claude:2", "user-real-key-2");
    kc.fake.put(kSvc, "notes:theme", "someone-elses-entry");
    const size_t before = kc.fake.size();

    auto r = svc.cleanupOrphanKeychainEntries();
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value(), 0);
    DOCTEST_CHECK_EQ(kc.fake.size(), before);          // 一个都没删
    DOCTEST_CHECK(kc.fake.has(kSvc, "claude:1"));
    DOCTEST_CHECK(kc.fake.has(kSvc, "claude:2"));
    DOCTEST_CHECK(kc.fake.has(kSvc, "notes:theme"));
    return true;
})

// 旧形态库（无 api_key_encrypted 列）绝不能放行清理：只看「有没有 profile」会误判——
// 旧库同样有 claude_profiles 行，而 listProfiles 的 SELECT 不引用新列，在旧库上照样
// 成功返回行 → keep 传空 → 把用户 Keychain 里真实可用的 API Key 全清空。
REGISTER_TEST(credential_residue_cleanup_guard_unmigrated_legacy_db,
              "credential: 旧 schema 库（无 api_key_encrypted 列）不清理任何 Keychain 条目", {
    test_support::TestDb tdb(kCredPrefix, "legacy_db");
    ScopedFakeKc kc;

    // 建成旧形态表。列清单不可照抄当前 schema——补上 api_key_encrypted 就把护栏测没了。
    DOCTEST_REQUIRE(tdb.handle().exec("DROP TABLE claude_profiles").isOk());
    DOCTEST_REQUIRE(tdb.handle().exec(
        "CREATE TABLE claude_profiles ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " name TEXT UNIQUE NOT NULL, base_url TEXT NOT NULL,"
        " api_key_hint TEXT NOT NULL, api_key_check TEXT NOT NULL,"
        " keychain_account TEXT NOT NULL, model TEXT NOT NULL,"
        " is_active INTEGER NOT NULL DEFAULT 0,"
        " created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL,"
        " models_config TEXT NOT NULL DEFAULT '',"
        " extra_env TEXT NOT NULL DEFAULT '',"
        " shared_config_id INTEGER NOT NULL DEFAULT 0,"
        " auth_field TEXT NOT NULL DEFAULT 'both',"
        " dialect_override TEXT NOT NULL DEFAULT '',"
        " fallback_profile_id INTEGER NOT NULL DEFAULT 0,"
        " extra_settings_json TEXT NOT NULL DEFAULT '')")
                        .isOk());
    DOCTEST_REQUIRE(tdb.handle().exec(
        "INSERT INTO claude_profiles (name, base_url, api_key_hint, "
        "api_key_check, keychain_account, model, created_at, updated_at) VALUES "
        "('老库行', 'https://old.example.com', 'sk-ant-***abcd', 'deadbeef', "
        "'claude:1', 'm', 0, 0)")
                        .isOk());
    kc.fake.put(kSvc, "claude:1", "user-real-key-1");
    kc.fake.put(kSvc, "claude:2", "user-real-key-2");

    module::claude::ClaudeService svc(tdb.handle(), kSvc);

    // 前置证据：listProfiles 在旧库上照样成功返回行（这正是「只数 profile」会放行清理的原因）
    auto list = svc.listProfiles();
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_CHECK_EQ(static_cast<int>(list.value().size()), 1);

    auto r = svc.cleanupOrphanKeychainEntries();
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value(), 0);
    DOCTEST_CHECK(kc.fake.has(kSvc, "claude:1"));   // 一个都不许删
    DOCTEST_CHECK(kc.fake.has(kSvc, "claude:2"));
    DOCTEST_CHECK_EQ(kc.fake.size(), static_cast<size_t>(2));
    return true;
})

// 空 profile 护栏的另一半：两种「空」都跳过清理——列不可读（旧库，见上）、列可读但
// 没有一行存过密钥。后者是 fail-safe：那种状态下「Keychain 里只剩残留」是个赌注，
// 口径是宁可不清理也不能误删。一旦有一行真存着凭据 → 清理恢复执行（此时 Keychain
// 必是旧副本）。
REGISTER_TEST(credential_residue_cleanup_requires_at_least_one_stored_key,
              "credential: 全部 profile 都没存过密钥时不清理；有一行存了才清理", {
    test_support::TestDb tdb(kCredPrefix, "nokeys");
    ScopedFakeKc kc;
    module::claude::ClaudeService svc(tdb.handle(), kSvc);

    auto created = svc.createProfile("甲", "https://a.example.com",
                                     "sk-ant-holder-01", "m");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;
    // 清空凭据列 = 「用户从没配过 key」的形态：新形态库、列可读、值为空
    DOCTEST_REQUIRE(test_support::clearStoredCredential(tdb.handle(), id));

    kc.fake.put(kSvc, "claude:1", "user-real-key-1");
    kc.fake.put(kSvc, "notes:theme", "someone-elses-entry");

    auto skipped = svc.cleanupOrphanKeychainEntries();
    DOCTEST_REQUIRE(skipped.isOk());
    DOCTEST_CHECK_EQ(skipped.value(), 0);
    DOCTEST_CHECK(kc.fake.has(kSvc, "claude:1"));   // 保住

    // 补上密钥（同一行 → 库里的凭据体系成立）→ 残留清理恢复执行
    module::claude::Profile p = created.value();
    p.apiKeyPlain = "sk-ant-holder-02";
    DOCTEST_REQUIRE(svc.updateProfile(p).isOk());
    auto swept = svc.cleanupOrphanKeychainEntries();
    DOCTEST_REQUIRE(swept.isOk());
    DOCTEST_CHECK_EQ(swept.value(), 1);
    DOCTEST_CHECK(!kc.fake.has(kSvc, "claude:1"));
    DOCTEST_CHECK(kc.fake.has(kSvc, "notes:theme"));   // 前缀外仍保留
    return true;
})

REGISTER_TEST(credential_residue_cleanup_sweeps_only_claude_prefix,
              "credential: 有 profile 时清 claude:* 残留，前缀外的条目保留", {
    test_support::TestDb tdb(kCredPrefix, "sweep");
    ScopedFakeKc kc;
    module::claude::ClaudeService svc(tdb.handle(), kSvc);

    auto created = svc.createProfile("甲", "https://a.example.com",
                                     "sk-ant-sweep-key-01", "m");
    DOCTEST_REQUIRE(created.isOk());
    kc.fake.put(kSvc, "claude:1", "leftover-1");
    kc.fake.put(kSvc, "claude:2", "leftover-2");
    kc.fake.put(kSvc, "notes:theme", "someone-elses-entry");
    kc.fake.put(std::string(kSvc) + "-other", "claude:9", "different-namespace");

    auto r = svc.cleanupOrphanKeychainEntries();
    DOCTEST_REQUIRE(r.isOk());
    // 返回值只统计被删掉的 claude: 前缀条目数
    DOCTEST_CHECK_EQ(r.value(), 2);                    // 只数 claude: 前缀
    DOCTEST_CHECK(!kc.fake.has(kSvc, "claude:1"));
    DOCTEST_CHECK(!kc.fake.has(kSvc, "claude:2"));
    DOCTEST_CHECK(kc.fake.has(kSvc, "notes:theme"));
    // 别的 service 名下的同 account 名不受影响
    DOCTEST_CHECK(kc.fake.has(std::string(kSvc) + "-other", "claude:9"));
    // 清理不碰库里的凭据
    auto key = svc.loadApiKey(created.value().id);
    DOCTEST_REQUIRE(key.isOk());
    DOCTEST_CHECK_EQ(key.value(), std::string("sk-ant-sweep-key-01"));
    return true;
})

REGISTER_TEST(credential_export_json_contains_no_secret,
              "credential: 导出 JSON 不含凭据（明文 / 摘要 / 存储形态一律不得出现）", {
    test_support::TestDb tdb(kCredPrefix, "export");
    module::claude::ClaudeService svc(tdb.handle(), kSvc);

    const std::string key = "sk-ant-EXPORT-MUST-NOT-LEAK-0123456789";
    auto created = svc.createProfile("导出源", "https://e.example.com", key, "m");
    DOCTEST_REQUIRE(created.isOk());
    const module::claude::Profile& p = created.value();

    // 生产口径：keyPlaceholder 只传 hint（脱敏形态）
    const std::string json =
        module::claude::buildProfileExportJson(p, std::string(),
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
        module::claude::buildProfileExportJson(p, std::string(), key);
    DOCTEST_CHECK(leaky.find(key) != std::string::npos);
    return true;
})
