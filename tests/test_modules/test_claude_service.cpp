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
#include "platform/json.h"
#include "platform/credential_protection.h"
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
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>   // 审计 F3：run-settings 权限位断言
#endif

namespace {

constexpr const char* kTestKeychainService = "DevDeskTestClaudeService";

// 临时 db 文件名前缀（共享工厂 test_support::tempDbPath / TestDb 的第一实参）
constexpr const char* kDbPrefix = "devdesk_test_claude_";

// 清某个 profile 的测试密钥 / 头名大小写不敏感取头与计数：共享实现
// （原 cleanupKey / headerCI / countCI 同款合流，TIN-06b；service 传本文件常量）
using test_support::countCI;
using test_support::DataRootEnv;
using test_support::headerCI;
using test_support::HeaderMap;

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

// 接管通道用的临时 settings.json 文件（绝不碰真实 ~/.claude）。
// runProfile 零全局回归闸（D-1）用它断言「settings.json 未被写、sidecar 未建」。
//
// 2026-10-03（批次 E2）：sidecar 已不是文件，而是 app_config 表的一行，故本
// 结构**不再持有 sidecar 路径**，改持一个已 migrate 的连接供 paths() 填 db。
struct TempSyncPaths {
    std::filesystem::path settings;
    core::data::SqliteDb* db = nullptr;

    TempSyncPaths(const std::string& tag, core::data::SqliteDb& database) {
        const std::string base = test_support::tempDbPath(kDbPrefix, tag).string();   // 复用同一 tempPath 工厂
        settings = base + ".settings.json";
        db = &database;
        std::error_code ec;
        std::filesystem::remove(settings, ec);
    }
    ~TempSyncPaths() {
        std::error_code ec;
        std::filesystem::remove(settings, ec);
    }

