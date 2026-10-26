// tests/test_modules/test_claude_service.cpp
// ClaudeService 单测：Profile CRUD / 凭据两段写补偿 / settings.json 反向同步 / 代理前置校验。
// 跨文件前提：
//   - 凭据本体只落 claude_profiles.api_key_encrypted，platform::keychain 已不是凭据来源，
//     生产侧对它零写操作（残留清理已随旧凭据面一起移除）。
//   - sidecar（托管键表）是 app_config 表的一行而非文件，故 setSettingsSyncPaths 的 db 必须是已 migrate 的连接。
//   - Keychain service 名生产走 keychainServiceForDataRoot 按数据根派生；本文件一律显式传固定串，绕开派生。
//   - 数据库走临时文件（temp_directory_path），Keychain service 固定 "DevDeskTestClaudeService"，不碰生产数据。
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
#include <sys/stat.h>   // run-settings 权限位断言
#endif

namespace {

constexpr const char* kTestKeychainService = "DevDeskTestClaudeService";

// 临时 db 文件名前缀（共享工厂 test_support::tempDbPath / TestDb 的第一实参）
constexpr const char* kDbPrefix = "devdesk_test_claude_";

// 头名大小写不敏感取头与计数：共享实现
using test_support::countCI;
using test_support::DataRootEnv;
using test_support::headerCI;
using test_support::HeaderMap;

// 作用域内把 http_proxy 指到本地 mock、清空 no_proxy，析构还原原值。
// 用途：让 baseUrl host 为 opencode.ai 的探测请求仍落到本地 mock——curl 走代理时请求行是绝对 URI
// （GET http://opencode.ai/... HTTP/1.1），不解析该域名，请求不出本机。
// send() 未显式设 CURLOPT_PROXY，代理只从环境变量取，所以只能靠改 env 来劫持。
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
// runProfile 零全局回归闸用它断言「settings.json 未被写」。
// sidecar 是 app_config 表的一行而不是文件，故本结构**不持有 sidecar 路径**，只持一个已 migrate 的连接供 paths() 填 db。
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

constexpr const char* kRevTempDirPrefix = "devdesk_test_claude_rev_";

// sidecar 在 app_config 表里，paths() 必须一并拿到已 migrate 的连接
// （测试侧一律用同一个 tdb，避免各用例各开一库）。
module::claude::settings_sync::Paths makeSyncPaths(const test_support::TempDir& d,
                                                   core::data::SqliteDb& db) {
    module::claude::settings_sync::Paths p;
    p.settingsFile = d.file("settings.json");
    p.db = &db;
    return p;
}

} // namespace

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
    DOCTEST_CHECK(p.apiKeyPlain.empty());
    // hint = 前 6 + *** + 尾 4
    DOCTEST_CHECK(p.apiKeyHint.find("***") != std::string::npos);
    // api_key_check = 全量 SHA256 hex（64 字符），不是短前缀
    DOCTEST_CHECK_EQ(p.apiKeyCheck.size(), static_cast<size_t>(64));

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
    for (const auto& p : list.value()) {
        DOCTEST_CHECK(p.apiKeyPlain.empty());
    }

    return true;
})

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

    auto none = svc.getActiveProfile();
    DOCTEST_CHECK(none.isFail());
    DOCTEST_CHECK(none.errorCode() == core::ErrorCode::NoActiveProfile);

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

    auto list = svc.listProfiles();
    DOCTEST_REQUIRE(list.isOk());
    int activeCount = 0;
    for (const auto& p : list.value()) {
        if (p.isActive) ++activeCount;
    }
    DOCTEST_CHECK_EQ(activeCount, 1);

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

REGISTER_TEST(claude_service_first_create_auto_activates,
              "claude_service: createProfile 首建自动激活 + 不抢占 + 503 指引文案", {
    test_support::TestDb tdb(kDbPrefix, "autoactivate");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 1) 省略 activateIfNoneActive → 不激活（默认 false，既有调用方零影响）
    auto legacy = svc.createProfile("Legacy", "https://legacy.example.com",
                                    "sk-ant-legacy-0001", "m");
    DOCTEST_REQUIRE(legacy.isOk());
    DOCTEST_CHECK_FALSE(legacy.value().isActive);
    DOCTEST_CHECK(svc.getActiveProfile().isFail());
    test_support::cleanupProfileKey(kTestKeychainService, legacy.value().id);

    // 2) activateIfNoneActive=true 且无激活 → 自动激活
    auto first = svc.createProfile("First", "https://first.example.com",
                                   "sk-ant-first-0002", "m", true);
    DOCTEST_REQUIRE(first.isOk());
    DOCTEST_CHECK(first.value().isActive);
    auto act = svc.getActiveProfile();
    DOCTEST_REQUIRE(act.isOk());
    DOCTEST_CHECK_EQ(act.value().id, first.value().id);

    // 3) 已有激活 → 不抢占（返回 isActive=false，UI 据此显示「点击启用」）
    auto second = svc.createProfile("Second", "https://second.example.com",
                                    "sk-ant-second-0003", "m", true);
    DOCTEST_REQUIRE(second.isOk());
    DOCTEST_CHECK_FALSE(second.value().isActive);
    auto actStill = svc.getActiveProfile();
    DOCTEST_REQUIRE(actStill.isOk());
    DOCTEST_CHECK_EQ(actStill.value().id, first.value().id);

    // 4) 无激活时错误 message 带可操作指引；503 路径原样透传同一份文案
    auto del = svc.deleteProfile(first.value().id);
    DOCTEST_REQUIRE(del.isOk());
    auto none = svc.getActiveProfile();
    DOCTEST_REQUIRE(none.isFail());
    DOCTEST_CHECK(none.errorCode() == core::ErrorCode::NoActiveProfile);
    DOCTEST_CHECK(none.errorMessage().find("并点击「启用」") != std::string::npos);

    test_support::cleanupProfileKey(kTestKeychainService, second.value().id);
    return true;
})

REGISTER_TEST(claude_service_configured_proxy_port,
              "claude_service: setConfiguredProxyPort 默认/钳制 + 换端口重启保 token", {
    test_support::TestDb tdb(kDbPrefix, "cfgport");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    DOCTEST_CHECK_EQ(svc.configuredProxyPort(), 8888);
    svc.setConfiguredProxyPort(9000);
    DOCTEST_CHECK_EQ(svc.configuredProxyPort(), 9000);
    // 非法值回落默认（0 / 负数 / 越界），上界 65535 保留
    svc.setConfiguredProxyPort(0);
    DOCTEST_CHECK_EQ(svc.configuredProxyPort(), 8888);
    svc.setConfiguredProxyPort(-5);
    DOCTEST_CHECK_EQ(svc.configuredProxyPort(), 8888);
    svc.setConfiguredProxyPort(65536);
    DOCTEST_CHECK_EQ(svc.configuredProxyPort(), 8888);
    svc.setConfiguredProxyPort(65535);
    DOCTEST_CHECK_EQ(svc.configuredProxyPort(), 65535);

    // 设置页 applyProxyPort 的运行期原语就是 stop→start 换端口；这里传 port=0 让 OS 分配，
    // 两次 start 拿到不同端口即可证明端口确实换过。
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

    // 反重构：绕开 service 直接查表，是为了确认明文 key 不落 SQLite 的**存储列**——
    // 只调 loadApiKey 看不到这一点。
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
    DOCTEST_CHECK(hint.find("key") != std::string::npos);

    test_support::cleanupProfileKey(kTestKeychainService, id);
    return true;
})

