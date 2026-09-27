// tests/test_modules/test_claude_service.cpp
// ClaudeService 单元测试：Profile CRUD + Keychain + 激活 + 连通性
//
// 测试环境隔离：
//   - 数据库用临时文件（std::filesystem::temp_directory_path）
//   - Keychain service 用 "DevDeskTestClaudeService"，不污染生产数据
#include "doctest.h"

#include "claude_service.h"
#include "upstream_dialect.h"
#include "core/database.h"
#include "core/paths.h"
#include "core/sqlite_db.h"
#include "platform/http_server.h"
#include "platform/keychain.h"
#include "provider_config.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kTestKeychainService = "DevDeskTestClaudeService";

// 每个测试用一个独立的临时数据库文件
std::filesystem::path tempDbPath(const std::string& tag) {
    static int counter = 0;
    ++counter;
    auto name = "devdesk_test_claude_" + tag + "_" +
                std::to_string(counter) + ".db";
    return test_temp::tempPath(name);
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

// 探测请求发出的头部（探测经上游方言层装配后头名拼写由方言决定，
// 断言一律大小写不敏感；见 testConnection 方言用例）
using HeaderMap = std::map<std::string, std::string>;

std::string headerCI(const HeaderMap& h, const std::string& lowerKey) {
    for (const auto& kv : h) {
        std::string k = kv.first;
        for (auto& c : k) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (k == lowerKey) return kv.second;
    }
    return std::string();
}

// 统计同名头（大小写不敏感）出现次数：>1 说明大小写变体并存（上游会收到重复头）
int countCI(const HeaderMap& h, const std::string& lowerKey) {
    int n = 0;
    for (const auto& kv : h) {
        std::string k = kv.first;
        for (auto& c : k) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (k == lowerKey) ++n;
    }
    return n;
}

// 测试用：作用域内把 http_proxy 指到本地 mock、清空 no_proxy，析构还原原值。
// 用途：让 baseUrl host 为 opencode.ai 的探测请求仍落到本地 mock——curl 走
// 代理时请求行是绝对 URI（GET http://opencode.ai/... HTTP/1.1），不解析该
// 域名，请求不出本机（libcurl 从环境变量取代理，send() 未显式设置 CURLOPT_PROXY）。
void setEnvVar(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    if (value && *value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

struct ScopedProxyEnv {
    std::string savedProxy;
    std::string savedNoProxy;
    bool hadProxy = false;
    bool hadNoProxy = false;

    explicit ScopedProxyEnv(const std::string& proxyUrl) {
        const char* p = std::getenv("http_proxy");
        if (p) {
            hadProxy = true;
            savedProxy = p;
        }
        const char* n = std::getenv("no_proxy");
        if (n) {
            hadNoProxy = true;
            savedNoProxy = n;
        }
        setEnvVar("http_proxy", proxyUrl.c_str());
        setEnvVar("no_proxy", "");
    }
    ~ScopedProxyEnv() {
        setEnvVar("http_proxy", hadProxy ? savedProxy.c_str() : "");
        setEnvVar("no_proxy", hadNoProxy ? savedNoProxy.c_str() : "");
    }
    ScopedProxyEnv(const ScopedProxyEnv&) = delete;
    ScopedProxyEnv& operator=(const ScopedProxyEnv&) = delete;
};

// ---- 反向同步（M3）测试基建：临时 settings.json 目录（仿 test_settings_sync
// 的 TempDir；绝不碰真实 ~/.claude）----

struct TempDir {
    std::filesystem::path path;

    explicit TempDir(const std::string& tag) {
        static int counter = 0;
        ++counter;
        path = test_temp::tempPath("devdesk_test_claude_rev_" + tag + "_" +
                                   std::to_string(counter));
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
        if (ec) throw std::runtime_error("temp dir create failed");
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    std::filesystem::path file(const char* name) const {
        return path / name;
    }
    void write(const std::filesystem::path& f, const std::string& content) const {
        std::ofstream o(f, std::ios::binary | std::ios::trunc);
        o << content;
    }
    void remove(const std::filesystem::path& f) const {
        std::error_code ec;
        std::filesystem::remove(f, ec);
    }
};

module::claude::settings_sync::Paths makeSyncPaths(const TempDir& d) {
    module::claude::settings_sync::Paths p;
    p.settingsFile = d.file("settings.json");
    p.sidecarFile = d.file("snapshot.json");
    return p;
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

// ---- 3b. 首建自动激活（产品审计 2026-09-23 [6]） ----

REGISTER_TEST(claude_service_first_create_auto_activates,
              "claude_service: createProfile 首建自动激活 + 不抢占 + 503 指引文案", {
    TestDb tdb("autoactivate");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 1) 默认参数保持旧行为：不激活（既有调用方/测试零影响）
    auto legacy = svc.createProfile("Legacy", "https://legacy.example.com",
                                    "sk-ant-legacy-0001", "m");
    DOCTEST_REQUIRE(legacy.isOk());
    DOCTEST_CHECK_FALSE(legacy.value().isActive);
    DOCTEST_CHECK(svc.getActiveProfile().isFail());
    cleanupKey(legacy.value().id);

    // 2) activateIfNoneActive=true 且无激活 → 自动激活，返回 isActive=true
    auto first = svc.createProfile("First", "https://first.example.com",
                                   "sk-ant-first-0002", "m", true);
    DOCTEST_REQUIRE(first.isOk());
    DOCTEST_CHECK(first.value().isActive);
    auto act = svc.getActiveProfile();
    DOCTEST_REQUIRE(act.isOk());
    DOCTEST_CHECK_EQ(act.value().id, first.value().id);

    // 3) 已有激活 → 不抢占（返回值 isActive=false，UI 走「点击启用」文案）
    auto second = svc.createProfile("Second", "https://second.example.com",
                                    "sk-ant-second-0003", "m", true);
    DOCTEST_REQUIRE(second.isOk());
    DOCTEST_CHECK_FALSE(second.value().isActive);
    auto actStill = svc.getActiveProfile();
    DOCTEST_REQUIRE(actStill.isOk());
    DOCTEST_CHECK_EQ(actStill.value().id, first.value().id);

    // 4) 无激活时错误 message 带可操作指引（503 body 原样透传此文案）
    auto del = svc.deleteProfile(first.value().id);
    DOCTEST_REQUIRE(del.isOk());
    auto none = svc.getActiveProfile();
    DOCTEST_REQUIRE(none.isFail());
    DOCTEST_CHECK(none.errorCode() == core::ErrorCode::NoActiveProfile);
    DOCTEST_CHECK(none.errorMessage().find("并点击「启用」") != std::string::npos);

    cleanupKey(second.value().id);
    return true;
})

// ---- 3c. 配置代理端口（产品审计 2026-09-23 [14]） ----

REGISTER_TEST(claude_service_configured_proxy_port,
              "claude_service: setConfiguredProxyPort 默认/钳制 + 换端口重启保 token", {
    TestDb tdb("cfgport");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 默认 = 历史硬编码 8888
    DOCTEST_CHECK_EQ(svc.configuredProxyPort(), 8888);
    svc.setConfiguredProxyPort(9000);
    DOCTEST_CHECK_EQ(svc.configuredProxyPort(), 9000);
    // 非法值回落默认（0 / 负数 / 越界）；边界保留
    svc.setConfiguredProxyPort(0);
    DOCTEST_CHECK_EQ(svc.configuredProxyPort(), 8888);
    svc.setConfiguredProxyPort(-5);
    DOCTEST_CHECK_EQ(svc.configuredProxyPort(), 8888);
    svc.setConfiguredProxyPort(65536);
    DOCTEST_CHECK_EQ(svc.configuredProxyPort(), 8888);
    svc.setConfiguredProxyPort(65535);
    DOCTEST_CHECK_EQ(svc.configuredProxyPort(), 65535);

    // 设置页 applyProxyPort 的运行期原语：stop→start 换端口
    //（测试用 port=0 让 OS 分配，两次 start 即端口变更）
    auto first = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(first.isOk());
    const int portA = first.value();
    svc.stopProxy();
    auto second = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(second.isOk());
    DOCTEST_CHECK(second.value() != portA);          // 端口确实换了
    svc.stopProxy();
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

// ---- 8b. testConnection 经上游方言层装配请求头 ----
// 回归（专家审计 V1）：此前 testConnection 自己拼 x-api-key / anthropic-version，
// 没走 UpstreamDialect——对要求 x-opencode-session、且 Cloudflare 按 UA 拦截的
// 网关（opencode.ai/zen）探测会误报失败，而真实转发是通的。本用例断言探测
// 请求确实经方言层：统一 UA + 鉴权头 + opencode 会话头。

REGISTER_TEST(claude_service_test_connection_uses_dialect,
              "claude_service: testConnection 经上游方言（UA/鉴权/opencode 会话头）", {
    platform::http::Server upstream("127.0.0.1", 0);
    HeaderMap seen;
    int hitCount = 0;
    upstream.route("GET", ".*",
                   [&](const platform::http::RequestCtx& ctx,
                       platform::http::ResponseWriter& w) {
                       ++hitCount;
                       seen = ctx.headers;
                       w.setStatus(200);
                       w.setHeader("Content-Type", "application/json");
                       w.write(R"({"data":[{"id":"claude-sonnet-4-5"}]})");
                       w.end();
                   });
    DOCTEST_REQUIRE(upstream.start());
    const int port = upstream.actualPort();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TestDb tdb("conn_dialect");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    // ---- (1) 默认方言（loopback host）：统一 UA + 鉴权头由方言层注入 ----
    const std::string loopbackUrl = "http://127.0.0.1:" + std::to_string(port);
    auto created = svc.createProfile("ConnDialect", loopbackUrl,
                                     "sk-ant-conn-dialect-9999", "m");
    DOCTEST_REQUIRE(created.isOk());

    auto resp = svc.testConnection(created.value());
    DOCTEST_REQUIRE(resp.isOk());
    DOCTEST_CHECK_EQ(resp.value().statusCode, 200);
    DOCTEST_CHECK_EQ(hitCount, 1);
    // 统一 UA（opencode.ai 前置 Cloudflare 按 UA 指纹拦截，客户端签名不能泄漏）
    DOCTEST_CHECK_EQ(headerCI(seen, "user-agent"),
                     std::string(module::claude::kProxyUserAgent));
    DOCTEST_CHECK_EQ(countCI(seen, "user-agent"), 1);
    // 鉴权经方言层：x-api-key + Authorization: Bearer 双发
    DOCTEST_CHECK_EQ(headerCI(seen, "x-api-key"),
                     std::string("sk-ant-conn-dialect-9999"));
    DOCTEST_CHECK_EQ(headerCI(seen, "authorization"),
                     std::string("Bearer sk-ant-conn-dialect-9999"));
    DOCTEST_CHECK_EQ(countCI(seen, "authorization"), 1);
    DOCTEST_CHECK_EQ(headerCI(seen, "anthropic-version"),
                     std::string("2023-06-01"));
    // 非 opencode host：不注入会话头
    DOCTEST_CHECK_EQ(countCI(seen, "x-opencode-session"), 0);

    // ---- (2) opencode 方言：会话头注入（host 命中 opencode.ai → opencodeZen）----
    // 借 http_proxy 把 curl 指到本地 mock（请求行是绝对 URI，不解析该域名）
    ScopedProxyEnv proxyEnv("http://127.0.0.1:" + std::to_string(port));
    module::claude::Profile p = created.value();
    p.baseUrl = "http://opencode.ai/zen/go";
    seen.clear();
    auto resp2 = svc.testConnection(p);
    DOCTEST_REQUIRE(resp2.isOk());
    DOCTEST_CHECK_EQ(resp2.value().statusCode, 200);
    DOCTEST_CHECK_EQ(hitCount, 2);
    DOCTEST_CHECK_EQ(headerCI(seen, "x-opencode-session"),
                     module::claude::claudeUpstreamSessionId());
    DOCTEST_CHECK_EQ(countCI(seen, "x-opencode-session"), 1);
    DOCTEST_CHECK_EQ(headerCI(seen, "user-agent"),
                     std::string(module::claude::kProxyUserAgent));
    DOCTEST_CHECK_EQ(headerCI(seen, "x-api-key"),
                     std::string("sk-ant-conn-dialect-9999"));

    upstream.stop();
    cleanupKey(created.value().id);
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

// ---- 10a+. 最近代理请求事件槽（M2 §4.5 气泡③，产品审计 [18]）----
// recordProxyEvent/takeProxyEvent：take 读走即清（一事件一泡）、单槽后写
// 覆盖先写、滞留超 kProxyEventFreshSec 过期丢弃、时钟回摆不误吞

REGISTER_TEST(claude_proxy_event_slot,
              "claude_service: 代理事件槽 take-once/覆盖/过期语义", {
    TestDb tdb("proxy_event");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 初始无事件
    DOCTEST_CHECK(!svc.takeProxyEvent().has_value());

    // record 后 take 拿到同一码；再 take 已被消费（读走即清）
    svc.recordProxyEvent(200);
    const auto first = svc.takeProxyEvent();
    DOCTEST_REQUIRE(first.has_value());
    DOCTEST_CHECK_EQ(*first, 200);
    DOCTEST_CHECK(!svc.takeProxyEvent().has_value());

    // 单槽覆盖：连续 record 只留最后一条（0 = 上游不可达口径）
    svc.recordProxyEvent(200);
    svc.recordProxyEvent(0);
    const auto second = svc.takeProxyEvent();
    DOCTEST_REQUIRE(second.has_value());
    DOCTEST_CHECK_EQ(*second, 0);
    DOCTEST_CHECK(!svc.takeProxyEvent().has_value());

    // 过期丢弃：显式传 now = 记录时刻 + 窗口 + 1 → nullopt，且事件被消费
    //（不是留在槽里等下一个读取者）
    svc.recordProxyEvent(503);
    const std::time_t staleNow =
        std::time(nullptr) + module::claude::ClaudeService::kProxyEventFreshSec + 1;
    DOCTEST_CHECK(!svc.takeProxyEvent(staleNow).has_value());
    DOCTEST_CHECK(!svc.takeProxyEvent().has_value());

    // 新鲜事件正常取；显式传当前时刻等价默认参
    svc.recordProxyEvent(429);
    const auto fresh = svc.takeProxyEvent(std::time(nullptr));
    DOCTEST_REQUIRE(fresh.has_value());
    DOCTEST_CHECK_EQ(*fresh, 429);

    // 时钟回摆（now < 记录时刻）按未过期处理：宁可多弹不误吞
    svc.recordProxyEvent(500);
    const auto rolledBack = svc.takeProxyEvent(std::time(nullptr) - 3600);
    DOCTEST_REQUIRE(rolledBack.has_value());
    DOCTEST_CHECK_EQ(*rolledBack, 500);
    return true;
})

// ---- 10b. queryCalls / countCalls（产品审计 2026-09-23 [11]）----
// 日志弹窗承诺的「过滤 + 搜索」在 service 层的支撑：search 按 path/method
// 模糊匹配、statusClass 分桶、limit/offset 翻页、countCalls 总数

REGISTER_TEST(claude_call_log_query,
              "claude_service: queryCalls 搜索/状态码过滤/翻页 + countCalls", {
    TestDb tdb("callquery");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile("QueryTest", "https://q.example.com",
                                     "sk-ant-query-0001", "m");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t pid = created.value().id;
    const std::time_t base = std::time(nullptr);

    // 8 场景行（seq 递增 = 越后越新）+ 20 条老 filler 行（测 limit 截断）
    // 注：REGISTER_TEST body 是函数，DOCTEST 宏 return false → lambda 须显式
    // bool 返回，调用点用 DOCTEST_REQUIRE 传播失败
    auto addRow = [&](const std::string& m, const std::string& p, int st,
                      const std::string& err, int seq) -> bool {
        module::claude::ClaudeService::CallLogEntry e;
        e.profileId = pid;
        e.method = m;
        e.path = p;
        e.statusCode = st;
        e.error = err;
        e.latencyMs = 50 + seq;
        e.requestBytes = 10;
        e.responseBytes = 20;
        e.startedAt = base + seq;
        DOCTEST_REQUIRE(svc.logCall(e).isOk());
        return true;
    };
    DOCTEST_REQUIRE(addRow("POST", "/v1/messages", 200, "", 1));
    DOCTEST_REQUIRE(addRow("POST", "/v1/messages", 429, "", 2));
    DOCTEST_REQUIRE(addRow("GET", "/v1/models", 200, "", 3));
    DOCTEST_REQUIRE(addRow("POST", "/v1/complete", 500, "", 4));
    DOCTEST_REQUIRE(addRow("GET", "/health", 204, "", 5));
    DOCTEST_REQUIRE(addRow("POST", "/v1/messages", 0, "connection timeout", 6));
    DOCTEST_REQUIRE(addRow("GET", "/admin%status", 301, "", 7));
    DOCTEST_REQUIRE(addRow("GET", "/v1/status", 200, "", 8));
    for (int i = 0; i < 20; ++i) {
        DOCTEST_REQUIRE(addRow("GET", "/filler", 200, "", -1000 - i));
    }
    // 直插一行 status_code NULL（历史/旁路写入，仿 home/settings 测试）：
    // 应被 COALESCE 归入「错误」桶
    DOCTEST_REQUIRE(tdb.handle().exec(
        "INSERT INTO claude_call_logs (profile_id, method, path, started_at) "
        "VALUES (" + std::to_string(pid) + ", 'GET', '/legacy-null', " +
        std::to_string(static_cast<long long>(base) - 2000) + ")").isOk());

    using Q = module::claude::ClaudeService::CallLogQuery;

    // 全量：29 行，最新在前（row 8 = /v1/status）
    Q all;
    all.limit = 100;
    auto rAll = svc.queryCalls(all);
    DOCTEST_REQUIRE(rAll.isOk());
    DOCTEST_CHECK_EQ(rAll.value().size(), 29u);
    DOCTEST_CHECK_EQ(rAll.value()[0].path, std::string("/v1/status"));

    // 搜索命中 path（messages ×3）
    Q qMsg;
    qMsg.search = "messages";
    qMsg.limit = 100;
    auto rMsg = svc.queryCalls(qMsg);
    DOCTEST_REQUIRE(rMsg.isOk());
    DOCTEST_CHECK_EQ(rMsg.value().size(), 3u);
    auto cMsg = svc.countCalls(qMsg);
    DOCTEST_REQUIRE(cMsg.isOk());
    DOCTEST_CHECK_EQ(cMsg.value(), 3);

    // 搜索命中 method 且 ASCII 大小写不敏感（post → 4 行）
    Q qPost;
    qPost.search = "post";
    qPost.limit = 100;
    auto rPost = svc.queryCalls(qPost);
    DOCTEST_REQUIRE(rPost.isOk());
    DOCTEST_CHECK_EQ(rPost.value().size(), 4u);

    // LIKE 通配符转义：% 按字面量 → 只命中 /admin%status，不误伤 /v1/status
    Q qEsc;
    qEsc.search = "%status";
    qEsc.limit = 100;
    auto rEsc = svc.queryCalls(qEsc);
    DOCTEST_REQUIRE(rEsc.isOk());
    DOCTEST_CHECK_EQ(rEsc.value().size(), 1u);
    DOCTEST_CHECK_EQ(rEsc.value()[0].path, std::string("/admin%status"));

    // 状态码分桶（NULL 归错误桶）
    Q q2xx;
    q2xx.statusClass = Q::kStatus2xx;
    q2xx.limit = 100;
    auto r2xx = svc.queryCalls(q2xx);
    DOCTEST_REQUIRE(r2xx.isOk());
    DOCTEST_CHECK_EQ(r2xx.value().size(), 24u);  // 场景 4 + filler 20

    Q q4xx;
    q4xx.statusClass = Q::kStatus4xx;
    q4xx.limit = 100;
    DOCTEST_REQUIRE(svc.queryCalls(q4xx).isOk());
    DOCTEST_CHECK_EQ(svc.queryCalls(q4xx).value().size(), 1u);

    Q q5xx;
    q5xx.statusClass = Q::kStatus5xx;
    q5xx.limit = 100;
    DOCTEST_CHECK_EQ(svc.queryCalls(q5xx).value().size(), 1u);

    Q q3xx;
    q3xx.statusClass = Q::kStatus3xx;
    q3xx.limit = 100;
    DOCTEST_CHECK_EQ(svc.queryCalls(q3xx).value().size(), 1u);

    Q qErr;
    qErr.statusClass = Q::kStatusErr;
    qErr.limit = 100;
    auto rErr = svc.queryCalls(qErr);
    DOCTEST_REQUIRE(rErr.isOk());
    DOCTEST_CHECK_EQ(rErr.value().size(), 2u);  // timeout 行 + NULL 行

    // 组合：搜索 + 状态桶（messages ∧ 4xx → 1 行 429）
    Q qCombo;
    qCombo.search = "messages";
    qCombo.statusClass = Q::kStatus4xx;
    auto cCombo = svc.countCalls(qCombo);
    DOCTEST_REQUIRE(cCombo.isOk());
    DOCTEST_CHECK_EQ(cCombo.value(), 1);

    // 翻页：limit 10 × 3 页互不重叠，拼起来 == 全量顺序
    std::vector<int64_t> paged;
    for (int pg = 0; pg < 3; ++pg) {
        Q qPage;
        qPage.limit = 10;
        qPage.offset = pg * 10;
        auto rp = svc.queryCalls(qPage);
        DOCTEST_REQUIRE(rp.isOk());
        for (const auto& e : rp.value()) paged.push_back(e.id);
    }
    DOCTEST_CHECK_EQ(paged.size(), 29u);
    bool sameOrder = true;
    for (size_t i = 0; i < paged.size(); ++i) {
        if (paged[i] != rAll.value()[i].id) sameOrder = false;
    }
    DOCTEST_CHECK(sameOrder);

    // 非法参数归一：limit<=0 按 20；offset<0 按 0（首 20 行）
    Q qClamp;
    qClamp.limit = 0;
    qClamp.offset = -5;
    auto rClamp = svc.queryCalls(qClamp);
    DOCTEST_REQUIRE(rClamp.isOk());
    DOCTEST_CHECK_EQ(rClamp.value().size(), 20u);
    DOCTEST_CHECK_EQ(rClamp.value()[0].id, rAll.value()[0].id);

    cleanupKey(pid);
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
    auto started = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(started.isOk());
    const int port = started.value();
    DOCTEST_CHECK(port > 0);
    DOCTEST_CHECK(svc.isProxyRunning());
    DOCTEST_CHECK_EQ(svc.proxyPort(), port);

    // 重复启动应失败（AlreadyExists）
    auto again = svc.startProxy("127.0.0.1", 0);
    DOCTEST_CHECK(again.isFail());
    DOCTEST_CHECK(again.errorCode() == core::ErrorCode::AlreadyExists);

    svc.stopProxy();
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());
    DOCTEST_CHECK_EQ(svc.proxyPort(), 0);

    // 停止后再启动应该成功
    auto restarted = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(restarted.isOk());
    DOCTEST_CHECK(restarted.value() > 0);
    DOCTEST_CHECK(svc.isProxyRunning());

    svc.stopProxy();
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());
    return true;
})

// ===== 反向同步（M3，设计 2026-09-27-profile-reverse-sync-and-copy §3.1/§3.4/§3.5）=====
// 全部经临时 settings_sync::Paths 注入，绝不碰真实 ~/.claude。

// ---- 12. ① 切 profile 前捕获 settings.json 的真实上游值 ----

REGISTER_TEST(claude_reverse_capture_on_switch,
              "claude_service: 反向同步① 切 profile 前捕获 settings.json", {
    using namespace module::claude;
    TestDb tdb("revcap");
    TempDir d("revcap");
    auto paths = makeSyncPaths(d);

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    svc.setSettingsSyncPaths(paths);

    auto a = svc.createProfile("源A", "https://old.example.com",
                               "sk-old-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    Profile pa = a.value();
    pa.modelsConfigJson =
        R"({"default": {"model": "old-default"},)"
        R"("sonnet": {"model": "old-sonnet"},)"
        R"("haiku": {"model": "old-haiku"}})";
    pa.extraEnvJson = R"({"OLD_KEY": "old"})";
    DOCTEST_REQUIRE(svc.updateProfile(pa).isOk());
    // 首次激活：此时无激活 profile → G0 跳过（不读文件）
    DOCTEST_REQUIRE(svc.setActiveProfile(pa.id).isOk());

    auto b = svc.createProfile("目标B", "https://b.example.com",
                               "sk-b-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(b.isOk());

    // Claude Code 侧（或 cc-switch）改过的真实上游值
    d.write(paths.settingsFile, R"({"env": {
        "ANTHROPIC_BASE_URL": "https://new.example.com/",
        "ANTHROPIC_AUTH_TOKEN": "sk-new-upstream-9876543210",
        "ANTHROPIC_MODEL": "new-default",
        "ANTHROPIC_DEFAULT_SONNET_MODEL": "new-sonnet",
        "ANTHROPIC_DEFAULT_SONNET_MODEL_NAME": "Sonnet 显示名",
        "MY_EXTRA": "extra-value"
    }})");

    DOCTEST_REQUIRE(svc.setActiveProfile(b.value().id).isOk());

    auto gotA = svc.getProfile(pa.id);
    DOCTEST_REQUIRE(gotA.isOk());
    // base_url：尾斜杠清洗后覆盖
    DOCTEST_CHECK_EQ(gotA.value().baseUrl,
                     std::string("https://new.example.com"));
    // key：新上游 key 进 Keychain（经 updateProfile.apiKeyPlain）
    auto keyA = svc.loadApiKey(pa.id);
    DOCTEST_REQUIRE(keyA.isOk());
    DOCTEST_CHECK_EQ(keyA.value(), std::string("sk-new-upstream-9876543210"));
    // 槽位 model + 显示名
    auto mc = parseModelsConfig(gotA.value().modelsConfigJson);
    DOCTEST_REQUIRE(mc.isOk());
    DOCTEST_CHECK_EQ(mc.value().def.model, std::string("new-default"));
    DOCTEST_CHECK_EQ(mc.value().sonnet.model, std::string("new-sonnet"));
    DOCTEST_CHECK_EQ(mc.value().sonnet.name, std::string("Sonnet 显示名"));
    // 缺席的非守卫槽位（haiku）被清空
    DOCTEST_CHECK(mc.value().haiku.model.empty());
    // 旧 model 列跟随 default 槽位
    DOCTEST_CHECK_EQ(gotA.value().model, std::string("new-default"));
    // extra_env：出现覆盖 + 缺席删除（OLD_KEY 消失）
    auto ee = parseExtraEnv(gotA.value().extraEnvJson);
    DOCTEST_REQUIRE(ee.isOk());
    DOCTEST_CHECK_EQ(ee.value().at("MY_EXTRA"), std::string("extra-value"));
    DOCTEST_CHECK(ee.value().find("OLD_KEY") == ee.value().end());
    // 目标 profile 正常激活且未被误写；源 profile 不再激活
    auto gotB = svc.getProfile(b.value().id);
    DOCTEST_REQUIRE(gotB.isOk());
    DOCTEST_CHECK(gotB.value().isActive);
    DOCTEST_CHECK_EQ(gotB.value().baseUrl, std::string("https://b.example.com"));
    DOCTEST_CHECK(!gotA.value().isActive);

    cleanupKey(pa.id);
    cleanupKey(b.value().id);
    return true;
})

// ---- 13. ② 守卫键不写也不清（loopback BASE_URL 联动 token / 占位 / 空值）----
// 每形态都同批带一个**非守卫**变化（MY_EXTRA），保证确实发生了一次写库——
// 否则「没被覆盖」会因为整次 no-op 而白白通过
// 注：a/b 交替激活是为了让「被捕获的那条」始终是 a

REGISTER_TEST(claude_reverse_guard_keys_not_touched,
              "claude_service: 反向同步② 守卫键不写也不清", {
    using namespace module::claude;
    TestDb tdb("revguard");
    TempDir d("revguard");
    auto paths = makeSyncPaths(d);

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    svc.setSettingsSyncPaths(paths);

    auto a = svc.createProfile("守卫A", "https://old.example.com",
                               "sk-old-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());
    auto b = svc.createProfile("旁观B", "https://b.example.com",
                               "sk-b-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(b.isOk());

    // G2：loopback BASE_URL + 非占位 token（本机 cc-switch 形态）→ 都不写也不清
    d.write(paths.settingsFile, R"({"env": {
        "ANTHROPIC_BASE_URL": "http://127.0.0.1:15721",
        "ANTHROPIC_AUTH_TOKEN": "local-cc-switch-token",
        "MY_EXTRA": "extra-v1"
    }})");
    DOCTEST_REQUIRE(svc.setActiveProfile(b.value().id).isOk());
    {
        auto g = svc.getProfile(a.value().id);
        DOCTEST_REQUIRE(g.isOk());
        DOCTEST_CHECK_EQ(g.value().baseUrl,
                         std::string("https://old.example.com"));
        auto k = svc.loadApiKey(a.value().id);
        DOCTEST_REQUIRE(k.isOk());
        DOCTEST_CHECK_EQ(k.value(),
                         std::string("sk-old-key-0123456789abcdef"));
        auto ee = parseExtraEnv(g.value().extraEnvJson);
        DOCTEST_REQUIRE(ee.isOk());
        DOCTEST_CHECK_EQ(ee.value().at("MY_EXTRA"), std::string("extra-v1"));
    }

    // G3：占位 token PROXY_MANAGED → token 不写也不清
    d.write(paths.settingsFile, R"({"env": {
        "ANTHROPIC_BASE_URL": "https://old.example.com",
        "ANTHROPIC_AUTH_TOKEN": "PROXY_MANAGED",
        "MY_EXTRA": "extra-v2"
    }})");
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(b.value().id).isOk());
    {
        auto g = svc.getProfile(a.value().id);
        DOCTEST_REQUIRE(g.isOk());
        DOCTEST_CHECK_EQ(g.value().baseUrl,
                         std::string("https://old.example.com"));
        auto k = svc.loadApiKey(a.value().id);
        DOCTEST_REQUIRE(k.isOk());
        DOCTEST_CHECK_EQ(k.value(),
                         std::string("sk-old-key-0123456789abcdef"));
        auto ee = parseExtraEnv(g.value().extraEnvJson);
        DOCTEST_REQUIRE(ee.isOk());
        DOCTEST_CHECK_EQ(ee.value().at("MY_EXTRA"), std::string("extra-v2"));
    }

    // G4：键存在但值为空串 → 不写也不清（不拿空串覆盖、也不因空值清空）
    d.write(paths.settingsFile, R"({"env": {
        "ANTHROPIC_BASE_URL": "https://old.example.com",
        "ANTHROPIC_AUTH_TOKEN": "",
        "MY_EXTRA": "extra-v3"
    }})");
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(b.value().id).isOk());
    {
        auto g = svc.getProfile(a.value().id);
        DOCTEST_REQUIRE(g.isOk());
        DOCTEST_CHECK_EQ(g.value().baseUrl,
                         std::string("https://old.example.com"));
        auto k = svc.loadApiKey(a.value().id);
        DOCTEST_REQUIRE(k.isOk());
        DOCTEST_CHECK_EQ(k.value(),
                         std::string("sk-old-key-0123456789abcdef"));
        auto ee = parseExtraEnv(g.value().extraEnvJson);
        DOCTEST_REQUIRE(ee.isOk());
        DOCTEST_CHECK_EQ(ee.value().at("MY_EXTRA"), std::string("extra-v3"));
    }

    cleanupKey(a.value().id);
    cleanupKey(b.value().id);
    return true;
})