    void writeSettings(const std::string& content) const {
        std::ofstream(settings, std::ios::binary | std::ios::trunc) << content;
    }
    module::claude::settings_sync::Paths paths() const {
        module::claude::settings_sync::Paths p;
        p.settingsFile = settings;
        p.db = db;
        return p;
    }
};

// 整文件读成字符串（settings.json 未变断言与临时 settings 内容断言共用）
std::string readFileToString(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

// 反向同步（M3）测试基建：临时 settings.json 目录（共享头 test_support::TempDir，
// 前缀沿用原样；绝不碰真实 ~/.claude）
constexpr const char* kRevTempDirPrefix = "devdesk_test_claude_rev_";

// sidecar 自 2026-10-03（批次 E2）起是 app_config 表的一行，故这里要一并传入
// 已 migrate 的连接（测试侧一律用同一个 tdb，避免各用例各开一库）。
module::claude::settings_sync::Paths makeSyncPaths(const test_support::TempDir& d,
                                                   core::data::SqliteDb& db) {
    module::claude::settings_sync::Paths p;
    p.settingsFile = d.file("settings.json");
    p.db = &db;
    return p;
}

} // namespace

// ---- 1. createProfile + getProfile 往返 ----

REGISTER_TEST(claude_service_create_get_profile,
              "claude_service: createProfile + getProfile 往返", {
    test_support::TestDb tdb(kDbPrefix, "create_get");
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
    // api_key_check = **全量** SHA256 hex（64 字符；2026-10-02 存储统一前是
    // 前 8 hex「防误操作校验」，旧前提「与 Keychain 明文比对」已消失）
    DOCTEST_CHECK_EQ(p.apiKeyCheck.size(), static_cast<size_t>(64));

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

    test_support::cleanupProfileKey(kTestKeychainService, p.id);
    return true;
})

// ---- 2. listProfiles ----

REGISTER_TEST(claude_service_list_profiles,
              "claude_service: listProfiles 返回全部 profile", {
    test_support::TestDb tdb(kDbPrefix, "list");
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

    return true;
})

// ---- 3. setActiveProfile + getActiveProfile ----

REGISTER_TEST(claude_service_set_active_profile,
              "claude_service: setActiveProfile 切换激活（唯一）", {
    test_support::TestDb tdb(kDbPrefix, "active");
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

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// ---- 3b. 首建自动激活（产品审计 2026-09-23 [6]） ----

REGISTER_TEST(claude_service_first_create_auto_activates,
              "claude_service: createProfile 首建自动激活 + 不抢占 + 503 指引文案", {
    test_support::TestDb tdb(kDbPrefix, "autoactivate");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 1) 默认参数保持旧行为：不激活（既有调用方/测试零影响）
    auto legacy = svc.createProfile("Legacy", "https://legacy.example.com",
                                    "sk-ant-legacy-0001", "m");
    DOCTEST_REQUIRE(legacy.isOk());
    DOCTEST_CHECK_FALSE(legacy.value().isActive);
    DOCTEST_CHECK(svc.getActiveProfile().isFail());
    test_support::cleanupProfileKey(kTestKeychainService, legacy.value().id);

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

    test_support::cleanupProfileKey(kTestKeychainService, second.value().id);
    return true;
})

// ---- 3c. 配置代理端口（产品审计 2026-09-23 [14]） ----

REGISTER_TEST(claude_service_configured_proxy_port,
              "claude_service: setConfiguredProxyPort 默认/钳制 + 换端口重启保 token", {
    test_support::TestDb tdb(kDbPrefix, "cfgport");
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
    test_support::TestDb tdb(kDbPrefix, "loadkey");
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

    test_support::cleanupProfileKey(kTestKeychainService, id);
    return true;
})

// ---- 5. updateProfile ----

REGISTER_TEST(claude_service_update_profile,
              "claude_service: updateProfile 改字段 + 可选换 key", {
    test_support::TestDb tdb(kDbPrefix, "update");
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

    test_support::cleanupProfileKey(kTestKeychainService, p.id);
    return true;
})

// ---- 5b. CPX-07：两段写失败补偿（审计「Keychain + DB 两段写」条目） ----

REGISTER_TEST(claude_service_update_db_fail_rolls_back_keychain,
              "claude_service: updateProfile DB 写失败按旧值回写 Keychain（CPX-07）", {
    test_support::TestDb tdb(kDbPrefix, "updcomp");
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

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, bid);
    test_support::cleanupProfileKey(kTestKeychainService, twin.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, other.value().id);
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

// ---------- 凭据保护注入缝（2026-10-02 存储统一）----------
// 迁移前「段一写失败」的注入面在 Keychain 后端；凭据进库后该失败点变成
// platform::credential::protect / unprotect，注入面随之挪过来。形态与
// ScopedFakeKeychain 一字同款（RAII，析构必复位）。
core::Result<std::string> failProtect(const std::string&) {
    return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                           "注入：protect 失败");
}

core::Result<std::string> failUnprotect(const std::string&) {
    return core::Result<std::string>::fail(
        core::ErrorCode::Internal, "注入：列形态损坏，解不开");
}

// protect 恒失败（模拟 Windows DPAPI 报错 / 平台保护不可用）
struct ScopedFailProtect {
    platform::credential::Backend table;
    const platform::credential::Backend* prev;
    ScopedFailProtect() {
        table = platform::credential::Backend{
            &failProtect, &platform::credential::realUnprotect};
        prev = platform::credential::setBackendForTest(&table);
    }
    ~ScopedFailProtect() { platform::credential::setBackendForTest(prev); }
    ScopedFailProtect(const ScopedFailProtect&) = delete;
    ScopedFailProtect& operator=(const ScopedFailProtect&) = delete;
};

// unprotect 恒失败（模拟换机 / 换用户 / 列被篡改）
struct ScopedFailUnprotect {
    platform::credential::Backend table;
    const platform::credential::Backend* prev;
    ScopedFailUnprotect() {
        table = platform::credential::Backend{
            &platform::credential::realProtect, &failUnprotect};
        prev = platform::credential::setBackendForTest(&table);
    }
    ~ScopedFailUnprotect() { platform::credential::setBackendForTest(prev); }
    ScopedFailUnprotect(const ScopedFailUnprotect&) = delete;
    ScopedFailUnprotect& operator=(const ScopedFailUnprotect&) = delete;
};

} // namespace

// 段一（凭据三列）失败 → 旧值完好（写入前就失败，不留半截）
REGISTER_TEST(claude_cpx14_stage1_key_write_fail_restores_old,
              "claude_service: CPX-14 段一保护失败 → 旧密钥完好", {
    test_support::TestDb tdb(kDbPrefix, "cpx14stage1");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile("Outer", "https://a.example.com",
                                     "sk-ant-old-0001", "m");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;

    // 注入：新密钥写入时 protect 失败（旧值 = 当前凭据，先捕获）
    {
        ScopedFailProtect cred;
        module::claude::Profile p = created.value();
        p.apiKeyPlain = "sk-ant-new-0002";
        auto upd = svc.updateProfile(p);
        DOCTEST_CHECK(upd.isFail());
    }

    // 段一在落任何字节之前失败 → 旧值**压根没被动过**（比「写坏再回写」更强）
    auto key = svc.loadApiKey(id);
    DOCTEST_REQUIRE(key.isOk());
    DOCTEST_CHECK_EQ(key.value(), std::string("sk-ant-old-0001"));
    // 段一失败即返回，段二没跑过：DB 元数据与旧密钥仍一致
    auto got = svc.getProfile(id);
    DOCTEST_REQUIRE(got.isOk());
    DOCTEST_CHECK_EQ(got.value().apiKeyCheck, created.value().apiKeyCheck);
    DOCTEST_CHECK_EQ(got.value().apiKeyHint, created.value().apiKeyHint);

    // 正常路径未被注入层带坏（同一用例内换回合法密钥 → 写入成功）
    module::claude::Profile p2 = created.value();
    p2.apiKeyPlain = "sk-ant-new-0002";
    DOCTEST_CHECK(svc.updateProfile(p2).isOk());
    auto key2 = svc.loadApiKey(id);
    DOCTEST_REQUIRE(key2.isOk());
    DOCTEST_CHECK_EQ(key2.value(), std::string("sk-ant-new-0002"));
    // 判据真的在跟：全量 SHA256 随新密钥重算
    auto got2 = svc.getProfile(id);
    DOCTEST_REQUIRE(got2.isOk());
    DOCTEST_CHECK_NE(got2.value().apiKeyCheck, created.value().apiKeyCheck);
    DOCTEST_CHECK_EQ(got2.value().apiKeyCheck.size(), static_cast<size_t>(64));
    return true;
})

// 段二（DB 删行）失败 → 凭据已清时补偿回写 + 行保留可重试
REGISTER_TEST(claude_cpx14_delete_stage2_fail_restores_key,
              "claude_service: CPX-14 段二删行失败回写已清凭据", {
    test_support::TestDb tdb(kDbPrefix, "cpx14delstage2");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile("Victim", "https://v.example.com",
                                     "sk-ant-victim-0003", "m");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;

    // 段二注入失败：BEFORE DELETE 触发器 RAISE(ABORT) → DELETE 语句确定性失败
    DOCTEST_REQUIRE(tdb.handle().exec(
        "CREATE TRIGGER cpx14_block_delete BEFORE DELETE ON claude_profiles "
        "BEGIN SELECT RAISE(ABORT, 'cpx14: delete blocked'); END").isOk());

    auto del = svc.deleteProfile(id);
    DOCTEST_CHECK(del.isFail());

    // 段一确实清过凭据列（不是「段一失败即中止」）：段二失败后补偿把它写回来
    auto k = svc.loadApiKey(id);
    DOCTEST_REQUIRE(k.isOk());
    DOCTEST_CHECK_EQ(k.value(), std::string("sk-ant-victim-0003"));
    // 行保留（用户可重试，密钥没丢）
    DOCTEST_CHECK(svc.getProfile(id).isOk());

    // 撤掉注入源后重试 → 成功，行没了（凭据随之不可达）
    DOCTEST_REQUIRE(tdb.handle().exec("DROP TRIGGER cpx14_block_delete").isOk());
    DOCTEST_CHECK(svc.deleteProfile(id).isOk());
    DOCTEST_CHECK(svc.getProfile(id).isFail());
    return true;
})

// 建行失败零残留（2026-10-02 存储统一：凭据与元数据同一条 INSERT，天然原子）
// 旧实现是「段一 Keychain + 段二回填 account + 任一失败回滚 INSERT」，回滚路径
// 随两存储合并而删除——本用例证明两条失败路径都不留半截行 / 半截凭据。
REGISTER_TEST(claude_cpx14_create_fail_no_residue,
              "claude_service: 建行失败零残留（重名 / 保护失败）", {
    test_support::TestDb tdb(kDbPrefix, "createfail");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto base = svc.createProfile("Base", "https://base.example.com",
                                  "sk-ant-base-0004", "m");
    DOCTEST_REQUIRE(base.isOk());
    auto before = svc.listProfiles();
    DOCTEST_REQUIRE(before.isOk());
    const size_t countBefore = before.value().size();

    // 路径一：重名 → INSERT 撞 UNIQUE，单条语句原子失败，一行都不落
    auto dup = svc.createProfile("Base", "https://d.example.com",
                                 "sk-ant-doomed-0005", "m");
    DOCTEST_CHECK(dup.isFail());
    DOCTEST_CHECK(dup.errorCode() == core::ErrorCode::ProfileAlreadyExists);

    // 路径二：protect 失败 → 在 INSERT **之前**失败
    {
        ScopedFailProtect cred;
        auto doomed = svc.createProfile("Doomed", "https://d.example.com",
                                        "sk-ant-doomed-0006", "m");
        DOCTEST_CHECK(doomed.isFail());
    }

    auto after = svc.listProfiles();
    DOCTEST_REQUIRE(after.isOk());
    DOCTEST_CHECK_EQ(after.value().size(), countBefore);

    // 基线那行的密钥没被两次失败带坏
    auto baseKey = svc.loadApiKey(base.value().id);
    DOCTEST_REQUIRE(baseKey.isOk());
    DOCTEST_CHECK_EQ(baseKey.value(), std::string("sk-ant-base-0004"));

    // 撤掉注入源后重试 → 成功（正常路径未被注入层带坏）
    auto ok = svc.createProfile("Doomed", "https://d.example.com",
                                "sk-ant-doomed-0006", "m");
    DOCTEST_REQUIRE(ok.isOk());
    auto okKey = svc.loadApiKey(ok.value().id);
    DOCTEST_REQUIRE(okKey.isOk());
    DOCTEST_CHECK_EQ(okKey.value(), std::string("sk-ant-doomed-0006"));
    return true;
})

// ---- 6. deleteProfile 清理 SQLite + Keychain ----

REGISTER_TEST(claude_service_delete_profile,
              "claude_service: deleteProfile 清 SQLite + Keychain", {
    test_support::TestDb tdb(kDbPrefix, "delete");
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

// ---- 6b. deleteProfile：Keychain 清理失败时中止（已移除） ----
// E-1/E-2 凭据入库（cbccb96）后 createProfile 只把密钥写进 SQLite 的
// api_key_encrypted 列、不再写 Keychain 文件；deleteProfile 也只清 DB 凭据列
// + 删行，不再调 platform::keychain::deleteSecret。本用例的两个前提（createProfile
// 会写 Keychain 文件 / deleteProfile 删 Keychain 失败时中止）都已失效——
// 断言 exists(keyPath) 必红（文件从不被建）、r.isFail() 必红（deleteProfile 不再
// 碰 Keychain，必成功）。旧的「把密钥文件换非空目录注入删除失败」的注入机制
// 依赖文件系统 Keychain 存储，迁库后无对应注入点。安全属性（凭据清理失败时
// 中止 deleteProfile、行保留可重试）改由 TwoPhaseKeyWrite 在 DB 层结构性保证
//（profile_repository.cpp:675-683：先清三列、失败即 return，行不动）。
// 此处留注释不留 SKIP：该行为已不存在，SKIP 会留一个「该跑但跳过」的假象。

// ---- 6c. 启动时孤儿 Keychain 清理 ----

REGISTER_TEST(claude_service_orphan_keychain_cleanup,
              "claude_service: cleanupOrphanKeychainEntries 清孤儿密钥", {
    test_support::TestDb tdb(kDbPrefix, "orphan");
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

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// ---- 6c-2. P0 护栏①（2026-09-30 数据丢失事故）：空数据根 → 零删除 ----
// 事故形态：keychain service "DevDesk" 是全局命名空间，不随 DEVDESK_DATA_ROOT
// 隔离；空根实例（测试 / 误设环境变量）启动时库中零 profile → keep 为空集合，
// 而 deleteSecretsNotIn 是「凡不在 keep 里的都删」→ 用户真实密钥被清光。
// 护栏①：keep 为空直接跳过清理（连扫描都不发起，一个条目都不碰）。
// 注入缝把「用户真实密钥」做成假 keychain 里的事件，断言它一字未动。
REGISTER_TEST(claude_cleanup_empty_keep_never_sweeps,
              "claude_service: P0 空 profile 孤儿清理零删除（护栏①）", {
    test_support::TestDb tdb(kDbPrefix, "orphanguard");
    ScopedFakeKeychain kc;
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 假 keychain = 用户真实命名空间的替身：profile 密钥 + 未来其它用途条目
    DOCTEST_REQUIRE(platform::keychain::storeSecret(
        kTestKeychainService, "claude:7", "sk-ant-user-real").isOk());
    DOCTEST_REQUIRE(platform::keychain::storeSecret(
        kTestKeychainService, "notes:theme", "dark").isOk());

    // 空数据根形态：库中零 profile
    auto list = svc.listProfiles();
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_CHECK_EQ(static_cast<int>(list.value().size()), 0);

    auto r = svc.cleanupOrphanKeychainEntries();
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value(), 0);   // 零删除

    // 「用户密钥」原样还在（值逐字相同）
    auto k = platform::keychain::loadSecret(kTestKeychainService, "claude:7");
    DOCTEST_REQUIRE(k.isOk());
    DOCTEST_CHECK_EQ(k.value(), std::string("sk-ant-user-real"));
    auto o = platform::keychain::loadSecret(kTestKeychainService, "notes:theme");
    DOCTEST_REQUIRE(o.isOk());
    DOCTEST_CHECK_EQ(o.value(), std::string("dark"));
    return true;
})

// ---- 6c-3. 服务名按数据根派生 ----

REGISTER_TEST(claude_keychain_service_for_data_root,
              "claude_service: keychainServiceForDataRoot 派生纯函数", {
    const std::string a =
        module::claude::keychainServiceForDataRoot("/tmp/rootA");
    const std::string b =
        module::claude::keychainServiceForDataRoot("/tmp/rootB");
    // 形态：DevDesk-<8 hex>
    DOCTEST_CHECK(a.rfind("DevDesk-", 0) == 0);
    DOCTEST_CHECK_EQ(a.size(), size_t{16});
    // 不同根 → 不同 service（数据根隔离的根基）
    DOCTEST_CHECK(a != b);
    // 同根 → 稳定（跨次启动一致）
    DOCTEST_CHECK_EQ(a, module::claude::keychainServiceForDataRoot("/tmp/rootA"));
    // 绝不与裸前缀同形（派生名必带短哈希后缀）
    DOCTEST_CHECK(a != std::string("DevDesk"));
    // 空根 → 空串（调用方兜底裸前缀）
    DOCTEST_CHECK(module::claude::keychainServiceForDataRoot("").empty());
    return true;
})

// ---- 7. createProfile 校验：重名 / 空字段 ----

REGISTER_TEST(claude_service_create_validation,
              "claude_service: createProfile 校验重名 + 空字段", {
    test_support::TestDb tdb(kDbPrefix, "validate");
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

    test_support::cleanupProfileKey(kTestKeychainService, ok1.value().id);
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

    test_support::TestDb tdb(kDbPrefix, "testconn");
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

    test_support::cleanupProfileKey(kTestKeychainService, id);
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

    test_support::TestDb tdb(kDbPrefix, "conn_dialect");
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
    test_support::cleanupProfileKey(kTestKeychainService, created.value().id);
    return true;
})

// ---- 8c. testConnection 接 profile.dialectOverride（CPX-13 接线）----
// 「探测与转发同源」的最后一环：profile 钉住方言时，探测请求必须和两条转发
// 路径（proxy_router / streaming_proxy）按同一方言装头，否则配了覆盖的
// profile 会出现「探测按 A 方言、转发按 B 方言」的错位。
// 断言的是上游实际收到的头（探测路径的真实效果），不是 forBaseUrl 的返回值：
// host 是 loopback（推断 = 默认方言），覆盖 opencode_zen → 会话头必须出现。

REGISTER_TEST(claude_service_test_connection_dialect_override,
              "claude_service: testConnection 接 profile.dialectOverride", {
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

    test_support::TestDb tdb(kDbPrefix, "conn_ovr");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    const std::string loopbackUrl = "http://127.0.0.1:" + std::to_string(port);
    // 第 8 个参数 = dialectOverride（值域见 upstream_dialect.h）
    auto created = svc.createProfile("ConnOvr", loopbackUrl,
                                     "sk-ant-conn-ovr-0001", "m",
                                     /*activateIfNoneActive=*/false,
                                     /*sharedConfigId=*/0, "both",
                                     "opencode_zen");
    DOCTEST_REQUIRE(created.isOk());

    // ---- (1) 覆盖 opencode_zen：loopback 推断不出 zen → 会话头出现即证明接了覆盖 ----
    auto resp = svc.testConnection(created.value());
    DOCTEST_REQUIRE(resp.isOk());
    DOCTEST_CHECK_EQ(resp.value().statusCode, 200);
    DOCTEST_CHECK_EQ(hitCount, 1);
    DOCTEST_CHECK_EQ(headerCI(seen, "x-opencode-session"),
                     module::claude::claudeUpstreamSessionId());
    DOCTEST_CHECK_EQ(countCI(seen, "x-opencode-session"), 1);
    // 方言层其余行为照旧
    DOCTEST_CHECK_EQ(headerCI(seen, "user-agent"),
                     std::string(module::claude::kProxyUserAgent));
    DOCTEST_CHECK_EQ(countCI(seen, "user-agent"), 1);
    DOCTEST_CHECK_EQ(headerCI(seen, "x-api-key"),
                     std::string("sk-ant-conn-ovr-0001"));
    DOCTEST_CHECK_EQ(headerCI(seen, "anthropic-version"), std::string("2023-06-01"));

    // ---- (2) 空串 = 未覆盖：与改动前逐项一致（不注入会话头）----
    module::claude::Profile p = created.value();
    p.dialectOverride.clear();
    seen.clear();
    auto resp2 = svc.testConnection(p);
    DOCTEST_REQUIRE(resp2.isOk());
    DOCTEST_CHECK_EQ(resp2.value().statusCode, 200);
    DOCTEST_CHECK_EQ(hitCount, 2);
    DOCTEST_CHECK_EQ(countCI(seen, "x-opencode-session"), 0);
    DOCTEST_CHECK_EQ(headerCI(seen, "user-agent"),
                     std::string(module::claude::kProxyUserAgent));
    DOCTEST_CHECK_EQ(countCI(seen, "user-agent"), 1);
    DOCTEST_CHECK_EQ(headerCI(seen, "x-api-key"),
                     std::string("sk-ant-conn-ovr-0001"));
    DOCTEST_CHECK_EQ(headerCI(seen, "authorization"),
                     std::string("Bearer sk-ant-conn-ovr-0001"));
    DOCTEST_CHECK_EQ(headerCI(seen, "anthropic-version"), std::string("2023-06-01"));

    upstream.stop();
    test_support::cleanupProfileKey(kTestKeychainService, created.value().id);
    return true;
})

// ---- 9. testConnection 网络不可达 ----

REGISTER_TEST(claude_service_test_connection_unreachable,
              "claude_service: testConnection 上游不可达返回失败", {
    test_support::TestDb tdb(kDbPrefix, "connfail");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 用一个几乎肯定会拒绝连接的端口
    auto created = svc.createProfile("Bad", "http://127.0.0.1:1",
                                     "sk-ant-bad-7777", "m");
    DOCTEST_REQUIRE(created.isOk());

    auto resp = svc.testConnection(created.value());
    DOCTEST_CHECK(resp.isFail());
    DOCTEST_CHECK(resp.errorCode() == core::ErrorCode::NetworkUnreachable);

    test_support::cleanupProfileKey(kTestKeychainService, created.value().id);
    return true;
})

// ---- 10. logCall / recentCalls ----

REGISTER_TEST(claude_service_call_logs,
              "claude_service: logCall + recentCalls 往返", {
    test_support::TestDb tdb(kDbPrefix, "logs");
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

    test_support::cleanupProfileKey(kTestKeychainService, id);
    return true;
})

// ---- 10a+. 最近代理请求事件槽（M2 §4.5 气泡③，产品审计 [18]）----
// recordProxyEvent/takeProxyEvent：take 读走即清（一事件一泡）、单槽后写
// 覆盖先写、滞留超 kProxyEventFreshSec 过期丢弃、时钟回摆不误吞

REGISTER_TEST(claude_proxy_event_slot,
              "claude_service: 代理事件槽 take-once/覆盖/过期语义", {
    test_support::TestDb tdb(kDbPrefix, "proxy_event");
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
    test_support::TestDb tdb(kDbPrefix, "callquery");
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

    test_support::cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// ---- 11. startProxy / stopProxy / isProxyRunning ----

REGISTER_TEST(claude_service_proxy_lifecycle,
              "claude_service: startProxy / stopProxy 生命周期", {
    test_support::TestDb tdb(kDbPrefix, "proxy");
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
    test_support::TestDb tdb(kDbPrefix, "revcap");
    test_support::TempDir d("revcap", kRevTempDirPrefix);
    auto paths = makeSyncPaths(d, tdb.handle());

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

    test_support::cleanupProfileKey(kTestKeychainService, pa.id);
    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// ---- 13. ② 守卫键不写也不清（loopback BASE_URL 联动 token / 占位 / 空值）----
// 每形态都同批带一个**非守卫**变化（MY_EXTRA），保证确实发生了一次写库——
// 否则「没被覆盖」会因为整次 no-op 而白白通过
// 注：a/b 交替激活是为了让「被捕获的那条」始终是 a

REGISTER_TEST(claude_reverse_guard_keys_not_touched,
              "claude_service: 反向同步② 守卫键不写也不清", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "revguard");
    test_support::TempDir d("revguard", kRevTempDirPrefix);
    auto paths = makeSyncPaths(d, tdb.handle());

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

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// ---- 14. ③ 缺席即清空（非守卫托管字段）----

REGISTER_TEST(claude_reverse_absent_clears,
              "claude_service: 反向同步③ 缺席即清空", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "revclear");
    test_support::TempDir d("revclear", kRevTempDirPrefix);
    auto paths = makeSyncPaths(d, tdb.handle());

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
    DOCTEST_CHECK(g.value().apiKeyHint.empty());       // 三列一并清空
    DOCTEST_CHECK(g.value().apiKeyCheck.empty());
    auto k = svc.loadApiKey(a.value().id);             // 凭据列已清
    DOCTEST_CHECK(k.isFail());
    DOCTEST_CHECK(k.errorCode() == core::ErrorCode::NotFound);
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

    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// ---- 15. ④ 坏 JSON / env 非对象 → 中止切换（S-2）且 profile 一字未改 ----

REGISTER_TEST(claude_reverse_broken_json_aborts,
              "claude_service: 反向同步④ 坏 JSON 中止切换且 profile 一字未改", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "revbad");
    test_support::TempDir d("revbad", kRevTempDirPrefix);
    auto paths = makeSyncPaths(d, tdb.handle());

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

    test_support::cleanupProfileKey(kTestKeychainService, pa.id);
    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// ---- 16. ⑤ 文件不存在 / 空文件 / 无 env → no-op 成功且照常切换 ----

REGISTER_TEST(claude_reverse_missing_or_empty_noop,
              "claude_service: 反向同步⑤ 缺失或空文件 no-op 且照常切换", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "revnoop");
    test_support::TempDir d("revnoop", kRevTempDirPrefix);
    auto paths = makeSyncPaths(d, tdb.handle());

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

    test_support::cleanupProfileKey(kTestKeychainService, pa.id);
    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// ---- 17. ⑥ 无激活 profile（G0）：坏 settings.json 也不挡首建自动激活 ----

REGISTER_TEST(claude_reverse_no_active_skips,
              "claude_service: 反向同步⑥ 无激活 profile 时不读文件", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "revnoact");
    test_support::TempDir d("revnoact", kRevTempDirPrefix);
    auto paths = makeSyncPaths(d, tdb.handle());

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

    test_support::cleanupProfileKey(kTestKeychainService, c.value().id);
    return true;
})

// ---- 18. ⑦ 无变化 → 零写入（不 UPDATE、不写 Keychain、不 bump updated_at）----

REGISTER_TEST(claude_reverse_no_change_zero_write,
              "claude_service: 反向同步⑦ 无变化零写入 updated_at 不变", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "revsame");
    test_support::TempDir d("revsame", kRevTempDirPrefix);
    auto paths = makeSyncPaths(d, tdb.handle());

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

    test_support::cleanupProfileKey(kTestKeychainService, pa.id);
    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// ---- 19. ⑧ sidecar 托管键（G1）：不写也不清 ----

REGISTER_TEST(claude_reverse_sidecar_managed_key_skipped,
              "claude_service: 反向同步⑧ sidecar 托管键不被覆盖", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "revside");
    test_support::TempDir d("revside", kRevTempDirPrefix);
    auto paths = makeSyncPaths(d, tdb.handle());

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
    test_support::writeRawConfigRow(
        &tdb.handle(), module::claude::settings_sync::kSidecarConfigKey,
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

    test_support::cleanupProfileKey(kTestKeychainService, pa.id);
    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// ===== Profile 复制（M4，设计 §4.1/§4.2/§4.3）=====

// ---- 20. 副本字段 / Keychain 全等同源、id 与 account 不同、不激活、源行未动 ----

REGISTER_TEST(claude_dup_profile_basic,
              "claude_service: 复制 profile 全等同源且不激活", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "dup");
    test_support::TempDir d("dup", kRevTempDirPrefix);
    auto paths = makeSyncPaths(d, tdb.handle());

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
    // 凭据本体现在在副本自己那一行里（旧形态是 Keychain 的 claude:<新 id> 条目）
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

    test_support::cleanupProfileKey(kTestKeychainService, pa.id);
    test_support::cleanupProfileKey(kTestKeychainService, cp.id);
    test_support::cleanupProfileKey(kTestKeychainService, dup2.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, dup3.value().id);
    return true;
})

// ---- 21. 源 key 缺失（Keychain 孤儿）→ fail 且不建行 ----

REGISTER_TEST(claude_dup_missing_key_fails,
              "claude_service: 复制时源密钥缺失 fail 且无新行", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "dupkey");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto a = svc.createProfile("缺钥源", "https://src.example.com",
                               "sk-src-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    // 造「没存过密钥」的形态（2026-10-02：凭据在库列里，不再靠删 Keychain 条目）
    DOCTEST_REQUIRE(test_support::clearStoredCredential(tdb.handle(), a.value().id));

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

// 复制失败三态（2026-09-30 复盘）：条目不存在 / 读失败 / 值为空串，文案可分辨
// 且带恢复提示——迁移前是「条目不存在 / 读失败 / 值为空串」三态，凭据进库后
// 「列空」与「值为空串」在单列模型下是同一种形态，收敛为**两态**：
//   列空（NotFound）→「密钥缺失」+ 恢复提示
//   读失败（非 NotFound，如 Windows DPAPI 解不开）→ 原样透传（读不了 ≠ 没有）
// 两态都确定性构造（不再依赖真实 Keychain 行为，macOS 上也能跑）。
REGISTER_TEST(claude_dup_missing_key_two_states,
              "claude_service: 复制失败两态（列空 / 解不开）+ 恢复提示", {
    test_support::TestDb tdb(kDbPrefix, "dup2state");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    const std::string hint = "编辑该 Profile 重新填一次 API Key 即可";

    auto created = svc.createProfile("两态源", "https://s.example.com",
                                     "sk-ant-2state-0001", "m");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;

    // 态一：凭据列为空（没有密钥）→「密钥缺失」+ 恢复提示
    DOCTEST_REQUIRE(tdb.handle().exec(
        "UPDATE claude_profiles SET api_key_encrypted = '', api_key_hint = '', "
        "api_key_check = '' WHERE id = " + std::to_string(id)).isOk());
    auto dupMissing = svc.duplicateProfile(id);
    DOCTEST_CHECK(dupMissing.isFail());
    DOCTEST_CHECK(dupMissing.errorCode() == core::ErrorCode::InvalidApiKey);
    DOCTEST_CHECK(dupMissing.errorMessage().find("密钥缺失") !=
                  std::string::npos);
    DOCTEST_CHECK(dupMissing.errorMessage().find(hint) != std::string::npos);

    // 恢复正常凭据（态二要正常读列）
    module::claude::Profile fix = created.value();
    fix.apiKeyPlain = "sk-ant-2state-0001";
    DOCTEST_REQUIRE(svc.updateProfile(fix).isOk());

    // 态二：解不开（非 NotFound 的真实错误，如换机 / 换用户 / 列被篡改）→
    // 原样透传错误码与信息（读不了 ≠ 没有，不能报「密钥缺失」误导用户去重填）
    {
        ScopedFailUnprotect cred;
        auto dupErr = svc.duplicateProfile(id);
        DOCTEST_CHECK(dupErr.isFail());
        DOCTEST_CHECK(dupErr.errorCode() == core::ErrorCode::Internal);
        DOCTEST_CHECK(dupErr.errorMessage().find("读取源 Profile 密钥失败") !=
                      std::string::npos);
        DOCTEST_CHECK(dupErr.errorMessage().find("注入：列形态损坏，解不开") !=
                      std::string::npos);
        DOCTEST_CHECK(dupErr.errorMessage().find("密钥缺失") ==
                      std::string::npos);
    }

    // 两个失败态都没建行（不建无 key 的副本）
    auto after = svc.listProfiles();
    DOCTEST_REQUIRE(after.isOk());
    DOCTEST_CHECK_EQ(static_cast<int>(after.value().size()), 1);

    // 撤掉注入源后复制成功（正常路径未被注入层带坏）
    auto ok = svc.duplicateProfile(id);
    DOCTEST_REQUIRE(ok.isOk());
    auto copyKey = svc.loadApiKey(ok.value().id);
    DOCTEST_REQUIRE(copyKey.isOk());
    DOCTEST_CHECK_EQ(copyKey.value(), std::string("sk-ant-2state-0001"));
    return true;
})

// ---- 22. 二段写失败 → 回滚（零残留：行没了、Keychain 条目也清了）----

REGISTER_TEST(claude_dup_stage2_rollback,
              "claude_service: 复制二段失败回滚且零残留", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "duprb");
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

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

// ---- 23. 共享配置片段：CRUD + 绑定/解绑（设计 §2.1/§2.2/§2.6）----

REGISTER_TEST(claude_shared_config_crud_and_binding,
              "claude_service: 片段 CRUD 与 profile 绑定/解绑", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "sharedcrud");
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
    DOCTEST_CHECK_EQ(list.value()[0].extraSettingsJson, f.value().extraSettingsJson);

    // 改（改名 + 改 env）；不存在的 id → NotFound
    SharedConfig upd = f.value();
    upd.name = "片段A2";
    upd.extraSettingsJson = R"({"MY_KEY": "v2", "SECOND_KEY": "1"})";
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

    //（原「冻结键集（G10 用）/ 别名归一键集 / 解绑键集空」三段随
    // advanced-json-editor §8.4 拆除：片段不再携带 env 键，sharedEnvKeysFor
    // 与 G10 守卫同批移除；绑定/解绑的 DB 往返断言保留如下）
    // 解绑（updateProfile 置 0）→ DB 列回 0
    Profile unbind = ga.value();
    unbind.sharedConfigId = 0;
    DOCTEST_REQUIRE(svc.updateProfile(unbind).isOk());
    auto ga2 = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(ga2.isOk());
    DOCTEST_CHECK_EQ(ga2.value().sharedConfigId, 0);

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

// ---- 24. 删除片段：同事务解绑绑定它的 profile（不留悬空 id）----

REGISTER_TEST(claude_shared_config_delete_unbinds,
              "claude_service: 删片段同事务解绑绑定它的 profile", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "shareddel");
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

    //（原「悬空 id 读路径冻结键集为空」断言随 sharedEnvKeysFor 拆除——
    // advanced-json-editor §8.4；悬空绑定读路径容错由 getProfile 的
    // sharedConfigId==0 断言承接）

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, c.value().id);
    return true;
})

// ---- 25. 复制 profile 携带片段绑定（设计 §2.6）----

REGISTER_TEST(claude_shared_config_duplicate_copies_binding,
              "claude_service: 复制 profile 携带片段绑定", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "shareddup");
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

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, dup.value().id);
    return true;
})

