// tests/test_modules/test_claude_service.cpp
// ClaudeService 单元测试：Profile CRUD + Keychain + 激活 + 连通性 + 共享配置片段
//
// 测试环境隔离：
//   - 数据库用临时文件（std::filesystem::temp_directory_path）
//   - Keychain service 用 "DevDeskTestClaudeService"，不污染生产数据
#include "doctest.h"
#include "test_support.h"

#include "claude_service.h"
#include "upstream_dialect.h"
#include "core/database.h"
#include "core/paths.h"
#include "core/sqlite_db.h"
#include "platform/http_server.h"
#include "platform/keychain.h"
#include "provider_config.h"
#include "settings_sync.h"
#include "platform/terminal_launch.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>   // ExclusivePortGuard：SO_EXCLUSIVEADDRUSE（见该 struct 注释）
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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
    // 审计 CP-07：Database/SqliteDb 已不可默认构造（open() 是唯一入口），
    // 「先占位、后 move 进来」改用指针表达
    std::unique_ptr<core::Database> db;

    explicit TestDb(const std::string& tag) {
        path = tempDbPath(tag);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        auto r = core::Database::open(path);
        if (r.isFail()) {
            throw std::runtime_error("Database::open failed: " +
                                     r.errorMessage());
        }
        db = std::make_unique<core::Database>(std::move(r.unsafeValue()));
        auto m = db->migrate();
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

    core::data::SqliteDb& handle() { return db->db(); }
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
        test_support::setEnvVar("http_proxy", proxyUrl.c_str());
        test_support::setEnvVar("no_proxy", "");
    }
    ~ScopedProxyEnv() {
        test_support::setEnvVar("http_proxy", hadProxy ? savedProxy.c_str() : "");
        test_support::setEnvVar("no_proxy", hadNoProxy ? savedNoProxy.c_str() : "");
    }
    ScopedProxyEnv(const ScopedProxyEnv&) = delete;
    ScopedProxyEnv& operator=(const ScopedProxyEnv&) = delete;
};

// 动态空闲端口（借 Server bind 0 向系统要一个随即释放）：多 worktree / 残留进程
// 并发跑测试时不再撞写死端口；失败返回 0（调用方 REQUIRE 拦）
int pickFreePort() {
    platform::http::Server probe("127.0.0.1", 0);
    if (!probe.start()) return 0;
    const int port = probe.actualPort();
    probe.stop();
    return port;
}

// 接管通道路径用的临时文件（settings.json + sidecar；绝不碰真实 ~/.claude）。
// runProfile 的「非激活 → 先切激活」分支要靠「坏 settings.json 让反向同步失败」
// 来观察，故必须注入通道路径。
struct TempSyncPaths {
    std::filesystem::path settings;
    std::filesystem::path sidecar;

    explicit TempSyncPaths(const std::string& tag) {
        const std::string base = tempDbPath(tag).string();   // 复用同一 tempPath 工厂
        settings = base + ".settings.json";
        sidecar = base + ".sidecar.json";
        std::error_code ec;
        std::filesystem::remove(settings, ec);
        std::filesystem::remove(sidecar, ec);
    }
    ~TempSyncPaths() {
        std::error_code ec;
        std::filesystem::remove(settings, ec);
        std::filesystem::remove(sidecar, ec);
    }

    void writeSettings(const std::string& content) const {
        std::ofstream(settings, std::ios::binary | std::ios::trunc) << content;
    }
    module::claude::settings_sync::Paths paths() const {
        module::claude::settings_sync::Paths p;
        p.settingsFile = settings;
        p.sidecarFile = sidecar;
        return p;
    }
};

// 「把端口占住」的**独占**绑定守卫（profile-run 的 startProxy 失败用例专用）。
// ⚠️ 不能用第二个 platform::http::Server 占位：cpp-httplib 给监听 socket 设了
// SO_REUSEADDR（Windows）/ SO_REUSEPORT（POSIX），两个 Server 能**同时**绑上同一
// 端口（占位方与被占方的 start() 都返回 true）→ 占位语义不成立（本机实测）。
// 这里改用独占语义的裸 socket，后到的 bind 必定失败：
//   Windows：SO_EXCLUSIVEADDRUSE → 带 SO_REUSEADDR 的二次 bind 也报 WSAEADDRINUSE
//   POSIX：  不设 REUSEADDR/REUSEPORT 的裸 bind → 对端单方设 REUSEPORT 也绑不上
struct ExclusivePortGuard {
#if defined(_WIN32)
    using NativeSocket = SOCKET;
    static constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
    using NativeSocket = int;
    static constexpr NativeSocket kInvalidSocket = -1;
#endif

    NativeSocket sock = kInvalidSocket;

    explicit ExclusivePortGuard(int port) {
#if defined(_WIN32)
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
#endif
        sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock == kInvalidSocket) return;
#if defined(_WIN32)
        const BOOL exclusive = TRUE;
        ::setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&exclusive),
                     static_cast<int>(sizeof(exclusive)));
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<unsigned short>(port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(sock, reinterpret_cast<sockaddr*>(&addr),
                   static_cast<int>(sizeof(addr))) != 0) {
            closeSocket();
        }
    }
    ~ExclusivePortGuard() { closeSocket(); }
    ExclusivePortGuard(const ExclusivePortGuard&) = delete;
    ExclusivePortGuard& operator=(const ExclusivePortGuard&) = delete;

    bool ok() const { return sock != kInvalidSocket; }

private:
    void closeSocket() {
        if (sock == kInvalidSocket) return;
#if defined(_WIN32)
        ::closesocket(sock);
        WSACleanup();
#else
        ::close(sock);
#endif
        sock = kInvalidSocket;
    }
};

// 反向同步（M3）测试基建：临时 settings.json 目录（共享头 test_support::TempDir，
// 前缀沿用原样；绝不碰真实 ~/.claude）
constexpr const char* kRevTempDirPrefix = "devdesk_test_claude_rev_";

