// tests/test_modules/test_settings_sync.cpp
// Claude Code settings.json 接管通道（takeOver / restore / 反向读）单测。
// 跨文件前提：
//   - 全部经 Paths 注入临时目录与临时库，绝不碰真实 ~/.claude；Paths.db 显式传入，
//     本文件不注册 appDatabase。
//   - sidecar 是 app_config 表的一行，库未 migrate 或行损坏时 takeOver/restore 一律中止，
//     不得改用户文件。
//   - readSettingsEnvForSync 的 ok 与 present 是两个独立维度：ok=false 才中止调用方切换，
//     present=false 只表示「这份文件里没有可同步的 env」。
#include "doctest.h"
#include "test_support.h"

#include "claude_service.h"
#include "core/database.h"
#include "core/module_interface.h"
#include "core/router.h"
#include "platform/json.h"
#include "platform/keychain.h"
#include "provider_config.h"
#include "settings_sync.h"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>  // 仅反例用例（独占句柄造「存在但打不开」）
#endif

namespace {

constexpr const char* kTestKeychainService = "DevDeskTestSettingsSync";

constexpr const char* kSyncTempDirPrefix = "devdesk_test_sync_";

// 临时 db 文件名前缀（tempDbPath / TestDb 的第一实参）
constexpr const char* kDbPrefix = "devdesk_test_sync_db_";

// sidecar（接管前 settings.json 的 env 快照）是 app_config 表的一行（键 kSidecarConfigKey）
// ——故每个用例除了临时 settings.json 目录，还得有一份已 migrate 的库供 sidecar 落行。
// 命名对应：目录 d → 库 sdb，目录 d2 → 库 sdb2；同一用例内两个目录不能共用一份库
//（sidecar 只有一行，共用会让两个 Paths 互相看见对方的快照）。
module::claude::settings_sync::Paths makePaths(const test_support::TempDir& d,
                                               core::data::SqliteDb& db) {
    module::claude::settings_sync::Paths p;
    p.settingsFile = d.file("settings.json");
    p.db = &db;
    return p;
}

// sidecar 的库夹具（只造库，不注册 appDatabase——本文件全程显式传 db）
struct SyncDb {
    test_support::TestDb tdb;
    explicit SyncDb(const std::string& tag)
        : tdb(kDbPrefix, "syncsidecar_" + tag) {}
    core::data::SqliteDb& handle() { return tdb.handle(); }
};

// 读 settings.json 的 env 块为 map（不存在 / 无 env → 空表）
std::map<std::string, std::string> readEnv(const std::filesystem::path& f) {
    std::map<std::string, std::string> out;
    std::ifstream i(f, std::ios::binary);
    if (!i) return out;
    std::ostringstream ss;
    ss << i.rdbuf();
    auto parsed = platform::json::parse(ss.str());
    if (parsed.isFail() || !parsed.value().isObject()) return out;
    auto obj = parsed.value().asObject();
    if (!obj.has("env") || !obj.get("env").isObject()) return out;
    auto env = obj.get("env").asObject();
    for (const auto& k : env.keys()) {
        out[k] = env.get(k).asString();
    }
    return out;
}

// 读 settings.json 的非 env 顶层字段值（字符串化），验证保留
std::string topLevelString(const std::filesystem::path& f, const char* key) {
    std::ifstream i(f, std::ios::binary);
    std::ostringstream ss;
    ss << i.rdbuf();
    auto parsed = platform::json::parse(ss.str());
    if (parsed.isFail() || !parsed.value().isObject()) return {};
    auto obj = parsed.value().asObject();
    if (!obj.has(key)) return {};
    return obj.get(key).asString();
}


// 假 Keychain 注入缝（tests/test_platform/test_keychain.cpp 的 CountingKeychain 同款）。
// 走真门面的端到端用例里 createProfile 会向**系统钥匙串**写测试密钥，而清理只在函数尾部
// deleteSecret——中间任一 DOCTEST_REQUIRE 失败就残留一条真实条目。装上假后端后该用例对
// 真钥匙串零读写，且「写了几条」本身变成可断言的。
// 不要因为某个用例「不查 keychain」就省掉它：污染发生在 createProfile 内部。
struct FakeKeychain {
    std::map<std::string, std::string> data;

    static std::string keyOf(const std::string& service,
                             const std::string& account) {
        return service + '\x1f' + account;
    }
    void store(const std::string& service, const std::string& account,
               const std::string& secret) {
        data[keyOf(service, account)] = secret;
    }
    std::string load(const std::string& service,
                     const std::string& account) const {
        const auto it = data.find(keyOf(service, account));
        return it == data.end() ? std::string() : it->second;
    }
    void remove(const std::string& service, const std::string& account) {
        data.erase(keyOf(service, account));
    }
    size_t count() const { return data.size(); }
};

FakeKeychain* g_fakeKeychain = nullptr;

core::Result<void> fakeStoreSecret(const std::string& service,
                                   const std::string& account,
                                   const std::string& secret) {
    g_fakeKeychain->store(service, account, secret);
    return core::Result<void>::ok();
}

core::Result<std::string> fakeLoadSecret(const std::string& service,
                                         const std::string& account) {
    if (g_fakeKeychain->load(service, account).empty()) {
        return core::Result<std::string>::fail(core::ErrorCode::NotFound,
                                               "Keychain item not found");
    }
    return core::Result<std::string>::ok(g_fakeKeychain->load(service, account));
}

core::Result<void> fakeDeleteSecret(const std::string& service,
                                    const std::string& account) {
    g_fakeKeychain->remove(service, account);
    return core::Result<void>::ok();   // 不存在也 Ok（幂等，同真实实现）
}

core::Result<std::vector<std::string>> fakeDeleteSecretsNotIn(
    const std::string& service, const std::vector<std::string>& keepAccounts) {
    const std::string prefix = service + '\x1f';
    std::vector<std::string> removed;
    for (auto it = g_fakeKeychain->data.begin();
         it != g_fakeKeychain->data.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            const std::string account = it->first.substr(prefix.size());
            if (std::find(keepAccounts.begin(), keepAccounts.end(), account) ==
                keepAccounts.end()) {
                removed.push_back(account);
                it = g_fakeKeychain->data.erase(it);
                continue;
            }
        }
        ++it;
    }
    return core::Result<std::vector<std::string>>::ok(std::move(removed));
}

// RAII：安装假后端，析构恢复真实后端——用例 fail-fast（return false）或抛异常
// 都不会把假后端泄漏给后续用例
struct ScopedFakeKeychain {
    FakeKeychain fake;
    platform::keychain::Backend table;
    const platform::keychain::Backend* prev;