// ---- 26. 未绑定片段的回归：列为 0、键集空（既有行为零变化）----

REGISTER_TEST(claude_shared_config_unbound_regression,
              "claude_service: 未绑定片段读路径零值（回归）", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "sharednone");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    DOCTEST_CHECK(svc.listSharedConfigs().value().empty());
    auto a = svc.createProfile("不绑", "https://a.example.com",
                               "sk-ind-test-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_CHECK_EQ(a.value().sharedConfigId, 0);
    auto ga = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(ga.isOk());
    DOCTEST_CHECK_EQ(ga.value().sharedConfigId, 0);

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

// ===== 高级设置 JSON（2026-09-30 advanced-json-editor 设计 §3.3）=====

// ---- 26b. Profile::extraSettingsJson 往返：createProfile 尾参 / updateProfile
//      两分支 / getProfile / listProfiles（「漏改 SELECT 列静默失效」的回归闸，
//      同 fallback_profile_id 口径）
REGISTER_TEST(claude_profile_extra_settings_roundtrip,
              "claude_service: extraSettingsJson createProfile 尾参 / updateProfile 两分支 / 三条 SELECT 往返", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "xsettings_rt");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    const std::string settingsJson = R"({"model": "glm-5.3", "permissions": {"allow": ["Bash"]}})";
    auto a = svc.createProfile("带JSON", "https://a.example.com",
                               "sk-xs-test-0123456789abcdef", "",
                               /*activateIfNoneActive=*/false,
                               /*sharedConfigId=*/0, "both", "",
                               /*fallbackProfileId=*/0,
                               /*extraSettingsJson=*/settingsJson);
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_CHECK_EQ(a.value().extraSettingsJson, settingsJson);

    // 未传尾参（既有调用方）→ 默认空串，既有行为零变化
    auto b = svc.createProfile("无JSON", "https://b.example.com",
                               "sk-xs-b-0123456789abcdef", "");
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_CHECK_EQ(b.value().extraSettingsJson, std::string());

    // 三条 SELECT 都要读到（rowToProfile 的 if (isOk()) 守卫会把缺列吞成空串）
    auto ga = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(ga.isOk());
    DOCTEST_CHECK_EQ(ga.value().extraSettingsJson, settingsJson);
    auto la = svc.listProfiles();
    DOCTEST_REQUIRE(la.isOk());
    bool found = false;
    for (const auto& p : la.value()) {
        if (p.id == a.value().id) {
            found = true;
            DOCTEST_CHECK_EQ(p.extraSettingsJson, settingsJson);
        }
    }
    DOCTEST_CHECK(found);
    // 第三条：getActiveProfile（代理接管链 claude_service.cpp:815 直供它，
    // 漏列同样被 rowToProfile 吞成空串——本用例原先缺这条腿，注释自称
    // 「三条 SELECT 往返」而实际只覆盖两条）。口径对齐兄弟用例 :27。
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());
    auto gact = svc.getActiveProfile();
    DOCTEST_REQUIRE(gact.isOk());
    DOCTEST_CHECK_EQ(gact.value().id, a.value().id);
    DOCTEST_CHECK_EQ(gact.value().extraSettingsJson, settingsJson);
    // 另一条（未带 JSON 的 b）激活后同样读到空串——正向断言不能被「恒非空」
    // 蒙混：缺列时这里会是非空吗？不会，缺列 = 空；这条钉的是列确实被读到
    // 而非碰巧相等（同 authField 用例的双向口径）。
    DOCTEST_REQUIRE(svc.setActiveProfile(b.value().id).isOk());
    auto gactB = svc.getActiveProfile();
    DOCTEST_REQUIRE(gactB.isOk());
    DOCTEST_CHECK_EQ(gactB.value().id, b.value().id);
    DOCTEST_CHECK_EQ(gactB.value().extraSettingsJson, std::string());
    // 交还：后续分支的 updateProfile 断言不依赖激活态，但留个确定态
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());

    // updateProfile 分支一（apiKeyPlain 非空）也携带该列
    Profile upd = ga.value();
    upd.apiKeyPlain = "sk-xs-new-0123456789abcdef";
    DOCTEST_REQUIRE(svc.updateProfile(upd).isOk());
    auto gu = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(gu.isOk());
    DOCTEST_CHECK_EQ(gu.value().extraSettingsJson, settingsJson);

    // updateProfile 分支二（只改元数据）：改 JSON 落库 + 清空落库
    Profile upd2 = gu.value();
    upd2.apiKeyPlain.clear();
    upd2.extraSettingsJson = R"({"statusLine": {"type": "command"}})";
    DOCTEST_REQUIRE(svc.updateProfile(upd2).isOk());
    auto gu2 = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(gu2.isOk());
    DOCTEST_CHECK_EQ(gu2.value().extraSettingsJson,
                     R"({"statusLine": {"type": "command"}})");
    upd2.extraSettingsJson.clear();
    DOCTEST_REQUIRE(svc.updateProfile(upd2).isOk());
    auto gu3 = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(gu3.isOk());
    DOCTEST_CHECK_EQ(gu3.value().extraSettingsJson, std::string());

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// ---- 26c. 复制链带 extraSettingsJson（审计 F-3）：纯 JSON profile（两个 env
//      字段均空、仅 extraSettingsJson 非空）复制不得静默丢字段——门槛条件
//      （models/extra_env 非空）不成立时，二段补写与 createProfile 尾参双通道
//      必须有一个把它带上
REGISTER_TEST(claude_profile_extra_settings_duplicate_pure_json,
              "claude_service: 纯 JSON profile 复制携带 extraSettingsJson（F-3 门槛）", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "xsettings_dup");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    const std::string settingsJson = R"({"hooks": {"PreToolUse": []}})";
    auto a = svc.createProfile("纯JSON源", "https://a.example.com",
                               "sk-xsd-test-0123456789abcdef", "",
                               /*activateIfNoneActive=*/false,
                               /*sharedConfigId=*/0, "both", "",
                               /*fallbackProfileId=*/0,
                               /*extraSettingsJson=*/settingsJson);
    DOCTEST_REQUIRE(a.isOk());
    auto ga = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(ga.isOk());
    DOCTEST_CHECK(ga.value().modelsConfigJson.empty());   // 前置：门槛不成立
    DOCTEST_CHECK(ga.value().extraEnvJson.empty());

    auto dup = svc.duplicateProfile(a.value().id);
    DOCTEST_REQUIRE(dup.isOk());
    auto gd = svc.getProfile(dup.value().id);
    DOCTEST_REQUIRE(gd.isOk());
    DOCTEST_CHECK_EQ(gd.value().extraSettingsJson, settingsJson);

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, dup.value().id);
    return true;
})