REGISTER_TEST(claude_service_update_profile,
              "claude_service: updateProfile 改字段 + 可选换 key", {
    test_support::TestDb tdb(kDbPrefix, "update");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile("Before", "https://old.example.com",
                                     "sk-ant-old-1111", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(created.isOk());
    module::claude::Profile p = created.value();

    // 1) 只改 name / url / model，apiKeyPlain 留空 → 不换 key
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
    auto key1 = svc.loadApiKey(p.id);
    DOCTEST_REQUIRE(key1.isOk());
    DOCTEST_CHECK_EQ(key1.value(), std::string("sk-ant-old-1111"));

    // 2) 换 key（apiKeyPlain 非空）
    module::claude::Profile p2 = got.value();
    p2.apiKeyPlain = "sk-ant-new-2222";
    auto upd2 = svc.updateProfile(p2);
    DOCTEST_REQUIRE(upd2.isOk());

    auto key2 = svc.loadApiKey(p.id);
    DOCTEST_REQUIRE(key2.isOk());
    DOCTEST_CHECK_EQ(key2.value(), std::string("sk-ant-new-2222"));

    // api_key_check 是密钥的纯函数，随 key 变化
    auto got2 = svc.getProfile(p.id);
    DOCTEST_REQUIRE(got2.isOk());
    DOCTEST_CHECK(got2.value().apiKeyCheck != p.apiKeyCheck);

    test_support::cleanupProfileKey(kTestKeychainService, p.id);
    return true;
})

REGISTER_TEST(claude_service_update_db_fail_rolls_back_keychain,
              "claude_service: updateProfile DB 写失败按旧值回写 Keychain", {
    test_support::TestDb tdb(kDbPrefix, "updcomp");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto a = svc.createProfile("NameA", "https://a.example.com",
                               "sk-ant-aaa-0001", "m");
    DOCTEST_REQUIRE(a.isOk());
    auto b = svc.createProfile("NameB", "https://b.example.com",
                               "sk-ant-bbb-0002", "m");
    DOCTEST_REQUIRE(b.isOk());
    const int64_t bid = b.value().id;

    // 失败注入：把 B 的 name 改成 A 的 name → UPDATE 撞 claude_profiles.name 的 UNIQUE 约束 = 段二（DB 写）确定性失败。
    // 刻意不经注入缝：本用例的价值就在补偿逻辑走**真实**凭据存储落盘再回写；改走注入缝会让它与下一组用例同形、丧失鉴别力。
    module::claude::Profile pb = b.value();
    pb.name = "NameA";
    pb.apiKeyPlain = "sk-ant-bbb-9999";
    auto upd = svc.updateProfile(pb);
    DOCTEST_CHECK(upd.isFail());
    DOCTEST_CHECK(upd.errorCode() == core::ErrorCode::ProfileAlreadyExists);

    // 补偿生效：凭据里是旧密钥，而不是刚写进去的新密钥
    auto key = svc.loadApiKey(bid);
    DOCTEST_REQUIRE(key.isOk());
    DOCTEST_CHECK_EQ(key.value(), std::string("sk-ant-bbb-0002"));

    // DB 行一字未改 → 元数据与凭据里的密钥一致
    auto got = svc.getProfile(bid);
    DOCTEST_REQUIRE(got.isOk());
    DOCTEST_CHECK_EQ(got.value().name, std::string("NameB"));
    DOCTEST_CHECK_EQ(got.value().apiKeyHint, b.value().apiKeyHint);
    DOCTEST_CHECK_EQ(got.value().apiKeyCheck, b.value().apiKeyCheck);

    // 一致性锚：hint / check 是密钥的纯函数——同一把旧密钥另建一条必须算出相同值（钉「元数据 = 凭据实际值」），
    // 新密钥必须算出不同值（钉本组断言有鉴别力，否则怎么写都恒等）。
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

    auto ga = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(ga.isOk());
    DOCTEST_CHECK_EQ(ga.value().apiKeyHint, a.value().apiKeyHint);

    // 改回原名 + 换 key 走正常路径：补偿没把正常写路径带坏
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

// 「段一写失败」注入走 platform/credential_protection.h 的函数指针表（单槽位，默认指真实实现），
// 否则「凭据段自身失败」那一支（段一失败 → 回写旧值）根本走不到。
namespace {
// 「段一写失败」的点 = platform::credential::protect / unprotect，RAII 槽位注入（析构必复位）。
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
              "claude_service: 段一保护失败 → 旧密钥完好", {
    test_support::TestDb tdb(kDbPrefix, "cpx14stage1");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile("Outer", "https://a.example.com",
                                     "sk-ant-old-0001", "m");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;

    // 注入 protect 失败；旧值 = 当前凭据，先捕获
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
    // 段二没跑过：DB 元数据与旧密钥仍一致
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
    // 全量 SHA256 随新密钥重算（证明判据在跟，不是恒等）
    auto got2 = svc.getProfile(id);
    DOCTEST_REQUIRE(got2.isOk());
    DOCTEST_CHECK_NE(got2.value().apiKeyCheck, created.value().apiKeyCheck);
    DOCTEST_CHECK_EQ(got2.value().apiKeyCheck.size(), static_cast<size_t>(64));
    return true;
})

// 段二（DB 删行）失败 → 凭据已清时补偿回写 + 行保留可重试
REGISTER_TEST(claude_cpx14_delete_stage2_fail_restores_key,
              "claude_service: 段二删行失败回写已清凭据", {
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

    // 撤掉注入源后重试成功，行没了（凭据随之不可达）
    DOCTEST_REQUIRE(tdb.handle().exec("DROP TRIGGER cpx14_block_delete").isOk());
    DOCTEST_CHECK(svc.deleteProfile(id).isOk());
    DOCTEST_CHECK(svc.getProfile(id).isFail());
    return true;
})

// 建行失败零残留：凭据与元数据同一条 INSERT，天然原子，两条失败路径都不留半截行 / 半截凭据。
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

    // 路径一：重名 → INSERT 撞 UNIQUE，单语句原子失败，一行都不落
    auto dup = svc.createProfile("Base", "https://d.example.com",
                                 "sk-ant-doomed-0005", "m");
    DOCTEST_CHECK(dup.isFail());
    DOCTEST_CHECK(dup.errorCode() == core::ErrorCode::ProfileAlreadyExists);

    // 路径二：protect 失败 → 在 INSERT 之前就失败
    {
        ScopedFailProtect cred;
        auto doomed = svc.createProfile("Doomed", "https://d.example.com",
                                        "sk-ant-doomed-0006", "m");
        DOCTEST_CHECK(doomed.isFail());
    }

    auto after = svc.listProfiles();
    DOCTEST_REQUIRE(after.isOk());
    DOCTEST_CHECK_EQ(after.value().size(), countBefore);

    auto baseKey = svc.loadApiKey(base.value().id);
    DOCTEST_REQUIRE(baseKey.isOk());
    DOCTEST_CHECK_EQ(baseKey.value(), std::string("sk-ant-base-0004"));

    // 撤掉注入源后重试成功：正常路径未被注入层带坏
    auto ok = svc.createProfile("Doomed", "https://d.example.com",
                                "sk-ant-doomed-0006", "m");
    DOCTEST_REQUIRE(ok.isOk());
    auto okKey = svc.loadApiKey(ok.value().id);
    DOCTEST_REQUIRE(okKey.isOk());
    DOCTEST_CHECK_EQ(okKey.value(), std::string("sk-ant-doomed-0006"));
    return true;
})

REGISTER_TEST(claude_service_delete_profile,
              "claude_service: deleteProfile 清 SQLite + Keychain", {
    test_support::TestDb tdb(kDbPrefix, "delete");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile("ToDelete", "https://del.example.com",
                                     "sk-ant-del-3333", "claude-haiku-4-5");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;

    DOCTEST_REQUIRE(svc.deleteProfile(id).isOk());

    auto got = svc.getProfile(id);
    DOCTEST_CHECK(got.isFail());

    // 恒真：凭据已不落 Keychain，本用例建 profile 时从未往里写过条目。
    // 保留这条断言只为钉「deleteProfile 不会顺手写出一个新条目」。
    auto key = platform::keychain::loadSecret(
        kTestKeychainService, "claude:" + std::to_string(id));
    DOCTEST_CHECK(key.isFail());

    // 再删一次 → ProfileNotFound
    auto again = svc.deleteProfile(id);
    DOCTEST_CHECK(again.isFail());

    return true;
})

// deleteProfile 只清 DB 凭据列 + 删行。「凭据清理失败时中止 deleteProfile、行保留可重试」这条安全属性由
// TwoPhaseKeyWrite 在 DB 层结构性保证（profile_repository.cpp：先清三列、失败即 return，行不动）。

REGISTER_TEST(claude_keychain_service_for_data_root,
              "claude_service: keychainServiceForDataRoot 派生纯函数", {
    const std::string a =
        module::claude::keychainServiceForDataRoot("/tmp/rootA");
    const std::string b =
        module::claude::keychainServiceForDataRoot("/tmp/rootB");
    // 形态 DevDesk-<8 hex>
    DOCTEST_CHECK(a.rfind("DevDesk-", 0) == 0);
    DOCTEST_CHECK_EQ(a.size(), size_t{16});
    // 不同根 → 不同 service（数据根隔离的根基）
    DOCTEST_CHECK(a != b);
    // 同根 → 稳定（跨次启动一致）
    DOCTEST_CHECK_EQ(a, module::claude::keychainServiceForDataRoot("/tmp/rootA"));
    // 反重构：不得与裸前缀同形（派生名必带 8 hex 后缀），否则不同数据根共用一个命名空间
    DOCTEST_CHECK(a != std::string("DevDesk"));
    // 空根 → 空串（调用方兜底裸前缀）
    DOCTEST_CHECK(module::claude::keychainServiceForDataRoot("").empty());
    return true;
})

REGISTER_TEST(claude_service_create_validation,
              "claude_service: createProfile 校验重名 + 空字段", {
    test_support::TestDb tdb(kDbPrefix, "validate");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto e1 = svc.createProfile("", "https://x.example.com", "sk-x", "m");
    DOCTEST_CHECK(e1.isFail());
    DOCTEST_CHECK(e1.errorCode() == core::ErrorCode::ValidationError);

    auto e2 = svc.createProfile("N", "", "sk-x", "m");
    DOCTEST_CHECK(e2.isFail());
    DOCTEST_CHECK(e2.errorCode() == core::ErrorCode::ValidationError);

    auto e3 = svc.createProfile("N", "https://x.example.com", "", "m");
    DOCTEST_CHECK(e3.isFail());
    DOCTEST_CHECK(e3.errorCode() == core::ErrorCode::InvalidApiKey);

    auto ok1 = svc.createProfile("DupName", "https://x.example.com",
                                 "sk-ant-ddd-4444", "m");
    DOCTEST_REQUIRE(ok1.isOk());

    // 重名 → ProfileAlreadyExists（claude_profiles.name UNIQUE）
    auto dup = svc.createProfile("DupName", "https://y.example.com",
                                 "sk-ant-eee-5555", "m");
    DOCTEST_CHECK(dup.isFail());
    DOCTEST_CHECK(dup.errorCode() == core::ErrorCode::ProfileAlreadyExists);

    test_support::cleanupProfileKey(kTestKeychainService, ok1.value().id);
    return true;
})

REGISTER_TEST(claude_service_test_connection,
              "claude_service: testConnection 发 POST /v1/messages（1-token ping）", {
    platform::http::Server upstream("127.0.0.1", 0);
    int hitCount = 0;
    std::string lastPath;
    std::string lastApiKey;
    upstream.route("POST", "/v1/messages",
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

    // bind 后端口即可用，这里只是给 accept 循环一点余量
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto resp = svc.testConnection(created.value());
    DOCTEST_REQUIRE(resp.isOk());
    DOCTEST_CHECK_EQ(resp.value().statusCode, 200);
    DOCTEST_CHECK(resp.value().body.find("claude-sonnet-4-5") != std::string::npos);
    DOCTEST_CHECK_EQ(hitCount, 1);
    DOCTEST_CHECK_EQ(lastPath, std::string("/v1/messages"));
    // 上游收到的 x-api-key 是凭据列里的明文
    DOCTEST_CHECK_EQ(lastApiKey, std::string("sk-ant-conn-6666"));

    // 上游返回 401 也算 Ok：testConnection 只探测连通性，状态码原样带回给 UI 显示
    upstream.stop();
    platform::http::Server upstream401("127.0.0.1", 0);
    upstream401.route("POST", "/v1/messages",
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

// 探测请求必须走 UpstreamDialect，不能自己拼 x-api-key / anthropic-version：
// 对要求 x-opencode-session、且 Cloudflare 按 UA 拦截的网关（opencode.ai/zen）自拼会误报失败，而真实转发是通的。
REGISTER_TEST(claude_service_test_connection_uses_dialect,
              "claude_service: testConnection 经上游方言（UA/鉴权/opencode 会话头）", {
    platform::http::Server upstream("127.0.0.1", 0);
    HeaderMap seen;
    int hitCount = 0;
    upstream.route("POST", ".*",
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

    // (1) 默认方言（loopback host）：统一 UA + 鉴权头由方言层注入
    const std::string loopbackUrl = "http://127.0.0.1:" + std::to_string(port);
    auto created = svc.createProfile("ConnDialect", loopbackUrl,
                                     "sk-ant-conn-dialect-9999", "m");
    DOCTEST_REQUIRE(created.isOk());

    auto resp = svc.testConnection(created.value());
    DOCTEST_REQUIRE(resp.isOk());
    DOCTEST_CHECK_EQ(resp.value().statusCode, 200);
    DOCTEST_CHECK_EQ(hitCount, 1);
    // 统一 UA：opencode.ai 前置 Cloudflare 按 UA 指纹拦截，客户端签名不能泄漏
    DOCTEST_CHECK_EQ(headerCI(seen, "user-agent"),
                     std::string(module::claude::kProxyUserAgent));
    DOCTEST_CHECK_EQ(countCI(seen, "user-agent"), 1);
    // 鉴权经方言层双发：x-api-key + Authorization: Bearer
    DOCTEST_CHECK_EQ(headerCI(seen, "x-api-key"),
                     std::string("sk-ant-conn-dialect-9999"));
    DOCTEST_CHECK_EQ(headerCI(seen, "authorization"),
                     std::string("Bearer sk-ant-conn-dialect-9999"));
    DOCTEST_CHECK_EQ(countCI(seen, "authorization"), 1);
    DOCTEST_CHECK_EQ(headerCI(seen, "anthropic-version"),
                     std::string("2023-06-01"));
    // 非 opencode host：不注入会话头
    DOCTEST_CHECK_EQ(countCI(seen, "x-opencode-session"), 0);

    // (2) opencode 方言：会话头注入（host 命中 opencode.ai → opencodeZen）
    // 借 http_proxy 把 curl 指到本地 mock：走代理时请求行是绝对 URI，域名不被解析，请求不出本机
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

// 「探测与转发同源」：profile 钉住方言时，探测请求必须和两条转发路径（proxy_router / streaming_proxy）按同一方言装头，
// 否则配了覆盖的 profile 会出现「探测按 A 方言、转发按 B 方言」的错位。
// 断言的是上游实际收到的头（探测路径的真实效果），不是 forBaseUrl 的返回值：
// host 是 loopback（推断 = 默认方言），覆盖 opencode_zen → 会话头必须出现。
REGISTER_TEST(claude_service_test_connection_dialect_override,
              "claude_service: testConnection 接 profile.dialectOverride", {
    platform::http::Server upstream("127.0.0.1", 0);
    HeaderMap seen;
    int hitCount = 0;
    upstream.route("POST", ".*",
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

    // (1) 覆盖 opencode_zen：loopback 推断不出 zen → 会话头出现即证明接了覆盖
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

    // (2) 空串 = 未覆盖：不注入会话头
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

REGISTER_TEST(claude_service_test_connection_unreachable,
              "claude_service: testConnection 上游不可达返回失败", {
    test_support::TestDb tdb(kDbPrefix, "connfail");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile("Bad", "http://127.0.0.1:1",
                                     "sk-ant-bad-7777", "m");
    DOCTEST_REQUIRE(created.isOk());

    auto resp = svc.testConnection(created.value());
    DOCTEST_CHECK(resp.isFail());
    DOCTEST_CHECK(resp.errorCode() == core::ErrorCode::NetworkUnreachable);

    test_support::cleanupProfileKey(kTestKeychainService, created.value().id);
    return true;
})

REGISTER_TEST(claude_service_call_logs,
              "claude_service: logCall + recentCalls 往返", {
    test_support::TestDb tdb(kDbPrefix, "logs");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile("LogTest", "https://x.example.com",
                                     "sk-ant-log-8888", "m");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;

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
    // started_at DESC → 最新的是 429
    DOCTEST_CHECK_EQ(recent.value()[0].statusCode, 429);
    DOCTEST_CHECK_EQ(recent.value()[0].method, std::string("POST"));
    DOCTEST_CHECK_EQ(recent.value()[0].path, std::string("/v1/messages"));
    DOCTEST_CHECK_EQ(recent.value()[0].latencyMs, 300);

    auto two = svc.recentCalls(2);
    DOCTEST_REQUIRE(two.isOk());
    DOCTEST_CHECK_EQ(two.value().size(), 2u);

    // CASCADE：删 profile 连日志一起清
    DOCTEST_REQUIRE(svc.deleteProfile(id).isOk());
    auto afterDelete = svc.recentCalls(10);
    DOCTEST_REQUIRE(afterDelete.isOk());
    DOCTEST_CHECK_EQ(afterDelete.value().size(), 0u);

    test_support::cleanupProfileKey(kTestKeychainService, id);
    return true;
})

// recordProxyEvent/takeProxyEvent 单槽事件：take 读走即清（一事件一泡）、后写覆盖先写、滞留超 kProxyEventFreshSec 丢弃、时钟回摆不误吞。

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

// queryCalls：search 按 path/method 模糊匹配（LIKE 通配符已转义）、statusClass 分桶、limit/offset 翻页。

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
    // 反重构：addRow 必须返回 bool 而非 void —— DOCTEST_REQUIRE 展开成 `return false`，void lambda 里编译不过；
    // 调用点也必须用 DOCTEST_REQUIRE 传播，不能用 DOCTEST_CHECK（那会让失败被吞掉、继续跑完）。
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
    // 旁路直插一行 status_code NULL：必须被 COALESCE 归入「错误」桶，不能漏出任何桶
    DOCTEST_REQUIRE(tdb.handle().exec(
        "INSERT INTO claude_call_logs (profile_id, method, path, started_at) "
        "VALUES (" + std::to_string(pid) + ", 'GET', '/legacy-null', " +
        std::to_string(static_cast<long long>(base) - 2000) + ")").isOk());

    using Q = module::claude::ClaudeService::CallLogQuery;

    // 全量 29 行，最新在前
    Q all;
    all.limit = 100;
    auto rAll = svc.queryCalls(all);
    DOCTEST_REQUIRE(rAll.isOk());
    DOCTEST_CHECK_EQ(rAll.value().size(), 29u);
    DOCTEST_CHECK_EQ(rAll.value()[0].path, std::string("/v1/status"));

    // search 命中 path
    Q qMsg;
    qMsg.search = "messages";
    qMsg.limit = 100;
    auto rMsg = svc.queryCalls(qMsg);
    DOCTEST_REQUIRE(rMsg.isOk());
    DOCTEST_CHECK_EQ(rMsg.value().size(), 3u);
    auto cMsg = svc.countCalls(qMsg);
    DOCTEST_REQUIRE(cMsg.isOk());
    DOCTEST_CHECK_EQ(cMsg.value(), 3);

    // search 命中 method 且 ASCII 大小写不敏感
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

    // statusClass 分桶（4xx / 5xx / 3xx 各桶的量）
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

    // 搜索 ∧ 状态桶
    Q qCombo;
    qCombo.search = "messages";
    qCombo.statusClass = Q::kStatus4xx;
    auto cCombo = svc.countCalls(qCombo);
    DOCTEST_REQUIRE(cCombo.isOk());
    DOCTEST_CHECK_EQ(cCombo.value(), 1);

    // 翻页：3 页互不重叠且顺序 == 全量顺序
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

    // 非法参数归一：limit<=0 → 20；offset<0 → 0
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

REGISTER_TEST(claude_service_proxy_lifecycle,
              "claude_service: startProxy / stopProxy 生命周期", {
    test_support::TestDb tdb(kDbPrefix, "proxy");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    DOCTEST_CHECK_FALSE(svc.isProxyRunning());
    DOCTEST_CHECK_EQ(svc.proxyPort(), 0);

    // port=0 → OS 分配
    auto started = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(started.isOk());
    const int port = started.value();
    DOCTEST_CHECK(port > 0);
    DOCTEST_CHECK(svc.isProxyRunning());
    DOCTEST_CHECK_EQ(svc.proxyPort(), port);

    // 重复启动 → AlreadyExists
    auto again = svc.startProxy("127.0.0.1", 0);
    DOCTEST_CHECK(again.isFail());
    DOCTEST_CHECK(again.errorCode() == core::ErrorCode::AlreadyExists);

    svc.stopProxy();
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());
    DOCTEST_CHECK_EQ(svc.proxyPort(), 0);

    auto restarted = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(restarted.isOk());
    DOCTEST_CHECK(restarted.value() > 0);
    DOCTEST_CHECK(svc.isProxyRunning());

    svc.stopProxy();
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());
    return true;
})

// 02-proxy-host-validation：代理启动即启用 Host 白名单（白名单由实际 bind 地址
// 推导，不写死）——伪造 Host 的请求 403 在路由前被拦，合法回环 Host 照常进代理
// 路由（这里无激活 profile，走业务错而非 403）。
REGISTER_TEST(claude_service_proxy_host_whitelist,
              "claude_service: 代理启动即启用 Host 白名单（host-allowlist，bind 推导，伪造 403）", {
    test_support::TestDb tdb(kDbPrefix, "hostwl");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto started = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(started.isOk());
    const int port = started.value();
    const std::string url =
        "http://127.0.0.1:" + std::to_string(port) + "/v1/messages";

    // 伪造 Host → 403（应用层 Host 校验，与连接目标无关）
    platform::http::Request forged;
    forged.method = "GET";
    forged.url = url;
    forged.headers["Host"] = "evil.com";
    forged.timeoutMs = 5000;
    auto fr = platform::http::send(forged);
    DOCTEST_REQUIRE(fr.isOk());
    DOCTEST_CHECK_EQ(fr.value().statusCode, 403);

    // 合法 Host（URL 派生的 127.0.0.1:port）→ 不是 403：进代理路由后按无激活
    // profile 回业务错（4xx/5xx），证明放行且路由正常
    platform::http::Request legit;
    legit.method = "GET";
    legit.url = url;
    legit.timeoutMs = 5000;
    auto lr = platform::http::send(legit);
    DOCTEST_REQUIRE(lr.isOk());
    DOCTEST_CHECK(lr.value().statusCode != 403);

    svc.stopProxy();
    return true;
})

// 反向同步：全部经临时 settings_sync::Paths 注入，绝不碰真实 ~/.claude。
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
    // 无前置激活 profile → setActiveProfile 不读 settings.json
    DOCTEST_REQUIRE(svc.setActiveProfile(pa.id).isOk());

    auto b = svc.createProfile("目标B", "https://b.example.com",
                               "sk-b-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(b.isOk());

    // Claude Code / cc-switch 侧改过的真实上游值
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
    // 上游 token 经 updateProfile.apiKeyPlain 落到凭据列
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
    DOCTEST_CHECK_EQ(gotA.value().model, std::string("new-default"));
    // extra_env：出现覆盖 + 缺席删除（OLD_KEY 消失）
    auto ee = parseExtraEnv(gotA.value().extraEnvJson);
    DOCTEST_REQUIRE(ee.isOk());
    DOCTEST_CHECK_EQ(ee.value().at("MY_EXTRA"), std::string("extra-value"));
    DOCTEST_CHECK(ee.value().find("OLD_KEY") == ee.value().end());
    // 目标 profile 正常激活且未被误写；源 profile 取消激活
    auto gotB = svc.getProfile(b.value().id);
    DOCTEST_REQUIRE(gotB.isOk());
    DOCTEST_CHECK(gotB.value().isActive);
    DOCTEST_CHECK_EQ(gotB.value().baseUrl, std::string("https://b.example.com"));
    DOCTEST_CHECK(!gotA.value().isActive);

    test_support::cleanupProfileKey(kTestKeychainService, pa.id);
    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// 守卫键不写也不清：loopback BASE_URL 联动 token / 占位 PROXY_MANAGED / 值为空串，三形态。
// 反重构：每形态同批那个 MY_EXTRA 不能省——它保证确实发生了一次写库；没有它整次 no-op，
// 「没被覆盖」会白白通过（真正的回归会被它掩盖）。
// 反重构：a/b 交替激活的两次 setActiveProfile 不能合并成一次——捕获对象是「切换前那个激活 profile」，
// 只切一次的话被捕获的不是 a，下面全部断言指向错行、恒真通过。
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

    // loopback BASE_URL + 非占位 token（本机 cc-switch 形态）
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

    // 占位 token PROXY_MANAGED
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

    // 键存在但值为空串：不拿空串覆盖，也不因空值清空
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

    // 只有 default 槽位 + 一个新 extra 键 → 其余托管字段全清
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
    // extra_env 键集合完全由 settings.json 决定
    auto ee = parseExtraEnv(g.value().extraEnvJson);
    DOCTEST_REQUIRE(ee.isOk());
    DOCTEST_CHECK_EQ(static_cast<int>(ee.value().size()), 1);
    DOCTEST_CHECK_EQ(ee.value().at("NEW_KEY"), std::string("n"));

    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

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

    // 非法 JSON → fail，is_active 仍是 a、a 的字段一字未改
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

    // env 非对象 → 同款中止
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

    // 文件不存在：no-op 成功 + 照常切换 + a 一字未改
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

    // 0 字节空文件按「视为不存在」处理
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

    // 解析成功但无 env 键 → 同为 no-op
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

REGISTER_TEST(claude_reverse_no_active_skips,
              "claude_service: 反向同步⑥ 无激活 profile 时不读文件", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "revnoact");
    test_support::TempDir d("revnoact", kRevTempDirPrefix);
    auto paths = makeSyncPaths(d, tdb.handle());

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    svc.setSettingsSyncPaths(paths);

    // 坏 settings.json 不得挡住首建自动激活（无前置激活 profile → 不读文件）
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

    // settings.json 与 a 现值逐字段一致 → 不该发生任何写入
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

    // sidecar 声明 ANTHROPIC_MODEL 是托管键。NEW_KEY 不能省：它保证确实写了一次库，
    // 否则整次 no-op 会让「未被覆盖」白白通过。
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

    // settings.json 此刻是别的值：复制若误触发反向同步就会污染源
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
    // 凭据本体在副本自己那一行，不是共享源行
    auto k = svc.loadApiKey(cp.id);
    DOCTEST_REQUIRE(k.isOk());
    DOCTEST_CHECK_EQ(k.value(), std::string("sk-src-key-0123456789abcdef"));

    // 源行未动、激活态未变（副本不激活 ⇒ 复制与反向同步两需求零耦合）
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

    // 重名避让
    auto dup2 = svc.duplicateProfile(pa.id);
    DOCTEST_REQUIRE(dup2.isOk());
    DOCTEST_CHECK_EQ(dup2.value().name, std::string("复制源 副本 2"));
    auto dup3 = svc.duplicateProfile(pa.id);
    DOCTEST_REQUIRE(dup3.isOk());
    DOCTEST_CHECK_EQ(dup3.value().name, std::string("复制源 副本 3"));

    DOCTEST_CHECK(svc.duplicateProfile(99999).isFail());

    test_support::cleanupProfileKey(kTestKeychainService, pa.id);
    test_support::cleanupProfileKey(kTestKeychainService, cp.id);
    test_support::cleanupProfileKey(kTestKeychainService, dup2.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, dup3.value().id);
    return true;
})

REGISTER_TEST(claude_dup_missing_key_fails,
              "claude_service: 复制时源密钥缺失 fail 且无新行", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "dupkey");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto a = svc.createProfile("缺钥源", "https://src.example.com",
                               "sk-src-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    // 造「没存过密钥」：凭据在库列里，只能直接清列
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

// 复制失败两态，文案可分辨且带恢复提示。单列模型下「列空」与「值为空串」是同一种形态，故只有两态：
//   列空（NotFound）→「密钥缺失」+ 恢复提示
//   读失败（非 NotFound，如 Windows DPAPI 解不开）→ 原样透传（读不了 ≠ 没有）
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

    // 态二：解不开（非 NotFound 的真实错误）→ 原样透传错误码与信息。
    // 读不了 ≠ 没有，报「密钥缺失」会误导用户去重填一遍。
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

    // 撤掉注入源后复制成功：正常路径未被注入层带坏
    auto ok = svc.duplicateProfile(id);
    DOCTEST_REQUIRE(ok.isOk());
    auto copyKey = svc.loadApiKey(ok.value().id);
    DOCTEST_REQUIRE(copyKey.isOk());
    DOCTEST_CHECK_EQ(copyKey.value(), std::string("sk-ant-2state-0001"));
    return true;
})

REGISTER_TEST(claude_dup_stage2_rollback,
              "claude_service: 复制二段失败回滚且零残留", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "duprb");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto a = svc.createProfile("脏行源", "https://src.example.com",
                               "sk-src-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    // 直接 SQL 写坏 models_config 造 legacy 脏行：createProfile 不读它，二段 updateProfile 校验 JSON 时拒绝 → 触发回滚
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
    auto k = svc.loadApiKey(a.value().id);
    DOCTEST_REQUIRE(k.isOk());
    DOCTEST_CHECK_EQ(k.value(), std::string("sk-src-key-0123456789abcdef"));

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

REGISTER_TEST(claude_shared_config_crud_and_binding,
              "claude_service: 片段 CRUD 与 profile 绑定/解绑", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "sharedcrud");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 空名 / 非法 JSON 拒绝
    DOCTEST_CHECK(svc.createSharedConfig("   ", "{}").isFail());
    DOCTEST_CHECK(svc.createSharedConfig("片段A", "{bad-json").isFail());

    auto f = svc.createSharedConfig("片段A", R"({"MY_KEY": "from-shared"})");
    DOCTEST_REQUIRE(f.isOk());
    DOCTEST_CHECK(f.value().id > 0);
    // 重名 → AlreadyExists（name UNIQUE）
    auto dupName = svc.createSharedConfig("片段A", "{}");
    DOCTEST_CHECK(dupName.isFail());
    DOCTEST_CHECK(dupName.errorCode() == core::ErrorCode::AlreadyExists);

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

    // createProfile 尾部参数可建时即绑
    auto a = svc.createProfile("绑片段", "https://a.example.com",
                               "sk-ind-test-0123456789abcdef", "", false,
                               f.value().id);
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_CHECK_EQ(a.value().sharedConfigId, f.value().id);  // 返回值带上
    // getProfile / listProfiles 读路径也带该列
    auto ga = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(ga.isOk());
    DOCTEST_CHECK_EQ(ga.value().sharedConfigId, f.value().id);
    auto la = svc.listProfiles();
    DOCTEST_REQUIRE(la.isOk());
    DOCTEST_REQUIRE_EQ(la.value().size(), 1u);
    DOCTEST_CHECK_EQ(la.value()[0].sharedConfigId, f.value().id);

    // 解绑：updateProfile 置 0 → DB 列回 0
    Profile unbind = ga.value();
    unbind.sharedConfigId = 0;
    DOCTEST_REQUIRE(svc.updateProfile(unbind).isOk());
    auto ga2 = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(ga2.isOk());
    DOCTEST_CHECK_EQ(ga2.value().sharedConfigId, 0);

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

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

    // 绑定行解绑、profile 行一条不删（不做级联删）
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

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, c.value().id);
    return true;
})

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

    // 反重构：这里 models/extra_env 全空，二段补写的门槛条件不成立——绑定只能靠 createProfile 尾参那条通道带过去
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

// extraSettingsJson 往返：createProfile 尾参 / updateProfile 两分支 / 三条 SELECT。
// 跨文件不变量：rowToProfile 对每个可选列都是 `if (isOk())` 守卫——SELECT 漏掉该列不报错，
// 只会静默吞成默认值。所以「该字段在 getProfile / getActiveProfile / listProfiles 三条 SELECT 上都读得到」
// 必须逐条断言，漏改任何一条都不会自己变红。
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

    // 未传尾参 → 默认空串
    auto b = svc.createProfile("无JSON", "https://b.example.com",
                               "sk-xs-b-0123456789abcdef", "");
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_CHECK_EQ(b.value().extraSettingsJson, std::string());

    // getProfile / listProfiles 两条
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
    // 第三条 getActiveProfile：代理接管链直供它，漏列同样静默失效
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());
    auto gact = svc.getActiveProfile();
    DOCTEST_REQUIRE(gact.isOk());
    DOCTEST_CHECK_EQ(gact.value().id, a.value().id);
    DOCTEST_CHECK_EQ(gact.value().extraSettingsJson, settingsJson);
    // 反向口径：b 那条激活后读到空串。缺列时这里也是空，所以这条不是「恒非空」的反证，
    // 它的作用是钉「列被读到」而非「碰巧相等」。
    DOCTEST_REQUIRE(svc.setActiveProfile(b.value().id).isOk());
    auto gactB = svc.getActiveProfile();
    DOCTEST_REQUIRE(gactB.isOk());
    DOCTEST_CHECK_EQ(gactB.value().id, b.value().id);
    DOCTEST_CHECK_EQ(gactB.value().extraSettingsJson, std::string());
    // 交还激活态，为后续分支留确定初态
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());

    // updateProfile 分支一（apiKeyPlain 非空）
    Profile upd = ga.value();
    upd.apiKeyPlain = "sk-xs-new-0123456789abcdef";
    DOCTEST_REQUIRE(svc.updateProfile(upd).isOk());
    auto gu = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(gu.isOk());
    DOCTEST_CHECK_EQ(gu.value().extraSettingsJson, settingsJson);

    // updateProfile 分支二（只改元数据）：改 JSON 与清空都要落库
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

// 复制链带 extraSettingsJson：纯 JSON profile（两个 env 字段均空、仅 extraSettingsJson 非空）复制不得静默丢字段。
// 门槛条件：models_config / extra_env 至少一个非空时才走二段补写；此形态下二段不触发，
// createProfile 尾参与二段补写两条通道必须有一条把它带上。
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

// fallbackProfileId 默认 0 + 三条 SELECT 往返（同 extraSettingsJson 口径：rowToProfile 缺列静默吞成默认值）。
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

    // getProfile / getActiveProfile / listProfiles 三条都要读到
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

    // 清回 0 = 不启用；updateProfile 空密钥分支也要写该列
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
    // 校验失败时行未被污染（仍是 0）
    auto ga = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(ga.isOk());
    DOCTEST_CHECK_EQ(ga.value().fallbackProfileId, 0);

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

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

    DOCTEST_REQUIRE(svc.deleteProfile(b.value().id).isOk());
    auto ga = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(ga.isOk());
    DOCTEST_CHECK_EQ(ga.value().fallbackProfileId, 0);

    // 删带备用的行本身不炸
    DOCTEST_REQUIRE(svc.deleteProfile(a.value().id).isOk());

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

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

// resolveFallbackProfile 读路径三态：id==0 / 指向不存在的 id / 备用凭据列空 → nullopt；可用 → 带明文 key 的备用。
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

    // 未配备用（零 I/O）→ nullopt
    DOCTEST_CHECK_FALSE(svc.resolveFallbackProfile(a.value()).has_value());

    // 指向不存在的 profile → nullopt（读路径容错，回错不换家）
    Profile dangling = a.value();
    dangling.fallbackProfileId = 987654;
    DOCTEST_CHECK_FALSE(svc.resolveFallbackProfile(dangling).has_value());

    // 备用存在但凭据列空 → nullopt（「备用失效」档）
    Profile noKey = a.value();
    noKey.fallbackProfileId = b.value().id;
    DOCTEST_REQUIRE(test_support::clearStoredCredential(tdb.handle(), b.value().id));
    DOCTEST_CHECK_FALSE(svc.resolveFallbackProfile(noKey).has_value());

    // 备用可用 → 返回含明文 key 的完整备用，供代理转发注入。上面已清空凭据，先经服务写回。
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

// authField 三态（both / bearer / api_key）决定直连带哪个密钥头。
REGISTER_TEST(claude_service_auth_field_default_and_roundtrip,
              "claude_service: 认证字段默认 both + getProfile/getActiveProfile 往返", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "authfield_rt");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 不传尾参 → 默认 both
    auto d = svc.createProfile("默认", "https://d.example.com",
                               "sk-def-test-0123456789abcdef", "m");
    DOCTEST_REQUIRE(d.isOk());
    DOCTEST_CHECK_EQ(d.value().authField, std::string("both"));
    auto gd = svc.getProfile(d.value().id);
    DOCTEST_REQUIRE(gd.isOk());
    DOCTEST_CHECK_EQ(gd.value().authField, std::string("both"));

    // 显式 bearer：三条 SELECT 都要读到
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

REGISTER_TEST(claude_service_auth_field_update_both_branches,
              "claude_service: 认证字段 updateProfile 两分支（换 key / 不换 key）", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "authfield_upd");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto a = svc.createProfile("双分支", "https://u.example.com",
                               "sk-upd-test-0123456789abcdef", "m");
    DOCTEST_REQUIRE(a.isOk());

    // 分支一：apiKeyPlain 非空（换 key）
    Profile p1 = a.value();
    p1.apiKeyPlain = "sk-upd-test-newkey-0123456789";
    p1.authField = "api_key";
    DOCTEST_REQUIRE(svc.updateProfile(p1).isOk());
    auto g1 = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(g1.isOk());
    DOCTEST_CHECK_EQ(g1.value().authField, std::string("api_key"));

    // 反重构：分支二必须走 apiKeyPlain 留空这条。两条 UPDATE 语句是分开写的，只跑换 key 那条覆盖不到它。
    Profile p2 = g1.value();
    p2.authField = "bearer";
    DOCTEST_REQUIRE(svc.updateProfile(p2).isOk());
    auto g2 = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(g2.isOk());
    DOCTEST_CHECK_EQ(g2.value().authField, std::string("bearer"));

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

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
    // 模拟 UI 新建：以 createProfile 返回值为基，二段补写 models/env。反重构：必须以 createProfile 的返回值
    // 为基（而非重读一遍），否则二段补写用不到 createProfile 装进内存的那份字段，覆盖不回写路径。
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

REGISTER_TEST(claude_service_auth_field_invalid_normalized,
              "claude_service: 认证字段非法值写侧规范化为 both", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "authfield_norm");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    // createProfile 传非法串 → 落库与返回值都是 both
    auto a = svc.createProfile("规范化建", "https://n.example.com",
                               "sk-norm-test-0123456789abcdef", "m",
                               /*activateIfNoneActive=*/false,
                               /*sharedConfigId=*/0, "X");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_CHECK_EQ(a.value().authField, std::string("both"));

    // updateProfile 传非法串（直接构造 Profile）→ 读回 both
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

REGISTER_TEST(claude_service_dialect_override_default_and_roundtrip,
              "claude_service: dialectOverride 默认空串 + 三处 SELECT 往返", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "dialect_rt");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 不传尾参 → 空串（空串 = 未覆盖 = 按 host 推断）
    auto d = svc.createProfile("方言默认", "https://d.example.com",
                               "sk-dial-default-0123456789abcdef", "m");
    DOCTEST_REQUIRE(d.isOk());
    DOCTEST_CHECK(d.value().dialectOverride.empty());
    auto gd = svc.getProfile(d.value().id);
    DOCTEST_REQUIRE(gd.isOk());
    DOCTEST_CHECK(gd.value().dialectOverride.empty());

    // 显式 opencode_zen：getProfile / getActiveProfile / listProfiles 三条都要读到
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

    // 覆盖值对 profile 生效：host 后缀普通（默认方言）也被钉成 zen 方言
    const UpstreamDialect pinned =
        UpstreamDialect::forBaseUrl(gz.value().baseUrl,
                                    gz.value().dialectOverride);
    DOCTEST_CHECK(pinned.kind == UpstreamDialect::Kind::OpencodeZen);

    test_support::cleanupProfileKey(kTestKeychainService, d.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, z.value().id);
    return true;
})

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

    // 分支一：apiKeyPlain 非空（换 key），覆盖改成通用方言
    Profile p1 = a.value();
    p1.apiKeyPlain = "sk-dial-upd-newkey-0123456789";
    p1.dialectOverride = "anthropic_default";
    DOCTEST_REQUIRE(svc.updateProfile(p1).isOk());
    auto g1 = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(g1.isOk());
    DOCTEST_CHECK_EQ(g1.value().dialectOverride, std::string("anthropic_default"));

    // 分支二：apiKeyPlain 留空（不换 key），覆盖改回 zen
    Profile p2 = g1.value();
    p2.dialectOverride = "opencode_zen";
    DOCTEST_REQUIRE(svc.updateProfile(p2).isOk());
    auto g2 = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(g2.isOk());
    DOCTEST_CHECK_EQ(g2.value().dialectOverride, std::string("opencode_zen"));

    // 清空 = 回到 host 推断（空串是合法值，不是「写不进去」）
    Profile p3 = g2.value();
    p3.dialectOverride.clear();
    DOCTEST_REQUIRE(svc.updateProfile(p3).isOk());
    auto g3 = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(g3.isOk());
    DOCTEST_CHECK(g3.value().dialectOverride.empty());

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

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
    // 模拟 UI 新建：以 createProfile 返回值为基，二段补写 models/env（与 authField 那条同构）
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

REGISTER_TEST(claude_service_dialect_override_invalid_normalized,
              "claude_service: dialectOverride 非法值写侧归一为空串", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "dialect_norm");
    ClaudeService svc(tdb.handle(), kTestKeychainService);

    // createProfile 传非法串（大小写也对不上）→ 落库与返回值都是空串
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

    // live 侧带非守卫变化，保证确实写了一次库（否则整次 no-op，dialectOverride 原样保留会白白通过）
    d.write(paths.settingsFile, R"({"env": {
        "ANTHROPIC_BASE_URL": "https://new.example.com/",
        "ANTHROPIC_AUTH_TOKEN": "sk-new-upstream-9876543210",
        "MY_EXTRA": "extra-value"
    }})");

    DOCTEST_REQUIRE(svc.setActiveProfile(b.value().id).isOk());

    auto gotA = svc.getProfile(a.value().id);
    DOCTEST_REQUIRE(gotA.isOk());
    // 确实被反向同步写过（base_url 已收编）
    DOCTEST_CHECK_EQ(gotA.value().baseUrl, std::string("https://new.example.com"));
    // 而方言覆盖原样保留：反向同步只碰 models / env / base_url / token
    DOCTEST_CHECK_EQ(gotA.value().dialectOverride, std::string("opencode_zen"));

    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    test_support::cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// runProfile 直连形态：临时 settings + --settings，不启代理、不切激活、不读写 ~/.claude/settings.json。