    ScopedFakeKeychain() {
        g_fakeKeychain = &fake;
        table = platform::keychain::Backend{
            &fakeStoreSecret, &fakeLoadSecret, &fakeDeleteSecret,
            &fakeDeleteSecretsNotIn};
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

REGISTER_TEST(sync_build_proxy_env, "settings_sync: buildProxyEnv 槽位/派生/跳过", {
    using module::claude::buildProxyEnv;

    const std::string modelsJson = R"({
        "default": {"model": "fangzhou/glm-5.3[1M]", "name": ""},
        "sonnet":  {"model": "fangzhou/glm-5.3[1M]", "name": "我的 Sonnet"},
        "opus":    {"model": "", "name": ""},
        "haiku":   {"model": "claude-3-5-haiku", "name": ""},
        "fable":   {"model": "", "name": ""},
        "subagent": {"model": "glm-5.3[1M]", "name": ""}
    })";
    const auto env = buildProxyEnv(modelsJson, /*extraEnvJson=*/"", 8888);

    DOCTEST_CHECK_EQ(env.at("ANTHROPIC_BASE_URL"),
                     std::string("http://127.0.0.1:8888"));
    // 代理无鉴权：写占位符（真实上游 key 由代理转发时注入）
    DOCTEST_CHECK_EQ(env.at("ANTHROPIC_AUTH_TOKEN"),
                     std::string("PROXY_MANAGED"));
    // default 槽位：model 写 ANTHROPIC_MODEL；name 空 → 派生剥掉 [1M] 标记。
    // ANTHROPIC_MODEL 没有官方的 _NAME 变体，故不生成
    DOCTEST_CHECK_EQ(env.at("ANTHROPIC_MODEL"), std::string("fangzhou/glm-5.3[1M]"));
    DOCTEST_CHECK(env.find("ANTHROPIC_MODEL_NAME") == env.end());
    // sonnet：显式配置的显示名优先于派生
    DOCTEST_CHECK_EQ(env.at("ANTHROPIC_DEFAULT_SONNET_MODEL"),
                     std::string("fangzhou/glm-5.3[1M]"));
    DOCTEST_CHECK_EQ(env.at("ANTHROPIC_DEFAULT_SONNET_MODEL_NAME"),
                     std::string("我的 Sonnet"));
    // haiku：name 空 → 派生；无 [1M] 标记则原样保留
    DOCTEST_CHECK_EQ(env.at("ANTHROPIC_DEFAULT_HAIKU_MODEL_NAME"),
                     std::string("claude-3-5-haiku"));
    // opus/fable 槽位为空 → 整个键跳过
    DOCTEST_CHECK(env.find("ANTHROPIC_DEFAULT_OPUS_MODEL") == env.end());
    DOCTEST_CHECK(env.find("ANTHROPIC_DEFAULT_FABLE_MODEL") == env.end());
    // subagent 槽位没有 _NAME 变体
    DOCTEST_CHECK_EQ(env.at("CLAUDE_CODE_SUBAGENT_MODEL"), std::string("glm-5.3[1M]"));
    DOCTEST_CHECK(env.find("CLAUDE_CODE_SUBAGENT_MODEL_NAME") == env.end());

    // 恒定写入占位凭据：buildProxyEnv 没有「空 token 跳过」分支
    const auto noTok = buildProxyEnv("", "", 8888);
    DOCTEST_CHECK_EQ(noTok.at("ANTHROPIC_AUTH_TOKEN"),
                     std::string("PROXY_MANAGED"));
    return true;
})

REGISTER_TEST(sync_build_proxy_env_extra, "settings_sync: extra_env 别名与优先级", {
    using module::claude::buildProxyEnv;

    const std::string extra = R"({
        "AUTO_COMPACT_ENABLED": "1",
        "DISABLE_AUTOUPDATER": "1",
        "ANTHROPIC_BASE_URL": "https://stale.example.com",
        "ANTHROPIC_AUTH_TOKEN": "stale-token"
    })";
    const auto env = buildProxyEnv("", extra, 9999);

    // 别名归一：AUTO_COMPACT_ENABLED=1（开）→ DISABLE_AUTO_COMPACT=0（不禁用）
    DOCTEST_CHECK_EQ(env.at("DISABLE_AUTO_COMPACT"), std::string("0"));
    DOCTEST_CHECK(env.find("AUTO_COMPACT_ENABLED") == env.end());
    DOCTEST_CHECK_EQ(env.at("DISABLE_AUTOUPDATER"), std::string("1"));
    // 冲突时托管键优先：代理 BASE_URL 与占位凭据覆盖导入的旧值
    DOCTEST_CHECK_EQ(env.at("ANTHROPIC_BASE_URL"),
                     std::string("http://127.0.0.1:9999"));
    DOCTEST_CHECK_EQ(env.at("ANTHROPIC_AUTH_TOKEN"),
                     std::string("PROXY_MANAGED"));
    return true;
})

REGISTER_TEST(sync_takeover_restore_roundtrip, "settings_sync: 接管合并与还原往返", {
    using namespace module::claude;
    test_support::TempDir d("roundtrip", kSyncTempDirPrefix);
    SyncDb sdb("sync_roundtrip");
    auto paths = makePaths(d, sdb.handle());

    d.write(paths.settingsFile, R"({
        "permissions": {"allow": ["Bash"]},
        "model": "claude-sonnet-4-5",
        "env": {
            "ANTHROPIC_BASE_URL": "https://user-own.example.com",
            "MY_CUSTOM_KEY": "keep-me",
            "DISABLE_AUTOUPDATER": "1"
        }
    })");
    const std::string original = d.read(paths.settingsFile);

    Profile p;
    p.model = "fangzhou/glm-5.3[1M]";
    p.modelsConfigJson = R"({"default": {"model": "fangzhou/glm-5.3[1M]"}})";
    p.extraEnvJson = R"({"DISABLE_AUTOUPDATER": "1"})";
    auto r = settings_sync::takeOver(p, 8888, paths);
    DOCTEST_REQUIRE(r.ok);
    DOCTEST_CHECK(r.warnings.empty());

    {
        const auto env = readEnv(paths.settingsFile);
        DOCTEST_CHECK_EQ(env.at("ANTHROPIC_BASE_URL"),
                         std::string("http://127.0.0.1:8888"));
        DOCTEST_CHECK_EQ(env.at("ANTHROPIC_AUTH_TOKEN"),
                         std::string("PROXY_MANAGED"));
        DOCTEST_CHECK_EQ(env.at("ANTHROPIC_MODEL"),
                         std::string("fangzhou/glm-5.3[1M]"));
        DOCTEST_CHECK_EQ(env.at("MY_CUSTOM_KEY"), std::string("keep-me"));
        DOCTEST_CHECK_EQ(env.at("DISABLE_AUTOUPDATER"), std::string("1"));
        DOCTEST_CHECK_EQ(topLevelString(paths.settingsFile, "model"),
                         std::string("claude-sonnet-4-5"));
        DOCTEST_CHECK(test_support::configRowExists(&sdb.handle(), module::claude::settings_sync::kSidecarConfigKey));
    }

    // 还原契约：同名托管键恢复用户原值，接管前不存在的键被删除，其余不动
    auto rr = settings_sync::restore(paths);
    DOCTEST_REQUIRE(rr.ok);
    {
        const auto env = readEnv(paths.settingsFile);
        DOCTEST_CHECK_EQ(env.at("ANTHROPIC_BASE_URL"),
                         std::string("https://user-own.example.com"));
        DOCTEST_CHECK(env.find("ANTHROPIC_AUTH_TOKEN") == env.end());  // 原本不存在 → 删除
        DOCTEST_CHECK(env.find("ANTHROPIC_MODEL") == env.end());
        DOCTEST_CHECK_EQ(env.at("MY_CUSTOM_KEY"), std::string("keep-me"));
        DOCTEST_CHECK_EQ(env.at("DISABLE_AUTOUPDATER"), std::string("1"));
        DOCTEST_CHECK_EQ(topLevelString(paths.settingsFile, "model"),
                         std::string("claude-sonnet-4-5"));
        DOCTEST_CHECK_FALSE(test_support::configRowExists(&sdb.handle(), module::claude::settings_sync::kSidecarConfigKey));
    }
    // original 故意不参与断言：还原后格式被 pretty 归一，只有 env 值逐键等价才成立，
    // 字节级比较必然不等。这行 (void) 只是消未用告警，不是「忘了断言」
    (void)original;
    return true;
})