module::claude::settings_sync::Paths makeSyncPaths(const test_support::TempDir& d) {
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

// ---- 5b. CPX-07：两段写失败补偿（审计「Keychain + DB 两段写」条目） ----

REGISTER_TEST(claude_service_update_db_fail_rolls_back_keychain,
              "claude_service: updateProfile DB 写失败按旧值回写 Keychain（CPX-07）", {
    TestDb tdb("updcomp");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto a = svc.createProfile("NameA", "https://a.example.com",
                               "sk-ant-aaa-0001", "m");
    DOCTEST_REQUIRE(a.isOk());
    auto b = svc.createProfile("NameB", "https://b.example.com",
                               "sk-ant-bbb-0002", "m");
    DOCTEST_REQUIRE(b.isOk());
    const int64_t bid = b.value().id;

    // 失败注入：把 B 的 name 改成 A 的 name → UPDATE 撞 claude_profiles.name 的
    // UNIQUE 约束 = 段二（DB 写）确定性失败，断言「段一已写成新密钥、段二失败后
    // 被回写」。刻意不经注入缝（CPX-14 起可用）：本用例的价值就在补偿逻辑走
    // **真实** Keychain 落盘再回写；Keychain 自身失败的分支见下方 CPX-14 三条用例。
    module::claude::Profile pb = b.value();
    pb.name = "NameA";
    pb.apiKeyPlain = "sk-ant-bbb-9999";
    auto upd = svc.updateProfile(pb);
    DOCTEST_CHECK(upd.isFail());
    DOCTEST_CHECK(upd.errorCode() == core::ErrorCode::ProfileAlreadyExists);

    // 补偿生效：Keychain 里是旧密钥，而不是刚写进去的新密钥
    auto key = svc.loadApiKey(bid);
    DOCTEST_REQUIRE(key.isOk());
    DOCTEST_CHECK_EQ(key.value(), std::string("sk-ant-bbb-0002"));

    // DB 行一字未改 → 元数据与 Keychain 里的密钥一致
    auto got = svc.getProfile(bid);
    DOCTEST_REQUIRE(got.isOk());
    DOCTEST_CHECK_EQ(got.value().name, std::string("NameB"));
    DOCTEST_CHECK_EQ(got.value().apiKeyHint, b.value().apiKeyHint);
    DOCTEST_CHECK_EQ(got.value().apiKeyCheck, b.value().apiKeyCheck);

    // 一致性锚：hint / check 是密钥的纯函数——同一把旧密钥另建一条必须算出相同
    // 值（证明「元数据 = Keychain 实际值」），新密钥必须算出不同的值（证明本
    // 断言有鉴别力，不是「怎么写都一样」）
    auto twin = svc.createProfile("Twin", "https://t.example.com",
                                  "sk-ant-bbb-0002", "m");
    DOCTEST_REQUIRE(twin.isOk());
    DOCTEST_CHECK_EQ(got.value().apiKeyHint, twin.value().apiKeyHint);
    DOCTEST_CHECK_EQ(got.value().apiKeyCheck, twin.value().apiKeyCheck);
    auto other = svc.createProfile("Other", "https://o.example.com",
                                   "sk-ant-bbb-9999", "m");
    DOCTEST_REQUIRE(other.isOk());
    DOCTEST_CHECK(got.value().apiKeyHint != other.value().apiKeyHint);
    DOCTEST_CHECK(got.value().apiKeyCheck != other.value().apiKeyCheck);

    // 撞名的那条（A）不受影响
    auto ga = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(ga.isOk());
    DOCTEST_CHECK_EQ(ga.value().apiKeyHint, a.value().apiKeyHint);

    // 改回原名 + 换 key 仍是正常路径（补偿没有把正常写路径带坏）
    pb.name = "NameB";
    pb.apiKeyPlain = "sk-ant-bbb-9999";
    DOCTEST_CHECK(svc.updateProfile(pb).isOk());
    auto key2 = svc.loadApiKey(bid);
    DOCTEST_REQUIRE(key2.isOk());
    DOCTEST_CHECK_EQ(key2.value(), std::string("sk-ant-bbb-9999"));

    cleanupKey(a.value().id);
    cleanupKey(bid);
    cleanupKey(twin.value().id);
    cleanupKey(other.value().id);
    return true;
})

// ---- 5c. CPX-14：Keychain 注入缝 + 两段写三条失败分支 ----
// 背景：TwoPhaseKeyWrite（CPX-07）的补偿路径此前只能靠「DB 写必然失败」（UNIQUE
// 撞名）间接触发；「Keychain 自身失败」那一支（段一失败 → 回写旧值）根本走不到，
// 因为 Keychain 是自由函数、没有注入口（见上一条用例里的注记）。CPX-14 在
// platform/keychain.h 加了最小注入缝（函数指针表 + 单槽位，默认指真实实现），
// 下面三条用例分别覆盖：段一写失败 / 段二删行失败 / 段二回填失败。

namespace {

// 假 Keychain：密钥存内存 map（service + 0x1F + account → secret）。
// store 的失败注入语义是「**先落值、再报失败**」：各平台存储不承诺失败时旧值
// 完好（见 TwoPhaseKeyWrite::writeKey 的注释——失败也可能已破坏旧值），补偿
// 回写正是为这种情形准备的。若假实现「失败且不落值」，用例就丧失鉴别力
//（不补偿也照样读回旧值，反证跑不出 FAIL）。
struct FakeKeychain {
    static constexpr const char* kStoreFailSentinel = "INJECT-STORE-FAIL";

    std::map<std::string, std::string> data;
    std::string lastStoredAccount;  // 最近一次 store 的 account（认领新 profile）

    static std::string keyOf(const std::string& service,
                             const std::string& account) {
        return service + '\x1f' + account;
    }
    bool has(const std::string& service, const std::string& account) const {
        return data.find(keyOf(service, account)) != data.end();
    }
    std::string get(const std::string& service,
                    const std::string& account) const {
        const auto it = data.find(keyOf(service, account));
        return it == data.end() ? std::string() : it->second;
    }
};

// 函数指针表不能带捕获 → 假实现经文件级指针取当前实例（用例内单实例，生命周期
// 由 ScopedFakeKeychain 管）
FakeKeychain* g_fakeKeychain = nullptr;

core::Result<void> fakeStore(const std::string& service,
                             const std::string& account,
                             const std::string& secret) {
    FakeKeychain& f = *g_fakeKeychain;
    f.lastStoredAccount = account;
    f.data[FakeKeychain::keyOf(service, account)] = secret;   // 先落值
    if (secret.find(FakeKeychain::kStoreFailSentinel) != std::string::npos) {
        return core::Result<void>::fail(
            core::ErrorCode::Internal,
            "注入：storeSecret 失败（值已落盘，模拟失败时旧值已被破坏）");
    }
    return core::Result<void>::ok();
}

core::Result<std::string> fakeLoad(const std::string& service,
                                   const std::string& account) {
    FakeKeychain& f = *g_fakeKeychain;
    const auto it = f.data.find(FakeKeychain::keyOf(service, account));
    if (it == f.data.end()) {
        // 与真实实现同语义：不存在 → NotFound
        return core::Result<std::string>::fail(core::ErrorCode::NotFound,
                                               "Keychain item not found");
    }
    return core::Result<std::string>::ok(it->second);
}

core::Result<void> fakeDelete(const std::string& service,
                              const std::string& account) {
    FakeKeychain& f = *g_fakeKeychain;
    f.data.erase(FakeKeychain::keyOf(service, account));
    return core::Result<void>::ok();   // 不存在也 Ok（幂等，与真实实现同语义）
}

core::Result<std::vector<std::string>> fakeDeleteNotIn(
    const std::string& service, const std::vector<std::string>& keepAccounts) {
    FakeKeychain& f = *g_fakeKeychain;
    std::vector<std::string> removed;
    const std::string prefix = service + '\x1f';
    for (auto it = f.data.begin(); it != f.data.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            const std::string account = it->first.substr(prefix.size());
            bool keep = false;
            for (const auto& k : keepAccounts) {
                if (k == account) { keep = true; break; }
            }
            if (!keep) {
                removed.push_back(account);
                it = f.data.erase(it);
                continue;
            }
        }
        ++it;
    }
    return core::Result<std::vector<std::string>>::ok(std::move(removed));
}

// RAII：安装假后端，析构恢复真实后端——用例体内 fail-fast（return false）或
// 抛异常都不会把假后端泄漏给后续用例
struct ScopedFakeKeychain {
    FakeKeychain fake;
    platform::keychain::Backend table;
    const platform::keychain::Backend* prev;

    ScopedFakeKeychain() {
        g_fakeKeychain = &fake;
        table = platform::keychain::Backend{&fakeStore, &fakeLoad, &fakeDelete,
                                            &fakeDeleteNotIn};
        prev = platform::keychain::setBackendForTest(&table);
    }
    ~ScopedFakeKeychain() {
        platform::keychain::setBackendForTest(prev);
        g_fakeKeychain = nullptr;
    }
    ScopedFakeKeychain(const ScopedFakeKeychain&) = delete;
    ScopedFakeKeychain& operator=(const ScopedFakeKeychain&) = delete;
};

} // namespace