//
// run-settings 临时文件里带**明文 API key**，POSIX 下必须 0600。
// 反重构：平台分支必须留在函数体内——doctest 宏参数里嵌预处理指令是 UB，
// -Werror,-Wembedded-directive 下直接编译失败；写进宏参数会让整个测试文件编不过。
// Windows 走 DPAPI/ACL 口径，权限位不适用 → 该分支恒 true，不构成保护断言（真实保护由 credential 用例覆盖）。
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

    // 接管通道注入（指向临时文件）：零全局回归闸断言 settings.json 与 sidecar 都不被本次运行触碰
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

    // 假 launch 收到 --settings + run-settings/ 下的绝对路径
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

    // 落盘文件 = --settings 指向的同一路径；内容 = 合法 settings JSON（env 子对象）
    DOCTEST_REQUIRE(std::filesystem::exists(settingsFile));
    // 权限 0600：atomicWriteFile 的 prepareRestrictedFile 让 tmp 出生即 0600，写入侧再过一道
    // restrictToOwnerOnly；此处断言的是最终落盘态，两道都得保住。
    DOCTEST_CHECK(runSettingsFileIsOwnerOnly(settingsFile));
    auto parsed = platform::json::parse(readFileToString(settingsFile));
    DOCTEST_REQUIRE(parsed.isOk());
    DOCTEST_REQUIRE(parsed.value().isObject());
    auto envVal = parsed.value().asObject().get("env");
    DOCTEST_REQUIRE(envVal.isObject());
    auto envObj = envVal.asObject();
    // BASE_URL 去尾斜杠（profile 里存的是带尾 '/' 的 url）
    DOCTEST_CHECK_EQ(envObj.get("ANTHROPIC_BASE_URL").asString(),
                     std::string("https://api.anthropic.com"));
    // authField 默认 both → 只写 ANTHROPIC_AUTH_TOKEN
    DOCTEST_CHECK_EQ(envObj.get("ANTHROPIC_AUTH_TOKEN").asString(),
                     std::string("sk-ant-run-0001"));
    DOCTEST_CHECK(!envObj.has("ANTHROPIC_API_KEY"));

    // 零全局回归闸：settings.json 内容未变、sidecar 未建、激活态未变、代理未动
    DOCTEST_CHECK_EQ(readFileToString(paths.settings), settingsBefore);
    DOCTEST_CHECK(!test_support::configRowExists(
        &tdb.handle(), module::claude::settings_sync::kSidecarConfigKey));
    auto activeAfter = svc.getActiveProfile();
    DOCTEST_REQUIRE(activeAfter.isOk());
    DOCTEST_CHECK_EQ(activeAfter.value().id, a.value().id);
    DOCTEST_CHECK(!svc.isProxyRunning());

    // 假 launch 没真起会话，生产侧的自删不会发生 → 用例自清临时文件
    std::error_code rmEc;
    std::filesystem::remove(settingsFile, rmEc);
    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