REGISTER_TEST(sync_retakeover_keeps_first_snapshot, "settings_sync: 热切换保留首份快照并并入托管键", {
    using namespace module::claude;
    test_support::TempDir d("retake", kSyncTempDirPrefix);
    SyncDb sdb("sync_retake");
    auto paths = makePaths(d, sdb.handle());

    d.write(paths.settingsFile,
            R"({"env": {"MY_KEY": "user"}})");

    // 第一次接管：profile A 带 sonnet 槽位
    Profile a;
    const std::string modelsA = R"({"sonnet": {"model": "m-a"}})";
    a.modelsConfigJson = modelsA;
    DOCTEST_REQUIRE(settings_sync::takeOver(a, 8888, paths).ok);

    // 第二次接管（切 profile）：B 无 sonnet 槽位 → 该键不在 B 的托管集，但并集后
    // 仍归 DevDesk 管理（restore 时删除，不给客户端留陈旧模型名）
    Profile b;
    const std::string modelsB = R"({"opus": {"model": "m-b"}})";
    b.modelsConfigJson = modelsB;
    DOCTEST_REQUIRE(settings_sync::takeOver(b, 8888, paths).ok);
    {
        const auto env = readEnv(paths.settingsFile);
        DOCTEST_CHECK(env.find("ANTHROPIC_DEFAULT_SONNET_MODEL") == env.end());
        DOCTEST_CHECK_EQ(env.at("ANTHROPIC_DEFAULT_OPUS_MODEL"),
                         std::string("m-b"));
        DOCTEST_CHECK_EQ(env.at("MY_KEY"), std::string("user"));
    }
    // 但 sidecar 仍记录它（还原语义：旧托管键仍归 DevDesk 管）
    {
        const auto sidecarText = test_support::readRawConfigRow(&sdb.handle(), module::claude::settings_sync::kSidecarConfigKey);
        DOCTEST_CHECK(sidecarText.find("ANTHROPIC_DEFAULT_SONNET_MODEL") !=
                      std::string::npos);
        DOCTEST_CHECK(sidecarText.find("ANTHROPIC_DEFAULT_OPUS_MODEL") !=
                      std::string::npos);
    }

    // 还原：A/B 的托管键全部退场（并集生效），用户键保留
    DOCTEST_REQUIRE(settings_sync::restore(paths).ok);
    {
        const auto env = readEnv(paths.settingsFile);
        DOCTEST_CHECK(env.find("ANTHROPIC_DEFAULT_SONNET_MODEL") == env.end());
        DOCTEST_CHECK(env.find("ANTHROPIC_DEFAULT_OPUS_MODEL") == env.end());
        DOCTEST_CHECK_EQ(env.at("MY_KEY"), std::string("user"));
    }
    return true;
})

REGISTER_TEST(sync_takeover_missing_file, "settings_sync: 无 settings.json 时创建最小文件", {
    using namespace module::claude;
    test_support::TempDir d("missing", kSyncTempDirPrefix);
    SyncDb sdb("sync_missing");
    auto paths = makePaths(d, sdb.handle());

    Profile p;
    p.model = "m-1";
    DOCTEST_REQUIRE(settings_sync::takeOver(p, 8888, paths).ok);

    DOCTEST_CHECK(d.exists(paths.settingsFile));
    {
        const auto env = readEnv(paths.settingsFile);
        DOCTEST_CHECK_EQ(env.at("ANTHROPIC_BASE_URL"),
                         std::string("http://127.0.0.1:8888"));
        std::ifstream i(paths.settingsFile);
        std::ostringstream ss;
        ss << i.rdbuf();
        auto parsed = platform::json::parse(ss.str());
        DOCTEST_REQUIRE(parsed.isOk());
        auto keys = parsed.value().asObject().keys();
        DOCTEST_CHECK_EQ(keys.size(), 1u);
        DOCTEST_CHECK_EQ(keys[0], std::string("env"));
    }

    DOCTEST_REQUIRE(settings_sync::restore(paths).ok);
    const auto env = readEnv(paths.settingsFile);
    DOCTEST_CHECK(env.find("ANTHROPIC_BASE_URL") == env.end());
    DOCTEST_CHECK_FALSE(test_support::configRowExists(&sdb.handle(), module::claude::settings_sync::kSidecarConfigKey));
    return true;
})

// 解析失败一律中止不写：宁可让用户接不进来，也不能弄坏他的 settings.json
REGISTER_TEST(sync_parse_failure_aborts, "settings_sync: settings.json 解析失败中止不写", {
    using namespace module::claude;
    test_support::TempDir d("parsefail", kSyncTempDirPrefix);
    SyncDb sdb("sync_parsefail");
    auto paths = makePaths(d, sdb.handle());

    const std::string broken = "{ not valid json !!!";
    d.write(paths.settingsFile, broken);

    Profile p;
    auto r = settings_sync::takeOver(p, 8888, paths);
    DOCTEST_CHECK(!r.ok);
    DOCTEST_CHECK(!r.error.empty());
    DOCTEST_CHECK_EQ(d.read(paths.settingsFile), broken);
    DOCTEST_CHECK_FALSE(test_support::configRowExists(&sdb.handle(), module::claude::settings_sync::kSidecarConfigKey));

    // sidecar 损坏时 takeOver 同样中止（不把上次托管键误当用户原值）
    d.write(paths.settingsFile, "{}");
    test_support::writeRawConfigRow(&sdb.handle(), module::claude::settings_sync::kSidecarConfigKey, "{broken");
    DOCTEST_CHECK(!settings_sync::takeOver(p, 8888, paths).ok);

    // sidecar 损坏时 restore 报错并保留文件（供人工恢复）
    auto rr = settings_sync::restore(paths);
    DOCTEST_CHECK(!rr.ok);
    DOCTEST_CHECK(test_support::configRowExists(&sdb.handle(), module::claude::settings_sync::kSidecarConfigKey));
    return true;
})

// 首份快照污染守卫：已是本机代理形态的键不进快照——否则还原时会被当用户原值写回去
REGISTER_TEST(sync_takeover_snapshot_skips_proxy_form,
              "settings_sync: 首份快照跳过本机代理形态键（污染守卫）", {
    using namespace module::claude;
    test_support::TempDir d("pollute", kSyncTempDirPrefix);
    SyncDb sdb("sync_pollute");
    auto paths = makePaths(d, sdb.handle());

    // 上次残留 / 别的工具（cc-switch 等）写的代理形态值 + 一个用户自有键
    d.write(paths.settingsFile, R"({
        "model": "claude-sonnet-4-5",
        "env": {
            "ANTHROPIC_BASE_URL": "http://127.0.0.1:8888",
            "ANTHROPIC_AUTH_TOKEN": "PROXY_MANAGED",
            "MY_KEY": "user"
        }
    })");

    Profile p;
    p.model = "m-new";
    auto r = settings_sync::takeOver(p, 9999, paths);
    DOCTEST_REQUIRE(r.ok);
    // 命中污染守卫 → 经 warnings 上报（UI 可透传）
    DOCTEST_REQUIRE_EQ(r.warnings.size(), 1u);
    DOCTEST_CHECK(r.warnings[0].find("ANTHROPIC_BASE_URL") != std::string::npos);
    DOCTEST_CHECK(r.warnings[0].find("ANTHROPIC_AUTH_TOKEN") != std::string::npos);

    // 还原：代理形态值不被"恢复"（它们从来不是用户原值），用户键/非 env 字段保留
    DOCTEST_REQUIRE(settings_sync::restore(paths).ok);
    const auto env = readEnv(paths.settingsFile);
    DOCTEST_CHECK(env.find("ANTHROPIC_BASE_URL") == env.end());
    DOCTEST_CHECK(env.find("ANTHROPIC_AUTH_TOKEN") == env.end());
    DOCTEST_CHECK_EQ(env.at("MY_KEY"), std::string("user"));
    DOCTEST_CHECK_EQ(topLevelString(paths.settingsFile, "model"),
                     std::string("claude-sonnet-4-5"));
    return true;
})