// ===== failover 备用（2026-09-27 shared-config-and-failover 设计 §3.7/§3.8）=====

// ---- 27a. 默认 0 + 三条 SELECT 往返（getActiveProfile 是「漏改 SELECT 列静默
//      失效」的回归闸，同 auth_field 口径）----

REGISTER_TEST(claude_service_fallback_default_and_roundtrip,
              "claude_service: failover 备用默认 0 + createProfile 尾参 / getProfile / "
              "getActiveProfile / listProfiles 往返", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "fb_rt");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto a = svc.createProfile("主家", "https://a.example.com",
                               "sk-a-test-0123456789abcdef", "m");
    DOCTEST_REQUIRE(a.isOk());
    auto b = svc.createProfile("备家", "https://b.example.com",
                               "sk-b-test-0123456789abcdef", "m",
                               /*activateIfNoneActive=*/false,
                               /*sharedConfigId=*/0, "both", "",
                               /*fallbackProfileId=*/a.value().id);
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_CHECK_EQ(b.value().fallbackProfileId, a.value().id);

    // 三条 SELECT 都要读到（rowToProfile 的 if (isOk()) 守卫会把缺列吞成 0）
    auto gb = svc.getProfile(b.value().id);
    DOCTEST_REQUIRE(gb.isOk());
    DOCTEST_CHECK_EQ(gb.value().fallbackProfileId, a.value().id);
    DOCTEST_REQUIRE(svc.setActiveProfile(b.value().id).isOk());
    auto gact = svc.getActiveProfile();
    DOCTEST_REQUIRE(gact.isOk());
    DOCTEST_CHECK_EQ(gact.value().fallbackProfileId, a.value().id);
    auto all = svc.listProfiles();
    DOCTEST_REQUIRE(all.isOk());
    bool listed = false;
    for (const auto& p : all.value()) {
        if (p.id == b.value().id && p.fallbackProfileId == a.value().id) {
            listed = true;
        }
    }
    DOCTEST_CHECK(listed);

    // 清回 0 = 不启用（updateProfile 空密钥分支也要写该列）
    Profile p = gb.value();
    p.fallbackProfileId = 0;
    DOCTEST_REQUIRE(svc.updateProfile(p).isOk());
    auto cleared = svc.getProfile(b.value().id);
    DOCTEST_REQUIRE(cleared.isOk());
    DOCTEST_CHECK_EQ(cleared.value().fallbackProfileId, 0);

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// ---- 27b. 自引用校验（设计 §3.7：拒 fallback_profile_id == id）----