// 生效 JSON（profile ⊕ 片段，片段赢）并入临时 settings 顶层。反重构：这段接线整段删掉本用例即红，
// 不能因为「看不出它干什么」就清掉。
REGISTER_TEST(claude_service_run_profile_merges_extra_settings,
              "claude_service: runProfile 临时 settings 并入生效 JSON（片段赢/model 抑制，F-01 接线钉）", {
    test_support::DataRootEnv root(kDbPrefix, "runprofile6");
    test_support::TestDb tdb(kDbPrefix, "runprofile6");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    TempSyncPaths paths("runprofile6", tdb.handle());
    svc.setSettingsSyncPaths(paths.paths());

    // 片段与 profile 各含独有键 + 同键（片段赢）；JSON 含顶层 model（触发 ANTHROPIC_MODEL 抑制——
    // legacy model 列本会生成该 env 键）
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
    // 生效 JSON 并入顶层：同键片段赢，独有键与 profile 键都在
    DOCTEST_CHECK_EQ(rootObj.get("statusLine").asString(),
                     std::string("frag-s"));
    DOCTEST_CHECK_EQ(rootObj.get("model").asString(), std::string("json-model"));
    DOCTEST_REQUIRE(rootObj.get("permissions").isObject());
    // model 抑制随行生效：顶层有 model 时 env 不再生成 ANTHROPIC_MODEL（legacy m-legacy 不落 env）
    auto envObj = rootObj.get("env").asObject();
    DOCTEST_CHECK(!envObj.has("ANTHROPIC_MODEL"));
    DOCTEST_CHECK_EQ(envObj.get("ANTHROPIC_AUTH_TOKEN").asString(),
                     std::string("sk-ant-run-0006"));
    // 零全局闸：settings.json / sidecar 未被触碰
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

    // 注入 credential::unprotect 失败。反重构：ScopedFailUnprotect 必须放在 createProfile 之后——
    // 放前面连写入路径也会失败，测的就不是「读失败」这条分支了。
    ScopedFailUnprotect cred;

    int calls = 0;
    svc.setTerminalLauncher([&](const platform::terminal::LaunchSpec&) {
        ++calls;
        return core::Result<void>::ok();
    });
    auto r = svc.runProfile(a.value().id, nullptr);
    DOCTEST_CHECK(r.isFail());     // 中止
    DOCTEST_CHECK_EQ(calls, 0);    // 不开会话
    // 未落盘：run-settings 目录都不该建（中止发生在写文件之前）
    DOCTEST_CHECK(!std::filesystem::exists(
        std::filesystem::path(root.dir) / "run-settings"));

    return true;
})