// 无 sidecar 兜底：settings.json 仍是本机代理形态（上次接管残留）→ restore 做最小清除
REGISTER_TEST(sync_restore_without_sidecar_clears_residual,
              "settings_sync: 无快照但仍是代理形态 → 最小清除（restore）", {
    using namespace module::claude;
    test_support::TempDir d("residual", kSyncTempDirPrefix);
    SyncDb sdb("sync_residual");
    auto paths = makePaths(d, sdb.handle());
    d.write(paths.settingsFile, R"({
        "model": "claude-sonnet-4-5",
        "env": {
            "ANTHROPIC_BASE_URL": "http://127.0.0.1:8888",
            "ANTHROPIC_AUTH_TOKEN": "PROXY_MANAGED",
            "ANTHROPIC_API_KEY": "sk-user-real",
            "ANTHROPIC_MODEL": "m-keep",
            "MY_KEY": "user"
        }
    })");

    auto r = settings_sync::restore(paths);
    DOCTEST_REQUIRE(r.ok);
    const auto env = readEnv(paths.settingsFile);
    DOCTEST_CHECK(env.find("ANTHROPIC_BASE_URL") == env.end());    // 回环 → 删
    DOCTEST_CHECK(env.find("ANTHROPIC_AUTH_TOKEN") == env.end());  // 占位符 → 删
    // 非占位符凭据 / 非代理形态键 / 顶层字段一律不动
    DOCTEST_CHECK_EQ(env.at("ANTHROPIC_API_KEY"), std::string("sk-user-real"));
    DOCTEST_CHECK_EQ(env.at("ANTHROPIC_MODEL"), std::string("m-keep"));
    DOCTEST_CHECK_EQ(env.at("MY_KEY"), std::string("user"));
    DOCTEST_CHECK_EQ(topLevelString(paths.settingsFile, "model"),
                     std::string("claude-sonnet-4-5"));
    DOCTEST_CHECK_FALSE(test_support::configRowExists(&sdb.handle(), module::claude::settings_sync::kSidecarConfigKey));  // 不新增 sidecar
    return true;
})

// 无 sidecar 且非代理形态 → 真 no-op，文件一字不改
REGISTER_TEST(sync_restore_without_sidecar_noop,
              "settings_sync: 无快照且非代理形态 → 真 no-op（restore）", {
    using namespace module::claude;
    test_support::TempDir d("noop", kSyncTempDirPrefix);
    SyncDb sdb("sync_noop");
    auto paths = makePaths(d, sdb.handle());
    const std::string original = R"({
        "permissions": {"allow": ["Bash"]},
        "env": {
            "ANTHROPIC_BASE_URL": "https://api.example.com",
            "ANTHROPIC_AUTH_TOKEN": "sk-user-own",
            "MY_KEY": "user"
        }
    })";
    d.write(paths.settingsFile, original);
    auto r = settings_sync::restore(paths);
    DOCTEST_REQUIRE(r.ok);
    DOCTEST_CHECK_EQ(d.read(paths.settingsFile), original);  // 逐字节不变
    DOCTEST_CHECK_FALSE(test_support::configRowExists(&sdb.handle(), module::claude::settings_sync::kSidecarConfigKey));

    // settings.json 不存在 / 无 env 块 → 同样 no-op（不创建文件、不改内容）
    test_support::TempDir d2("noop2", kSyncTempDirPrefix);
    SyncDb sdb2("sync_noop2");
    auto paths2 = makePaths(d2, sdb2.handle());
    DOCTEST_CHECK(settings_sync::restore(paths2).ok);
    DOCTEST_CHECK(!d2.exists(paths2.settingsFile));
    d2.write(paths2.settingsFile, R"({"permissions": {}})");
    const std::string noEnv = d2.read(paths2.settingsFile);
    DOCTEST_CHECK(settings_sync::restore(paths2).ok);
    DOCTEST_CHECK_EQ(d2.read(paths2.settingsFile), noEnv);
    return true;
})