REGISTER_TEST(claude_service_fallback_self_reference_rejected,
              "claude_service: failover 备用 = 自身被拒（updateProfile 校验）", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "fb_self");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto a = svc.createProfile("自身", "https://self.example.com",
                               "sk-self-test-0123456789abcdef", "m");
    DOCTEST_REQUIRE(a.isOk());
    Profile p = a.value();
    p.fallbackProfileId = p.id;
    auto r = svc.updateProfile(p);
    DOCTEST_REQUIRE(r.isFail());
    DOCTEST_CHECK(r.errorMessage().find("自身") != std::string::npos);
    // 行未被污染（仍是 0）
    auto ga = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(ga.isOk());
    DOCTEST_CHECK_EQ(ga.value().fallbackProfileId, 0);

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

// ---- 27c. 删除 profile 时同事务解绑「备用指向它」的行（设计 §3.7）----

REGISTER_TEST(claude_service_delete_profile_unbinds_fallback,
              "claude_service: failover 删 profile 解绑指向它的备用引用", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "fb_unbind");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto a = svc.createProfile("主家", "https://a.example.com",
                               "sk-a-test-0123456789abcdef", "m");
    DOCTEST_REQUIRE(a.isOk());
    auto b = svc.createProfile("备家", "https://b.example.com",
                               "sk-b-test-0123456789abcdef", "m");
    DOCTEST_REQUIRE(b.isOk());
    Profile p = a.value();
    p.fallbackProfileId = b.value().id;
    DOCTEST_REQUIRE(svc.updateProfile(p).isOk());

    // 删掉备用 → 主家解绑回 0
    DOCTEST_REQUIRE(svc.deleteProfile(b.value().id).isOk());
    auto ga = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(ga.isOk());
    DOCTEST_CHECK_EQ(ga.value().fallbackProfileId, 0);

    // 删带备用的行本身（清理）不炸
    DOCTEST_REQUIRE(svc.deleteProfile(a.value().id).isOk());

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// ---- 27d. 复制携带备用（设计 §3.7：备用属于配置，复制 = 拿到同一套）----

REGISTER_TEST(claude_service_duplicate_copies_fallback,
              "claude_service: failover duplicateProfile 携带备用 id", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "fb_dup");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto a = svc.createProfile("主家", "https://a.example.com",
                               "sk-a-test-0123456789abcdef", "m");
    DOCTEST_REQUIRE(a.isOk());
    auto b = svc.createProfile("备家", "https://b.example.com",
                               "sk-b-test-0123456789abcdef", "m");
    DOCTEST_REQUIRE(b.isOk());
    Profile p = a.value();
    p.fallbackProfileId = b.value().id;
    DOCTEST_REQUIRE(svc.updateProfile(p).isOk());

    auto dup = svc.duplicateProfile(a.value().id);
    DOCTEST_REQUIRE(dup.isOk());
    DOCTEST_CHECK_EQ(dup.value().fallbackProfileId, b.value().id);

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, dup.value().id);
    return true;
})

// ---- 27e. resolveFallbackProfile 读路径（设计 §3.8）：id==0 / 不存在 / 无 key
//      → nullopt；可用 → 带明文 key 的备用 ----

REGISTER_TEST(claude_service_resolve_fallback_profile,
              "claude_service: resolveFallbackProfile（无备用/失效/可用三态）", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "fb_resolve");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto a = svc.createProfile("主家", "https://a.example.com",
                               "sk-a-test-0123456789abcdef", "m");
    DOCTEST_REQUIRE(a.isOk());
    auto b = svc.createProfile("备家", "https://b.example.com",
                               "sk-b-test-0123456789abcdef", "m");
    DOCTEST_REQUIRE(b.isOk());

    // 未配备用（最常见路径，零 I/O）→ nullopt
    DOCTEST_CHECK_FALSE(svc.resolveFallbackProfile(a.value()).has_value());

    // 指向不存在的 profile → nullopt（读路径容错，原样回错不换家）
    Profile dangling = a.value();
    dangling.fallbackProfileId = 987654;
    DOCTEST_CHECK_FALSE(svc.resolveFallbackProfile(dangling).has_value());

    // 备用存在但凭据列空 → nullopt（「备用失效」档）
    Profile noKey = a.value();
    noKey.fallbackProfileId = b.value().id;
    DOCTEST_REQUIRE(test_support::clearStoredCredential(tdb.handle(), b.value().id));
    DOCTEST_CHECK_FALSE(svc.resolveFallbackProfile(noKey).has_value());

    // 备用可用 → 返回完整备用（含明文 key，供代理转发注入）
    // 上面已清空 → 先重建 key（经服务写回本行凭据列）
    {
        Profile reb = b.value();
        reb.apiKeyPlain = "sk-b-test-0123456789abcdef";
        DOCTEST_REQUIRE(svc.updateProfile(reb).isOk());
    }
    auto fb = svc.resolveFallbackProfile(noKey);
    DOCTEST_REQUIRE(fb.has_value());
    DOCTEST_CHECK_EQ(fb->id, b.value().id);
    DOCTEST_CHECK_EQ(fb->baseUrl, std::string("https://b.example.com"));
    DOCTEST_CHECK_EQ(fb->apiKeyPlain, std::string("sk-b-test-0123456789abcdef"));

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// ===== 认证字段三态（鉴权头；2026-09-27 profile-auth-field 设计 §3/§4）=====

// ---- 27. 默认 both + 三处 SELECT 往返（getActiveProfile 是「漏改 SELECT 列
//      静默失效」的回归闸：rowToProfile 的 if (isOk()) 守卫会把缺列吞成默认值）----

REGISTER_TEST(claude_service_auth_field_default_and_roundtrip,
              "claude_service: 认证字段默认 both + getProfile/getActiveProfile 往返", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "authfield_rt");
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

    test_support::cleanupProfileKey(kTestKeychainService, d.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// ---- 28. updateProfile 两条 UPDATE 分支都能改该字段 ----

REGISTER_TEST(claude_service_auth_field_update_both_branches,
              "claude_service: 认证字段 updateProfile 两分支（换 key / 不换 key）", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "authfield_upd");
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

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

// ---- 29. 二段补写不覆盖（createProfile 返回值必须带上该字段）----

REGISTER_TEST(claude_service_auth_field_survives_two_stage_update,
              "claude_service: 认证字段经二段补写（models/env）仍保持", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "authfield_two");
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

    test_support::cleanupProfileKey(kTestKeychainService, c.value().id);
    return true;
})

// ---- 30. 复制保留该字段 ----

REGISTER_TEST(claude_service_duplicate_keeps_auth_field,
              "claude_service: 复制 profile 保留认证字段", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "authfield_dup");
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

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, dup.value().id);
    return true;
})

// ---- 31. 写侧规范化（非法值 → both，设计 D-8）----

REGISTER_TEST(claude_service_auth_field_invalid_normalized,
              "claude_service: 认证字段非法值写侧规范化为 both", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "authfield_norm");
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

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

// ---- 32. 方言覆盖：默认空串 + 三处 SELECT 往返（CPX-13）----
// getActiveProfile 是「漏改 SELECT 列静默失效」的回归闸：rowToProfile 的
// if (isOk()) 守卫会把缺列吞成默认值

REGISTER_TEST(claude_service_dialect_override_default_and_roundtrip,
              "claude_service: dialectOverride 默认空串 + 三处 SELECT 往返", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "dialect_rt");
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

    test_support::cleanupProfileKey(kTestKeychainService, d.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, z.value().id);
    return true;
})

// ---- 33. updateProfile 两条 UPDATE 分支都能改该字段 ----

REGISTER_TEST(claude_service_dialect_override_update_both_branches,
              "claude_service: dialectOverride updateProfile 两分支（换 key / 不换 key）", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "dialect_upd");
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

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

// ---- 34. 二段补写不覆盖（createProfile 返回值必须带上该字段）----

REGISTER_TEST(claude_service_dialect_override_survives_two_stage_update,
              "claude_service: dialectOverride 经二段补写（models/env）仍保持", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "dialect_two");
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

    test_support::cleanupProfileKey(kTestKeychainService, c.value().id);
    return true;
})

// ---- 35. 复制保留该字段 ----

REGISTER_TEST(claude_service_duplicate_keeps_dialect_override,
              "claude_service: 复制 profile 保留 dialectOverride", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "dialect_dup");
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

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, dup.value().id);
    return true;
})

// ---- 36. 写侧 / 读侧规范化（非法值 → 空串 = 未覆盖，同 authField 口径）----

REGISTER_TEST(claude_service_dialect_override_invalid_normalized,
              "claude_service: dialectOverride 非法值写侧归一为空串", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "dialect_norm");
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

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