// 段一（Keychain 写）失败 → 按捕获到的旧值回写
REGISTER_TEST(claude_cpx14_stage1_key_write_fail_restores_old,
              "claude_service: CPX-14 段一 Keychain 写失败回写旧值", {
    TestDb tdb("cpx14stage1");
    ScopedFakeKeychain kc;
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile("Outer", "https://a.example.com",
                                     "sk-ant-old-0001", "m");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;
    const std::string account = created.value().keychainAccount;
    DOCTEST_CHECK_EQ(kc.fake.get(kTestKeychainService, account),
                     std::string("sk-ant-old-0001"));

    // 注入：新密钥含哨兵 → 假后端落值后报失败 → writeKey 内部按旧值回写
    module::claude::Profile p = created.value();
    p.apiKeyPlain = std::string("sk-ant-new-0002-") +
                    FakeKeychain::kStoreFailSentinel;
    auto upd = svc.updateProfile(p);
    DOCTEST_CHECK(upd.isFail());

    // 补偿生效：Keychain 里是旧值，而不是刚写坏的新值
    DOCTEST_CHECK_EQ(kc.fake.get(kTestKeychainService, account),
                     std::string("sk-ant-old-0001"));
    // 段一失败即返回，段二没跑过：DB 元数据与旧密钥仍一致
    auto got = svc.getProfile(id);
    DOCTEST_REQUIRE(got.isOk());
    DOCTEST_CHECK_EQ(got.value().apiKeyCheck, created.value().apiKeyCheck);
    DOCTEST_CHECK_EQ(got.value().apiKeyHint, created.value().apiKeyHint);

    // 正常路径未被注入层带坏（同一用例内换回合法密钥 → 写入成功）
    p.apiKeyPlain = "sk-ant-new-0002";
    DOCTEST_CHECK(svc.updateProfile(p).isOk());
    DOCTEST_CHECK_EQ(kc.fake.get(kTestKeychainService, account),
                     std::string("sk-ant-new-0002"));
    return true;
})

// 段二（DB 删行）失败 → 密钥已删时补偿回写 + 行保留可重试
REGISTER_TEST(claude_cpx14_delete_stage2_fail_restores_key,
              "claude_service: CPX-14 段二删行失败回写已删密钥", {
    TestDb tdb("cpx14delstage2");
    ScopedFakeKeychain kc;
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile("Victim", "https://v.example.com",
                                     "sk-ant-victim-0003", "m");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;
    const std::string account = created.value().keychainAccount;
    DOCTEST_REQUIRE(kc.fake.has(kTestKeychainService, account));

    // 段二注入失败：BEFORE DELETE 触发器 RAISE(ABORT) → DELETE 语句确定性失败
    DOCTEST_REQUIRE(tdb.handle().exec(
        "CREATE TRIGGER cpx14_block_delete BEFORE DELETE ON claude_profiles "
        "BEGIN SELECT RAISE(ABORT, 'cpx14: delete blocked'); END").isOk());

    auto del = svc.deleteProfile(id);
    DOCTEST_CHECK(del.isFail());

    // 段一确实删过密钥（不是「段一失败即中止」）：段二失败后补偿把它写回来
    DOCTEST_CHECK_EQ(kc.fake.get(kTestKeychainService, account),
                     std::string("sk-ant-victim-0003"));
    // 行保留（用户可重试，密钥没丢）
    DOCTEST_CHECK(svc.getProfile(id).isOk());

    // 撤掉注入源后重试 → 成功，两边都清干净（补偿没把正常路径带坏）
    DOCTEST_REQUIRE(tdb.handle().exec("DROP TRIGGER cpx14_block_delete").isOk());
    DOCTEST_CHECK(svc.deleteProfile(id).isOk());
    DOCTEST_CHECK(svc.getProfile(id).isFail());
    DOCTEST_CHECK(!kc.fake.has(kTestKeychainService, account));
    return true;
})