REGISTER_TEST(sync_overlay_detection, "settings_sync: managed/project 层覆盖检测", {
    using namespace module::claude;
    test_support::TempDir d("overlay", kSyncTempDirPrefix);
    SyncDb sdb("sync_overlay");
    auto paths = makePaths(d, sdb.handle());

    // 伪造 project 级 settings.local.json 含同名 env 键
    const auto projectLocal = d.file("settings.local.json");
    d.write(projectLocal,
            R"({"env": {"ANTHROPIC_BASE_URL": "https://project.example.com"}})");
    paths.overlayFiles = {projectLocal};

    Profile p;
    auto r = settings_sync::takeOver(p, 8888, paths);
    DOCTEST_REQUIRE(r.ok);
    DOCTEST_REQUIRE_EQ(r.warnings.size(), 1u);
    DOCTEST_CHECK(r.warnings[0].find("ANTHROPIC_BASE_URL") != std::string::npos);
    DOCTEST_CHECK(r.warnings[0].find("settings.local.json") != std::string::npos);

    // 只含无关 env 键的 overlay → 不警告
    d.write(projectLocal, R"({"env": {"MY_KEY": "x"}})");
    auto r2 = settings_sync::takeOver(p, 8888, paths);
    DOCTEST_REQUIRE(r2.ok);
    DOCTEST_CHECK(r2.warnings.empty());

    // overlay 文件不存在 / 无 env 块 → 不警告
    paths.overlayFiles = {d.file("nope.json")};
    d.write(projectLocal, R"({"permissions": {}})");
    paths.overlayFiles.push_back(projectLocal);
    auto r3 = settings_sync::takeOver(p, 8888, paths);
    DOCTEST_REQUIRE(r3.ok);
    DOCTEST_CHECK(r3.warnings.empty());
    return true;
})

REGISTER_TEST(sync_overlay_detection_managed_derived,
              "settings_sync: 覆盖检测含托管键全集与 profile 实键", {
    using namespace module::claude;
    test_support::TempDir d("overlayderived", kSyncTempDirPrefix);
    SyncDb sdb("sync_overlayderived");
    auto paths = makePaths(d, sdb.handle());

    // profile：sonnet 槽位有值 + extra_env 一个自有键
    Profile p;
    ModelsConfig models;
    models.sonnet.model = "m-sonnet";
    auto mc = serializeModelsConfig(models);
    DOCTEST_REQUIRE(mc.isOk());
    p.modelsConfigJson = mc.value();
    p.extraEnvJson = R"({"MY_KEY": "1"})";

    // ① 两个 overlay 层各写一个托管键 → 各一条警告：
    //    *_MODEL_NAME 属槽位托管键，MY_KEY 属本轮 profile 实键
    const auto overlayName = d.file("settings.local.json");
    const auto overlayExtra = d.file("settings.project.json");
    d.write(overlayName,
            R"({"env": {"ANTHROPIC_DEFAULT_SONNET_MODEL_NAME": "x"}})");
    d.write(overlayExtra, R"({"env": {"MY_KEY": "1"}})");
    paths.overlayFiles = {overlayName, overlayExtra};
    auto r = settings_sync::takeOver(p, 8888, paths);
    DOCTEST_REQUIRE(r.ok);
    DOCTEST_REQUIRE_EQ(r.warnings.size(), 2u);
    DOCTEST_CHECK(r.warnings[0].find("ANTHROPIC_DEFAULT_SONNET_MODEL_NAME") !=
                  std::string::npos);
    DOCTEST_CHECK(r.warnings[1].find("MY_KEY") != std::string::npos);

    // ② 空槽位的 *_NAME 键仍在检测集内（haiku 槽位为空 → buildProxyEnv 不生成
    //    该键，只有托管键全集能识别上位覆盖）
    d.write(overlayName,
            R"({"env": {"ANTHROPIC_DEFAULT_HAIKU_MODEL_NAME": "x"}})");
    paths.overlayFiles = {overlayName};
    auto r2 = settings_sync::takeOver(p, 8888, paths);
    DOCTEST_REQUIRE(r2.ok);
    DOCTEST_REQUIRE_EQ(r2.warnings.size(), 1u);
    DOCTEST_CHECK(r2.warnings[0].find("ANTHROPIC_DEFAULT_HAIKU_MODEL_NAME") !=
                  std::string::npos);

    d.write(overlayName, R"({"env": {"SOME_UNMANAGED_KEY": "x"}})");
    paths.overlayFiles = {overlayName};
    auto r3 = settings_sync::takeOver(p, 8888, paths);
    DOCTEST_REQUIRE(r3.ok);
    DOCTEST_CHECK(r3.warnings.empty());
    return true;
})

REGISTER_TEST(sync_crash_recovery_and_atomic, "settings_sync: 崩溃补还原与原子写残留", {
    using namespace module::claude;
    test_support::TempDir d("crash", kSyncTempDirPrefix);
    SyncDb sdb("sync_crash");
    auto paths = makePaths(d, sdb.handle());

    const std::string original = R"({"env": {"MY_KEY": "user"}})";
    d.write(paths.settingsFile, original);

    // 模拟崩溃：takeOver 后没走 restore（sidecar 残留）
    Profile p;
    p.model = "m-1";
    DOCTEST_REQUIRE(settings_sync::takeOver(p, 8888, paths).ok);

    auto r = settings_sync::recoverPendingSnapshot(paths);
    DOCTEST_REQUIRE(r.ok);
    const auto env = readEnv(paths.settingsFile);
    DOCTEST_CHECK_EQ(env.at("MY_KEY"), std::string("user"));
    DOCTEST_CHECK(env.find("ANTHROPIC_BASE_URL") == env.end());
    DOCTEST_CHECK_FALSE(test_support::configRowExists(&sdb.handle(), module::claude::settings_sync::kSidecarConfigKey));

    DOCTEST_CHECK(settings_sync::recoverPendingSnapshot(paths).ok);

    DOCTEST_REQUIRE(settings_sync::takeOver(p, 8888, paths).ok);
    DOCTEST_REQUIRE(settings_sync::restore(paths).ok);
    for (const auto& entry : std::filesystem::directory_iterator(d.path)) {
        DOCTEST_CHECK(entry.path().string().find(".devdesk-tmp") ==
                      std::string::npos);
    }
    return true;
})

REGISTER_TEST(sync_service_wiring, "claude_service: 接管通道三触发点接线", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "wiring");
    test_support::TempDir d("wiring", kSyncTempDirPrefix);
    SyncDb sdb("sync_wiring");
    auto paths = makePaths(d, sdb.handle());

    d.write(paths.settingsFile, R"({"env": {"MY_KEY": "user"}})");

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    // 未注入路径（默认关闭）→ startProxy 不写任何文件
    {
        auto r = svc.startProxy("127.0.0.1", 0);
        DOCTEST_REQUIRE(r.isOk());
        const auto envBefore = readEnv(paths.settingsFile);
        DOCTEST_CHECK_EQ(envBefore.at("MY_KEY"), std::string("user"));
        DOCTEST_CHECK(envBefore.find("ANTHROPIC_BASE_URL") == envBefore.end());
        svc.stopProxy();
    }

    svc.setSettingsSyncPaths(paths);
    svc.setSettingsTakeOverEnabled(true);
    auto created = svc.createProfile("官方中转", "https://gw.example.com",
                                     "sk-ind-test-0123456789abcdef", "");
    DOCTEST_REQUIRE(created.isOk());
    Profile p = created.value();
    p.modelsConfigJson = R"({"default": {"model": "m-default"},
                             "sonnet": {"model": "m-sonnet"}})";
    p.extraEnvJson = R"({"DISABLE_AUTOUPDATER": "1"})";
    DOCTEST_REQUIRE(svc.updateProfile(p).isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(p.id).isOk());

    // 触发点 1：startProxy 成功 → 接管（端口 0 = OS 分配，env 用实际端口）
    {
        auto r = svc.startProxy("127.0.0.1", 0);
        DOCTEST_REQUIRE(r.isOk());
        const int port = static_cast<int>(r.value());
        const auto env = readEnv(paths.settingsFile);
        DOCTEST_CHECK_EQ(env.at("ANTHROPIC_BASE_URL"),
                         std::string("http://127.0.0.1:") + std::to_string(port));
        DOCTEST_CHECK_EQ(env.at("ANTHROPIC_AUTH_TOKEN"),
                         std::string("PROXY_MANAGED"));
        DOCTEST_CHECK_EQ(env.at("ANTHROPIC_MODEL"), std::string("m-default"));
        DOCTEST_CHECK_EQ(env.at("ANTHROPIC_DEFAULT_SONNET_MODEL"),
                         std::string("m-sonnet"));
        DOCTEST_CHECK_EQ(env.at("DISABLE_AUTOUPDATER"), std::string("1"));
        DOCTEST_CHECK_EQ(env.at("MY_KEY"), std::string("user"));
        DOCTEST_CHECK(svc.isClaudeCodeManaged());
    }

    // 触发点 2：运行中切 profile → env 跟随
    {
        auto created2 = svc.createProfile("备用", "https://gw2.example.com",
                                          "sk-ind-test-fedcba9876543210", "");
        DOCTEST_REQUIRE(created2.isOk());
        Profile p2 = created2.value();
        p2.modelsConfigJson = R"({"default": {"model": "m-second"}})";
        DOCTEST_REQUIRE(svc.updateProfile(p2).isOk());
        DOCTEST_REQUIRE(svc.setActiveProfile(p2.id).isOk());
        const auto env = readEnv(paths.settingsFile);
        DOCTEST_CHECK_EQ(env.at("ANTHROPIC_MODEL"), std::string("m-second"));
        // 首份快照仍有效（用户 MY_KEY 在还原语义里保留）
        DOCTEST_CHECK_EQ(env.at("MY_KEY"), std::string("user"));
    }

    // 触发点 3：stopProxy → 还原
    svc.stopProxy();
    {
        const auto env = readEnv(paths.settingsFile);
        DOCTEST_CHECK_EQ(env.at("MY_KEY"), std::string("user"));
        DOCTEST_CHECK(env.find("ANTHROPIC_BASE_URL") == env.end());
        DOCTEST_CHECK(env.find("ANTHROPIC_MODEL") == env.end());
        DOCTEST_CHECK(!svc.isClaudeCodeManaged());
    }

    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

REGISTER_TEST(sync_service_warnings_surface, "claude_service: 接管警告经 lastSyncWarnings 暴露", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "warn");
    test_support::TempDir d("warn", kSyncTempDirPrefix);
    SyncDb sdb("sync_warn");
    auto paths = makePaths(d, sdb.handle());
    const auto overlay = d.file("managed-settings.json");
    d.write(overlay, R"({"env": {"ANTHROPIC_BASE_URL": "https://corp.example.com"}})");
    paths.overlayFiles = {overlay};

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    svc.setSettingsSyncPaths(paths);
    svc.setSettingsTakeOverEnabled(true);
    auto r = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(!svc.lastSyncWarnings().empty());
    DOCTEST_CHECK(svc.lastSyncWarnings()[0].find("ANTHROPIC_BASE_URL") !=
                  std::string::npos);
    svc.stopProxy();
    DOCTEST_CHECK(svc.lastSyncWarnings().empty());  // 还原后清空
    return true;
})

