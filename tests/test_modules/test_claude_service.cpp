// tests/test_modules/test_claude_service.cpp
// ClaudeService 单元测试：Profile CRUD + Keychain + 激活 + 连通性
//
// 测试环境隔离：
//   - 数据库用临时文件（std::filesystem::temp_directory_path）
//   - Keychain service 用 "DevDeskTestClaudeService"，不污染生产数据
#include "doctest.h"

#include "claude_service.h"
#include "core/database.h"
#include "core/paths.h"
#include "core/sqlite_db.h"
#include "platform/http_server.h"
#include "platform/keychain.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr const char* kTestKeychainService = "DevDeskTestClaudeService";

// 每个测试用一个独立的临时数据库文件
std::filesystem::path tempDbPath(const std::string& tag) {
    static int counter = 0;
    ++counter;
    auto name = "devdesk_test_claude_" + tag + "_" +
                std::to_string(counter) + ".db";
    return std::filesystem::temp_directory_path() / name;
}

// RAII：创建已 migrate 的临时数据库
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

// 在测试用 Keychain service 下清理某个 profile 的密钥
void cleanupKey(int64_t profileId) {
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(profileId));
}

} // namespace

// ---- 1. createProfile + getProfile 往返 ----

REGISTER_TEST(claude_service_create_get_profile,
              "claude_service: createProfile + getProfile 往返", {
    TestDb tdb("create_get");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile("官方", "https://api.anthropic.com",
                                     "sk-ant-test-0123456789abcdef",
                                     "claude-sonnet-4-5");
    DOCTEST_REQUIRE(created.isOk());
    const auto& p = created.value();
    DOCTEST_CHECK(p.id > 0);
    DOCTEST_CHECK_EQ(p.name, std::string("官方"));
    DOCTEST_CHECK_EQ(p.baseUrl, std::string("https://api.anthropic.com"));
    DOCTEST_CHECK_EQ(p.model, std::string("claude-sonnet-4-5"));
    DOCTEST_CHECK_FALSE(p.isActive);
    // API key 明文绝不能出现在返回的 Profile 中
    DOCTEST_CHECK(p.apiKeyPlain.empty());
    // hint 应脱敏（前 6 + *** + 后 4）
    DOCTEST_CHECK(p.apiKeyHint.find("***") != std::string::npos);
    DOCTEST_CHECK(p.apiKeyCheck.size() == 8);
    DOCTEST_CHECK_EQ(p.keychainAccount, std::string("claude:") +
                                        std::to_string(p.id));

    // getProfile 应返回除 apiKeyPlain 外全部字段
    auto got = svc.getProfile(p.id);
    DOCTEST_REQUIRE(got.isOk());
    DOCTEST_CHECK_EQ(got.value().name, p.name);
    DOCTEST_CHECK_EQ(got.value().baseUrl, p.baseUrl);
    DOCTEST_CHECK_EQ(got.value().model, p.model);
    DOCTEST_CHECK(got.value().apiKeyPlain.empty());

    // 不存在的 id → ProfileNotFound
    auto missing = svc.getProfile(99999);
    DOCTEST_CHECK(missing.isFail());
    DOCTEST_CHECK(missing.errorCode() == core::ErrorCode::ProfileNotFound);

    cleanupKey(p.id);
    return true;
})

// ---- 2. listProfiles ----

REGISTER_TEST(claude_service_list_profiles,
              "claude_service: listProfiles 返回全部 profile", {
    TestDb tdb("list");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto a = svc.createProfile("A", "https://a.example.com",
                               "sk-ant-aaaa-1111", "claude-sonnet-4-5");
    auto b = svc.createProfile("B", "https://b.example.com",
                               "sk-ant-bbbb-2222", "claude-opus-4-5");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(b.isOk());

    auto list = svc.listProfiles();
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_CHECK_EQ(list.value().size(), 2u);
    // 不应有任何 apiKeyPlain
    for (const auto& p : list.value()) {
        DOCTEST_CHECK(p.apiKeyPlain.empty());
    }

    cleanupKey(a.value().id);
    cleanupKey(b.value().id);
    return true;
})

// ---- 3. setActiveProfile + getActiveProfile ----