// 段二（回填 keychain_account）失败 → 删行回滚 + 段一写入的密钥被清掉
REGISTER_TEST(claude_cpx14_create_stage2_fail_rolls_back,
              "claude_service: CPX-14 回填 account 失败删行回滚", {
    TestDb tdb("cpx14createstage2");
    ScopedFakeKeychain kc;
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 基线一条无关 profile（创建走完整两段写正常路径）
    auto base = svc.createProfile("Base", "https://base.example.com",
                                  "sk-ant-base-0004", "m");
    DOCTEST_REQUIRE(base.isOk());
    auto before = svc.listProfiles();
    DOCTEST_REQUIRE(before.isOk());
    const size_t countBefore = before.value().size();

    // 段二注入失败：BEFORE UPDATE 触发器 RAISE(ABORT) → 回填 keychain_account
    // 的 UPDATE 必失败（此刻段一已把密钥写进 Keychain）
    DOCTEST_REQUIRE(tdb.handle().exec(
        "CREATE TRIGGER cpx14_block_update BEFORE UPDATE ON claude_profiles "
        "BEGIN SELECT RAISE(ABORT, 'cpx14: update blocked'); END").isOk());

    auto created = svc.createProfile("Doomed", "https://d.example.com",
                                     "sk-ant-doomed-0005", "m");
    DOCTEST_CHECK(created.isFail());

    // 零残留一：段一写入的密钥被补偿清掉（不留孤儿密钥）
    const std::string doomedAccount = kc.fake.lastStoredAccount;
    DOCTEST_CHECK_EQ(doomedAccount.substr(0, 7), std::string("claude:"));
    DOCTEST_CHECK(!kc.fake.has(kTestKeychainService, doomedAccount));
    // 零残留二：INSERT 的行也回滚了
    auto after = svc.listProfiles();
    DOCTEST_REQUIRE(after.isOk());
    DOCTEST_CHECK_EQ(after.value().size(), countBefore);

    // 撤掉注入源后重试 → 成功（正常路径未被注入层带坏）
    DOCTEST_REQUIRE(tdb.handle().exec("DROP TRIGGER cpx14_block_update").isOk());
    auto ok = svc.createProfile("Doomed", "https://d.example.com",
                                "sk-ant-doomed-0005", "m");
    DOCTEST_REQUIRE(ok.isOk());
    DOCTEST_CHECK_EQ(kc.fake.get(kTestKeychainService, ok.value().keychainAccount),
                     std::string("sk-ant-doomed-0005"));
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
    // <appData>/keychain/<service>/<encode(account)>.key，account "claude:<id>"
    // 经 CP-11 可逆编码（非白名单字符 %XX 转义）后为 "claude%3A<id>"。
    // 把密钥文件换成非空目录 → fs::remove 必失败，注入 Keychain 删除失败
    const auto keyPath = core::paths::appDataDir() / "keychain" /
                         kTestKeychainService /
                         ("claude%3A" + std::to_string(id) + ".key");
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
    test_support::TempDir d("revcap", kRevTempDirPrefix);
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
    test_support::TempDir d("revguard", kRevTempDirPrefix);
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
    test_support::TempDir d("revclear", kRevTempDirPrefix);
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
    test_support::TempDir d("revbad", kRevTempDirPrefix);
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
    test_support::TempDir d("revnoop", kRevTempDirPrefix);
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
    test_support::TempDir d("revnoact", kRevTempDirPrefix);
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
    test_support::TempDir d("revsame", kRevTempDirPrefix);
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
    test_support::TempDir d("revside", kRevTempDirPrefix);
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
    test_support::TempDir d("dup", kRevTempDirPrefix);
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

// ---- 23. 共享配置片段：CRUD + 绑定/解绑（设计 §2.1/§2.2/§2.6）----

REGISTER_TEST(claude_shared_config_crud_and_binding,
              "claude_service: 片段 CRUD 与 profile 绑定/解绑", {
    using namespace module::claude;
    TestDb tdb("sharedcrud");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 建：空名 / 非法 JSON 拒绝
    DOCTEST_CHECK(svc.createSharedConfig("   ", "{}").isFail());
    DOCTEST_CHECK(svc.createSharedConfig("片段A", "{bad-json").isFail());

    auto f = svc.createSharedConfig("片段A", R"({"MY_KEY": "from-shared"})");
    DOCTEST_REQUIRE(f.isOk());
    DOCTEST_CHECK(f.value().id > 0);
    // 重名 → AlreadyExists（name UNIQUE）
    auto dupName = svc.createSharedConfig("片段A", "{}");
    DOCTEST_CHECK(dupName.isFail());
    DOCTEST_CHECK(dupName.errorCode() == core::ErrorCode::AlreadyExists);

    // 列
    auto list = svc.listSharedConfigs();
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_REQUIRE_EQ(list.value().size(), 1u);
    DOCTEST_CHECK_EQ(list.value()[0].name, std::string("片段A"));
    DOCTEST_CHECK_EQ(list.value()[0].extraEnvJson, f.value().extraEnvJson);

    // 改（改名 + 改 env）；不存在的 id → NotFound
    SharedConfig upd = f.value();
    upd.name = "片段A2";
    upd.extraEnvJson = R"({"MY_KEY": "v2", "SECOND_KEY": "1"})";
    DOCTEST_REQUIRE(svc.updateSharedConfig(upd).isOk());
    auto list2 = svc.listSharedConfigs();
    DOCTEST_REQUIRE(list2.isOk());
    DOCTEST_REQUIRE_EQ(list2.value().size(), 1u);
    DOCTEST_CHECK_EQ(list2.value()[0].name, std::string("片段A2"));
    SharedConfig ghost;
    ghost.id = 9999;
    ghost.name = "x";
    DOCTEST_CHECK(svc.updateSharedConfig(ghost).isFail());

    // 绑定：createProfile 尾部参数可建时即绑
    auto a = svc.createProfile("绑片段", "https://a.example.com",
                               "sk-ind-test-0123456789abcdef", "", false,
                               f.value().id);
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_CHECK_EQ(a.value().sharedConfigId, f.value().id);  // 返回值带上
    // 读路径带该列（getProfile / listProfiles）
    auto ga = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(ga.isOk());
    DOCTEST_CHECK_EQ(ga.value().sharedConfigId, f.value().id);
    auto la = svc.listProfiles();
    DOCTEST_REQUIRE(la.isOk());
    DOCTEST_REQUIRE_EQ(la.value().size(), 1u);
    DOCTEST_CHECK_EQ(la.value()[0].sharedConfigId, f.value().id);

    // 冻结键集（G10 用）= 片段 extra_env 的键
    auto keys = svc.sharedEnvKeysFor(a.value().id);
    DOCTEST_REQUIRE(keys.isOk());
    DOCTEST_CHECK_EQ(keys.value().size(), 2u);
    bool hasMyKey = false;
    for (const auto& k : keys.value()) {
        if (k == "MY_KEY") hasMyKey = true;
    }
    DOCTEST_CHECK(hasMyKey);

    // 别名归一：键集口径与写侧一致（AUTO_COMPACT_ENABLED → DISABLE_AUTO_COMPACT）
    auto f2 = svc.createSharedConfig("片段别名", R"({"AUTO_COMPACT_ENABLED": "1"})");
    DOCTEST_REQUIRE(f2.isOk());
    Profile aliased = ga.value();
    aliased.sharedConfigId = f2.value().id;
    DOCTEST_REQUIRE(svc.updateProfile(aliased).isOk());
    auto keysAlias = svc.sharedEnvKeysFor(a.value().id);
    DOCTEST_REQUIRE(keysAlias.isOk());
    DOCTEST_REQUIRE_EQ(keysAlias.value().size(), 1u);
    DOCTEST_CHECK_EQ(keysAlias.value()[0], std::string("DISABLE_AUTO_COMPACT"));

    // 解绑（updateProfile 置 0）→ 键集空、DB 列回 0
    Profile unbind = ga.value();
    unbind.sharedConfigId = 0;
    DOCTEST_REQUIRE(svc.updateProfile(unbind).isOk());
    auto keys2 = svc.sharedEnvKeysFor(a.value().id);
    DOCTEST_REQUIRE(keys2.isOk());
    DOCTEST_CHECK(keys2.value().empty());
    auto ga2 = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(ga2.isOk());
    DOCTEST_CHECK_EQ(ga2.value().sharedConfigId, 0);

    cleanupKey(a.value().id);
    return true;
})

// ---- 24. 删除片段：同事务解绑绑定它的 profile（不留悬空 id）----

REGISTER_TEST(claude_shared_config_delete_unbinds,
              "claude_service: 删片段同事务解绑绑定它的 profile", {
    using namespace module::claude;
    TestDb tdb("shareddel");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto f = svc.createSharedConfig("片段S", R"({"K": "1"})");
    DOCTEST_REQUIRE(f.isOk());
    auto a = svc.createProfile("绑A", "https://a.example.com",
                               "sk-ind-test-a-0123456789ab", "", false,
                               f.value().id);
    auto b = svc.createProfile("绑B", "https://b.example.com",
                               "sk-ind-test-b-0123456789ab", "", false,
                               f.value().id);
    auto c = svc.createProfile("没绑", "https://c.example.com",
                               "sk-ind-test-c-0123456789ab", "");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_REQUIRE(c.isOk());

    DOCTEST_REQUIRE(svc.deleteSharedConfig(f.value().id).isOk());

    // 绑定行解绑、未绑行不受影响、profile 行一条不删（不做级联删）
    auto ga = svc.getProfile(a.value().id);
    auto gb = svc.getProfile(b.value().id);
    auto gc = svc.getProfile(c.value().id);
    DOCTEST_REQUIRE(ga.isOk());
    DOCTEST_REQUIRE(gb.isOk());
    DOCTEST_REQUIRE(gc.isOk());
    DOCTEST_CHECK_EQ(ga.value().sharedConfigId, 0);
    DOCTEST_CHECK_EQ(gb.value().sharedConfigId, 0);
    DOCTEST_CHECK_EQ(gc.value().sharedConfigId, 0);
    DOCTEST_CHECK_EQ(svc.listProfiles().value().size(), 3u);
    auto list = svc.listSharedConfigs();
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_CHECK(list.value().empty());

    // 再删 → NotFound（不静默成功）；id=0 拒绝
    auto again = svc.deleteSharedConfig(f.value().id);
    DOCTEST_CHECK(again.isFail());
    DOCTEST_CHECK(again.errorCode() == core::ErrorCode::NotFound);
    DOCTEST_CHECK(svc.deleteSharedConfig(0).isFail());

    // 悬空 id 的读路径容错：解绑后冻结键集为空，不炸
    auto keys = svc.sharedEnvKeysFor(a.value().id);
    DOCTEST_REQUIRE(keys.isOk());
    DOCTEST_CHECK(keys.value().empty());

    cleanupKey(a.value().id);
    cleanupKey(b.value().id);
    cleanupKey(c.value().id);
    return true;
})

// ---- 25. 复制 profile 携带片段绑定（设计 §2.6）----

REGISTER_TEST(claude_shared_config_duplicate_copies_binding,
              "claude_service: 复制 profile 携带片段绑定", {
    using namespace module::claude;
    TestDb tdb("shareddup");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto f = svc.createSharedConfig("片段D", R"({"K": "1"})");
    DOCTEST_REQUIRE(f.isOk());
    auto a = svc.createProfile("源", "https://a.example.com",
                               "sk-ind-test-0123456789abcdef", "", false,
                               f.value().id);
    DOCTEST_REQUIRE(a.isOk());

    // 二段补写条件（models/extra_env 非空）不成立时也必须带上绑定
    auto dup = svc.duplicateProfile(a.value().id);
    DOCTEST_REQUIRE(dup.isOk());
    DOCTEST_CHECK_EQ(dup.value().sharedConfigId, f.value().id);
    auto gd = svc.getProfile(dup.value().id);
    DOCTEST_REQUIRE(gd.isOk());
    DOCTEST_CHECK_EQ(gd.value().sharedConfigId, f.value().id);
    auto keys = svc.sharedEnvKeysFor(dup.value().id);
    DOCTEST_REQUIRE(keys.isOk());
    DOCTEST_CHECK_EQ(keys.value().size(), 1u);

    cleanupKey(a.value().id);
    cleanupKey(dup.value().id);
    return true;
})

// ---- 26. 未绑定片段的回归：列为 0、键集空（既有行为零变化）----

REGISTER_TEST(claude_shared_config_unbound_regression,
              "claude_service: 未绑定片段读路径零值（回归）", {
    using namespace module::claude;
    TestDb tdb("sharednone");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    DOCTEST_CHECK(svc.listSharedConfigs().value().empty());
    auto a = svc.createProfile("不绑", "https://a.example.com",
                               "sk-ind-test-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_CHECK_EQ(a.value().sharedConfigId, 0);
    auto ga = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(ga.isOk());
    DOCTEST_CHECK_EQ(ga.value().sharedConfigId, 0);
    auto keys = svc.sharedEnvKeysFor(a.value().id);
    DOCTEST_REQUIRE(keys.isOk());
    DOCTEST_CHECK(keys.value().empty());

    cleanupKey(a.value().id);
    return true;
})

// ===== 认证字段三态（鉴权头；2026-09-27 profile-auth-field 设计 §3/§4）=====

// ---- 27. 默认 both + 三处 SELECT 往返（getActiveProfile 是「漏改 SELECT 列
//      静默失效」的回归闸：rowToProfile 的 if (isOk()) 守卫会把缺列吞成默认值）----

REGISTER_TEST(claude_service_auth_field_default_and_roundtrip,
              "claude_service: 认证字段默认 both + getProfile/getActiveProfile 往返", {
    using namespace module::claude;
    TestDb tdb("authfield_rt");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 不传尾参 → 默认 'both'
    auto d = svc.createProfile("默认", "https://d.example.com",
                               "sk-def-test-0123456789abcdef", "m");
    DOCTEST_REQUIRE(d.isOk());
    DOCTEST_CHECK_EQ(d.value().authField, std::string("both"));
    auto gd = svc.getProfile(d.value().id);
    DOCTEST_REQUIRE(gd.isOk());
    DOCTEST_CHECK_EQ(gd.value().authField, std::string("both"));

    // 显式 'bearer'：getProfile / getActiveProfile / listProfiles 三条 SELECT 都要读到
    auto b = svc.createProfile("Bearer", "https://b.example.com",
                               "sk-bear-test-0123456789abcdef", "m",
                               /*activateIfNoneActive=*/false,
                               /*sharedConfigId=*/0, "bearer");
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_CHECK_EQ(b.value().authField, std::string("bearer"));
    auto gb = svc.getProfile(b.value().id);
    DOCTEST_REQUIRE(gb.isOk());
    DOCTEST_CHECK_EQ(gb.value().authField, std::string("bearer"));
    DOCTEST_REQUIRE(svc.setActiveProfile(b.value().id).isOk());
    auto gact = svc.getActiveProfile();
    DOCTEST_REQUIRE(gact.isOk());
    DOCTEST_CHECK_EQ(gact.value().id, b.value().id);
    DOCTEST_CHECK_EQ(gact.value().authField, std::string("bearer"));
    auto all = svc.listProfiles();
    DOCTEST_REQUIRE(all.isOk());
    bool listed = false;
    for (const auto& p : all.value()) {
        if (p.id == b.value().id) {
            listed = true;
            DOCTEST_CHECK_EQ(p.authField, std::string("bearer"));
        }
    }
    DOCTEST_CHECK(listed);

    cleanupKey(d.value().id);
    cleanupKey(b.value().id);
    return true;
})

// ---- 28. updateProfile 两条 UPDATE 分支都能改该字段 ----

REGISTER_TEST(claude_service_auth_field_update_both_branches,
              "claude_service: 认证字段 updateProfile 两分支（换 key / 不换 key）", {
    using namespace module::claude;
    TestDb tdb("authfield_upd");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto a = svc.createProfile("双分支", "https://u.example.com",
                               "sk-upd-test-0123456789abcdef", "m");
    DOCTEST_REQUIRE(a.isOk());

    // 分支一：apiKeyPlain 非空（换 key 分支）
    Profile p1 = a.value();
    p1.apiKeyPlain = "sk-upd-test-newkey-0123456789";
    p1.authField = "api_key";
    DOCTEST_REQUIRE(svc.updateProfile(p1).isOk());
    auto g1 = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(g1.isOk());
    DOCTEST_CHECK_EQ(g1.value().authField, std::string("api_key"));

    // 分支二：apiKeyPlain 留空（不换 key 分支）
    Profile p2 = g1.value();
    p2.authField = "bearer";
    DOCTEST_REQUIRE(svc.updateProfile(p2).isOk());
    auto g2 = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(g2.isOk());
    DOCTEST_CHECK_EQ(g2.value().authField, std::string("bearer"));

    cleanupKey(a.value().id);
    return true;
})

// ---- 29. 二段补写不覆盖（createProfile 返回值必须带上该字段）----

REGISTER_TEST(claude_service_auth_field_survives_two_stage_update,
              "claude_service: 认证字段经二段补写（models/env）仍保持", {
    using namespace module::claude;
    TestDb tdb("authfield_two");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto c = svc.createProfile("二段", "https://two.example.com",
                               "sk-two-test-0123456789abcdef", "m",
                               /*activateIfNoneActive=*/false,
                               /*sharedConfigId=*/0, "bearer");
    DOCTEST_REQUIRE(c.isOk());
    // 模拟 UI 新建：以 createProfile 返回值为基，二段补写 models/env
    Profile p = c.value();
    p.modelsConfigJson = R"({"default": {"model": "m-def"}})";
    p.extraEnvJson = R"({"EXTRA_A": "1"})";
    DOCTEST_REQUIRE(svc.updateProfile(p).isOk());

    auto g = svc.getProfile(c.value().id);
    DOCTEST_REQUIRE(g.isOk());
    DOCTEST_CHECK_EQ(g.value().authField, std::string("bearer"));
    DOCTEST_CHECK_EQ(g.value().modelsConfigJson, p.modelsConfigJson);

    cleanupKey(c.value().id);
    return true;
})

// ---- 30. 复制保留该字段 ----

REGISTER_TEST(claude_service_duplicate_keeps_auth_field,
              "claude_service: 复制 profile 保留认证字段", {
    using namespace module::claude;
    TestDb tdb("authfield_dup");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto a = svc.createProfile("复制源", "https://src2.example.com",
                               "sk-src-test-0123456789abcdef", "m",
                               /*activateIfNoneActive=*/false,
                               /*sharedConfigId=*/0, "api_key");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_CHECK_EQ(a.value().authField, std::string("api_key"));

    auto dup = svc.duplicateProfile(a.value().id);
    DOCTEST_REQUIRE(dup.isOk());
    DOCTEST_CHECK_EQ(dup.value().authField, std::string("api_key"));
    auto g = svc.getProfile(dup.value().id);
    DOCTEST_REQUIRE(g.isOk());
    DOCTEST_CHECK_EQ(g.value().authField, std::string("api_key"));

    cleanupKey(a.value().id);
    cleanupKey(dup.value().id);
    return true;
})

// ---- 31. 写侧规范化（非法值 → both，设计 D-8）----

REGISTER_TEST(claude_service_auth_field_invalid_normalized,
              "claude_service: 认证字段非法值写侧规范化为 both", {
    using namespace module::claude;
    TestDb tdb("authfield_norm");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    // createProfile 传非法串 → 落库与返回值都是 'both'
    auto a = svc.createProfile("规范化建", "https://n.example.com",
                               "sk-norm-test-0123456789abcdef", "m",
                               /*activateIfNoneActive=*/false,
                               /*sharedConfigId=*/0, "X");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_CHECK_EQ(a.value().authField, std::string("both"));

    // updateProfile 传非法串（直接构造 Profile）→ 读回 'both'
    Profile p;
    p.id = a.value().id;
    p.name = "规范化建";
    p.baseUrl = "https://n.example.com";
    p.authField = "X";
    DOCTEST_REQUIRE(svc.updateProfile(p).isOk());
    auto g = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(g.isOk());
    DOCTEST_CHECK_EQ(g.value().authField, std::string("both"));

    cleanupKey(a.value().id);
    return true;
})

// ---- 32. 方言覆盖：默认空串 + 三处 SELECT 往返（CPX-13）----
// getActiveProfile 是「漏改 SELECT 列静默失效」的回归闸：rowToProfile 的
// if (isOk()) 守卫会把缺列吞成默认值

REGISTER_TEST(claude_service_dialect_override_default_and_roundtrip,
              "claude_service: dialectOverride 默认空串 + 三处 SELECT 往返", {
    using namespace module::claude;
    TestDb tdb("dialect_rt");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 不传尾参 → 空串（未覆盖 = 按 host 推断；存量调用零变化）
    auto d = svc.createProfile("方言默认", "https://d.example.com",
                               "sk-dial-default-0123456789abcdef", "m");
    DOCTEST_REQUIRE(d.isOk());
    DOCTEST_CHECK(d.value().dialectOverride.empty());
    auto gd = svc.getProfile(d.value().id);
    DOCTEST_REQUIRE(gd.isOk());
    DOCTEST_CHECK(gd.value().dialectOverride.empty());

    // 显式 opencode_zen：getProfile / getActiveProfile / listProfiles 三条 SELECT 都要读到
    auto z = svc.createProfile("方言Zen", "https://zen.example.com",
                               "sk-dial-zen-0123456789abcdef", "m",
                               /*activateIfNoneActive=*/false,
                               /*sharedConfigId=*/0, "both", "opencode_zen");
    DOCTEST_REQUIRE(z.isOk());
    DOCTEST_CHECK_EQ(z.value().dialectOverride, std::string("opencode_zen"));
    auto gz = svc.getProfile(z.value().id);
    DOCTEST_REQUIRE(gz.isOk());
    DOCTEST_CHECK_EQ(gz.value().dialectOverride, std::string("opencode_zen"));
    DOCTEST_REQUIRE(svc.setActiveProfile(z.value().id).isOk());
    auto gact = svc.getActiveProfile();
    DOCTEST_REQUIRE(gact.isOk());
    DOCTEST_CHECK_EQ(gact.value().id, z.value().id);
    DOCTEST_CHECK_EQ(gact.value().dialectOverride, std::string("opencode_zen"));
    auto all = svc.listProfiles();
    DOCTEST_REQUIRE(all.isOk());
    bool listed = false;
    for (const auto& p : all.value()) {
        if (p.id == z.value().id) {
            listed = true;
            DOCTEST_CHECK_EQ(p.dialectOverride, std::string("opencode_zen"));
        }
    }
    DOCTEST_CHECK(listed);

    // ① 覆盖值对 profile 生效：host 后缀普通（默认方言）也被钉成 zen 方言
    const UpstreamDialect pinned =
        UpstreamDialect::forBaseUrl(gz.value().baseUrl,
                                    gz.value().dialectOverride);
    DOCTEST_CHECK(pinned.kind == UpstreamDialect::Kind::OpencodeZen);

    cleanupKey(d.value().id);
    cleanupKey(z.value().id);
    return true;
})

// ---- 33. updateProfile 两条 UPDATE 分支都能改该字段 ----

REGISTER_TEST(claude_service_dialect_override_update_both_branches,
              "claude_service: dialectOverride updateProfile 两分支（换 key / 不换 key）", {
    using namespace module::claude;
    TestDb tdb("dialect_upd");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto a = svc.createProfile("方言双分支", "https://zen.example.com",
                               "sk-dial-upd-0123456789abcdef", "m",
                               /*activateIfNoneActive=*/false,
                               /*sharedConfigId=*/0, "both", "opencode_zen");
    DOCTEST_REQUIRE(a.isOk());

    // 分支一：apiKeyPlain 非空（换 key 分支）——覆盖改成通用方言
    Profile p1 = a.value();
    p1.apiKeyPlain = "sk-dial-upd-newkey-0123456789";
    p1.dialectOverride = "anthropic_default";
    DOCTEST_REQUIRE(svc.updateProfile(p1).isOk());
    auto g1 = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(g1.isOk());
    DOCTEST_CHECK_EQ(g1.value().dialectOverride, std::string("anthropic_default"));

    // 分支二：apiKeyPlain 留空（不换 key 分支）——覆盖改回 zen
    Profile p2 = g1.value();
    p2.dialectOverride = "opencode_zen";
    DOCTEST_REQUIRE(svc.updateProfile(p2).isOk());
    auto g2 = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(g2.isOk());
    DOCTEST_CHECK_EQ(g2.value().dialectOverride, std::string("opencode_zen"));

    // 清空 = 回到 host 推断（空串是合法值，不是"写不进去"）
    Profile p3 = g2.value();
    p3.dialectOverride.clear();
    DOCTEST_REQUIRE(svc.updateProfile(p3).isOk());
    auto g3 = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(g3.isOk());
    DOCTEST_CHECK(g3.value().dialectOverride.empty());

    cleanupKey(a.value().id);
    return true;
})

// ---- 34. 二段补写不覆盖（createProfile 返回值必须带上该字段）----

REGISTER_TEST(claude_service_dialect_override_survives_two_stage_update,
              "claude_service: dialectOverride 经二段补写（models/env）仍保持", {
    using namespace module::claude;
    TestDb tdb("dialect_two");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto c = svc.createProfile("方言二段", "https://two.example.com",
                               "sk-dial-two-0123456789abcdef", "m",
                               /*activateIfNoneActive=*/false,
                               /*sharedConfigId=*/0, "both", "opencode_zen");
    DOCTEST_REQUIRE(c.isOk());
    // 模拟 UI 新建：以 createProfile 返回值为基，二段补写 models/env
    Profile p = c.value();
    p.modelsConfigJson = R"({"default": {"model": "m-def"}})";
    p.extraEnvJson = R"({"EXTRA_A": "1"})";
    DOCTEST_REQUIRE(svc.updateProfile(p).isOk());

    auto g = svc.getProfile(c.value().id);
    DOCTEST_REQUIRE(g.isOk());
    DOCTEST_CHECK_EQ(g.value().dialectOverride, std::string("opencode_zen"));
    DOCTEST_CHECK_EQ(g.value().modelsConfigJson, p.modelsConfigJson);

    cleanupKey(c.value().id);
    return true;
})

// ---- 35. 复制保留该字段 ----

REGISTER_TEST(claude_service_duplicate_keeps_dialect_override,
              "claude_service: 复制 profile 保留 dialectOverride", {
    using namespace module::claude;
    TestDb tdb("dialect_dup");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto a = svc.createProfile("方言复制源", "https://src3.example.com",
                               "sk-dial-src-0123456789abcdef", "m",
                               /*activateIfNoneActive=*/false,
                               /*sharedConfigId=*/0, "both", "opencode_zen");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_CHECK_EQ(a.value().dialectOverride, std::string("opencode_zen"));

    auto dup = svc.duplicateProfile(a.value().id);
    DOCTEST_REQUIRE(dup.isOk());
    DOCTEST_CHECK_EQ(dup.value().dialectOverride, std::string("opencode_zen"));
    auto g = svc.getProfile(dup.value().id);
    DOCTEST_REQUIRE(g.isOk());
    DOCTEST_CHECK_EQ(g.value().dialectOverride, std::string("opencode_zen"));

    cleanupKey(a.value().id);
    cleanupKey(dup.value().id);
    return true;
})

// ---- 36. 写侧 / 读侧规范化（非法值 → 空串 = 未覆盖，同 authField 口径）----

REGISTER_TEST(claude_service_dialect_override_invalid_normalized,
              "claude_service: dialectOverride 非法值写侧归一为空串", {
    using namespace module::claude;
    TestDb tdb("dialect_norm");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    // createProfile 传非法串 → 落库与返回值都是空串（未覆盖）
    auto a = svc.createProfile("方言规范化", "https://n.example.com",
                               "sk-dial-norm-0123456789abcdef", "m",
                               /*activateIfNoneActive=*/false,
                               /*sharedConfigId=*/0, "both", "OpencodeZen");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_CHECK(a.value().dialectOverride.empty());
    auto ga = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(ga.isOk());
    DOCTEST_CHECK(ga.value().dialectOverride.empty());

    // updateProfile 传非法串（直接构造 Profile）→ 读回空串
    Profile p;
    p.id = a.value().id;
    p.name = "方言规范化";
    p.baseUrl = "https://n.example.com";
    p.dialectOverride = "zen";
    DOCTEST_REQUIRE(svc.updateProfile(p).isOk());
    auto g = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(g.isOk());
    DOCTEST_CHECK(g.value().dialectOverride.empty());

    cleanupKey(a.value().id);
    return true;
})

// ---- 37. 反向同步不丢该字段（settings.json env 无对应键 = 不适用，但也不清）----

REGISTER_TEST(claude_service_dialect_override_survives_reverse_sync,
              "claude_service: 反向同步不丢 dialectOverride", {
    using namespace module::claude;
    TestDb tdb("dialect_sync");
    TempDir d("dialect_sync");
    auto paths = makeSyncPaths(d);

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    svc.setSettingsSyncPaths(paths);

    auto a = svc.createProfile("方言同步源", "https://old.example.com",
                               "sk-dial-sync-a-0123456789abcdef", "m",
                               /*activateIfNoneActive=*/false,
                               /*sharedConfigId=*/0, "both", "opencode_zen");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());

    auto b = svc.createProfile("方言同步目标", "https://b.example.com",
                               "sk-dial-sync-b-0123456789abcdef", "m");
    DOCTEST_REQUIRE(b.isOk());

    // live 侧带一条非守卫变化，保证确实发生了一次写库（不是整次 no-op 白过）
    d.write(paths.settingsFile, R"({"env": {
        "ANTHROPIC_BASE_URL": "https://new.example.com/",
        "ANTHROPIC_AUTH_TOKEN": "sk-new-upstream-9876543210",
        "MY_EXTRA": "extra-value"
    }})");

    DOCTEST_REQUIRE(svc.setActiveProfile(b.value().id).isOk());

    auto gotA = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(gotA.isOk());
    // 确实被反向同步写过（base_url 已收编）…
    DOCTEST_CHECK_EQ(gotA.value().baseUrl, std::string("https://new.example.com"));
    // …而方言覆盖原样保留（反向同步只碰 models/env/base_url/token，不碰它）
    DOCTEST_CHECK_EQ(gotA.value().dialectOverride, std::string("opencode_zen"));

    cleanupKey(a.value().id);
    cleanupKey(b.value().id);
    return true;
})