// 接管失败并入 lastSyncWarnings，且失败不阻断起代理 / 切 profile
REGISTER_TEST(sync_service_takeover_failure_reported,
              "claude_service: 接管失败并入 lastSyncWarnings 且不阻断操作", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "tofail");
    test_support::TempDir d("tofail", kSyncTempDirPrefix);
    SyncDb sdb("sync_tofail");
    auto paths = makePaths(d, sdb.handle());

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    svc.setSettingsSyncPaths(paths);
    svc.setSettingsTakeOverEnabled(true);
    auto a = svc.createProfile("接管失败A", "https://gw.example.com",
                               "sk-ind-test-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());

    // 起代理：settings.json 是坏 JSON → 接管失败，但代理照常可用
    d.write(paths.settingsFile, "{ not valid json !!!");
    auto started = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(started.isOk());
    DOCTEST_CHECK(svc.isProxyRunning());
    DOCTEST_REQUIRE(!svc.lastSyncWarnings().empty());
    DOCTEST_CHECK(svc.lastSyncWarnings()[0].find("接管失败") != std::string::npos);

    // 运行中切 profile：settings.json 恢复可读、sidecar 损坏 → 接管仍失败，
    // 但切换必须成功（DB 已切），失败原因经 lastSyncWarnings 可见
    d.write(paths.settingsFile, R"({"env": {"MY_KEY": "user"}})");
    test_support::writeRawConfigRow(&sdb.handle(), module::claude::settings_sync::kSidecarConfigKey, "{broken");
    auto b = svc.createProfile("接管失败B", "https://gw2.example.com",
                               "sk-ind-test-fedcba9876543210", "");
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(b.value().id).isOk());
    {
        auto g = svc.getProfile(b.value().id);
        DOCTEST_REQUIRE(g.isOk());
        DOCTEST_CHECK(g.value().isActive);  // 切换成功（不因接管失败回滚）
        DOCTEST_REQUIRE(!svc.lastSyncWarnings().empty());
        DOCTEST_CHECK(svc.lastSyncWarnings()[0].find("接管失败") !=
                      std::string::npos);
    }

    svc.stopProxy();  // sidecar 损坏 → 还原报错并保留文件，不崩
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(a.value().id));
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(b.value().id));
    return true;
})

// 应用退出序列 = Router::stopAll → onStop → 还原。生产上 main.cpp 的 atexit handler 走的正是这条链。
extern "C" ::core::Module* create_claude_module();

REGISTER_TEST(sync_exit_sequence_stop_all,
              "claude_module: 退出序列 Router::stopAll 触发还原", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "exit");
    test_support::TempDir d("exit", kSyncTempDirPrefix);
    SyncDb sdb("sync_exit");
    auto paths = makePaths(d, sdb.handle());
    d.write(paths.settingsFile, R"({"env": {"MY_KEY": "user"}})");

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    ClaudeService::setGlobal(&svc);
    struct GlobalGuard {
        ~GlobalGuard() { ClaudeService::setGlobal(nullptr); }
    } guard;

    svc.setSettingsSyncPaths(paths);
    svc.setSettingsTakeOverEnabled(true);
    auto r = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(r.isOk());
    {
        const auto env = readEnv(paths.settingsFile);
        DOCTEST_CHECK(env.find("ANTHROPIC_BASE_URL") != env.end());
        DOCTEST_CHECK(test_support::configRowExists(&sdb.handle(), module::claude::settings_sync::kSidecarConfigKey));
        DOCTEST_CHECK(svc.isClaudeCodeManaged());
    }

    // 生产序列 = 装配段 registerModule → startAll onStart（模块恒先启动后停止）；
    // onStart 重入幂等守卫后，未启动模块的 onStop = no-op，故这里按真实顺序补
    // onStart。被锁的契约是「启动过的模块退出时触发还原」，不变。
    std::unique_ptr<::core::Module> mod(create_claude_module());
    ::core::Router router;
    router.registerModule(mod.get());
    mod->onStart();
    router.stopAll();

    const auto env = readEnv(paths.settingsFile);
    DOCTEST_CHECK_EQ(env.at("MY_KEY"), std::string("user"));
    DOCTEST_CHECK(env.find("ANTHROPIC_BASE_URL") == env.end());
    DOCTEST_CHECK(env.find("ANTHROPIC_AUTH_TOKEN") == env.end());
    DOCTEST_CHECK_FALSE(test_support::configRowExists(&sdb.handle(), module::claude::settings_sync::kSidecarConfigKey));
    DOCTEST_CHECK(!svc.isClaudeCodeManaged());
    DOCTEST_CHECK(!svc.isProxyRunning());
    return true;
})

// 删除激活 profile 且代理运行中 → 停代理（触发还原），settings.json 不能继续
// 指向无 profile 的代理。
REGISTER_TEST(sync_delete_active_profile_stops_proxy,
              "claude_service: 删激活 Profile 停代理并还原 settings", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "delact");
    test_support::TempDir d("delact", kSyncTempDirPrefix);
    SyncDb sdb("sync_delact");
    auto paths = makePaths(d, sdb.handle());
    d.write(paths.settingsFile, R"({"env": {"MY_KEY": "user"}})");

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    svc.setSettingsSyncPaths(paths);
    svc.setSettingsTakeOverEnabled(true);
    auto created = svc.createProfile("激活删除目标", "https://gw.example.com",
                                     "sk-ind-test-0123456789abcdef", "");
    DOCTEST_REQUIRE(created.isOk());
    auto created2 = svc.createProfile("旁观者", "https://gw2.example.com",
                                      "sk-ind-test-fedcba9876543210", "");
    DOCTEST_REQUIRE(created2.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());
    auto r = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(svc.isProxyRunning());
    DOCTEST_CHECK(svc.isClaudeCodeManaged());

    // 1) 删非激活 Profile：代理保持、不触发还原（出参被清零）
    {
        bool stopped = true;  // 故意初值 true，验证函数显式置 false
        DOCTEST_REQUIRE(svc.deleteProfile(created2.value().id, &stopped).isOk());
        DOCTEST_CHECK(!stopped);
        DOCTEST_CHECK(svc.isProxyRunning());
        DOCTEST_CHECK(svc.isClaudeCodeManaged());
    }

    // 2) 删激活 Profile：代理停止 + settings.json 还原（sidecar 消费）
    {
        bool stopped = false;
        DOCTEST_REQUIRE(svc.deleteProfile(created.value().id, &stopped).isOk());
        DOCTEST_CHECK(stopped);
        DOCTEST_CHECK(!svc.isProxyRunning());
        DOCTEST_CHECK(!svc.isClaudeCodeManaged());
        const auto env = readEnv(paths.settingsFile);
        DOCTEST_CHECK_EQ(env.at("MY_KEY"), std::string("user"));
        DOCTEST_CHECK(env.find("ANTHROPIC_BASE_URL") == env.end());
    }

    // 3) 代理未运行时删激活 Profile：不报错、不动文件、stopped=false
    {
        auto c3 = svc.createProfile("停机激活", "https://gw3.example.com",
                                    "sk-ind-test-abc0123456789def", "");
        DOCTEST_REQUIRE(c3.isOk());
        DOCTEST_REQUIRE(svc.setActiveProfile(c3.value().id).isOk());
        bool stopped = true;
        DOCTEST_REQUIRE(svc.deleteProfile(c3.value().id, &stopped).isOk());
        DOCTEST_CHECK(!stopped);
        DOCTEST_CHECK(!svc.isProxyRunning());
        const auto env = readEnv(paths.settingsFile);
        DOCTEST_CHECK_EQ(env.at("MY_KEY"), std::string("user"));
    }

    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