REGISTER_TEST(claude_service_run_profile_zero_global_change_gate,
              "claude_service: runProfile 零全局回归闸——settings.json 未写、is_active 未变、代理未动（激活/非激活都跑）", {
    test_support::DataRootEnv root(kDbPrefix, "runprofile3");
    test_support::TestDb tdb(kDbPrefix, "runprofile3");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 接管通道路径注入（指向临时 settings.json）：回归闸断言的就是它不被碰
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

    // 目标 = 激活 profile（a）：成功，settings.json 一字未变，激活态未变
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

    // 目标 ≠ 激活 profile（b）：不切激活，坏 settings.json 也不挡路（直连形态根本不解析全局文件）；
    // 先写坏文件本身作反证。
    paths.writeSettings("{ 这不是合法 JSON");
    auto rb = svc.runProfile(b.value().id, nullptr);
    DOCTEST_REQUIRE(rb.isOk());
    DOCTEST_CHECK_EQ(readFileToString(paths.settings),
                     std::string("{ 这不是合法 JSON"));

    // 回归闸汇总：sidecar 未建、激活态未变、代理未动、两次都开窗
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

    // 反重构：用同名**普通文件**占住 run-settings 路径而不是「只读目录」——Windows 上只读目录拦不住建文件，
    // 普通文件占位才是跨平台可复现的「目录不可建」形态。
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

// buildDirectEnv 是纯函数：直连形态下最终落到 --settings 临时文件里的 env 全由它生成。
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
    // both（默认）与未知值走同一分支：authFieldFromText 对未知值兜底成 both
    {
        auto env = module::claude::buildDirectEnv(
            "https://api.anthropic.com", "sk-both-1", "both", "", "");
        DOCTEST_CHECK_EQ(env["ANTHROPIC_AUTH_TOKEN"], std::string("sk-both-1"));
        DOCTEST_CHECK(env.find("ANTHROPIC_API_KEY") == env.end());
        auto env2 = module::claude::buildDirectEnv(
            "https://x.example.com", "sk-both-2", "什么奇怪的值", "", "");
        DOCTEST_CHECK_EQ(env2["ANTHROPIC_AUTH_TOKEN"], std::string("sk-both-2"));
        DOCTEST_CHECK(env2.find("ANTHROPIC_API_KEY") == env2.end());
    }
    // 空 baseUrl / 空密钥 → 不产生空值键（BASE_URL 空 = Claude Code 回落内置默认上游）
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
    // 反重构：签名里没有「片段 env 叠加」形参，别再往里加——片段一律走 extraSettingsJson 的 JSON 域
    auto env = module::claude::buildDirectEnv(
        "https://api.anthropic.com/", "sk-real-key", "both",
        models, extra);

    // 托管键压过 extra_env 的同名旧值（BASE_URL 去尾斜杠后取 profile 值）
    DOCTEST_CHECK_EQ(env["ANTHROPIC_BASE_URL"],
                     std::string("https://api.anthropic.com"));
    DOCTEST_CHECK_EQ(env["ANTHROPIC_AUTH_TOKEN"], std::string("sk-real-key"));
    // extra 带进的另一个密钥键不存活：直连只写托管的那一个
    DOCTEST_CHECK(env.find("ANTHROPIC_API_KEY") == env.end());
    // 非托管 extra_env 键原样入表
    DOCTEST_CHECK_EQ(env["MY_VAR"], std::string("v"));
    // 非空槽位写 envKey；name 空 → 派生显示名（剥 [1M] 类标记）
    DOCTEST_CHECK_EQ(env["ANTHROPIC_MODEL"], std::string("claude-sonnet-4"));
    DOCTEST_CHECK_EQ(env["ANTHROPIC_DEFAULT_SONNET_MODEL"],
                     std::string("claude-sonnet-4-5"));
    DOCTEST_CHECK_EQ(env["ANTHROPIC_DEFAULT_SONNET_MODEL_NAME"],
                     std::string("S4.5"));
    // model 为空的槽位跳过，不生成 env 键
    DOCTEST_CHECK(env.find("ANTHROPIC_DEFAULT_OPUS_MODEL") == env.end());
    // extra_env 别名归一：AUTO_COMPACT_ENABLED=1 → DISABLE_AUTO_COMPACT=0
    DOCTEST_CHECK_EQ(env["DISABLE_AUTO_COMPACT"], std::string("0"));
    DOCTEST_CHECK(env.find("AUTO_COMPACT_ENABLED") == env.end());
    return true;
})