// ---- 37. 反向同步不丢该字段（settings.json env 无对应键 = 不适用，但也不清）----

REGISTER_TEST(claude_service_dialect_override_survives_reverse_sync,
              "claude_service: 反向同步不丢 dialectOverride", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "dialect_sync");
    test_support::TempDir d("dialect_sync");
    auto paths = makeSyncPaths(d, tdb.handle());

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

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// ---- 运行（profile-run 设计 §5/§8；D-1（V3）：直连——临时 settings + --settings，
//      零全局改动：不启代理、不切激活、不读写 ~/.claude/settings.json）----

// run-settings 临时设置文件权限（2026-10-03 安全审计 F3）：里面带**明文 API
// key**，必须 0600。平台分支放在函数体内（不能进 doctest 宏参数——宏参数里嵌
// 预处理指令是 UB，-Werror,-Wembedded-directive 直接编译失败）。
// Windows：DPAPI/ACL 口径，权限位不适用 → 恒 true（真实保护见 credential 用例）。
bool runSettingsFileIsOwnerOnly(const std::filesystem::path& p) {
#if defined(_WIN32)
    (void)p;
    return true;
#else
    struct stat st {};
    if (::stat(p.string().c_str(), &st) != 0) return false;
    return (static_cast<unsigned>(st.st_mode) & 07777u) == 0600u;
#endif
}