// 接管默认开（见 claude_service.h setSettingsTakeOverEnabled）；关掉开关 =
// DevDesk 不碰该文件，三条路径一起门控：startProxy 接管 / stopProxy 还原 /
// 启动补还原。
REGISTER_TEST(sync_switch_off_no_write,
              "claude_service: 接管开关关闭时不写 settings.json", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "off");
    test_support::TempDir d("off", kSyncTempDirPrefix);
    SyncDb sdb("sync_off");
    auto paths = makePaths(d, sdb.handle());
    const std::string original = R"({"env": {"MY_KEY": "user"}})";
    d.write(paths.settingsFile, original);

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    DOCTEST_CHECK(svc.settingsTakeOverEnabled());  // 默认开
    svc.setSettingsSyncPaths(paths);
    svc.setSettingsTakeOverEnabled(false);         // 显式关闭

    auto created = svc.createProfile("关闭态", "https://gw.example.com",
                                     "sk-ind-test-0123456789abcdef", "");
    DOCTEST_REQUIRE(created.isOk());
    Profile p = created.value();
    p.modelsConfigJson = R"({"default": {"model": "m-default"}})";
    DOCTEST_REQUIRE(svc.updateProfile(p).isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(p.id).isOk());

    // 路径 1：启动补还原（main.cpp 启动序列）→ 文件逐字节不变
    svc.recoverPendingSettingsRestore();
    DOCTEST_CHECK_EQ(d.read(paths.settingsFile), original);

    // 路径 2：startProxy → 不接管（无 sidecar、无托管键）
    auto r = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(d.read(paths.settingsFile), original);
    DOCTEST_CHECK(!svc.isClaudeCodeManaged());
    DOCTEST_CHECK_FALSE(test_support::configRowExists(&sdb.handle(), module::claude::settings_sync::kSidecarConfigKey));

    // 路径 3：stopProxy → 不还原（未接管过，同样不动文件）
    svc.stopProxy();
    DOCTEST_CHECK_EQ(d.read(paths.settingsFile), original);
    DOCTEST_CHECK_FALSE(test_support::configRowExists(&sdb.handle(), module::claude::settings_sync::kSidecarConfigKey));

    // 遗留 sidecar（上次没走完还原）+ 开关关闭 → 补还原同样跳过，快照留给
    // 下次开启接管时处理（不代改用户文件）
    test_support::writeRawConfigRow(
        &sdb.handle(), module::claude::settings_sync::kSidecarConfigKey,
            R"({"managedKeys": {"ANTHROPIC_BASE_URL": ""},)"
            R"( "envSnapshot": {"MY_KEY": "user"}})");
    svc.recoverPendingSettingsRestore();
    DOCTEST_CHECK_EQ(d.read(paths.settingsFile), original);
    DOCTEST_CHECK(test_support::configRowExists(&sdb.handle(), module::claude::settings_sync::kSidecarConfigKey));

    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

// 六种文件形态分别处置：缺失 / 0 字节空文件 / 无 env 键 → no-op 成功
//（present=false，调用方照常切换）；坏 JSON / 顶层非对象 / env 非对象 →
// 中止（ok=false + 带路径的 error）。

namespace {

bool hasKeyEntry(const std::vector<std::string>& keys, const char* key) {
    for (const auto& k : keys) {
        if (k == key) return true;
    }
    return false;
}

} // namespace

REGISTER_TEST(sync_reverse_read_missing_empty_noenv,
              "settings_sync: 反向读——缺失/空文件/无 env 均 no-op 成功（reverse）", {
    using module::claude::settings_sync::readSettingsEnvForSync;
    test_support::TempDir d("revnoop", kSyncTempDirPrefix);
    SyncDb sdb("sync_revnoop");
    auto paths = makePaths(d, sdb.handle());

    // 文件不存在 → no-op 成功（调用方照常完成本次切换）
    {
        auto r = readSettingsEnvForSync(paths);
        DOCTEST_CHECK(r.ok);
        DOCTEST_CHECK(!r.present);
        DOCTEST_CHECK(r.error.empty());
        DOCTEST_CHECK(r.envJson.empty());
        DOCTEST_CHECK(r.managedKeys.empty());
    }
    // 0 字节空文件（视为不存在——不属于「存在但读不了」的中止侧）
    {
        d.write(paths.settingsFile, "");
        auto r = readSettingsEnvForSync(paths);
        DOCTEST_CHECK(r.ok);
        DOCTEST_CHECK(!r.present);
        DOCTEST_CHECK(r.error.empty());
    }
    {
        d.write(paths.settingsFile, R"({"permissions": {"allow": ["Bash"]}})");
        auto r = readSettingsEnvForSync(paths);
        DOCTEST_CHECK(r.ok);
        DOCTEST_CHECK(!r.present);
        DOCTEST_CHECK(r.error.empty());
    }
    return true;
})

REGISTER_TEST(sync_reverse_read_aborts_on_broken,
              "settings_sync: 反向读——坏 JSON/顶层非对象/env 非对象中止（reverse）", {
    using module::claude::settings_sync::readSettingsEnvForSync;
    test_support::TempDir d("revbad", kSyncTempDirPrefix);
    SyncDb sdb("sync_revbad");
    auto paths = makePaths(d, sdb.handle());

    // 非法 JSON → 中止（调用方据此不切换，profile 一字未改）
    {
        d.write(paths.settingsFile, "{ not valid json !!!");
        auto r = readSettingsEnvForSync(paths);
        DOCTEST_CHECK(!r.ok);
        DOCTEST_CHECK(!r.present);
        DOCTEST_CHECK(r.error.find("settings.json") != std::string::npos);
    }
    {
        d.write(paths.settingsFile, "[1, 2, 3]");
        auto r = readSettingsEnvForSync(paths);
        DOCTEST_CHECK(!r.ok);
        DOCTEST_CHECK(!r.present);
        DOCTEST_CHECK(!r.error.empty());
    }
    {
        d.write(paths.settingsFile, R"({"env": "not-an-object"})");
        auto r = readSettingsEnvForSync(paths);
        DOCTEST_CHECK(!r.ok);
        DOCTEST_CHECK(!r.present);
        DOCTEST_CHECK(r.error.find("env") != std::string::npos);
    }
    return true;
})

// 存在但打不开（IO/权限）≠ 0 字节空文件 → 中止（仅 Windows：独占句柄是本平台
// 构造「存在但读不出」的唯一可靠形态）
#if defined(_WIN32)
REGISTER_TEST(sync_reverse_read_unreadable_aborts,
              "settings_sync: 反向读——存在但打不开中止（reverse 读失败）", {
    using module::claude::settings_sync::readSettingsEnvForSync;
    test_support::TempDir d("revunreadable", kSyncTempDirPrefix);
    SyncDb sdb("sync_revunreadable");
    auto paths = makePaths(d, sdb.handle());
    d.write(paths.settingsFile, R"({"env": {"MY_KEY": "user"}})");

    // 独占句柄（share mode = 0）：文件存在、GetFileAttributes 可见，ifstream 打不开
    HANDLE h = CreateFileW(paths.settingsFile.wstring().c_str(), GENERIC_READ,
                           0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    DOCTEST_REQUIRE(h != INVALID_HANDLE_VALUE);
    {
        auto r = readSettingsEnvForSync(paths);
        DOCTEST_CHECK(!r.ok);  // 读失败中止，不静默当空文件
        DOCTEST_CHECK(!r.present);
        DOCTEST_CHECK(r.error.find(paths.settingsFile.string()) !=
                      std::string::npos);  // 错误含路径
    }
    CloseHandle(h);

    // 对照：0 字节空文件仍 no-op 成功（属「不存在」侧的处置）
    d.write(paths.settingsFile, "");
    auto r2 = readSettingsEnvForSync(paths);
    DOCTEST_CHECK(r2.ok);
    DOCTEST_CHECK(!r2.present);
    return true;
})
#endif  // _WIN32

REGISTER_TEST(sync_reverse_read_env_and_sidecar,
              "settings_sync: 反向读——env 原文与 sidecar 托管键（reverse）", {
    using module::claude::settings_sync::readSettingsEnvForSync;
    test_support::TempDir d("revok", kSyncTempDirPrefix);
    SyncDb sdb("sync_revok");
    auto paths = makePaths(d, sdb.handle());
    d.write(paths.settingsFile, R"json({
        "permissions": {"allow": ["Bash"]},
        "env": {
            "ANTHROPIC_BASE_URL": "https://gw.example.com",
            "MY_KEY": "user"
        }
    })json");

    // 正常：present + env 对象原文（只有 env 子对象，不含非 env 顶层字段）
    {
        auto r = readSettingsEnvForSync(paths);
        DOCTEST_REQUIRE(r.ok);
        DOCTEST_CHECK(r.present);
        DOCTEST_CHECK(r.managedKeys.empty());  // sidecar 缺失 → 空集
        auto parsed = platform::json::parse(r.envJson);
        DOCTEST_REQUIRE(parsed.isOk());
        DOCTEST_REQUIRE(parsed.value().isObject());
        auto env = parsed.value().asObject();
        DOCTEST_CHECK_EQ(env.get("ANTHROPIC_BASE_URL").asString(),
                         std::string("https://gw.example.com"));
        DOCTEST_CHECK_EQ(env.get("MY_KEY").asString(), std::string("user"));
        DOCTEST_CHECK(!env.has("permissions"));
    }
    // sidecar 存在 → 取 managedKeys 键集（对象 shape，非数组）
    {
        test_support::writeRawConfigRow(
            &sdb.handle(), module::claude::settings_sync::kSidecarConfigKey,
                R"({"managedKeys": {"ANTHROPIC_BASE_URL": "", "ANTHROPIC_MODEL": ""},)"
                R"( "envSnapshot": {"MY_KEY": "user"}})");
        auto r = readSettingsEnvForSync(paths);
        DOCTEST_REQUIRE(r.ok);
        DOCTEST_CHECK(r.present);
        DOCTEST_REQUIRE_EQ(r.managedKeys.size(), 2u);
        DOCTEST_CHECK(hasKeyEntry(r.managedKeys, "ANTHROPIC_BASE_URL"));
        DOCTEST_CHECK(hasKeyEntry(r.managedKeys, "ANTHROPIC_MODEL"));
    }
    // sidecar 损坏 → 空集 + 不失败、不阻断（反向读是只读路径，坏 sidecar 不该
    // 把用户挡在切换之外）
    {
        test_support::writeRawConfigRow(&sdb.handle(), module::claude::settings_sync::kSidecarConfigKey, "{broken");
        auto r = readSettingsEnvForSync(paths);
        DOCTEST_REQUIRE(r.ok);
        DOCTEST_CHECK(r.present);
        DOCTEST_CHECK(r.managedKeys.empty());
        DOCTEST_CHECK(!r.envJson.empty());
    }
    return true;
})