// ---- 14. ③ 缺席即清空（非守卫托管字段）----

REGISTER_TEST(claude_reverse_absent_clears,
              "claude_service: 反向同步③ 缺席即清空", {
    using namespace module::claude;
    TestDb tdb("revclear");
    TempDir d("revclear");
    auto paths = makeSyncPaths(d);

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    svc.setSettingsSyncPaths(paths);

    auto a = svc.createProfile("清空A", "https://old.example.com",
                               "sk-old-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    Profile pa = a.value();
    pa.modelsConfigJson =
        R"({"default": {"model": "old-default"},)"
        R"("sonnet": {"model": "old-sonnet"}})";
    pa.extraEnvJson = R"({"OLD_KEY": "old"})";
    DOCTEST_REQUIRE(svc.updateProfile(pa).isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(pa.id).isOk());
    auto b = svc.createProfile("目标B", "https://b.example.com",
                               "sk-b-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(b.isOk());

    // 合法 settings.json：只有 default 槽位 + 一个新 extra 键 → 其余全清
    d.write(paths.settingsFile,
            R"({"env": {"ANTHROPIC_MODEL": "only-default", "NEW_KEY": "n"}})");
    DOCTEST_REQUIRE(svc.setActiveProfile(b.value().id).isOk());

    auto g = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(g.isOk());
    DOCTEST_CHECK(g.value().baseUrl.empty());          // 缺席 → 清空
    DOCTEST_CHECK(g.value().keychainAccount.empty());  // 三列元数据一并清空
    DOCTEST_CHECK(g.value().apiKeyHint.empty());
    DOCTEST_CHECK(g.value().apiKeyCheck.empty());
    auto k = svc.loadApiKey(a.value().id);             // Keychain 条目已删
    DOCTEST_CHECK(k.isFail());
    DOCTEST_CHECK_EQ(g.value().model, std::string("only-default"));
    auto mc = parseModelsConfig(g.value().modelsConfigJson);
    DOCTEST_REQUIRE(mc.isOk());
    DOCTEST_CHECK_EQ(mc.value().def.model, std::string("only-default"));
    DOCTEST_CHECK(mc.value().sonnet.model.empty());
    // extra_env 键集合完全由 settings.json 决定：出现的新键在、缺席的旧键没了
    auto ee = parseExtraEnv(g.value().extraEnvJson);
    DOCTEST_REQUIRE(ee.isOk());
    DOCTEST_CHECK_EQ(static_cast<int>(ee.value().size()), 1);
    DOCTEST_CHECK_EQ(ee.value().at("NEW_KEY"), std::string("n"));

    cleanupKey(b.value().id);
    return true;
})

// ---- 15. ④ 坏 JSON / env 非对象 → 中止切换（S-2）且 profile 一字未改 ----

REGISTER_TEST(claude_reverse_broken_json_aborts,
              "claude_service: 反向同步④ 坏 JSON 中止切换且 profile 一字未改", {
    using namespace module::claude;
    TestDb tdb("revbad");
    TempDir d("revbad");
    auto paths = makeSyncPaths(d);

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    svc.setSettingsSyncPaths(paths);

    auto a = svc.createProfile("坏JSON-A", "https://old.example.com",
                               "sk-old-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    Profile pa = a.value();
    pa.modelsConfigJson = R"({"default": {"model": "old-default"}})";
    pa.extraEnvJson = R"({"OLD_KEY": "old"})";
    DOCTEST_REQUIRE(svc.updateProfile(pa).isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(pa.id).isOk());
    auto b = svc.createProfile("坏JSON-B", "https://b.example.com",
                               "sk-b-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(b.isOk());

    const auto abortsAndKeepsA = [&](const char* what) -> bool {
        auto r = svc.setActiveProfile(b.value().id);
        if (r.isOk()) {
            std::cerr << "FAIL: " << what << " 未中止切换" << std::endl;
            return false;
        }
        auto act = svc.getActiveProfile();
        if (act.isFail() || act.value().id != pa.id) {
            std::cerr << "FAIL: " << what << " 后 is_active 变了" << std::endl;
            return false;
        }
        return true;
    };

    // 非法 JSON → fail（S-2）且 is_active 仍是 a、a 的字段一字未改
    d.write(paths.settingsFile, R"({"env": {"ANTHROPIC_MODEL": )");
    DOCTEST_CHECK(abortsAndKeepsA("坏 JSON"));
    {
        auto g = svc.getProfile(pa.id);
        DOCTEST_REQUIRE(g.isOk());
        DOCTEST_CHECK_EQ(g.value().baseUrl,
                         std::string("https://old.example.com"));
        DOCTEST_CHECK_EQ(g.value().modelsConfigJson, pa.modelsConfigJson);
        DOCTEST_CHECK_EQ(g.value().extraEnvJson, pa.extraEnvJson);
        auto k = svc.loadApiKey(pa.id);
        DOCTEST_REQUIRE(k.isOk());
        DOCTEST_CHECK_EQ(k.value(),
                         std::string("sk-old-key-0123456789abcdef"));
    }

    // env 非对象 → 同款中止（空文件不在此列——见用例⑤）
    d.write(paths.settingsFile, R"({"env": "not-an-object"})");
    DOCTEST_CHECK(abortsAndKeepsA("env 非对象"));
    {
        auto g = svc.getProfile(pa.id);
        DOCTEST_REQUIRE(g.isOk());
        DOCTEST_CHECK_EQ(g.value().baseUrl,
                         std::string("https://old.example.com"));
        auto k = svc.loadApiKey(pa.id);
        DOCTEST_REQUIRE(k.isOk());
        DOCTEST_CHECK_EQ(k.value(),
                         std::string("sk-old-key-0123456789abcdef"));
    }

    cleanupKey(pa.id);
    cleanupKey(b.value().id);
    return true;
})

// ---- 16. ⑤ 文件不存在 / 空文件 / 无 env → no-op 成功且照常切换 ----

REGISTER_TEST(claude_reverse_missing_or_empty_noop,
              "claude_service: 反向同步⑤ 缺失或空文件 no-op 且照常切换", {
    using namespace module::claude;
    TestDb tdb("revnoop");
    TempDir d("revnoop");
    auto paths = makeSyncPaths(d);

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    svc.setSettingsSyncPaths(paths);

    auto a = svc.createProfile("空-A", "https://old.example.com",
                               "sk-old-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    Profile pa = a.value();
    pa.extraEnvJson = R"({"OLD_KEY": "old"})";
    DOCTEST_REQUIRE(svc.updateProfile(pa).isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(pa.id).isOk());
    auto b = svc.createProfile("空-B", "https://b.example.com",
                               "sk-b-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(b.isOk());

    // 文件根本不存在（G7）：no-op 成功 + 照常切换 + a 一字未改
    {
        DOCTEST_REQUIRE(svc.setActiveProfile(b.value().id).isOk());
        auto act = svc.getActiveProfile();
        DOCTEST_REQUIRE(act.isOk());
        DOCTEST_CHECK_EQ(act.value().id, b.value().id);
        auto g = svc.getProfile(pa.id);
        DOCTEST_REQUIRE(g.isOk());
        DOCTEST_CHECK_EQ(g.value().baseUrl,
                         std::string("https://old.example.com"));
        DOCTEST_CHECK_EQ(g.value().extraEnvJson, pa.extraEnvJson);
        auto k = svc.loadApiKey(pa.id);
        DOCTEST_REQUIRE(k.isOk());
        DOCTEST_CHECK_EQ(k.value(),
                         std::string("sk-old-key-0123456789abcdef"));
    }

    // 0 字节空文件（G9，视为不存在）：a 侧同样一字未改
    d.write(paths.settingsFile, "");
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(b.value().id).isOk());
    {
        auto g = svc.getProfile(pa.id);
        DOCTEST_REQUIRE(g.isOk());
        DOCTEST_CHECK_EQ(g.value().baseUrl,
                         std::string("https://old.example.com"));
        auto k = svc.loadApiKey(pa.id);
        DOCTEST_REQUIRE(k.isOk());
        DOCTEST_CHECK_EQ(k.value(),
                         std::string("sk-old-key-0123456789abcdef"));
        auto ee = parseExtraEnv(g.value().extraEnvJson);
        DOCTEST_REQUIRE(ee.isOk());
        DOCTEST_CHECK_EQ(ee.value().at("OLD_KEY"), std::string("old"));
    }

    // 解析成功但无 env 键（G9 另一半）
    d.write(paths.settingsFile, R"({"permissions": {"allow": ["Bash"]}})");
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(b.value().id).isOk());
    {
        auto g = svc.getProfile(pa.id);
        DOCTEST_REQUIRE(g.isOk());
        DOCTEST_CHECK_EQ(g.value().baseUrl,
                         std::string("https://old.example.com"));
        auto k = svc.loadApiKey(pa.id);
        DOCTEST_REQUIRE(k.isOk());
        DOCTEST_CHECK_EQ(k.value(),
                         std::string("sk-old-key-0123456789abcdef"));
    }

    cleanupKey(pa.id);
    cleanupKey(b.value().id);
    return true;
})

// ---- 17. ⑥ 无激活 profile（G0）：坏 settings.json 也不挡首建自动激活 ----

REGISTER_TEST(claude_reverse_no_active_skips,
              "claude_service: 反向同步⑥ 无激活 profile 时不读文件", {
    using namespace module::claude;
    TestDb tdb("revnoact");
    TempDir d("revnoact");
    auto paths = makeSyncPaths(d);

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    svc.setSettingsSyncPaths(paths);

    // 坏 settings.json：首建自动激活路径不得被它挡住（不读文件）
    d.write(paths.settingsFile, "{ not json at all");
    auto c = svc.createProfile("首建", "https://first.example.com",
                               "sk-first-key-0123456789", "",
                               /*activateIfNoneActive=*/true);
    DOCTEST_REQUIRE(c.isOk());
    DOCTEST_CHECK(c.value().isActive);
    auto act = svc.getActiveProfile();
    DOCTEST_REQUIRE(act.isOk());
    DOCTEST_CHECK_EQ(act.value().id, c.value().id);

    cleanupKey(c.value().id);
    return true;
})

// ---- 18. ⑦ 无变化 → 零写入（不 UPDATE、不写 Keychain、不 bump updated_at）----

REGISTER_TEST(claude_reverse_no_change_zero_write,
              "claude_service: 反向同步⑦ 无变化零写入 updated_at 不变", {
    using namespace module::claude;
    TestDb tdb("revsame");
    TempDir d("revsame");
    auto paths = makeSyncPaths(d);

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    svc.setSettingsSyncPaths(paths);

    const std::string key = "sk-same-key-0123456789abcdef";
    auto a = svc.createProfile("同值A", "https://same.example.com", key, "");
    DOCTEST_REQUIRE(a.isOk());
    Profile pa = a.value();
    ModelsConfig mc;
    mc.def.model = "m-default";
    mc.sonnet.model = "m-sonnet";
    auto mcText = serializeModelsConfig(mc);
    DOCTEST_REQUIRE(mcText.isOk());
    pa.modelsConfigJson = mcText.value();
    pa.extraEnvJson = R"({"SAME_KEY": "same"})";
    DOCTEST_REQUIRE(svc.updateProfile(pa).isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(pa.id).isOk());

    auto before = svc.getProfile(pa.id);
    DOCTEST_REQUIRE(before.isOk());

    auto b = svc.createProfile("同值B", "https://b.example.com",
                               "sk-b-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(b.isOk());

    // settings.json 与 a 的现值逐字段一致 → 不该发生任何写入
    d.write(paths.settingsFile,
            std::string(R"({"env": {)") +
                R"("ANTHROPIC_BASE_URL": "https://same.example.com",)" +
                R"("ANTHROPIC_AUTH_TOKEN": ")" + key + R"(",)" +
                R"("ANTHROPIC_MODEL": "m-default",)" +
                R"("ANTHROPIC_DEFAULT_SONNET_MODEL": "m-sonnet",)" +
                R"("SAME_KEY": "same"}})");

    DOCTEST_REQUIRE(svc.setActiveProfile(b.value().id).isOk());
    auto after = svc.getProfile(pa.id);
    DOCTEST_REQUIRE(after.isOk());
    DOCTEST_CHECK_EQ(after.value().updatedAt, before.value().updatedAt);
    DOCTEST_CHECK_EQ(after.value().baseUrl, before.value().baseUrl);
    DOCTEST_CHECK_EQ(after.value().modelsConfigJson,
                     before.value().modelsConfigJson);
    DOCTEST_CHECK_EQ(after.value().extraEnvJson, before.value().extraEnvJson);
    DOCTEST_CHECK_EQ(after.value().apiKeyCheck, before.value().apiKeyCheck);
    auto k = svc.loadApiKey(pa.id);
    DOCTEST_REQUIRE(k.isOk());
    DOCTEST_CHECK_EQ(k.value(), key);

    cleanupKey(pa.id);
    cleanupKey(b.value().id);
    return true;
})

// ---- 19. ⑧ sidecar 托管键（G1）：不写也不清 ----

REGISTER_TEST(claude_reverse_sidecar_managed_key_skipped,
              "claude_service: 反向同步⑧ sidecar 托管键不被覆盖", {
    using namespace module::claude;
    TestDb tdb("revside");
    TempDir d("revside");
    auto paths = makeSyncPaths(d);

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    svc.setSettingsSyncPaths(paths);

    auto a = svc.createProfile("托管A", "https://old.example.com",
                               "sk-old-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    Profile pa = a.value();
    ModelsConfig mc;
    mc.def.model = "keep-me";
    auto mcText = serializeModelsConfig(mc);
    DOCTEST_REQUIRE(mcText.isOk());
    pa.modelsConfigJson = mcText.value();
    DOCTEST_REQUIRE(svc.updateProfile(pa).isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(pa.id).isOk());
    auto b = svc.createProfile("托管B", "https://b.example.com",
                               "sk-b-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(b.isOk());

    // sidecar 声明 ANTHROPIC_MODEL 是托管键；同批还有一个非托管变化（NEW_KEY），
    // 保证确实写了一次库（否则整次 no-op 会让「未被覆盖」白白通过）
    d.write(paths.sidecarFile,
            R"({"managedKeys": {"ANTHROPIC_MODEL": ""}, "envSnapshot": {}})");
    d.write(paths.settingsFile, R"({"env": {
        "ANTHROPIC_MODEL": "from-settings",
        "NEW_KEY": "n"
    }})");
    DOCTEST_REQUIRE(svc.setActiveProfile(b.value().id).isOk());

    auto g = svc.getProfile(pa.id);
    DOCTEST_REQUIRE(g.isOk());
    auto mc2 = parseModelsConfig(g.value().modelsConfigJson);
    DOCTEST_REQUIRE(mc2.isOk());
    DOCTEST_CHECK_EQ(mc2.value().def.model, std::string("keep-me"));
    DOCTEST_CHECK_EQ(g.value().model, std::string("keep-me"));
    auto ee = parseExtraEnv(g.value().extraEnvJson);
    DOCTEST_REQUIRE(ee.isOk());
    DOCTEST_CHECK_EQ(ee.value().at("NEW_KEY"), std::string("n"));

    cleanupKey(pa.id);
    cleanupKey(b.value().id);
    return true;
})

// ===== Profile 复制（M4，设计 §4.1/§4.2/§4.3）=====

// ---- 20. 副本字段 / Keychain 全等同源、id 与 account 不同、不激活、源行未动 ----

REGISTER_TEST(claude_dup_profile_basic,
              "claude_service: 复制 profile 全等同源且不激活", {
    using namespace module::claude;
    TestDb tdb("dup");
    TempDir d("dup");
    auto paths = makeSyncPaths(d);

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    svc.setSettingsSyncPaths(paths);

    auto a = svc.createProfile("复制源", "https://src.example.com",
                               "sk-src-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    Profile pa = a.value();
    pa.modelsConfigJson = R"({"default": {"model": "m-def"},)"
                          R"("sonnet": {"model": "m-son"}})";
    pa.extraEnvJson = R"({"EXTRA_A": "1"})";
    DOCTEST_REQUIRE(svc.updateProfile(pa).isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(pa.id).isOk());

    // 此刻 settings.json 有别的值：复制若误触发反向同步就会污染源
    d.write(paths.settingsFile, R"({"env": {
        "ANTHROPIC_BASE_URL": "https://other.example.com",
        "ANTHROPIC_AUTH_TOKEN": "sk-other-key-0987654321"
    }})");

    auto dup = svc.duplicateProfile(pa.id);
    DOCTEST_REQUIRE(dup.isOk());
    const Profile cp = dup.value();
    DOCTEST_CHECK_NE(cp.id, pa.id);
    DOCTEST_CHECK_EQ(cp.name, std::string("复制源 副本"));
    DOCTEST_CHECK_EQ(cp.baseUrl, pa.baseUrl);
    DOCTEST_CHECK_EQ(cp.modelsConfigJson, pa.modelsConfigJson);
    DOCTEST_CHECK_EQ(cp.extraEnvJson, pa.extraEnvJson);
    DOCTEST_CHECK_EQ(cp.model, std::string("m-def"));
    DOCTEST_CHECK_EQ(cp.apiKeyCheck, pa.apiKeyCheck);
    DOCTEST_CHECK(!cp.isActive);                       // S-4：副本不激活
    DOCTEST_CHECK_EQ(cp.keychainAccount,
                     std::string("claude:") + std::to_string(cp.id));
    DOCTEST_CHECK_NE(cp.keychainAccount, pa.keychainAccount);
    // Keychain：副本 account 下是同一条源密钥
    auto k = svc.loadApiKey(cp.id);
    DOCTEST_REQUIRE(k.isOk());
    DOCTEST_CHECK_EQ(k.value(), std::string("sk-src-key-0123456789abcdef"));

    // 源行未动、激活态未变、未被反向同步污染（副本不激活 ⇒ 两需求零耦合）
    auto src = svc.getProfile(pa.id);
    DOCTEST_REQUIRE(src.isOk());
    DOCTEST_CHECK(src.value().isActive);
    DOCTEST_CHECK_EQ(src.value().name, std::string("复制源"));
    DOCTEST_CHECK_EQ(src.value().baseUrl, std::string("https://src.example.com"));
    DOCTEST_CHECK_EQ(src.value().modelsConfigJson, pa.modelsConfigJson);
    DOCTEST_CHECK_EQ(src.value().extraEnvJson, pa.extraEnvJson);
    auto act = svc.getActiveProfile();
    DOCTEST_REQUIRE(act.isOk());
    DOCTEST_CHECK_EQ(act.value().id, pa.id);

    // 重名避让：「副本 2」「副本 3」
    auto dup2 = svc.duplicateProfile(pa.id);
    DOCTEST_REQUIRE(dup2.isOk());
    DOCTEST_CHECK_EQ(dup2.value().name, std::string("复制源 副本 2"));
    auto dup3 = svc.duplicateProfile(pa.id);
    DOCTEST_REQUIRE(dup3.isOk());
    DOCTEST_CHECK_EQ(dup3.value().name, std::string("复制源 副本 3"));

    // 源不存在 → fail
    DOCTEST_CHECK(svc.duplicateProfile(99999).isFail());

    cleanupKey(pa.id);
    cleanupKey(cp.id);
    cleanupKey(dup2.value().id);
    cleanupKey(dup3.value().id);
    return true;
})

// ---- 21. 源 key 缺失（Keychain 孤儿）→ fail 且不建行 ----

REGISTER_TEST(claude_dup_missing_key_fails,
              "claude_service: 复制时源密钥缺失 fail 且无新行", {
    using namespace module::claude;
    TestDb tdb("dupkey");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto a = svc.createProfile("缺钥源", "https://src.example.com",
                               "sk-src-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    cleanupKey(a.value().id);            // 造 Keychain 孤儿

    auto before = svc.listProfiles();
    DOCTEST_REQUIRE(before.isOk());
    DOCTEST_CHECK_EQ(static_cast<int>(before.value().size()), 1);

    auto dup = svc.duplicateProfile(a.value().id);
    DOCTEST_CHECK(dup.isFail());
    DOCTEST_CHECK(dup.errorMessage().find("密钥缺失") != std::string::npos);

    auto after = svc.listProfiles();
    DOCTEST_REQUIRE(after.isOk());
    DOCTEST_CHECK_EQ(static_cast<int>(after.value().size()), 1);
    return true;
})

// ---- 22. 二段写失败 → 回滚（零残留：行没了、Keychain 条目也清了）----

REGISTER_TEST(claude_dup_stage2_rollback,
              "claude_service: 复制二段失败回滚且零残留", {
    using namespace module::claude;
    TestDb tdb("duprb");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 先归零孤儿基线（清掉同套件其它用例可能残留在测试 Keychain 命名空间
    // 的条目，让下面的孤儿计数断言只反映本用例）
    auto orphansBase = svc.cleanupOrphanKeychainEntries();
    DOCTEST_REQUIRE(orphansBase.isOk());

    auto a = svc.createProfile("脏行源", "https://src.example.com",
                               "sk-src-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    // 直接 SQL 写坏 models_config 造 legacy 脏行：createProfile 阶段不读它，
    // 二段 updateProfile 会校验 JSON 并拒绝 → 触发回滚
    auto bad = tdb.handle().exec(
        "UPDATE claude_profiles SET models_config = '{bad-json' WHERE id = " +
        std::to_string(a.value().id));
    DOCTEST_REQUIRE(bad.isOk());

    auto dup = svc.duplicateProfile(a.value().id);
    DOCTEST_CHECK(dup.isFail());

    // 行已回滚：只剩源这一行
    auto list = svc.listProfiles();
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_CHECK_EQ(static_cast<int>(list.value().size()), 1);
    // Keychain 条目也已清（无残留孤儿）
    auto orphansAfter = svc.cleanupOrphanKeychainEntries();
    DOCTEST_REQUIRE(orphansAfter.isOk());
    DOCTEST_CHECK_EQ(orphansAfter.value(), 0);
    // 源 key 未受影响
    auto k = svc.loadApiKey(a.value().id);
    DOCTEST_REQUIRE(k.isOk());
    DOCTEST_CHECK_EQ(k.value(), std::string("sk-src-key-0123456789abcdef"));

    cleanupKey(a.value().id);
    return true;
})
