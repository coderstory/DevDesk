// tests/test_modules/test_secrets_store.cpp
// app_secrets 薄封装单测（need305）：get/set/erase 往返、protect 落库形态、
// 覆盖写刷新 updated_at、unprotect 解不开原样上抛、appDatabase 未注册失败路径。
// protect/unprotect 经 core::secrets_store 自带注入缝换内存假实现（need305
// 审计 P0 依赖倒置后缝随接口下沉 core）——不触真实保护实现，「落库形态」
// 才可断言（enc:: 前缀差异可见，明文直落会被 get 端 fail 暴露）。
// 注意：Backend 对象必须是 guard 的**成员**（构造函数局部变量在 ctor 返回即销毁，
// 注入缝槽位会拿到悬垂指针——形态同 test_credential_storage 的 ScopedFakeKc）。
#include "doctest.h"
#include "test_support.h"

#include "core/secrets_store.h"

#include <ctime>
#include <string>

namespace {

core::Result<std::string> fakeProtect(const std::string& plain) {
    return core::Result<std::string>::ok("enc::" + plain);
}
core::Result<std::string> fakeUnprotect(const std::string& stored) {
    if (stored.rfind("enc::", 0) != 0) {
        return core::Result<std::string>::fail(core::ErrorCode::InvalidApiKey,
                                               "fake: not protected form");
    }
    return core::Result<std::string>::ok(stored.substr(5));
}

struct CredentialBackendGuard {
    core::secrets_store::Backend table;   // 成员：槽位指向它，guard 活着就有效
    const core::secrets_store::Backend* prev = nullptr;
    CredentialBackendGuard() {
        table = core::secrets_store::Backend{&fakeProtect, &fakeUnprotect};
        prev = core::secrets_store::setBackendForTest(&table);
    }
    ~CredentialBackendGuard() { core::secrets_store::setBackendForTest(prev); }
    CredentialBackendGuard(const CredentialBackendGuard&) = delete;
    CredentialBackendGuard& operator=(const CredentialBackendGuard&) = delete;
};

} // namespace

REGISTER_TEST(secrets_store_roundtrip_form_and_timestamp,
              "secrets_store: set/get 往返 + protect 落库形态 + 覆盖写刷新 updated_at", {
    test_support::TestDb tdb("devdesk_test_secrets_", "store");
    CredentialBackendGuard cred;
    auto& db = tdb.handle();

    // get：无行 → NotFound（与「有但解不开」必须可分辨）
    auto missing = core::secrets_store::get(db, "svc.a", "acct");
    DOCTEST_CHECK(missing.isFail());
    DOCTEST_CHECK(missing.errorCode() == core::ErrorCode::NotFound);

    // set → get 往返；落库形态是 protect 产物（enc:: 前缀），不是明文
    DOCTEST_CHECK(core::secrets_store::set(db, "svc.a", "acct", "plain-1").isOk());
    auto got = core::secrets_store::get(db, "svc.a", "acct");
    DOCTEST_REQUIRE(got.isOk());
    DOCTEST_CHECK_EQ(got.value(), std::string("plain-1"));
    auto prep = db.prepare(
        "SELECT value, updated_at FROM app_secrets WHERE service = 'svc.a'");
    DOCTEST_REQUIRE(prep.isOk());
    auto rows = prep.value().fetchAll();
    DOCTEST_REQUIRE(rows.isOk());
    DOCTEST_REQUIRE_EQ(rows.value().size(), 1u);
    DOCTEST_CHECK_EQ(rows.value()[0].getString("value").value(),
                     std::string("enc::plain-1"));

    // 覆盖写：值刷新 + updated_at 从陈旧值跳到当前（陈旧值预置为 1 小时前）
    const auto stale = static_cast<int64_t>(std::time(nullptr)) - 3600;
    DOCTEST_CHECK(db.exec(
        "INSERT OR REPLACE INTO app_secrets(service, account, value, updated_at) "
        "VALUES ('svc.ts', 'acct', 'enc::old', " + std::to_string(stale) + ")").isOk());
    DOCTEST_CHECK(core::secrets_store::set(db, "svc.ts", "acct", "new-val").isOk());
    auto prep2 = db.prepare(
        "SELECT value, updated_at FROM app_secrets WHERE service = 'svc.ts'");
    DOCTEST_REQUIRE(prep2.isOk());
    auto rows2 = prep2.value().fetchAll();
    DOCTEST_REQUIRE(rows2.isOk());
    DOCTEST_REQUIRE_EQ(rows2.value().size(), 1u);
    DOCTEST_CHECK_EQ(rows2.value()[0].getString("value").value(),
                     std::string("enc::new-val"));
    DOCTEST_CHECK(rows2.value()[0].getInt64("updated_at").value() > stale);
    return true;
})

REGISTER_TEST(secrets_store_erase_and_unprotect_propagation,
              "secrets_store: erase 幂等 + unprotect 解不开原样上抛", {
    test_support::TestDb tdb("devdesk_test_secrets_", "erase");
    CredentialBackendGuard cred;
    auto& db = tdb.handle();

    // erase：幂等（条目不存在也算成功）
    DOCTEST_CHECK(core::secrets_store::set(db, "svc.a", "acct", "plain-1").isOk());
    DOCTEST_CHECK(core::secrets_store::erase(db, "svc.a", "acct").isOk());
    DOCTEST_CHECK(core::secrets_store::erase(db, "svc.a", "acct").isOk());
    DOCTEST_CHECK(core::secrets_store::get(db, "svc.a", "acct").isFail());

    // unprotect 解不开 → 原样上抛不静默回落：手工塞一行非保护形态
    DOCTEST_CHECK(db.exec(
        "INSERT OR REPLACE INTO app_secrets(service, account, value, updated_at) "
        "VALUES ('svc.b', 'acct', 'raw-plain', 0)").isOk());
    auto broken = core::secrets_store::get(db, "svc.b", "acct");
    DOCTEST_CHECK(broken.isFail());
    DOCTEST_CHECK(broken.errorCode() == core::ErrorCode::InvalidApiKey);
    return true;
})

REGISTER_TEST(secrets_store_unregistered_app_database,
              "secrets_store: appDatabase 未注册 → fail（不崩、不静默成功）", {
    // 不挂 ScopedAppDatabase：test_main 启动时已把全局复位为 nullptr
    auto g = core::secrets_store::get("svc.x", "acct");
    DOCTEST_CHECK(g.isFail());
    auto s = core::secrets_store::set("svc.x", "acct", "v");
    DOCTEST_CHECK(s.isFail());
    auto e = core::secrets_store::erase("svc.x", "acct");
    DOCTEST_CHECK(e.isFail());
    return true;
})