// ---- 运行（profile-run 设计 §5/§8；D-1 顺序：代理 → 激活 → 启动会话）----

REGISTER_TEST(claude_service_run_profile_starts_proxy_and_passes_spec,
              "claude_service: runProfile 拉起代理（configuredProxyPort）+ 假 launch 收到正确参数", {
    TestDb tdb("runprofile1");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    const int port = pickFreePort();
    DOCTEST_REQUIRE(port != 0);
    svc.setConfiguredProxyPort(port);
    DOCTEST_CHECK_EQ(svc.configuredProxyPort(), port);

    auto a = svc.createProfile("官方", "https://api.anthropic.com",
                               "sk-ant-run-0001", "");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());

    int calls = 0;
    platform::terminal::LaunchSpec got;
    svc.setTerminalLauncher([&](const platform::terminal::LaunchSpec& s) {
        ++calls;
        got = s;
        return core::Result<void>::ok();
    });

    // ① 代理未运行 → 用 configuredProxyPort 拉起（不是默认 8888）
    DOCTEST_CHECK(!svc.isProxyRunning());
    std::string hint;
    auto r = svc.runProfile(a.value().id, &hint);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(svc.isProxyRunning());
    DOCTEST_CHECK_EQ(svc.proxyPort(), port);

    // ④ 调用参数（title / cwd / program / args）
    DOCTEST_CHECK_EQ(calls, 1);
    DOCTEST_CHECK_EQ(got.title, std::string("官方 · Claude Code"));
    DOCTEST_CHECK_EQ(got.workingDir, platform::terminal::homeDir());
    DOCTEST_CHECK(!got.workingDir.empty());
    DOCTEST_CHECK_EQ(got.program, std::string("claude"));
    DOCTEST_CHECK(got.args.empty());

    svc.stopProxy();
    cleanupKey(a.value().id);
    return true;
})