REGISTER_TEST(claude_service_set_active_profile,
              "claude_service: setActiveProfile 切换激活（唯一）", {
    TestDb tdb("active");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto a = svc.createProfile("Active-A", "https://a.example.com",
                               "sk-ant-aaaa-1111", "claude-sonnet-4-5");
    auto b = svc.createProfile("Active-B", "https://b.example.com",
                               "sk-ant-bbbb-2222", "claude-opus-4-5");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(b.isOk());

    // 初始无激活 → NoActiveProfile
    auto none = svc.getActiveProfile();
    DOCTEST_CHECK(none.isFail());
    DOCTEST_CHECK(none.errorCode() == core::ErrorCode::NoActiveProfile);

    // 激活 A
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());
    auto actA = svc.getActiveProfile();
    DOCTEST_REQUIRE(actA.isOk());
    DOCTEST_CHECK_EQ(actA.value().id, a.value().id);
    DOCTEST_CHECK_EQ(actA.value().name, std::string("Active-A"));

    // 切换到 B：A 应自动取消激活（is_active 唯一索引保证）
    DOCTEST_REQUIRE(svc.setActiveProfile(b.value().id).isOk());
    auto actB = svc.getActiveProfile();
    DOCTEST_REQUIRE(actB.isOk());
    DOCTEST_CHECK_EQ(actB.value().id, b.value().id);
    DOCTEST_CHECK_EQ(actB.value().name, std::string("Active-B"));

    // 全部 profile 中只有 1 个 is_active
    auto list = svc.listProfiles();
    DOCTEST_REQUIRE(list.isOk());
    int activeCount = 0;
    for (const auto& p : list.value()) {
        if (p.isActive) ++activeCount;
    }
    DOCTEST_CHECK_EQ(activeCount, 1);

    // 再次激活 A，回到 A
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());
    auto actA2 = svc.getActiveProfile();
    DOCTEST_REQUIRE(actA2.isOk());
    DOCTEST_CHECK_EQ(actA2.value().id, a.value().id);

    // 不存在的 id → ProfileNotFound
    auto bad = svc.setActiveProfile(99999);
    DOCTEST_CHECK(bad.isFail());
    DOCTEST_CHECK(bad.errorCode() == core::ErrorCode::ProfileNotFound);

    cleanupKey(a.value().id);
    cleanupKey(b.value().id);
    return true;
})

// ---- 4. loadApiKey（从 Keychain 读明文） ----

REGISTER_TEST(claude_service_load_api_key,
              "claude_service: loadApiKey 从 Keychain 读明文", {
    TestDb tdb("loadkey");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    const std::string secret = "sk-ant-api03-XYZ-super-secret-key";
    auto created = svc.createProfile("KeyTest", "https://api.anthropic.com",
                                     secret, "claude-sonnet-4-5");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;

    auto key = svc.loadApiKey(id);
    DOCTEST_REQUIRE(key.isOk());
    DOCTEST_CHECK_EQ(key.value(), secret);

    // 明文 key 不落 SQLite：直接查表确认 api_key_hint / api_key_check 都不含明文
    auto prep = tdb.handle().prepare(
        "SELECT api_key_hint, api_key_check FROM claude_profiles WHERE id = ?");
    DOCTEST_REQUIRE(prep.isOk());
    prep.value().bind(1, id);
    auto rows = prep.value().fetchAll();
    DOCTEST_REQUIRE(rows.isOk());
    DOCTEST_CHECK_EQ(rows.value().size(), 1u);
    const auto hint = rows.value()[0].getString("api_key_hint").value();
    const auto check = rows.value()[0].getString("api_key_check").value();
    DOCTEST_CHECK(hint.find(secret) == std::string::npos);
    DOCTEST_CHECK(check.find(secret) == std::string::npos);
    // hint 应含末 4 位
    DOCTEST_CHECK(hint.find("key") != std::string::npos);

    cleanupKey(id);
    return true;
})

// ---- 5. updateProfile ----