// 惰性装配反转 atexit LIFO：main.cpp 的 getClaudeService() 持有 static unique_ptr<ClaudeService>，
// 惰性装配路径下它构造在 armExitHook 注册之后 → 退出时析构先于 exit hook，hook 里的模块 onStop
// （claude_module.cpp 的 ClaudeService::global() 分支）读到的是已析构对象。
// 契约：析构即摘牌，global() 回 nullptr → onStop 走「未运行」分支，不悬垂读、不进 stopProxy。
REGISTER_TEST(claude_service_destroy_clears_global,
              "claude_service: 析构清 global 单例指针（退出时序 UAF 防护）", {
    test_support::TestDb tdb(kDbPrefix, "global_clear");
    {
        module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
        module::claude::ClaudeService::setGlobal(&svc);
        // 反恒真：这一行保证契约没有退化成「setGlobal 根本不生效」——那样析构后的 nullptr 断言会恒绿
        DOCTEST_CHECK(module::claude::ClaudeService::global() == &svc);
    }   // ← svc 析构（等价于退出时 unique_ptr 先析构、exit hook 的 onStop 后跑）
    DOCTEST_CHECK(module::claude::ClaudeService::global() == nullptr);
    return true;
})

// 守卫反向：析构里的 `if (g == this)` 只摘自己的牌，未注册对象不得误清别人的注册。
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