REGISTER_TEST(claude_service_run_profile_proxy_start_failure_aborts,
              "claude_service: runProfile 代理起不来 → 返回 fail 且不开会话", {
    TestDb tdb("runprofile2");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    const int port = pickFreePort();
    DOCTEST_REQUIRE(port != 0);
    // 占住该端口 → startProxy 必失败（**独占**绑定：见 ExclusivePortGuard 注释）
    ExclusivePortGuard blocker(port);
    DOCTEST_REQUIRE(blocker.ok());
    svc.setConfiguredProxyPort(port);

    auto a = svc.createProfile("官方", "https://api.anthropic.com",
                               "sk-ant-run-0002", "");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());

    int calls = 0;
    svc.setTerminalLauncher([&](const platform::terminal::LaunchSpec&) {
        ++calls;
        return core::Result<void>::ok();
    });
    auto r = svc.runProfile(a.value().id, nullptr);
    DOCTEST_CHECK(r.isFail());     // 中止
    DOCTEST_CHECK_EQ(calls, 0);    // 不打开会话

    // 占位随 blocker 作用域结束自动释放（RAII），无需显式 stop
    cleanupKey(a.value().id);
    return true;
})

REGISTER_TEST(claude_service_run_profile_switches_only_when_inactive,
              "claude_service: runProfile 非激活 → 先切激活（失败即中止）；已激活 → 不切", {
    TestDb tdb("runprofile3");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 注入接管通道路径（不注入 → 反向同步整段跳过，②③ 就无从区分）
    TempSyncPaths paths("runprofile3");
    svc.setSettingsSyncPaths(paths.paths());

    auto a = svc.createProfile("甲", "https://a.example.com", "sk-ant-run-a", "");
    auto b = svc.createProfile("乙", "https://b.example.com", "sk-ant-run-b", "");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());

    const int port = pickFreePort();
    DOCTEST_REQUIRE(port != 0);
    svc.setConfiguredProxyPort(port);
    // 先起代理：避免 runProfile 内的 takeOver 覆盖掉下面写入的坏 settings.json
    DOCTEST_REQUIRE(svc.startProxy("127.0.0.1", port).isOk());

    int calls = 0;
    svc.setTerminalLauncher([&](const platform::terminal::LaunchSpec&) {
        ++calls;
        return core::Result<void>::ok();
    });

    // ③ 目标 = 激活 profile：跳过 setActiveProfile → 坏 settings.json 不参与 → 成功
    paths.writeSettings("{ 这不是合法 JSON");
    std::string hint;
    auto r3 = svc.runProfile(a.value().id, &hint);
    DOCTEST_CHECK(r3.isOk());
    DOCTEST_CHECK_EQ(calls, 1);

    // ② 目标 ≠ 激活 profile：会调 setActiveProfile → 反向同步读坏文件失败 → 中止且不开会话
    paths.writeSettings("{ 这不是合法 JSON");
    auto r2 = svc.runProfile(b.value().id, nullptr);
    DOCTEST_CHECK(r2.isFail());
    DOCTEST_CHECK_EQ(calls, 1);                       // 未新增调用
    auto active = svc.getActiveProfile();
    DOCTEST_REQUIRE(active.isOk());
    DOCTEST_CHECK_EQ(active.value().id, a.value().id);  // 激活态未被改（中止在事务前）

    svc.stopProxy();
    cleanupKey(a.value().id);
    cleanupKey(b.value().id);
    return true;
})

REGISTER_TEST(claude_service_run_profile_invalid_id,
              "claude_service: runProfile id=0 / 不存在 → fail（不开会话）", {
    TestDb tdb("runprofile4");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    int calls = 0;
    svc.setTerminalLauncher([&](const platform::terminal::LaunchSpec&) {
        ++calls;
        return core::Result<void>::ok();
    });
    auto zero = svc.runProfile(0, nullptr);
    DOCTEST_CHECK(zero.isFail());
    DOCTEST_CHECK(zero.errorCode() == core::ErrorCode::ValidationError);
    auto missing = svc.runProfile(9999, nullptr);
    DOCTEST_CHECK(missing.isFail());
    DOCTEST_CHECK_EQ(calls, 0);
    return true;
})