REGISTER_TEST(claude_service_update_profile,
              "claude_service: updateProfile 改字段 + 可选换 key", {
    TestDb tdb("update");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile("Before", "https://old.example.com",
                                     "sk-ant-old-1111", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(created.isOk());
    module::claude::Profile p = created.value();

    // 1. 只改 name / url / model（apiKeyPlain 留空 → 不换 key）
    p.name = "After";
    p.baseUrl = "https://new.example.com";
    p.model = "claude-opus-4-5";
    auto upd1 = svc.updateProfile(p);
    DOCTEST_REQUIRE(upd1.isOk());

    auto got = svc.getProfile(p.id);
    DOCTEST_REQUIRE(got.isOk());
    DOCTEST_CHECK_EQ(got.value().name, std::string("After"));
    DOCTEST_CHECK_EQ(got.value().baseUrl, std::string("https://new.example.com"));
    DOCTEST_CHECK_EQ(got.value().model, std::string("claude-opus-4-5"));
    // key 未变
    auto key1 = svc.loadApiKey(p.id);
    DOCTEST_REQUIRE(key1.isOk());
    DOCTEST_CHECK_EQ(key1.value(), std::string("sk-ant-old-1111"));

    // 2. 换 key（apiKeyPlain 非空）
    module::claude::Profile p2 = got.value();
    p2.apiKeyPlain = "sk-ant-new-2222";
    auto upd2 = svc.updateProfile(p2);
    DOCTEST_REQUIRE(upd2.isOk());

    auto key2 = svc.loadApiKey(p.id);
    DOCTEST_REQUIRE(key2.isOk());
    DOCTEST_CHECK_EQ(key2.value(), std::string("sk-ant-new-2222"));

    // api_key_check 应随 key 变化
    auto got2 = svc.getProfile(p.id);
    DOCTEST_REQUIRE(got2.isOk());
    DOCTEST_CHECK(got2.value().apiKeyCheck != p.apiKeyCheck);

    cleanupKey(p.id);
    return true;
})

// ---- 6. deleteProfile 清理 SQLite + Keychain ----

REGISTER_TEST(claude_service_delete_profile,
              "claude_service: deleteProfile 清 SQLite + Keychain", {
    TestDb tdb("delete");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile("ToDelete", "https://del.example.com",
                                     "sk-ant-del-3333", "claude-haiku-4-5");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;

    DOCTEST_REQUIRE(svc.deleteProfile(id).isOk());

    // SQLite 行应消失
    auto got = svc.getProfile(id);
    DOCTEST_CHECK(got.isFail());

    // Keychain 条目应消失
    auto key = platform::keychain::loadSecret(
        kTestKeychainService, "claude:" + std::to_string(id));
    DOCTEST_CHECK(key.isFail());

    // 再删一次 → ProfileNotFound
    auto again = svc.deleteProfile(id);
    DOCTEST_CHECK(again.isFail());

    return true;
})

// ---- 6b. deleteProfile：Keychain 清理失败时中止（SQLite 行保留） ----

#if defined(_WIN32)
REGISTER_TEST(claude_service_delete_profile_keychain_fail,
              "claude_service: Keychain 删除失败时中止 deleteProfile", {
    TestDb tdb("delfail");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile("Stuck", "https://stuck.example.com",
                                     "sk-ant-stuck-7777", "m");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;

    // Windows keychain 存储路径（见 keychain_win.cpp）：
    // <appData>/keychain/<service>/<sanitize(account)>.key，
    // account "claude:<id>" 安全化后为 "claude_<id>"。
    // 把密钥文件换成非空目录 → fs::remove 必失败，注入 Keychain 删除失败
    const auto keyPath = core::paths::appDataDir() / "keychain" /
                         kTestKeychainService /
                         ("claude_" + std::to_string(id) + ".key");
    std::error_code ec;
    DOCTEST_REQUIRE(std::filesystem::exists(keyPath, ec));
    std::filesystem::remove(keyPath, ec);
    std::filesystem::create_directories(keyPath, ec);
    {
        std::ofstream blocker(keyPath / "blocker.txt");
        blocker << "make dir non-empty";
    }

    // 中止：deleteProfile 失败，SQLite 行保留（可重试，数据不丢）
    auto r = svc.deleteProfile(id);
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK(svc.getProfile(id).isOk());

    // 恢复现场后重试 → 成功，两边都清干净
    std::filesystem::remove_all(keyPath, ec);
    DOCTEST_REQUIRE(platform::keychain::storeSecret(
        kTestKeychainService, "claude:" + std::to_string(id),
        "sk-ant-stuck-7777").isOk());
    DOCTEST_REQUIRE(svc.deleteProfile(id).isOk());
    DOCTEST_CHECK(svc.getProfile(id).isFail());
    DOCTEST_CHECK(platform::keychain::loadSecret(
        kTestKeychainService, "claude:" + std::to_string(id)).isFail());
    return true;
})
#endif  // _WIN32

// ---- 6c. 启动时孤儿 Keychain 清理 ----

REGISTER_TEST(claude_service_orphan_keychain_cleanup,
              "claude_service: cleanupOrphanKeychainEntries 清孤儿密钥", {
    TestDb tdb("orphan");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto a = svc.createProfile("KeepA", "https://a.example.com",
                               "sk-ant-keep-aaaa", "m");
    DOCTEST_REQUIRE(a.isOk());
    auto b = svc.createProfile("KeepB", "https://b.example.com",
                               "sk-ant-keep-bbbb", "m");
    DOCTEST_REQUIRE(b.isOk());

    // 塞一个无对应 profile 的孤儿密钥（历史 bug 遗留形态）
    DOCTEST_REQUIRE(platform::keychain::storeSecret(
        kTestKeychainService, "claude:999", "sk-ant-orphan").isOk());

    auto r = svc.cleanupOrphanKeychainEntries();
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(r.value() >= 1);

    // 孤儿被清，在册 profile 的 key 完好
    DOCTEST_CHECK(platform::keychain::loadSecret(
        kTestKeychainService, "claude:999").isFail());
    auto ka = svc.loadApiKey(a.value().id);
    DOCTEST_REQUIRE(ka.isOk());
    DOCTEST_CHECK_EQ(ka.value(), std::string("sk-ant-keep-aaaa"));
    auto kb = svc.loadApiKey(b.value().id);
    DOCTEST_REQUIRE(kb.isOk());
    DOCTEST_CHECK_EQ(kb.value(), std::string("sk-ant-keep-bbbb"));

    // 再跑一次 → 无孤儿可清
    auto again = svc.cleanupOrphanKeychainEntries();
    DOCTEST_REQUIRE(again.isOk());
    DOCTEST_CHECK_EQ(again.value(), 0);

    cleanupKey(a.value().id);
    cleanupKey(b.value().id);
    return true;
})

// ---- 7. createProfile 校验：重名 / 空字段 ----

REGISTER_TEST(claude_service_create_validation,
              "claude_service: createProfile 校验重名 + 空字段", {
    TestDb tdb("validate");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 空 name
    auto e1 = svc.createProfile("", "https://x.example.com", "sk-x", "m");
    DOCTEST_CHECK(e1.isFail());
    DOCTEST_CHECK(e1.errorCode() == core::ErrorCode::ValidationError);

    // 空 baseUrl
    auto e2 = svc.createProfile("N", "", "sk-x", "m");
    DOCTEST_CHECK(e2.isFail());
    DOCTEST_CHECK(e2.errorCode() == core::ErrorCode::ValidationError);

    // 空 apiKey
    auto e3 = svc.createProfile("N", "https://x.example.com", "", "m");
    DOCTEST_CHECK(e3.isFail());
    DOCTEST_CHECK(e3.errorCode() == core::ErrorCode::InvalidApiKey);

    // 正常创建
    auto ok1 = svc.createProfile("DupName", "https://x.example.com",
                                 "sk-ant-ddd-4444", "m");
    DOCTEST_REQUIRE(ok1.isOk());

    // 重名 → ProfileAlreadyExists（name UNIQUE 约束）
    auto dup = svc.createProfile("DupName", "https://y.example.com",
                                 "sk-ant-eee-5555", "m");
    DOCTEST_CHECK(dup.isFail());
    DOCTEST_CHECK(dup.errorCode() == core::ErrorCode::ProfileAlreadyExists);

    cleanupKey(ok1.value().id);
    return true;
})

// ---- 8. testConnection（对本地 mock 上游发 GET /v1/models） ----

REGISTER_TEST(claude_service_test_connection,
              "claude_service: testConnection 发 GET /v1/models", {
    // 启动一个 mock 上游，响应 GET /v1/models
    platform::http::Server upstream("127.0.0.1", 0);
    int hitCount = 0;
    std::string lastPath;
    std::string lastApiKey;
    upstream.route("GET", "/v1/models",
                   [&](const platform::http::RequestCtx& ctx,
                       platform::http::ResponseWriter& w) {
                       ++hitCount;
                       lastPath = ctx.path;
                       auto it = ctx.headers.find("x-api-key");
                       if (it != ctx.headers.end()) lastApiKey = it->second;
                       w.setStatus(200);
                       w.setHeader("Content-Type", "application/json");
                       w.write(R"({"data":[{"id":"claude-sonnet-4-5"}]})");
                       w.end();
                   });
    DOCTEST_REQUIRE(upstream.start());
    const int port = upstream.actualPort();

    TestDb tdb("testconn");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    const std::string baseUrl = "http://127.0.0.1:" + std::to_string(port);
    auto created = svc.createProfile("Conn", baseUrl,
                                     "sk-ant-conn-6666", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;

    // 等 mock 上游 ready（bind 后立刻可用，但给一点余量）
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto resp = svc.testConnection(created.value());
    DOCTEST_REQUIRE(resp.isOk());
    DOCTEST_CHECK_EQ(resp.value().statusCode, 200);
    DOCTEST_CHECK(resp.value().body.find("claude-sonnet-4-5") != std::string::npos);
    DOCTEST_CHECK_EQ(hitCount, 1);
    DOCTEST_CHECK_EQ(lastPath, std::string("/v1/models"));
    // 上游收到的 x-api-key 应该是 Keychain 里的明文
    DOCTEST_CHECK_EQ(lastApiKey, std::string("sk-ant-conn-6666"));

    // 上游返回 401 时也算 Ok（UI 用于显示状态码）
    upstream.stop();
    platform::http::Server upstream401("127.0.0.1", 0);
    upstream401.route("GET", "/v1/models",
                      [](const platform::http::RequestCtx&,
                         platform::http::ResponseWriter& w) {
                          w.setStatus(401);
                          w.setHeader("Content-Type", "application/json");
                          w.write(R"({"error":{"type":"authentication_error"}})");
                          w.end();
                      });
    DOCTEST_REQUIRE(upstream401.start());

    module::claude::Profile p401 = created.value();
    p401.baseUrl = "http://127.0.0.1:" + std::to_string(upstream401.actualPort());
    auto resp401 = svc.testConnection(p401);
    DOCTEST_REQUIRE(resp401.isOk());
    DOCTEST_CHECK_EQ(resp401.value().statusCode, 401);
    upstream401.stop();

    cleanupKey(id);
    return true;
})

// ---- 9. testConnection 网络不可达 ----

REGISTER_TEST(claude_service_test_connection_unreachable,
              "claude_service: testConnection 上游不可达返回失败", {
    TestDb tdb("connfail");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 用一个几乎肯定会拒绝连接的端口
    auto created = svc.createProfile("Bad", "http://127.0.0.1:1",
                                     "sk-ant-bad-7777", "m");
    DOCTEST_REQUIRE(created.isOk());

    auto resp = svc.testConnection(created.value());
    DOCTEST_CHECK(resp.isFail());
    DOCTEST_CHECK(resp.errorCode() == core::ErrorCode::NetworkUnreachable);

    cleanupKey(created.value().id);
    return true;
})

// ---- 10. logCall / recentCalls ----

REGISTER_TEST(claude_service_call_logs,
              "claude_service: logCall + recentCalls 往返", {
    TestDb tdb("logs");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile("LogTest", "https://x.example.com",
                                     "sk-ant-log-8888", "m");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;

    // 写 3 条日志
    for (int i = 0; i < 3; ++i) {
        module::claude::ClaudeService::CallLogEntry e;
        e.profileId = id;
        e.method = "POST";
        e.path = "/v1/messages";
        e.statusCode = (i == 2) ? 429 : 200;
        e.latencyMs = 100 * (i + 1);
        e.requestBytes = 512;
        e.responseBytes = 2048;
        e.startedAt = std::time(nullptr) + i;  // 递增，保证排序确定
        DOCTEST_REQUIRE(svc.logCall(e).isOk());
    }

    auto recent = svc.recentCalls(10);
    DOCTEST_REQUIRE(recent.isOk());
    DOCTEST_CHECK_EQ(recent.value().size(), 3u);
    // 按 started_at DESC → 最新的是 429
    DOCTEST_CHECK_EQ(recent.value()[0].statusCode, 429);
    DOCTEST_CHECK_EQ(recent.value()[0].method, std::string("POST"));
    DOCTEST_CHECK_EQ(recent.value()[0].path, std::string("/v1/messages"));
    DOCTEST_CHECK_EQ(recent.value()[0].latencyMs, 300);

    // limit 生效
    auto two = svc.recentCalls(2);
    DOCTEST_REQUIRE(two.isOk());
    DOCTEST_CHECK_EQ(two.value().size(), 2u);

    // 删除 profile 应该 CASCADE 清空日志
    DOCTEST_REQUIRE(svc.deleteProfile(id).isOk());
    auto afterDelete = svc.recentCalls(10);
    DOCTEST_REQUIRE(afterDelete.isOk());
    DOCTEST_CHECK_EQ(afterDelete.value().size(), 0u);

    cleanupKey(id);
    return true;
})

// ---- 11. startProxy / stopProxy / isProxyRunning ----

REGISTER_TEST(claude_service_proxy_lifecycle,
              "claude_service: startProxy / stopProxy 生命周期", {
    TestDb tdb("proxy");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 初始未运行
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());
    DOCTEST_CHECK_EQ(svc.proxyPort(), 0);

    // port=0 → OS 分配
    auto started = svc.startProxy("127.0.0.1", 0, "test-token-abc");
    DOCTEST_REQUIRE(started.isOk());
    const int port = started.value();
    DOCTEST_CHECK(port > 0);
    DOCTEST_CHECK(svc.isProxyRunning());
    DOCTEST_CHECK_EQ(svc.proxyPort(), port);
    DOCTEST_CHECK_EQ(svc.proxyAuthToken(), std::string("test-token-abc"));

    // 重复启动应失败（AlreadyExists）
    auto again = svc.startProxy("127.0.0.1", 0, "other");
    DOCTEST_CHECK(again.isFail());
    DOCTEST_CHECK(again.errorCode() == core::ErrorCode::AlreadyExists);

    svc.stopProxy();
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());
    DOCTEST_CHECK_EQ(svc.proxyPort(), 0);

    // 停止后再启动应该成功
    auto restarted = svc.startProxy("127.0.0.1", 0, "");
    DOCTEST_REQUIRE(restarted.isOk());
    DOCTEST_CHECK(restarted.value() > 0);
    DOCTEST_CHECK(svc.isProxyRunning());
    // 空 token → 自动生成
    DOCTEST_CHECK(!svc.proxyAuthToken().empty());
    DOCTEST_CHECK_EQ(svc.proxyAuthToken().size(), 32u);

    svc.stopProxy();
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());
    return true;
})