// validateProxyPreconditions：无激活 profile / 上游不可达 / 非 2xx 都不得起代理，也不得留下接管痕迹。
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

    // 端口 1 几乎必然拒绝连接
    auto created = svc.createProfile("Bad", "http://127.0.0.1:1",
                                     "sk-ant-pv-7777", "m");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());  // 首次激活：不读文件

    auto r = svc.validateProxyPreconditions(500);
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK(r.errorCode() == core::ErrorCode::NetworkUnreachable);
    DOCTEST_CHECK(r.errorMessage().find("前置校验未通过（2/2 连通测试）") !=
                  std::string::npos);
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());
    DOCTEST_CHECK_EQ(svc.proxyPort(), 0);
    // settings.json 一字未改、无 sidecar：没走到 syncTakeOver
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
    upstream401.route("POST", "/v1/messages",
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
    upstream.route("POST", "/v1/messages",
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
    DOCTEST_CHECK_EQ(lastPath, std::string("/v1/messages"));
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());   // 校验本身绝不 bind

    // 通过后 startProxy 照常，端口 0 = OS 分配
    auto started = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(started.isOk());
    DOCTEST_CHECK(svc.isProxyRunning());
    DOCTEST_CHECK(svc.proxyPort() > 0);
    // 不变式：校验不在 startProxy 内部。反重构：不能为「省一次探测」把校验塞进 startProxy——
    // 那会让这里变成 2，且破坏 startProxy「无激活 profile 仍接管」的既有契约。
    DOCTEST_CHECK_EQ(hitCount, 1);
    svc.stopProxy();
    upstream.stop();
    test_support::cleanupProfileKey(kTestKeychainService, created.value().id);
    return true;
})