REGISTER_TEST(sync_restore_v1_sidecar_no_nonenv,
              "settings_sync: 旧形态 sidecar（无 nonEnv 字段）restore 不动非 env 键", {
    using namespace module::claude;
    test_support::TempDir d("xset5", kSyncTempDirPrefix);
    SyncDb sdb("sync_xset5");
    auto paths = makePaths(d, sdb.handle());
    d.write(paths.settingsFile,
            R"({"env": {"MY_KEY": "user", "ANTHROPIC_BASE_URL": "http://127.0.0.1:8888", "ANTHROPIC_AUTH_TOKEN": "PROXY_MANAGED"}, "statusLine": "user-s"})");
    // 手工造只有 managedKeys + envSnapshot 的 sidecar（无 nonEnv 字段）
    test_support::writeRawConfigRow(
        &sdb.handle(), module::claude::settings_sync::kSidecarConfigKey,
            R"({"managedKeys": {"ANTHROPIC_BASE_URL": "", "ANTHROPIC_AUTH_TOKEN": ""}, "envSnapshot": {"MY_KEY": "user"}})");

    DOCTEST_REQUIRE(settings_sync::restore(paths).ok);
    // env 域照旧还原；非 env 域全透传（空集 = 不透传判定不命中）
    const auto env = readEnv(paths.settingsFile);
    DOCTEST_CHECK_EQ(env.at("MY_KEY"), std::string("user"));
    DOCTEST_CHECK(env.find("ANTHROPIC_BASE_URL") == env.end());
    DOCTEST_CHECK_EQ(topLevelString(paths.settingsFile, "statusLine"),
                     std::string("user-s"));
    return true;
})

REGISTER_TEST(claude_service_last_sync_warnings_snapshot,
              "claude_service: lastSyncWarnings 返回快照（非活引用）+ 并发读写不崩", {
    static_assert(
        std::is_same_v<
            decltype(std::declval<const module::claude::ClaudeService&>()
                         .lastSyncWarnings()),
            std::vector<std::string>>,
        "lastSyncWarnings 必须按值返回快照：活引用 = UI 每帧读被 agent 线程"
        "整体重写的 vector（F2：数据竞争 + UAF）");
    static_assert(
        std::is_same_v<
            decltype(std::declval<const module::claude::SettingsTakeOver&>()
                         .warningsSnapshot()),
            std::vector<std::string>>,
        "warningsSnapshot 必须按值返回（协议侧锁内拷贝，写者也在同一把锁内）");

    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "snapshot");
    test_support::TempDir d("snapshot", kSyncTempDirPrefix);
    SyncDb sdb("sync_snapshot");
    auto paths = makePaths(d, sdb.handle());

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    svc.setSettingsSyncPaths(paths);
    svc.setSettingsTakeOverEnabled(true);
    auto a = svc.createProfile("快照A", "https://gw.example.com",
                               "sk-ind-test-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    auto b = svc.createProfile("快照B", "https://gw2.example.com",
                               "sk-ind-test-fedcba9876543210", "");
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());

    // 夹具：settings.json 可读（反向同步不中止）+ sidecar
    // 损坏（每次 syncTakeOver 都失败）→ 每次切 profile 都把 warnings_ **整体
    // 重写**一遍 = 修复前读者拿活引用时的重分配窗口
    d.write(paths.settingsFile, R"({"env": {"MY_KEY": "user"}})");
    test_support::writeRawConfigRow(&sdb.handle(), module::claude::settings_sync::kSidecarConfigKey, "{broken");
    auto started = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(started.isOk());
    DOCTEST_REQUIRE(!svc.lastSyncWarnings().empty());   // 预置：读者开局即有内容

    constexpr int kWriterIterations = 30;
    std::barrier<> startLine(2);
    std::atomic<bool> writerDone{false};
    std::atomic<int> badEntries{0};
    std::atomic<int> nonEmptySnapshots{0};
    std::atomic<int> readerIterations{0};

    auto writer = [&] {
        startLine.arrive_and_wait();
        for (int i = 0; i < kWriterIterations; ++i) {
            // 成败都行：失败（撞闸/接管失败）也照样重写 warnings_，正是要打的窗口
            (void)svc.setActiveProfile(i % 2 == 0 ? a.value().id : b.value().id);
        }
        writerDone.store(true);
    };
    auto reader = [&] {
        startLine.arrive_and_wait();
        do {   // do-while：至少读一次，读者这一侧不依赖调度时序
            auto snap = svc.lastSyncWarnings();     // 值快照（锁内拷贝）
            readerIterations.fetch_add(1);
            if (!snap.empty()) nonEmptySnapshots.fetch_add(1);
            for (const auto& w : snap) {
                // 本夹具下 warnings_ 只可能是「接管失败」一条：快照若撕裂
                // （读到半更新的 vector / 释放中的缓冲）就会在这里露出来
                if (w.find("接管失败") == std::string::npos)
                    badEntries.fetch_add(1);
            }
        } while (!writerDone.load());
    };
    std::thread tw(writer);
    std::thread tr(reader);
    tw.join();
    tr.join();

    DOCTEST_CHECK_EQ(badEntries.load(), 0);
    DOCTEST_CHECK(nonEmptySnapshots.load() > 0);   // 读者真读到过写者的内容
    DOCTEST_CHECK(readerIterations.load() > 0);

    // 快照语义：读到的副本与下一次读互不影响（服务端状态不被读者改）
    {
        auto snap = svc.lastSyncWarnings();
        snap.clear();
        DOCTEST_CHECK(!svc.lastSyncWarnings().empty());
    }

    svc.stopProxy();
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(a.value().id));
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(b.value().id));
    return true;
})