REGISTER_TEST(claude_service_run_profile_direct_env_writes_temp_settings,
              "claude_service: runProfile 直连——不启代理、写 run-settings 临时文件、假 launch 收到 --settings 参数", {
    test_support::DataRootEnv root(kDbPrefix, "runprofile1");
    test_support::TestDb tdb(kDbPrefix, "runprofile1");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 接管通道注入（指向临时文件）——零全局回归闸的一部分：settings.json 与
    // sidecar 都不该被本次运行触碰
    TempSyncPaths paths("runprofile1", tdb.handle());
    svc.setSettingsSyncPaths(paths.paths());
    const std::string settingsBefore = R"({
      "env": { "MY_EXISTING": "keep-me" },
      "other": 1
    })";
    paths.writeSettings(settingsBefore);

    auto a = svc.createProfile("官方", "https://api.anthropic.com/",
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

    // ① 不启代理：调用前后 isProxyRunning 恒 false
    DOCTEST_CHECK(!svc.isProxyRunning());
    std::string hint;
    auto r = svc.runProfile(a.value().id, &hint);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(!svc.isProxyRunning());

    // ② 假 launch 收到正确参数：--settings + run-settings/ 下的路径
    DOCTEST_CHECK_EQ(calls, 1);
    DOCTEST_CHECK_EQ(got.title, std::string("官方 · Claude Code"));
    DOCTEST_CHECK_EQ(got.workingDir, platform::terminal::homeDir());
    DOCTEST_CHECK(!got.workingDir.empty());
    DOCTEST_CHECK_EQ(got.program, std::string("claude"));
    DOCTEST_REQUIRE_EQ(got.args.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(got.args[0], std::string("--settings"));
    const std::filesystem::path settingsFile(got.args[1]);
    DOCTEST_CHECK(settingsFile.is_absolute());
    DOCTEST_CHECK_EQ(settingsFile.parent_path().string(),
                     (std::filesystem::path(root.dir) / "run-settings").string());
    DOCTEST_CHECK(settingsFile.filename().string().rfind("run-", 0) == 0);
    DOCTEST_CHECK_EQ(settingsFile.extension().string(), std::string(".json"));

    // ③ 落盘文件 = --settings 指向的同一路径；内容 = 合法 settings JSON（env 子对象）
    DOCTEST_REQUIRE(std::filesystem::exists(settingsFile));
    // ③b 权限 0600（安全审计 F3）：文件里有**明文 API key**。
    // atomicWriteFile 的 prepareRestrictedFile 使 tmp 出生即 0600；写入侧再过一道
    // restrictToOwnerOnly 做纵深防御，此处断言最终落盘必为 owner-only
    DOCTEST_CHECK(runSettingsFileIsOwnerOnly(settingsFile));
    auto parsed = platform::json::parse(readFileToString(settingsFile));
    DOCTEST_REQUIRE(parsed.isOk());
    DOCTEST_REQUIRE(parsed.value().isObject());
    auto envVal = parsed.value().asObject().get("env");
    DOCTEST_REQUIRE(envVal.isObject());
    auto envObj = envVal.asObject();
    // BASE_URL 去尾斜杠（profile 里存的是带 '/' 的 url）
    DOCTEST_CHECK_EQ(envObj.get("ANTHROPIC_BASE_URL").asString(),
                     std::string("https://api.anthropic.com"));
    // 密钥键三态默认 both → 只写 ANTHROPIC_AUTH_TOKEN
    DOCTEST_CHECK_EQ(envObj.get("ANTHROPIC_AUTH_TOKEN").asString(),
                     std::string("sk-ant-run-0001"));
    DOCTEST_CHECK(!envObj.has("ANTHROPIC_API_KEY"));

    // ④ 零全局回归闸：settings.json 内容未变、sidecar 未建、激活态未变、代理未动
    DOCTEST_CHECK_EQ(readFileToString(paths.settings), settingsBefore);
    DOCTEST_CHECK(!test_support::configRowExists(
        &tdb.handle(), module::claude::settings_sync::kSidecarConfigKey));
    auto activeAfter = svc.getActiveProfile();
    DOCTEST_REQUIRE(activeAfter.isOk());
    DOCTEST_CHECK_EQ(activeAfter.value().id, a.value().id);
    DOCTEST_CHECK(!svc.isProxyRunning());

    // 会话没真跑，正文自删不会发生 → 测试自清临时文件
    std::error_code rmEc;
    std::filesystem::remove(settingsFile, rmEc);
    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

// runProfile 合并接线钉（审计 F-01，仿 F-8 门面钉）：生效 JSON（profile ⊕
// 片段、片段赢）并入临时 settings 顶层——接线整段删除则本用例红（三链不分叉
// 裁决①的第三条链守护）
REGISTER_TEST(claude_service_run_profile_merges_extra_settings,
              "claude_service: runProfile 临时 settings 并入生效 JSON（片段赢/model 抑制，F-01 接线钉）", {
    test_support::DataRootEnv root(kDbPrefix, "runprofile6");
    test_support::TestDb tdb(kDbPrefix, "runprofile6");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    TempSyncPaths paths("runprofile6", tdb.handle());
    svc.setSettingsSyncPaths(paths.paths());

    // 片段与 profile 各含独有键 + 同键（片段赢）；JSON 含顶层 model（触发
    // ANTHROPIC_MODEL 抑制——legacy model 列本会生成该 env 键）
    auto frag = svc.createSharedConfig("片段R", R"({"statusLine": "frag-s"})");
    DOCTEST_REQUIRE(frag.isOk());
    auto a = svc.createProfile("直连JSON", "https://api.anthropic.com",
                               "sk-ant-run-0006", "m-legacy",
                               /*activateIfNoneActive=*/false,
                               /*sharedConfigId=*/frag.value().id);
    DOCTEST_REQUIRE(a.isOk());
    {
        module::claude::Profile p = a.value();
        p.extraSettingsJson =
            R"({"model": "json-model", "statusLine": "profile-s", "permissions": {"p": 1}})";
        DOCTEST_REQUIRE(svc.updateProfile(p).isOk());
    }

    platform::terminal::LaunchSpec got;
    svc.setTerminalLauncher([&](const platform::terminal::LaunchSpec& spec) {
        got = spec;
        return core::Result<void>::ok();
    });
    std::string hint;
    auto r = svc.runProfile(a.value().id, &hint);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_REQUIRE_EQ(got.args.size(), static_cast<size_t>(2));
    const std::filesystem::path settingsFile(got.args[1]);
    DOCTEST_REQUIRE(std::filesystem::exists(settingsFile));

    auto parsed = platform::json::parse(readFileToString(settingsFile));
    DOCTEST_REQUIRE(parsed.isOk());
    DOCTEST_REQUIRE(parsed.value().isObject());
    auto rootObj = parsed.value().asObject();
    // 生效 JSON 并入顶层：同键片段赢、独有键与 profile 键都在
    DOCTEST_CHECK_EQ(rootObj.get("statusLine").asString(),
                     std::string("frag-s"));
    DOCTEST_CHECK_EQ(rootObj.get("model").asString(), std::string("json-model"));
    DOCTEST_REQUIRE(rootObj.get("permissions").isObject());
    // model 抑制随行生效：env 无 ANTHROPIC_MODEL（legacy m-legacy 不落 env）
    auto envObj = rootObj.get("env").asObject();
    DOCTEST_CHECK(!envObj.has("ANTHROPIC_MODEL"));
    DOCTEST_CHECK_EQ(envObj.get("ANTHROPIC_AUTH_TOKEN").asString(),
                     std::string("sk-ant-run-0006"));
    // 零全局闸（同既有直连用例）：settings.json / sidecar 未被触碰
    DOCTEST_CHECK(!test_support::configRowExists(
        &tdb.handle(), module::claude::settings_sync::kSidecarConfigKey));

    std::error_code rmEc;
    std::filesystem::remove(settingsFile, rmEc);
    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

REGISTER_TEST(claude_service_run_profile_key_read_failure_aborts,
              "claude_service: runProfile 读凭据失败 → fail 且不落盘、不开窗", {
    test_support::DataRootEnv root(kDbPrefix, "runprofile2");
    test_support::TestDb tdb(kDbPrefix, "runprofile2");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto a = svc.createProfile("官方", "https://api.anthropic.com",
                               "sk-ant-run-0002", "");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());

    // 注入「读凭据失败」（2026-10-02 存储统一后注入面从 Keychain 挪到
    // credential::unprotect）：createProfile 在注入前已完成，只让「读」失败
    ScopedFailUnprotect cred;

    int calls = 0;
    svc.setTerminalLauncher([&](const platform::terminal::LaunchSpec&) {
        ++calls;
        return core::Result<void>::ok();
    });
    auto r = svc.runProfile(a.value().id, nullptr);
    DOCTEST_CHECK(r.isFail());     // 中止
    DOCTEST_CHECK_EQ(calls, 0);    // 不开会话
    // 未落盘：run-settings 目录都不该建（中止在写文件之前）
    DOCTEST_CHECK(!std::filesystem::exists(
        std::filesystem::path(root.dir) / "run-settings"));

    return true;
})

REGISTER_TEST(claude_service_run_profile_zero_global_change_gate,
              "claude_service: runProfile 零全局回归闸——settings.json 未写、is_active 未变、代理未动（激活/非激活都跑）", {
    test_support::DataRootEnv root(kDbPrefix, "runprofile3");
    test_support::TestDb tdb(kDbPrefix, "runprofile3");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 接管通道路径注入（指向临时 settings.json）——回归闸断言的就是它**不被碰**
    TempSyncPaths paths("runprofile3", tdb.handle());
    svc.setSettingsSyncPaths(paths.paths());

    auto a = svc.createProfile("甲", "https://a.example.com", "sk-ant-run-a", "");
    auto b = svc.createProfile("乙", "https://b.example.com", "sk-ant-run-b", "");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());

    int calls = 0;
    std::vector<std::filesystem::path> written;
    svc.setTerminalLauncher([&](const platform::terminal::LaunchSpec& s) {
        ++calls;
        if (s.args.size() == 2) {
            written.push_back(std::filesystem::path(s.args[1]));
        }
        return core::Result<void>::ok();
    });

    // ① 目标 = 激活 profile（a）：成功 + settings.json 一字未变 + 激活态未变
    const std::string settingsBefore = R"({
      "env": { "ANTHROPIC_BASE_URL": "http://127.0.0.1:8888",
               "ANTHROPIC_AUTH_TOKEN": "PROXY_MANAGED",
               "KEEP": "v" },
      "model": "claude-sonnet-4"
    })";
    paths.writeSettings(settingsBefore);
    std::string hint;
    auto ra = svc.runProfile(a.value().id, &hint);
    DOCTEST_REQUIRE(ra.isOk());
    DOCTEST_CHECK_EQ(readFileToString(paths.settings), settingsBefore);

    // ② 目标 ≠ 激活 profile（b）：**不再切激活**（D-1）——坏 settings.json 也不挡
    //    路（直连形态根本不解析全局文件）；写坏文件本身作为反证
    paths.writeSettings("{ 这不是合法 JSON");
    auto rb = svc.runProfile(b.value().id, nullptr);
    DOCTEST_REQUIRE(rb.isOk());
    DOCTEST_CHECK_EQ(readFileToString(paths.settings),
                     std::string("{ 这不是合法 JSON"));

    // ③ 回归闸汇总：sidecar 未建、激活态未变、代理未动、每次都开窗
    DOCTEST_CHECK(!test_support::configRowExists(
        &tdb.handle(), module::claude::settings_sync::kSidecarConfigKey));
    auto active = svc.getActiveProfile();
    DOCTEST_REQUIRE(active.isOk());
    DOCTEST_CHECK_EQ(active.value().id, a.value().id);
    DOCTEST_CHECK(!svc.isProxyRunning());
    DOCTEST_CHECK_EQ(calls, 2);
    for (const auto& w : written) {
        std::error_code rmEc;
        std::filesystem::remove(w, rmEc);
    }

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

REGISTER_TEST(claude_service_run_profile_write_failure_aborts,
              "claude_service: runProfile 写临时 settings 失败（run-settings 被文件占位）→ fail 且不开窗", {
    test_support::DataRootEnv root(kDbPrefix, "runprofile4");
    test_support::TestDb tdb(kDbPrefix, "runprofile4");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto a = svc.createProfile("官方", "https://api.anthropic.com",
                               "sk-ant-run-0004", "");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());

    // 用同名**普通文件**占住 run-settings 路径 → create_directories 必失败
    //（路径已存在且不是目录）；Windows 上「只读目录」拦不住建文件，这是跨平台
    // 可复现的目录不可建形态
    const std::filesystem::path dir = std::filesystem::path(root.dir) / "run-settings";
    {
        std::ofstream out(dir, std::ios::binary | std::ios::trunc);
        out << "占位";
    }

    int calls = 0;
    svc.setTerminalLauncher([&](const platform::terminal::LaunchSpec&) {
        ++calls;
        return core::Result<void>::ok();
    });
    auto r = svc.runProfile(a.value().id, nullptr);
    DOCTEST_CHECK(r.isFail());     // 中止
    DOCTEST_CHECK_EQ(calls, 0);    // 不开会话

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

REGISTER_TEST(claude_service_run_profile_invalid_id,
              "claude_service: runProfile id=0 / 不存在 → fail（不开会话）", {
    test_support::TestDb tdb(kDbPrefix, "runprofile5");
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

// ---- 直连 env 生成（buildDirectEnv 纯函数，profile-run 设计 §4.2）----

REGISTER_TEST(claude_build_direct_env_three_state_auth_and_base_url,
              "buildDirectEnv: 认证字段三态密钥键名 + BASE_URL 去尾斜杠 + 空值不写键", {
    // bearer → 只写 ANTHROPIC_AUTH_TOKEN
    {
        auto env = module::claude::buildDirectEnv(
            "https://api.anthropic.com/", "sk-bearer-1", "bearer", "", "");
        DOCTEST_CHECK_EQ(env["ANTHROPIC_BASE_URL"],
                         std::string("https://api.anthropic.com"));  // 去尾斜杠
        DOCTEST_CHECK_EQ(env["ANTHROPIC_AUTH_TOKEN"], std::string("sk-bearer-1"));
        DOCTEST_CHECK(env.find("ANTHROPIC_API_KEY") == env.end());
    }
    // api_key → 只写 ANTHROPIC_API_KEY
    {
        auto env = module::claude::buildDirectEnv(
            "https://gw.example.com", "sk-api-1", "api_key", "", "");
        DOCTEST_CHECK_EQ(env["ANTHROPIC_API_KEY"], std::string("sk-api-1"));
        DOCTEST_CHECK(env.find("ANTHROPIC_AUTH_TOKEN") == env.end());
    }
    // both（默认）/ 未知值 → 只写 ANTHROPIC_AUTH_TOKEN
    {
        auto env = module::claude::buildDirectEnv(
            "https://api.anthropic.com", "sk-both-1", "both", "", "");
        DOCTEST_CHECK_EQ(env["ANTHROPIC_AUTH_TOKEN"], std::string("sk-both-1"));
        DOCTEST_CHECK(env.find("ANTHROPIC_API_KEY") == env.end());
        // 未知 / 空串认证字段按 both 读（authFieldFromText 兜底，D-8 读侧口径）
        auto env2 = module::claude::buildDirectEnv(
            "https://x.example.com", "sk-both-2", "什么奇怪的值", "", "");
        DOCTEST_CHECK_EQ(env2["ANTHROPIC_AUTH_TOKEN"], std::string("sk-both-2"));
        DOCTEST_CHECK(env2.find("ANTHROPIC_API_KEY") == env2.end());
    }
    // 空 baseUrl / 空密钥 → 对应托管键不写（不产生空值键；BASE_URL 空 = Claude
    // Code 回落内置默认上游）
    {
        auto env = module::claude::buildDirectEnv("", "", "both", "", "");
        DOCTEST_CHECK(env.find("ANTHROPIC_BASE_URL") == env.end());
        DOCTEST_CHECK(env.find("ANTHROPIC_AUTH_TOKEN") == env.end());
        DOCTEST_CHECK(env.find("ANTHROPIC_API_KEY") == env.end());
    }
    return true;
})

REGISTER_TEST(claude_build_direct_env_slots_extra_env_and_managed_override,
              "buildDirectEnv: 槽位/extra_env 并入 + 托管键压过 extra_env + 别名归一（片段 env 形参已拆）", {
    const std::string models =
        R"({"default": {"model": "claude-sonnet-4", "name": ""},
            "sonnet": {"model": "claude-sonnet-4-5", "name": "S4.5"},
            "opus": {"model": "", "name": ""}})";
    const std::string extra = R"({
        "ANTHROPIC_BASE_URL": "http://127.0.0.1:8888",
        "ANTHROPIC_AUTH_TOKEN": "PROXY_MANAGED",
        "ANTHROPIC_MODEL": "extra-model",
        "AUTO_COMPACT_ENABLED": "1",
        "MY_VAR": "v"
    })";
    //（advanced-json-editor §8.4：共享片段 env 叠加形参已拆除——片段走 JSON 域）
    auto env = module::claude::buildDirectEnv(
        "https://api.anthropic.com/", "sk-real-key", "both",
        models, extra);

    // 托管键压过 extra_env 的同名旧值（BASE_URL 去尾斜杠后为 profile 值）
    DOCTEST_CHECK_EQ(env["ANTHROPIC_BASE_URL"],
                     std::string("https://api.anthropic.com"));
    DOCTEST_CHECK_EQ(env["ANTHROPIC_AUTH_TOKEN"], std::string("sk-real-key"));
    // 「第二密钥键」不存活：直连只写托管键一个（extra 带进的另一键被跳过）
    DOCTEST_CHECK(env.find("ANTHROPIC_API_KEY") == env.end());
    // extra_env 键原样入表
    DOCTEST_CHECK_EQ(env["MY_VAR"], std::string("v"));
    // 槽位：非空槽位写 envKey；name 空 → 派生显示名（剥 [1M] 类标记）
    DOCTEST_CHECK_EQ(env["ANTHROPIC_MODEL"], std::string("claude-sonnet-4"));
    DOCTEST_CHECK_EQ(env["ANTHROPIC_DEFAULT_SONNET_MODEL"],
                     std::string("claude-sonnet-4-5"));
    DOCTEST_CHECK_EQ(env["ANTHROPIC_DEFAULT_SONNET_MODEL_NAME"],
                     std::string("S4.5"));
    // 空槽位（opus）跳过
    DOCTEST_CHECK(env.find("ANTHROPIC_DEFAULT_OPUS_MODEL") == env.end());
    // extra_env 别名归一：AUTO_COMPACT_ENABLED=1 → DISABLE_AUTO_COMPACT=0
    DOCTEST_CHECK_EQ(env["DISABLE_AUTO_COMPACT"], std::string("0"));
    DOCTEST_CHECK(env.find("AUTO_COMPACT_ENABLED") == env.end());
    return true;
})

// ===== Agent 模型槽位解析（agent 子系统 M1，Task 6）=====
// resolveModelSlot：激活 profile 的槽位键 → 实际 model id（LlmClient 消费）

REGISTER_TEST(claude_service_resolve_model_slot,
              "claude_service: resolveModelSlot 解析激活 profile 槽位", {
    using namespace module::claude;
    // 本文件既有模式 = TestDb（含 migrate，claude_profiles 表在迁移里建）。
    // InMemoryDb 是无迁移裸库（test_streaming_proxy 只拿它测代理运行态），
    // profile CRUD 会因缺表失败
    test_support::TestDb tdb(kDbPrefix, "resolve_slot");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 无激活 profile（新库新 service，未 setActive）→ nullopt
    DOCTEST_CHECK(!svc.resolveModelSlot("default").has_value());
    DOCTEST_CHECK(!svc.resolveModelSlot("sonnet").has_value());

    auto p = svc.createProfile("R", "https://api.anthropic.com", "sk-test",
                               "claude-sonnet-4-5");
    DOCTEST_REQUIRE(p.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(p.value().id).isOk());

    // 无 models_config → default 槽位回落 profile.model（旧列语义：
    // 旧 model 列是 default 槽位的 fallback）
    DOCTEST_CHECK(svc.resolveModelSlot("default").value_or("") ==
                  "claude-sonnet-4-5");
    DOCTEST_CHECK(!svc.resolveModelSlot("sonnet").has_value());

    // 配置 models_config 后按槽位解析（updateProfile 为既有更新接口）
    Profile pa = p.value();
    pa.modelsConfigJson =
        R"({"default": {"model": "m-default"},)"
        R"("sonnet": {"model": "m-s"},)"
        R"("haiku": {"model": "m-h"},)"
        R"("subagent": {"model": "m-sub"}})";
    DOCTEST_REQUIRE(svc.updateProfile(pa).isOk());

    // default / def 等价，取 models_config 的 def 槽位
    DOCTEST_CHECK(svc.resolveModelSlot("default").value_or("") == "m-default");
    DOCTEST_CHECK(svc.resolveModelSlot("def").value_or("") == "m-default");
    // 其余槽位按键解析（haiku 独立断言：与 opus 成员互换须变红）
    DOCTEST_CHECK(svc.resolveModelSlot("sonnet").value_or("") == "m-s");
    DOCTEST_CHECK(svc.resolveModelSlot("haiku").value_or("") == "m-h");
    DOCTEST_CHECK(svc.resolveModelSlot("subagent").value_or("") == "m-sub");
    // 表内但未配置的槽位 → nullopt
    DOCTEST_CHECK(!svc.resolveModelSlot("opus").has_value());
    // 表内但 model 为空串 → nullopt（"model 空 → nullopt" 契约）
    {
        Profile pe = p.value();
        pe.modelsConfigJson = R"({"fable": {"model": "", "name": ""}})";
        DOCTEST_REQUIRE(svc.updateProfile(pe).isOk());
        DOCTEST_CHECK(!svc.resolveModelSlot("fable").has_value());
    }
    // 未知槽位键 → nullopt
    DOCTEST_CHECK(!svc.resolveModelSlot("no-such-slot").has_value());

    test_support::cleanupProfileKey(kTestKeychainService, p.value().id);
    return true;
})

// ---- 38. 退出时序：析构摘牌 global 单例（终审 2026-10-01 高 finding）----
//
// 场景=惰性装配反转 atexit LIFO：main.cpp 的 getClaudeService() 持有
// static unique_ptr<ClaudeService>，会话内惰性装配路径下它构造在 armExitHook
// 注册之后 → 退出时析构先于 exit hook，hook 里的模块 onStop（claude_module.cpp
// 的 ClaudeService::global() 分支）读到的是已析构对象。契约：析构即摘牌，
// global() 回 nullptr → onStop 走「未运行」分支，不悬垂读、不进 stopProxy。

REGISTER_TEST(claude_service_destroy_clears_global,
              "claude_service: 析构清 global 单例指针（退出时序 UAF 防护）", {
    test_support::TestDb tdb(kDbPrefix, "global_clear");
    {
        module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
        module::claude::ClaudeService::setGlobal(&svc);
        // 注册可见（非恒真断言：契约若退化成「setGlobal 不生效」本行先红）
        DOCTEST_CHECK(module::claude::ClaudeService::global() == &svc);
    }   // ← svc 析构（等价于退出时 unique_ptr 先析构、exit hook 的 onStop 后跑）
    DOCTEST_CHECK(module::claude::ClaudeService::global() == nullptr);
    return true;
})

// 守卫反向：`if (g == this)` 仅摘自己的牌——析构未注册对象不得误清别人的注册
REGISTER_TEST(claude_service_destroy_keeps_other_global,
              "claude_service: 析构未注册实例不动他人 global 注册（this 守卫）", {
    test_support::TestDb tdb(kDbPrefix, "global_guard");
    module::claude::ClaudeService registered(tdb.handle(), kTestKeychainService);
    module::claude::ClaudeService::setGlobal(&registered);
    {
        module::claude::ClaudeService other(tdb.handle(), kTestKeychainService);
    }   // other 析构：global 指向 registered，不该被清
    DOCTEST_CHECK(module::claude::ClaudeService::global() == &registered);
    module::claude::ClaudeService::setGlobal(nullptr);   // 用例内自清（同 resetCaseGlobals 口径）
    return true;
})

// ---- 18. validateProxyPreconditions（2026-10-01 事故：无 profile / 不连通不得起代理）----

REGISTER_TEST(claude_pv_no_active_profile,
              "claude_service: 前置校验① 无激活 profile → fail（NoActiveProfile）且零副作用", {
    test_support::TestDb tdb(kDbPrefix, "pv_noactive");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto r = svc.validateProxyPreconditions();
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK(r.errorCode() == core::ErrorCode::NoActiveProfile);
    DOCTEST_CHECK(r.errorMessage().find("前置校验未通过（1/2 激活 Profile）") !=
                  std::string::npos);
    DOCTEST_CHECK(r.errorMessage().find("没有激活的 Claude profile") !=
                  std::string::npos);          // 可操作指引随错误码原样透传
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());  // 不 bind
    DOCTEST_CHECK_EQ(svc.proxyPort(), 0);
    return true;
})

