// tests/test_modules/test_credential_storage.cpp
// E-1/E-2 凭据存储迁移的回归网（2026-10-02 存储统一设计 §4.2 / §6.3 / §6.4）：
//   1) 凭据真的在库里（列往返一致），且**旧 Keychain 不再被读 / 不再被写**
//   2) 旧 Keychain 残留清理的白名单语义 + **空 profile 护栏**（防误删，P0 事故回归网）
//   3) 导出 / 复制配置继续脱敏（剪贴板是全局可读出口，硬约束不许回退）
//
// 全程用内存假 Keychain 后端（不碰真实系统钥匙串，macOS 上照跑）。
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

// ---------- 内存假 Keychain（形态同 test_claude_service 的 fake，可独立跑）----------
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
            // 白名单前缀（keychain.h shouldDeleteOrphan 的三平台统一语义）：
            // claude: 之外的一律保留、不计数
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

// ---------- 1) 凭据在库里：列往返一致 ----------

REGISTER_TEST(credential_stored_in_db_column_roundtrip,
              "credential: 凭据落在 api_key_encrypted 列（往返一致）+ 全量 SHA256", {
    test_support::TestDb tdb(kCredPrefix, "roundtrip");
    module::claude::ClaudeService svc(tdb.handle(), kSvc);

    const std::string key = "sk-ant-roundtrip-0123456789abcdef";
    auto created = svc.createProfile("往返", "https://r.example.com", key, "m");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;

    // 读回一致（protect/unprotect 往返）
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

    DOCTEST_CHECK_EQ(stored, platform::credential::protect(key).value());
    if (!platform::credential::storesPlaintext()) {
        DOCTEST_CHECK(stored.find(key) == std::string::npos);   // 加密平台不得明文
    }
    // hint 脱敏（前 6 + *** + 尾 4），check 全量 64 hex
    DOCTEST_CHECK(hint.find("***") != std::string::npos);
    DOCTEST_CHECK(hint.find(key) == std::string::npos);
    DOCTEST_CHECK_EQ(check.size(), static_cast<size_t>(64));
    return true;
})

// ---------- 2) 旧 Keychain 不再被读 / 不再被写 ----------

REGISTER_TEST(credential_legacy_keychain_not_read_nor_written,
              "credential: 旧 Keychain 既不读也不写（迁库后 Keychain 非凭据源）", {
    test_support::TestDb tdb(kCredPrefix, "legacy");
    ScopedFakeKc kc;
    module::claude::ClaudeService svc(tdb.handle(), kSvc);

    const std::string dbKey = "sk-ant-db-authoritative-01";
    auto created = svc.createProfile("甲", "https://a.example.com", dbKey, "m");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;

    // 不写：整条创建路径一个 Keychain 条目都不该产生
    DOCTEST_CHECK_EQ(kc.fake.size(), static_cast<size_t>(0));

    // 不读：往旧 account 约定（claude:<id>）里塞一个**不同的值**——若实现仍读
    // Keychain，loadApiKey 会返回这个陈旧值
    kc.fake.put(kSvc, "claude:" + std::to_string(id), "sk-ant-STALE-keychain-99");
    auto key = svc.loadApiKey(id);
    DOCTEST_REQUIRE(key.isOk());
    DOCTEST_CHECK_EQ(key.value(), dbKey);

    // 更新密钥同样只动库，不碰 Keychain（旧值仍是那枚陈旧值）
    module::claude::Profile p = created.value();
    p.apiKeyPlain = "sk-ant-db-authoritative-02";
    DOCTEST_REQUIRE(svc.updateProfile(p).isOk());
    DOCTEST_CHECK_EQ(kc.fake.size(), static_cast<size_t>(1));
    auto changed = svc.loadApiKey(id);
    DOCTEST_REQUIRE(changed.isOk());
    DOCTEST_CHECK_EQ(changed.value(), std::string("sk-ant-db-authoritative-02"));
    return true;
})

// ---------- 3) 残留清理：空 profile 护栏（P0 误删事故回归网，最高优先级）----------

REGISTER_TEST(credential_residue_cleanup_guard_no_profiles_deletes_nothing,
              "credential: 数据根无 profile 时旧 Keychain 清理一个都不删（防误删）", {
    test_support::TestDb tdb(kCredPrefix, "guard");   // 空库：零 profile
    ScopedFakeKc kc;
    module::claude::ClaudeService svc(tdb.handle(), kSvc);

    // 预置「用户真实密钥」形态的条目 + 一条非 claude 前缀的他人条目
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
    DOCTEST_CHECK_EQ(r.value(), 2);                    // 只数 claude: 前缀
    DOCTEST_CHECK(!kc.fake.has(kSvc, "claude:1"));
    DOCTEST_CHECK(!kc.fake.has(kSvc, "claude:2"));
    // 前缀外保留（不删、不计数）
    DOCTEST_CHECK(kc.fake.has(kSvc, "notes:theme"));
    // 命名空间外的不受影响
    DOCTEST_CHECK(kc.fake.has(std::string(kSvc) + "-other", "claude:9"));
    // 库里的凭据不受清理影响
    auto key = svc.loadApiKey(created.value().id);
    DOCTEST_REQUIRE(key.isOk());
    DOCTEST_CHECK_EQ(key.value(), std::string("sk-ant-sweep-key-01"));
    return true;
})

// ---------- 4) 导出 / 复制配置继续脱敏（§6.4 硬约束 2）----------

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

    // 剪贴板是全局可读出口：明文绝不得出现
    DOCTEST_CHECK(json.find(key) == std::string::npos);
    // 摘要（api_key_check）与存储形态（api_key_encrypted）也不得出现——
    // 前者是密钥可爆破的判据、后者在加密平台是密文、在明文平台就是明文
    DOCTEST_CHECK(json.find(p.apiKeyCheck) == std::string::npos);
    const std::string stored =
        platform::credential::protect(key).value();
    if (!stored.empty() && !p.apiKeyHint.empty()) {
        DOCTEST_CHECK(json.find(p.apiKeyHint) != std::string::npos);  // 占位符真在
        DOCTEST_CHECK(json.find(stored) == std::string::npos);
    }

    // 反向：把明文当占位值传进去，导出里就会出现明文——这正是生产路径**不能**
    // 做的事（本断言证明上面那条不是恒真的空断言）
    const std::string leaky =
        module::claude::buildProfileExportJson(p, std::string(), key);
    DOCTEST_CHECK(leaky.find(key) != std::string::npos);
    return true;
})