// ---- 11b. 审计 2026-09-23 [10]：自动生成 Bearer token 的取值形态 ----
// 旧实现用单次 32 位 random_device 采样给 mt19937_64 播种，128 位 token
// 实际熵 ≤2^32。修复后 random_device 逐 32 位采样 4 次直填 16 字节。
// 熵本身要统计大样本才可测（单元层做不了），本测试固定可观测契约：
// 32 位小写 hex 十六进制 + 每次生成不同（种子路径已移除）。
REGISTER_TEST(claude_service_authtoken_hex_random_format,
              "claude_service: 自动生成 token 为 32 位小写 hex 且每次不同", {
    TestDb tdb("token");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto started = svc.startProxy("127.0.0.1", 0, "");
    DOCTEST_REQUIRE(started.isOk());
    const std::string t1 = svc.proxyAuthToken();
    svc.stopProxy();

    auto again = svc.startProxy("127.0.0.1", 0, "");
    DOCTEST_REQUIRE(again.isOk());
    const std::string t2 = svc.proxyAuthToken();
    svc.stopProxy();

    auto isLowerHex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    };
    DOCTEST_CHECK_EQ(t1.size(), 32u);
    DOCTEST_CHECK_EQ(t2.size(), 32u);
    bool hexOk = true;
    for (char c : t1) { if (!isLowerHex(c)) hexOk = false; }
    for (char c : t2) { if (!isLowerHex(c)) hexOk = false; }
    DOCTEST_CHECK(hexOk);
    DOCTEST_CHECK(t1 != t2);
    return true;
})