REGISTER_TEST(claude_pv_unreachable_no_takeover,
              "claude_service: 前置校验② 上游不可达 → fail（NetworkUnreachable）且 settings.json 未被接管", {
    test_support::TestDb tdb(kDbPrefix, "pv_conn");
    TempSyncPaths sync("pv_conn", tdb.handle());                   // 文件内桩：临时 settings.json + sidecar
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    svc.setSettingsSyncPaths(sync.paths());
    svc.setSettingsTakeOverEnabled(true);            // 接管开关默认即 true，此处显式钉住
    const std::string kOriginal = R"({"env":{"FOO":"bar"}})";
    sync.writeSettings(kOriginal);

    // 端口 1 = 几乎必然拒绝连接（既有用例同款，:1177）
    auto created = svc.createProfile("Bad", "http://127.0.0.1:1",
                                     "sk-ant-pv-7777", "m");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());  // 首次激活：G0 不读文件

    auto r = svc.validateProxyPreconditions(500);
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK(r.errorCode() == core::ErrorCode::NetworkUnreachable);
    DOCTEST_CHECK(r.errorMessage().find("前置校验未通过（2/2 连通测试）") !=
                  std::string::npos);
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());
    DOCTEST_CHECK_EQ(svc.proxyPort(), 0);
    // 事故本体断言：settings.json 一字未改、无 sidecar（= 没走到 syncTakeOver）
    DOCTEST_CHECK_EQ(readFileToString(sync.settings), kOriginal);
    DOCTEST_CHECK_FALSE(test_support::configRowExists(
        &tdb.handle(), module::claude::settings_sync::kSidecarConfigKey));
    DOCTEST_CHECK(svc.lastSyncWarnings().empty());
    test_support::cleanupProfileKey(kTestKeychainService, created.value().id);
    return true;
})

REGISTER_TEST(claude_pv_non_2xx_fails,
              "claude_service: 前置校验③ 上游 401 → fail（无/坏 key 不得起代理）", {
    platform::http::Server upstream401("127.0.0.1", 0);   // 模式照 :994-1010
    upstream401.route("GET", "/v1/models",
                      [](const platform::http::RequestCtx&,
                         platform::http::ResponseWriter& w) {
                          w.setStatus(401);
                          w.setHeader("Content-Type", "application/json");
                          w.write(R"({"error":{"type":"authentication_error"}})");
                          w.end();
                      });
    DOCTEST_REQUIRE(upstream401.start());

    test_support::TestDb tdb(kDbPrefix, "pv_401");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    auto created = svc.createProfile(
        "NoKey", "http://127.0.0.1:" + std::to_string(upstream401.actualPort()),
        "sk-ant-pv-401", "m");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());

    auto r = svc.validateProxyPreconditions(2000);
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK(r.errorCode() == core::ErrorCode::HttpError);
    DOCTEST_CHECK(r.errorMessage().find("401") != std::string::npos);
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());
    upstream401.stop();
    test_support::cleanupProfileKey(kTestKeychainService, created.value().id);
    return true;
})

REGISTER_TEST(claude_pv_ok_then_start_proxy,
              "claude_service: 前置校验④ 通过（2xx）→ 校验只探一次，startProxy 仍可用", {
    platform::http::Server upstream("127.0.0.1", 0);       // 模式照 :952-968
    int hitCount = 0;
    std::string lastPath;
    upstream.route("GET", "/v1/models",
                   [&](const platform::http::RequestCtx& ctx,
                       platform::http::ResponseWriter& w) {
                       ++hitCount;
                       lastPath = ctx.path;
                       w.setStatus(200);
                       w.setHeader("Content-Type", "application/json");
                       w.write(R"({"data":[{"id":"claude-sonnet-4-5"}]})");
                       w.end();
                   });
    DOCTEST_REQUIRE(upstream.start());

    test_support::TestDb tdb(kDbPrefix, "pv_ok");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    auto created = svc.createProfile(
        "Good", "http://127.0.0.1:" + std::to_string(upstream.actualPort()),
        "sk-ant-pv-ok-8888", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());

    auto v = svc.validateProxyPreconditions(2000);
    DOCTEST_CHECK(v.isOk());
    DOCTEST_CHECK_EQ(hitCount, 1);
    DOCTEST_CHECK_EQ(lastPath, std::string("/v1/models"));
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());   // 校验本身绝不 bind

    // 通过后 startProxy 照常（端口 0 = OS 分配）
    auto started = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(started.isOk());
    DOCTEST_CHECK(svc.isProxyRunning());
    DOCTEST_CHECK(svc.proxyPort() > 0);
    // 不变式：校验不在 startProxy 内部（否则这里会变成 2——且 startProxy 的
    // 「无激活 profile 仍接管」既有契约会被打破，见 test_settings_sync.cpp:744-758）
    DOCTEST_CHECK_EQ(hitCount, 1);
    svc.stopProxy();
    upstream.stop();
    test_support::cleanupProfileKey(kTestKeychainService, created.value().id);
    return true;
})

REGISTER_TEST(claude_pv_timeout_honored,
              "claude_service: 前置校验⑤ 探测超时按入参生效（不静默回落 15s）", {
    // mock 上游挂 1500ms 才回 200：probeTimeoutMs=150 → 必须 fail。
    // 若超时参数没接上（回落 testConnection 的 15000），请求会在 1.5s 后成功
    // → 本用例红（断言是 ok/fail 二值，不依赖墙钟测量；10× 余量保证负载抖动
    // 下也不会误判。收尾 slow.stop() 会 join 上游 worker，用例总耗时 ≈1.5s）
    platform::http::Server slow("127.0.0.1", 0);
    slow.route("GET", "/v1/models",
               [](const platform::http::RequestCtx&,
                  platform::http::ResponseWriter& w) {
                   std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                   w.setStatus(200);
                   w.setHeader("Content-Type", "application/json");
                   w.write(R"({"data":[]})");
                   w.end();
               });
    DOCTEST_REQUIRE(slow.start());

    test_support::TestDb tdb(kDbPrefix, "pv_timeout");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    auto created = svc.createProfile(
        "Slow", "http://127.0.0.1:" + std::to_string(slow.actualPort()),
        "sk-ant-pv-slow", "m");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());

    auto r = svc.validateProxyPreconditions(150);
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());
    slow.stop();
    test_support::cleanupProfileKey(kTestKeychainService, created.value().id);
    return true;
})