REGISTER_TEST(claude_pv_timeout_honored,
              "claude_service: 前置校验⑤ 探测超时按入参生效（不静默回落 15s）", {
    // mock 上游挂 1500ms 才回 200，probeTimeoutMs=150 → 必须 fail。
    // 超时参数若没接上（回落 testConnection 的 15000），请求会在 1.5s 后成功 → 本用例红。
    // 断言是 ok/fail 二值、不测墙钟，10× 余量保证负载抖动下也不误判；收尾 slow.stop() 会 join
    // 上游 worker，故本用例总耗时 ≈1.5s。
    platform::http::Server slow("127.0.0.1", 0);
    slow.route("POST", "/v1/messages",
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

// -- runProfile 工作目录参数（folder_picker 注入 → workingDir 链路）-------------------

// 一：传入显式工作目录 → LaunchSpec.workingDir 使用该目录（而非 homeDir）
REGISTER_TEST(claude_run_profile_explicit_working_dir,
              "claude_service: runProfile 显式 workingDir → LaunchSpec 收到该目录", {
    test_support::DataRootEnv root(kDbPrefix, "rundir1");
    test_support::TestDb tdb(kDbPrefix, "rundir1");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    TempSyncPaths paths("rundir1", tdb.handle());
    svc.setSettingsSyncPaths(paths.paths());

    auto a = svc.createProfile("DirRun", "https://api.anthropic.com/",
                               "sk-ant-rundir-0001", "");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());

    int calls = 0;
    platform::terminal::LaunchSpec got;
    svc.setTerminalLauncher([&](const platform::terminal::LaunchSpec& s) {
        ++calls;
        got = s;
        return core::Result<void>::ok();
    });

    const std::string customDir = "/tmp/devdesk-test-workdir";
    std::string hint;
    auto r = svc.runProfile(a.value().id, &hint, customDir);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(calls, 1);
    // 关键断言：workingDir 已替换为传入目录
    DOCTEST_CHECK_EQ(got.workingDir, customDir);
    DOCTEST_CHECK_EQ(got.program, std::string("claude"));

    // 清理临时 settings 文件
    DOCTEST_REQUIRE_EQ(got.args.size(), static_cast<size_t>(2));
    std::error_code rmEc;
    std::filesystem::remove(std::filesystem::path(got.args[1]), rmEc);
    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

// 二：不传 workingDir（回退 homeDir 默认值）—— 向后兼容现有调用方
REGISTER_TEST(claude_run_profile_default_working_dir,
              "claude_service: runProfile 不传 workingDir → 回退 homeDir", {
    test_support::DataRootEnv root(kDbPrefix, "rundir2");
    test_support::TestDb tdb(kDbPrefix, "rundir2");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    TempSyncPaths paths("rundir2", tdb.handle());
    svc.setSettingsSyncPaths(paths.paths());

    auto a = svc.createProfile("DefaultDir", "https://api.anthropic.com/",
                               "sk-ant-rundir-0002", "");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());

    int calls = 0;
    platform::terminal::LaunchSpec got;
    svc.setTerminalLauncher([&](const platform::terminal::LaunchSpec& s) {
        ++calls;
        got = s;
        return core::Result<void>::ok();
    });

    // 不传 workingDir → 应回退到 homeDir()
    auto r = svc.runProfile(a.value().id, nullptr);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(calls, 1);
    DOCTEST_CHECK_EQ(got.workingDir, platform::terminal::homeDir());
    DOCTEST_CHECK(!got.workingDir.empty());

    // 清理临时 settings
    DOCTEST_REQUIRE_EQ(got.args.size(), static_cast<size_t>(2));
    std::error_code rmEc;
    std::filesystem::remove(std::filesystem::path(got.args[1]), rmEc);
    test_support::cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

// 三：app_config last-dir 键值读写往返 —— claudeRunLastDir 键按既有 app_config 惯例
REGISTER_TEST(claude_module_last_dir_appconfig_roundtrip,
              "app_config: claude.run.lastDir 键读写往返（last-dir 记忆）", {
    test_support::TestDb tdb(kDbPrefix, "lastdir1");
    // 写
    const std::string lastDir = "/home/user/projects/my-repo";
    auto wrote = core::appconfig::set(tdb.handle(), "claude.run.lastDir", lastDir);
    DOCTEST_CHECK(wrote.isOk());
    // 读回
    auto got = core::appconfig::get(tdb.handle(), "claude.run.lastDir");
    DOCTEST_REQUIRE(got.isOk());
    DOCTEST_CHECK_EQ(got.value(), lastDir);
    // 键不存在 → fail
    auto missing = core::appconfig::get(tdb.handle(), "claude.run.lastDir.NEVER");
    DOCTEST_CHECK(missing.isFail());
    DOCTEST_CHECK(missing.errorCode() == core::ErrorCode::NotFound);
    return true;
})
