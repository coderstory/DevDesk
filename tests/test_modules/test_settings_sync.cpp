// tests/test_modules/test_settings_sync.cpp
// Claude Code settings.json 接管通道测试（provider-config Phase C，设计 §5.3）
//
// 全部通过 Paths 注入临时目录隔离，绝不碰真实 ~/.claude。
// 覆盖：
//   - buildProxyEnv：BASE_URL/token/槽位跳过/显示名派生/extra_env 别名与优先级
//   - takeOver 合并：非 env 字段保留、用户 env 键保留、托管键写入、sidecar 落地
//   - restore 往返：原值恢复、原本不存在的键删除、sidecar 删除
//   - 重复 takeOver：首份快照不覆盖、托管键并集（旧托管键不漏还原）
//   - settings.json 不存在 → 仅含 env 的最小文件
//   - 解析失败 → 中止不写
//   - 上层覆盖检测命中 / 未命中
//   - 崩溃补还原（recoverPendingSnapshot）
//   - 原子性：无 .devdesk-tmp 残留
//   - ClaudeService 接线：startProxy 接管 / stopProxy 还原 / 运行中切 profile 跟随
//   - 接管开关关闭：启动补还原 / startProxy / stopProxy 三条路径都不写文件
//   - 共享配置片段：合并优先级（片段 > profile，托管键最上）+ 片段键进托管键集
//     随还原撤走 + 解绑后同名键回落 profile 值
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
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>  // 仅反例用例（独占句柄造「存在但打不开」）
#endif

namespace {

constexpr const char* kTestKeychainService = "DevDeskTestSettingsSync";

// 临时 settings.json / sidecar 目录（共享头 test_support::TempDir，前缀沿用原样）
constexpr const char* kSyncTempDirPrefix = "devdesk_test_sync_";

module::claude::settings_sync::Paths makePaths(const test_support::TempDir& d) {
    module::claude::settings_sync::Paths p;
    p.settingsFile = d.file("settings.json");
    p.sidecarFile = d.file("snapshot.json");
    return p;
}

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

// 临时 db 文件名前缀（共享工厂 test_support::tempDbPath / TestDb 的第一实参）
constexpr const char* kDbPrefix = "devdesk_test_sync_db_";

// ---- 假 Keychain（CPX-14 注入缝，同 test_claude_service.cpp:592 ScopedFakeKeychain
// 与 tests/test_platform/test_keychain.cpp 的 CountingKeychain 写法）----
// 存在的理由（merge-review R2）：走真门面的端到端用例里 createProfile 会向
// **系统钥匙串**写测试密钥，而清理只在函数尾部 deleteSecret——中间任一
// DOCTEST_REQUIRE 失败就残留一条真实条目（与 P0 钥匙串事故同款形状：测试写
// 真实用户存储）。装上假后端后该用例对真钥匙串零读写，且「写了几条」本身
// 变成可断言的（count() 恒 0 = 真的没碰真后端之外的东西）。
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

// ---- 1. buildProxyEnv：基础 + 槽位 + 派生 + 空槽位跳过 ----
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
    // default：model 写 ANTHROPIC_MODEL；name 空 → 派生剥 [1M]；
    // ANTHROPIC_MODEL 无官方 _NAME 变体，不生成
    DOCTEST_CHECK_EQ(env.at("ANTHROPIC_MODEL"), std::string("fangzhou/glm-5.3[1M]"));
    DOCTEST_CHECK(env.find("ANTHROPIC_MODEL_NAME") == env.end());
    // sonnet：显示名显式配置优先于派生
    DOCTEST_CHECK_EQ(env.at("ANTHROPIC_DEFAULT_SONNET_MODEL"),
                     std::string("fangzhou/glm-5.3[1M]"));
    DOCTEST_CHECK_EQ(env.at("ANTHROPIC_DEFAULT_SONNET_MODEL_NAME"),
                     std::string("我的 Sonnet"));
    // haiku：name 空 → 派生（无标记原样）
    DOCTEST_CHECK_EQ(env.at("ANTHROPIC_DEFAULT_HAIKU_MODEL_NAME"),
                     std::string("claude-3-5-haiku"));
    // opus/fable 空槽位 → 跳过
    DOCTEST_CHECK(env.find("ANTHROPIC_DEFAULT_OPUS_MODEL") == env.end());
    DOCTEST_CHECK(env.find("ANTHROPIC_DEFAULT_FABLE_MODEL") == env.end());
    // subagent：无 _NAME 变体
    DOCTEST_CHECK_EQ(env.at("CLAUDE_CODE_SUBAGENT_MODEL"), std::string("glm-5.3[1M]"));
    DOCTEST_CHECK(env.find("CLAUDE_CODE_SUBAGENT_MODEL_NAME") == env.end());

    // 恒定写入占位凭据（无「空 token 跳过」分支了）
    const auto noTok = buildProxyEnv("", "", 8888);
    DOCTEST_CHECK_EQ(noTok.at("ANTHROPIC_AUTH_TOKEN"),
                     std::string("PROXY_MANAGED"));
    return true;
})

// ---- 2. buildProxyEnv：extra_env 别名归一 + 托管键优先 ----
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
    // 冲突时托管键（代理 BASE_URL/占位凭据）优先，导入的旧值被覆盖
    DOCTEST_CHECK_EQ(env.at("ANTHROPIC_BASE_URL"),
                     std::string("http://127.0.0.1:9999"));
    DOCTEST_CHECK_EQ(env.at("ANTHROPIC_AUTH_TOKEN"),
                     std::string("PROXY_MANAGED"));
    return true;
})

// ---- 3. takeOver 合并 + restore 往返（含原本不存在键的删除）----
REGISTER_TEST(sync_takeover_restore_roundtrip, "settings_sync: 接管合并与还原往返", {
    using namespace module::claude;
    test_support::TempDir d("roundtrip", kSyncTempDirPrefix);
    auto paths = makePaths(d);

    // 用户已有配置：非 env 字段 + 自有 env 键（有的键与托管键同名但值不同）
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

    // 接管后：托管键写入、用户自有键保留、非 env 字段保留、sidecar 存在
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
        DOCTEST_CHECK(d.exists(paths.sidecarFile));
    }

    // 还原：同名托管键恢复用户原值，原本不存在的键删除，其余不动
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
        // 非 env 字段仍保留
        DOCTEST_CHECK_EQ(topLevelString(paths.settingsFile, "model"),
                         std::string("claude-sonnet-4-5"));
        // sidecar 已删除
        DOCTEST_CHECK(!d.exists(paths.sidecarFile));
    }
    // env 值语义与接管前一致（格式被 pretty 归一，值逐键等价）
    (void)original;
    return true;
})

// ---- 4. 重复 takeOver：首份快照保留 + 托管键并集 ----
REGISTER_TEST(sync_retakeover_keeps_first_snapshot, "settings_sync: 热切换保留首份快照并并入托管键", {
    using namespace module::claude;
    test_support::TempDir d("retake", kSyncTempDirPrefix);
    auto paths = makePaths(d);

    d.write(paths.settingsFile,
            R"({"env": {"MY_KEY": "user"}})");

    // 第一次接管：profile A 带 sonnet 槽位
    Profile a;
    const std::string modelsA = R"({"sonnet": {"model": "m-a"}})";
    a.modelsConfigJson = modelsA;
    DOCTEST_REQUIRE(settings_sync::takeOver(a, 8888, paths).ok);

    // 第二次接管（切 profile）：B 无 sonnet 槽位 → 该键不在 B 的托管集，
    // 但并集后仍归 DevDesk 管理（restore 时删除，不留陈旧值）
    Profile b;
    const std::string modelsB = R"({"opus": {"model": "m-b"}})";
    b.modelsConfigJson = modelsB;
    DOCTEST_REQUIRE(settings_sync::takeOver(b, 8888, paths).ok);
    {
        const auto env = readEnv(paths.settingsFile);
        // 条目 1：上一轮托管、本轮不再生成的键从 env 删除（槽位清空后旧模型名
        // 不得继续被客户端使用）
        DOCTEST_CHECK(env.find("ANTHROPIC_DEFAULT_SONNET_MODEL") == env.end());
        DOCTEST_CHECK_EQ(env.at("ANTHROPIC_DEFAULT_OPUS_MODEL"),
                         std::string("m-b"));
        DOCTEST_CHECK_EQ(env.at("MY_KEY"), std::string("user"));
    }
    // 但 sidecar 仍记录它（还原语义不回归：旧托管键仍归 DevDesk 管）
    {
        const auto sidecarText = d.read(paths.sidecarFile);
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

// ---- 5. settings.json 不存在 → 创建仅含 env 的最小文件；还原后干净 ----
REGISTER_TEST(sync_takeover_missing_file, "settings_sync: 无 settings.json 时创建最小文件", {
    using namespace module::claude;
    test_support::TempDir d("missing", kSyncTempDirPrefix);
    auto paths = makePaths(d);

    Profile p;
    p.model = "m-1";
    DOCTEST_REQUIRE(settings_sync::takeOver(p, 8888, paths).ok);

    DOCTEST_CHECK(d.exists(paths.settingsFile));
    {
        const auto env = readEnv(paths.settingsFile);
        DOCTEST_CHECK_EQ(env.at("ANTHROPIC_BASE_URL"),
                         std::string("http://127.0.0.1:8888"));
        // 最小文件只有 env 一个顶层键
        std::ifstream i(paths.settingsFile);
        std::ostringstream ss;
        ss << i.rdbuf();
        auto parsed = platform::json::parse(ss.str());
        DOCTEST_REQUIRE(parsed.isOk());
        auto keys = parsed.value().asObject().keys();
        DOCTEST_CHECK_EQ(keys.size(), 1u);
        DOCTEST_CHECK_EQ(keys[0], std::string("env"));
    }

    // 还原：托管键全部删除（原本就不存在），sidecar 清掉
    DOCTEST_REQUIRE(settings_sync::restore(paths).ok);
    const auto env = readEnv(paths.settingsFile);
    DOCTEST_CHECK(env.find("ANTHROPIC_BASE_URL") == env.end());
    DOCTEST_CHECK(!d.exists(paths.sidecarFile));
    return true;
})

// ---- 6. 解析失败 → 中止不写（宁可不让接入，不能弄坏用户文件）----
REGISTER_TEST(sync_parse_failure_aborts, "settings_sync: settings.json 解析失败中止不写", {
    using namespace module::claude;
    test_support::TempDir d("parsefail", kSyncTempDirPrefix);
    auto paths = makePaths(d);

    const std::string broken = "{ not valid json !!!";
    d.write(paths.settingsFile, broken);

    Profile p;
    auto r = settings_sync::takeOver(p, 8888, paths);
    DOCTEST_CHECK(!r.ok);
    DOCTEST_CHECK(!r.error.empty());
    // 文件原样、无 sidecar
    DOCTEST_CHECK_EQ(d.read(paths.settingsFile), broken);
    DOCTEST_CHECK(!d.exists(paths.sidecarFile));

    // sidecar 损坏时 takeOver 同样中止（不把上次托管键误当用户原值）
    d.write(paths.settingsFile, "{}");
    d.write(paths.sidecarFile, "{broken");
    DOCTEST_CHECK(!settings_sync::takeOver(p, 8888, paths).ok);

    // sidecar 损坏时 restore 报错并保留文件（供人工恢复）
    auto rr = settings_sync::restore(paths);
    DOCTEST_CHECK(!rr.ok);
    DOCTEST_CHECK(d.exists(paths.sidecarFile));
    return true;
})

// ---- 6b. 首份快照污染守卫（条目 2①）：代理形态键不进快照 ----
REGISTER_TEST(sync_takeover_snapshot_skips_proxy_form,
              "settings_sync: 首份快照跳过本机代理形态键（污染守卫）", {
    using namespace module::claude;
    test_support::TempDir d("pollute", kSyncTempDirPrefix);
    auto paths = makePaths(d);

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

// ---- 6c. 无 sidecar 兜底（条目 2②）：仍指向死代理 → 最小清除 ----
REGISTER_TEST(sync_restore_without_sidecar_clears_residual,
              "settings_sync: 无快照但仍是代理形态 → 最小清除（restore）", {
    using namespace module::claude;
    test_support::TempDir d("residual", kSyncTempDirPrefix);
    auto paths = makePaths(d);
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
    DOCTEST_CHECK(!d.exists(paths.sidecarFile));  // 不新增 sidecar
    return true;
})

// ---- 6d. 无 sidecar 且无代理形态 → 真 no-op（一字不改）----
REGISTER_TEST(sync_restore_without_sidecar_noop,
              "settings_sync: 无快照且非代理形态 → 真 no-op（restore）", {
    using namespace module::claude;
    test_support::TempDir d("noop", kSyncTempDirPrefix);
    auto paths = makePaths(d);
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
    DOCTEST_CHECK(!d.exists(paths.sidecarFile));

    // settings.json 不存在 / 无 env 块 → 同样 no-op（不创建文件、不改内容）
    test_support::TempDir d2("noop2", kSyncTempDirPrefix);
    auto paths2 = makePaths(d2);
    DOCTEST_CHECK(settings_sync::restore(paths2).ok);
    DOCTEST_CHECK(!d2.exists(paths2.settingsFile));
    d2.write(paths2.settingsFile, R"({"permissions": {}})");
    const std::string noEnv = d2.read(paths2.settingsFile);
    DOCTEST_CHECK(settings_sync::restore(paths2).ok);
    DOCTEST_CHECK_EQ(d2.read(paths2.settingsFile), noEnv);
    return true;
})

// ---- 7. 上层覆盖检测：命中 / 未命中 ----
REGISTER_TEST(sync_overlay_detection, "settings_sync: managed/project 层覆盖检测", {
    using namespace module::claude;
    test_support::TempDir d("overlay", kSyncTempDirPrefix);
    auto paths = makePaths(d);

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

// ---- 7b. 覆盖检测键集来源（CPX-12）：托管键全集 ∪ 本轮实际托管键 ----
REGISTER_TEST(sync_overlay_detection_managed_derived,
              "settings_sync: 覆盖检测含托管键全集与 profile 实键", {
    using namespace module::claude;
    test_support::TempDir d("overlayderived", kSyncTempDirPrefix);
    auto paths = makePaths(d);

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

    // ② 空槽位的 *_NAME 键仍在检测集内（旧硬编码 8 键的缺口：haiku 槽位为空 →
    //    buildProxyEnv 不生成该键，只有托管键全集能识别上位覆盖）
    d.write(overlayName,
            R"({"env": {"ANTHROPIC_DEFAULT_HAIKU_MODEL_NAME": "x"}})");
    paths.overlayFiles = {overlayName};
    auto r2 = settings_sync::takeOver(p, 8888, paths);
    DOCTEST_REQUIRE(r2.ok);
    DOCTEST_REQUIRE_EQ(r2.warnings.size(), 1u);
    DOCTEST_CHECK(r2.warnings[0].find("ANTHROPIC_DEFAULT_HAIKU_MODEL_NAME") !=
                  std::string::npos);

    // ③ 只写未托管键 → 0 条
    d.write(overlayName, R"({"env": {"SOME_UNMANAGED_KEY": "x"}})");
    paths.overlayFiles = {overlayName};
    auto r3 = settings_sync::takeOver(p, 8888, paths);
    DOCTEST_REQUIRE(r3.ok);
    DOCTEST_CHECK(r3.warnings.empty());
    return true;
})

// ---- 8. 崩溃补还原 + 原子性（无 tmp 残留）----
REGISTER_TEST(sync_crash_recovery_and_atomic, "settings_sync: 崩溃补还原与原子写残留", {
    using namespace module::claude;
    test_support::TempDir d("crash", kSyncTempDirPrefix);
    auto paths = makePaths(d);

    const std::string original = R"({"env": {"MY_KEY": "user"}})";
    d.write(paths.settingsFile, original);

    // 模拟崩溃：takeOver 后没走 restore（sidecar 残留）
    Profile p;
    p.model = "m-1";
    DOCTEST_REQUIRE(settings_sync::takeOver(p, 8888, paths).ok);

    // 下次启动：recoverPendingSnapshot 自动补还原
    auto r = settings_sync::recoverPendingSnapshot(paths);
    DOCTEST_REQUIRE(r.ok);
    const auto env = readEnv(paths.settingsFile);
    DOCTEST_CHECK_EQ(env.at("MY_KEY"), std::string("user"));
    DOCTEST_CHECK(env.find("ANTHROPIC_BASE_URL") == env.end());
    DOCTEST_CHECK(!d.exists(paths.sidecarFile));

    // 无 sidecar 时 recover 是 no-op
    DOCTEST_CHECK(settings_sync::recoverPendingSnapshot(paths).ok);

    // 原子性：接管/还原后目录里没有 .devdesk-tmp 残留
    DOCTEST_REQUIRE(settings_sync::takeOver(p, 8888, paths).ok);
    DOCTEST_REQUIRE(settings_sync::restore(paths).ok);
    for (const auto& entry : std::filesystem::directory_iterator(d.path)) {
        DOCTEST_CHECK(entry.path().string().find(".devdesk-tmp") ==
                      std::string::npos);
    }
    return true;
})

// ---- 9. ClaudeService 接线：start 接管 / stop 还原 / 运行中切 profile 跟随 ----
REGISTER_TEST(sync_service_wiring, "claude_service: 接管通道三触发点接线", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "wiring");
    test_support::TempDir d("wiring", kSyncTempDirPrefix);
    auto paths = makePaths(d);

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

    // 注入路径 + 建 profile（带模型槽位 + extra_env）
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

    // 清理 keychain 条目
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

// ---- 10. 上层覆盖警告经 ClaudeService 暴露（Phase D UI 数据源）----
REGISTER_TEST(sync_service_warnings_surface, "claude_service: 接管警告经 lastSyncWarnings 暴露", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "warn");
    test_support::TempDir d("warn", kSyncTempDirPrefix);
    auto paths = makePaths(d);
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

// ---- 10b. 接管失败并入 lastSyncWarnings（条目 3）：失败不阻断起代理 / 切 profile ----
REGISTER_TEST(sync_service_takeover_failure_reported,
              "claude_service: 接管失败并入 lastSyncWarnings 且不阻断操作", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "tofail");
    test_support::TempDir d("tofail", kSyncTempDirPrefix);
    auto paths = makePaths(d);

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
    d.write(paths.sidecarFile, "{broken");
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

// ---- 11. 应用退出序列（W2 终验问题 1）：Router::stopAll → onStop → 还原 ----
// main.cpp 的 atexit handler 走的正是这条链；这里模拟该退出序列，断言
// 接管状态下退出后 settings.json 还原、sidecar 被消费、代理已停。
extern "C" ::core::Module* create_claude_module();

REGISTER_TEST(sync_exit_sequence_stop_all,
              "claude_module: 退出序列 Router::stopAll 触发还原", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "exit");
    test_support::TempDir d("exit", kSyncTempDirPrefix);
    auto paths = makePaths(d);
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
    // 接管已发生：托管键写入 + sidecar 落地
    {
        const auto env = readEnv(paths.settingsFile);
        DOCTEST_CHECK(env.find("ANTHROPIC_BASE_URL") != env.end());
        DOCTEST_CHECK(d.exists(paths.sidecarFile));
        DOCTEST_CHECK(svc.isClaudeCodeManaged());
    }

    // 模拟退出序列（main.cpp atexit → Router::stopAll）。生产序列 = 装配段
    // registerModule → startAll onStart（模块恒先启动后停止）；onStart 重入
    // 幂等守卫（活门控 Task 4）后，未启动模块的 onStop = no-op，故这里按真实
    // 顺序补 onStart——被锁的契约是「启动过的模块退出时触发还原」，不变。
    std::unique_ptr<::core::Module> mod(create_claude_module());
    ::core::Router router;
    router.registerModule(mod.get());
    mod->onStart();
    router.stopAll();

    // 断言：还原完成（用户键保留、托管键消失）、sidecar 已消费、代理已停
    const auto env = readEnv(paths.settingsFile);
    DOCTEST_CHECK_EQ(env.at("MY_KEY"), std::string("user"));
    DOCTEST_CHECK(env.find("ANTHROPIC_BASE_URL") == env.end());
    DOCTEST_CHECK(env.find("ANTHROPIC_AUTH_TOKEN") == env.end());
    DOCTEST_CHECK(!d.exists(paths.sidecarFile));
    DOCTEST_CHECK(!svc.isClaudeCodeManaged());
    DOCTEST_CHECK(!svc.isProxyRunning());
    return true;
})

// ---- 12. 产品审计 2026-09-23 [1]：删激活 Profile 停代理并还原 settings.json ----
// provider-config 设计 §7 安全第 5 条：删除激活 profile 且代理运行中 →
// 停代理（触发还原），settings.json 不能继续指向无 profile 的代理。
REGISTER_TEST(sync_delete_active_profile_stops_proxy,
              "claude_service: 删激活 Profile 停代理并还原 settings（审计 [1]）", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "delact");
    test_support::TempDir d("delact", kSyncTempDirPrefix);
    auto paths = makePaths(d);
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

    // 清理 keychain 条目
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

// ---- 13. 接管开关关闭（2026-09-27）：完全不写 settings.json ----
// 接管默认开（见 claude_service.h setSettingsTakeOverEnabled）；关掉开关 =
// DevDesk 不碰该文件，三条路径一起门控：startProxy 接管 / stopProxy 还原 /
// 启动补还原。本用例是「开关关闭时零写入」的自动化兜底（此前只有人工指纹比对）。
REGISTER_TEST(sync_switch_off_no_write,
              "claude_service: 接管开关关闭时不写 settings.json", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "off");
    test_support::TempDir d("off", kSyncTempDirPrefix);
    auto paths = makePaths(d);
    const std::string original = R"({"env": {"MY_KEY": "user"}})";
    d.write(paths.settingsFile, original);

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    DOCTEST_CHECK(svc.settingsTakeOverEnabled());  // 默认开（2026-09-27 恢复）
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
    DOCTEST_CHECK(!d.exists(paths.sidecarFile));

    // 路径 3：stopProxy → 不还原（未接管过，同样不动文件）
    svc.stopProxy();
    DOCTEST_CHECK_EQ(d.read(paths.settingsFile), original);
    DOCTEST_CHECK(!d.exists(paths.sidecarFile));

    // 遗留 sidecar（上次没走完还原）+ 开关关闭 → 补还原同样跳过，快照留给
    // 下次开启接管时处理（不代改用户文件）
    d.write(paths.sidecarFile,
            R"({"managedKeys": {"ANTHROPIC_BASE_URL": ""},)"
            R"( "envSnapshot": {"MY_KEY": "user"}})");
    svc.recoverPendingSettingsRestore();
    DOCTEST_CHECK_EQ(d.read(paths.settingsFile), original);
    DOCTEST_CHECK(d.exists(paths.sidecarFile));

    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

// ---- 14. 反向同步读侧（设计 §3.3 G7/G8/G9）----
// 六种文件形态分别处置：缺失 / 0 字节空文件 / 无 env 键 → no-op 成功
//（present=false，调用方照常切换）；坏 JSON / 顶层非对象 / env 非对象 →
// 中止（ok=false + 带路径的 error）。全部走临时目录，不碰真实 ~/.claude。

namespace {

bool hasKeyEntry(const std::vector<std::string>& keys, const char* key) {
    for (const auto& k : keys) {
        if (k == key) return true;
    }
    return false;
}

} // namespace

REGISTER_TEST(sync_reverse_read_missing_empty_noenv,
              "settings_sync: 反向读——缺失/空文件/无 env 均 no-op 成功（reverse G7/G9）", {
    using module::claude::settings_sync::readSettingsEnvForSync;
    test_support::TempDir d("revnoop", kSyncTempDirPrefix);
    auto paths = makePaths(d);

    // G7：文件不存在 → no-op 成功（调用方照常完成本次切换）
    {
        auto r = readSettingsEnvForSync(paths);
        DOCTEST_CHECK(r.ok);
        DOCTEST_CHECK(!r.present);
        DOCTEST_CHECK(r.error.empty());
        DOCTEST_CHECK(r.envJson.empty());
        DOCTEST_CHECK(r.managedKeys.empty());
    }
    // G9：0 字节空文件（视为不存在——不属于「存在但读不了」的中止侧）
    {
        d.write(paths.settingsFile, "");
        auto r = readSettingsEnvForSync(paths);
        DOCTEST_CHECK(r.ok);
        DOCTEST_CHECK(!r.present);
        DOCTEST_CHECK(r.error.empty());
    }
    // G9：解析成功但无 env 键
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
              "settings_sync: 反向读——坏 JSON/顶层非对象/env 非对象中止（reverse G8）", {
    using module::claude::settings_sync::readSettingsEnvForSync;
    test_support::TempDir d("revbad", kSyncTempDirPrefix);
    auto paths = makePaths(d);

    // 非法 JSON → 中止（S-2：调用方据此不切换，profile 一字未改）
    {
        d.write(paths.settingsFile, "{ not valid json !!!");
        auto r = readSettingsEnvForSync(paths);
        DOCTEST_CHECK(!r.ok);
        DOCTEST_CHECK(!r.present);
        DOCTEST_CHECK(r.error.find("settings.json") != std::string::npos);
    }
    // 顶层非对象
    {
        d.write(paths.settingsFile, "[1, 2, 3]");
        auto r = readSettingsEnvForSync(paths);
        DOCTEST_CHECK(!r.ok);
        DOCTEST_CHECK(!r.present);
        DOCTEST_CHECK(!r.error.empty());
    }
    // env 非对象
    {
        d.write(paths.settingsFile, R"({"env": "not-an-object"})");
        auto r = readSettingsEnvForSync(paths);
        DOCTEST_CHECK(!r.ok);
        DOCTEST_CHECK(!r.present);
        DOCTEST_CHECK(r.error.find("env") != std::string::npos);
    }
    return true;
})

// G8 读失败（审计）：存在但打不开（IO/权限）≠ 0 字节空文件 → 中止（仅 Windows：
// 独占句柄是本平台构造「存在但读不出」的唯一可靠形态）
#if defined(_WIN32)
REGISTER_TEST(sync_reverse_read_unreadable_aborts,
              "settings_sync: 反向读——存在但打不开中止（reverse G8 读失败）", {
    using module::claude::settings_sync::readSettingsEnvForSync;
    test_support::TempDir d("revunreadable", kSyncTempDirPrefix);
    auto paths = makePaths(d);
    d.write(paths.settingsFile, R"({"env": {"MY_KEY": "user"}})");

    // 独占句柄（share mode = 0）：文件存在、GetFileAttributes 可见，ifstream 打不开
    HANDLE h = CreateFileW(paths.settingsFile.wstring().c_str(), GENERIC_READ,
                           0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    DOCTEST_REQUIRE(h != INVALID_HANDLE_VALUE);
    {
        auto r = readSettingsEnvForSync(paths);
        DOCTEST_CHECK(!r.ok);  // G8：读失败中止，不静默当空文件
        DOCTEST_CHECK(!r.present);
        DOCTEST_CHECK(r.error.find(paths.settingsFile.string()) !=
                      std::string::npos);  // 错误含路径
    }
    CloseHandle(h);

    // 对照：0 字节空文件仍 no-op 成功（G9，属「不存在」侧的处置）
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
    auto paths = makePaths(d);
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
        d.write(paths.sidecarFile,
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
    // 把用户挡在切换之外；G2/G3 仍挡代理形态值）
    {
        d.write(paths.sidecarFile, "{broken");
        auto r = readSettingsEnvForSync(paths);
        DOCTEST_REQUIRE(r.ok);
        DOCTEST_CHECK(r.present);
        DOCTEST_CHECK(r.managedKeys.empty());
        DOCTEST_CHECK(!r.envJson.empty());
    }
    return true;
})

// 读 sidecar 的 nonEnvSnapshot / nonEnvKeys（§5.1 v2 形态断言用）
static std::map<std::string, std::string> readSidecarNonEnvSnapshot(
    const std::filesystem::path& f) {
    std::map<std::string, std::string> out;
    std::ifstream i(f, std::ios::binary);
    if (!i) return out;
    std::ostringstream ss;
    ss << i.rdbuf();
    auto parsed = platform::json::parse(ss.str());
    if (parsed.isFail() || !parsed.value().isObject()) return out;
    auto obj = parsed.value().asObject();
    if (!obj.has("nonEnvSnapshot") || !obj.get("nonEnvSnapshot").isObject())
        return out;
    auto snap = obj.get("nonEnvSnapshot").asObject();
    for (const auto& k : snap.keys()) {
        out[k] = snap.get(k).isString() ? snap.get(k).asString() : std::string();
    }
    return out;
}

static std::vector<std::string> readSidecarNonEnvKeys(
    const std::filesystem::path& f) {
    std::vector<std::string> out;
    std::ifstream i(f, std::ios::binary);
    if (!i) return out;
    std::ostringstream ss;
    ss << i.rdbuf();
    auto parsed = platform::json::parse(ss.str());
    if (parsed.isFail() || !parsed.value().isObject()) return out;
    auto obj = parsed.value().asObject();
    if (!obj.has("nonEnvKeys") || !obj.get("nonEnvKeys").isObject()) return out;
    return obj.get("nonEnvKeys").asObject().keys();
}

// ---- 15. 共享配置片段（advanced-json-editor §8 重构后）：env 域片段管线
// ---- 已拆除——片段不再携带 env 键，叠加走 JSON 域（effectiveExtraSettingsJson
// ---- + applyExtraSettingsToSettingsDoc）。本组钉住「域搬迁不残留双通道」。

REGISTER_TEST(sync_build_proxy_env_shared_config,
              "settings_sync: 片段 env 域管线已拆——buildProxyEnv 不再吃片段，JSON 域为唯一叠加通道", {
    using module::claude::buildProxyEnv;
    using module::claude::effectiveExtraSettingsJson;
    using module::claude::applyExtraSettingsToSettingsDoc;

    const std::string profileEnv =
        R"({"MY_KEY": "from-profile", "PROFILE_ONLY": "1"})";

    // env 域：profile 自身 env + 托管键（原「片段 env 叠加」形参已拆除——
    // 片段键不再出现在 env 块）
    const auto env = buildProxyEnv("", profileEnv, 8888);
    DOCTEST_CHECK_EQ(env.at("MY_KEY"), std::string("from-profile"));
    DOCTEST_CHECK_EQ(env.at("PROFILE_ONLY"), std::string("1"));
    DOCTEST_CHECK_EQ(env.at("ANTHROPIC_BASE_URL"),
                     std::string("http://127.0.0.1:8888"));
    DOCTEST_CHECK_EQ(env.at("ANTHROPIC_AUTH_TOKEN"),
                     std::string("PROXY_MANAGED"));
    DOCTEST_CHECK_EQ(env.size(), static_cast<size_t>(4));   // 无任何片段键混入

    // JSON 域 = 唯一片段通道：片段 JSON 叠在 profile JSON 之上（片段赢），
    // 经 apply 并入 settings 文档顶层（env 块不受影响）
    const std::string effective = effectiveExtraSettingsJson(
        R"({"statusLine": "profile-s"})",
        R"({"statusLine": "frag-s", "hooks": {}})");
    platform::json::JsonObject root;
    root.set("env", platform::json::JsonValue(platform::json::makeObject()));
    auto apply = applyExtraSettingsToSettingsDoc(root, effective);
    DOCTEST_REQUIRE_EQ(apply.writtenKeys.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(root.get("statusLine").asString(), std::string("frag-s"));
    DOCTEST_CHECK(root.get("hooks").isObject());
    DOCTEST_CHECK(root.get("env").isObject());
    return true;
})

REGISTER_TEST(sync_takeover_with_shared_config,
              "settings_sync: 生效 JSON（门面装配语义）进 nonEnv 记账，随还原撤走", {
    using namespace module::claude;
    test_support::TempDir d("shared", kSyncTempDirPrefix);
    auto paths = makePaths(d);
    d.write(paths.settingsFile, R"({"env": {"MY_KEY": "user"}})");

    // p.extraSettingsJson = 门面 §5.2① 装配后的生效 JSON（profile ⊕ 片段、
    // 片段赢——片段叠加本身在门面完成，直调 takeOver 拿到的是装配结果）
    Profile p;
    p.model = "m-1";
    p.extraEnvJson = R"({"PROFILE_ONLY": "1"})";
    p.extraSettingsJson = effectiveExtraSettingsJson(
        R"({"statusLine": "profile-s"})",
        R"({"statusLine": "frag-s", "hooks": {}})");

    auto r = settings_sync::takeOver(p, 8888, paths);
    DOCTEST_REQUIRE(r.ok);
    {
        // 片段赢的同名键以片段值写入顶层；env 块零片段痕迹
        DOCTEST_CHECK_EQ(topLevelString(paths.settingsFile, "statusLine"),
                         std::string("frag-s"));
        DOCTEST_CHECK(settings_sync::hasPendingSnapshot(paths));
        const auto keys = readSidecarNonEnvKeys(paths.sidecarFile);
        DOCTEST_CHECK(hasKeyEntry(keys, "statusLine"));
        DOCTEST_CHECK(hasKeyEntry(keys, "hooks"));
    }
    // 还原：全部撤走（快照无原值 → 删除）。口径 = 键必须不存在——`.empty()`
    // 把「键不存在 / 值为空串 / 文件解析失败」混为一谈，写坏文件也照样绿
    DOCTEST_REQUIRE(settings_sync::restore(paths).ok);
    {
        auto doc = platform::json::parse(d.read(paths.settingsFile));
        DOCTEST_REQUIRE(doc.isOk());
        DOCTEST_CHECK_FALSE(doc.value().asObject().has("statusLine"));
        DOCTEST_CHECK_FALSE(doc.value().asObject().has("hooks"));
    }
    // 解绑语义（同一装配，片段改空）：profile 自身值生效（回落）
    p.extraSettingsJson = effectiveExtraSettingsJson(
        R"({"statusLine": "profile-s"})", "");
    DOCTEST_REQUIRE(settings_sync::takeOver(p, 8888, paths).ok);
    DOCTEST_CHECK_EQ(topLevelString(paths.settingsFile, "statusLine"),
                     std::string("profile-s"));
    DOCTEST_REQUIRE(settings_sync::restore(paths).ok);
    return true;
})

// ---- 16. 服务级：删片段 → 同事务解绑 + 代理在跑则立即 re-takeover ----
//（节头原为「服务级 G10：片段键在 live 被改」——G10 随 advanced-json-editor
// §8.4 拆除，本组现钉 JSON 域删片段的当场回落，见下方重写版用例）

REGISTER_TEST(sync_service_shared_config_delete_retakeover,
              "claude_service: 删片段当场解绑并重写 live（JSON 域片段键撤走、profile 自身值回落）", {
    using namespace module::claude;
    test_support::TestDb tdb(kDbPrefix, "fragdel");
    test_support::TempDir d("fragdel", kSyncTempDirPrefix);
    auto paths = makePaths(d);

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    svc.setSettingsSyncPaths(paths);
    svc.setSettingsTakeOverEnabled(true);

    // 片段 = 高级属性 JSON 预设（v11 起片段不再携带 env 键）
    auto frag = svc.createSharedConfig("片段S", R"({"statusLine": "frag-s"})");
    DOCTEST_REQUIRE(frag.isOk());
    auto a = svc.createProfile("绑片段", "https://a.example.com",
                               "sk-ind-test-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    Profile pa = a.value();
    pa.extraSettingsJson = R"({"statusLine": "profile-s"})";   // 片段赢场景：行里保留
    pa.sharedConfigId = frag.value().id;
    DOCTEST_REQUIRE(svc.updateProfile(pa).isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(pa.id).isOk());
    DOCTEST_REQUIRE(svc.startProxy("127.0.0.1", 0).isOk());
    // 生效 JSON（门面装配）写入顶层：同键片段值赢
    DOCTEST_CHECK_EQ(topLevelString(paths.settingsFile, "statusLine"),
                     std::string("frag-s"));

    // 删片段：同事务解绑 + 代理在跑 → 当场 re-takeover（片段键撤走、同名键
    // 回落 profile 自身值——陈旧非 env 键删除 + 生效 JSON 重建的净结果）；
    // 「改了不生效」不会被留到下次切换
    DOCTEST_REQUIRE(svc.deleteSharedConfig(frag.value().id).isOk());
    DOCTEST_CHECK_EQ(topLevelString(paths.settingsFile, "statusLine"),
                     std::string("profile-s"));
    auto ga = svc.getProfile(pa.id);
    DOCTEST_REQUIRE(ga.isOk());
    DOCTEST_CHECK_EQ(ga.value().sharedConfigId, 0);

    svc.stopProxy();
    platform::keychain::deleteSecret(kTestKeychainService,
                                     "claude:" + std::to_string(pa.id));
    return true;
})

// ===== 接管/还原往返·高级设置 JSON（2026-09-30 advanced-json-editor 设计 §10，
// ===== sidecar v2：nonEnvKeys/nonEnvSnapshot + merge-only 补拍，F-1）=====

// ① 接管写入 env + JSON 属性；live 已有同名非 env 键时 JSON 值胜，旧值进
//    nonEnvSnapshot（D-3：被覆盖旧值还原无损）
REGISTER_TEST(sync_takeover_extra_settings_written,
              "settings_sync: 接管写入 JSON 属性，同名键 JSON 胜且旧值进 nonEnvSnapshot", {
    using namespace module::claude;
    test_support::TempDir d("xset1", kSyncTempDirPrefix);
    auto paths = makePaths(d);
    d.write(paths.settingsFile,
            R"({"env": {"MY_KEY": "user"}, "model": "live-model", "statusLine": "live-s"})");

    Profile p;
    p.model = "m-1";
    p.extraSettingsJson = R"({"model": "json-model", "statusLine": "json-s"})";

    auto r = settings_sync::takeOver(p, 8888, paths);
    DOCTEST_REQUIRE(r.ok);
    // JSON 赢过 live 既有同名键（D-3）
    DOCTEST_CHECK_EQ(topLevelString(paths.settingsFile, "model"),
                     std::string("json-model"));
    DOCTEST_CHECK_EQ(topLevelString(paths.settingsFile, "statusLine"),
                     std::string("json-s"));
    // env 照常写入；且 JSON 的 "model"（非空字符串）触发 ANTHROPIC_MODEL 抑制
    //（D-4：def 槽位 env 键被顶层 model 对映键压制，不写入）
    {
        const auto envWritten = readEnv(paths.settingsFile);
        DOCTEST_CHECK(envWritten.find("ANTHROPIC_MODEL") == envWritten.end());
    }
    // 旧值进 nonEnvSnapshot（还原无损的前提）
    const auto snap = readSidecarNonEnvSnapshot(paths.sidecarFile);
    DOCTEST_CHECK_EQ(snap.at("model"), std::string("live-model"));
    DOCTEST_CHECK_EQ(snap.at("statusLine"), std::string("live-s"));
    const auto keys = readSidecarNonEnvKeys(paths.sidecarFile);
    DOCTEST_CHECK(hasKeyEntry(keys, "model"));
    DOCTEST_CHECK(hasKeyEntry(keys, "statusLine"));
    DOCTEST_REQUIRE(settings_sync::restore(paths).ok);
    DOCTEST_CHECK_EQ(topLevelString(paths.settingsFile, "model"),
                     std::string("live-model"));
    DOCTEST_CHECK_EQ(topLevelString(paths.settingsFile, "statusLine"),
                     std::string("live-s"));
    return true;
})

// ② ANTHROPIC_MODEL 抑制：JSON 含非空 "model" → env 无该键（apply 步移除，
//    F-5 时序）且该键仍在 managedKeys 并集（restore 语义不变）
REGISTER_TEST(sync_takeover_model_suppress,
              "settings_sync: JSON model 键抑制 ANTHROPIC_MODEL，managedKeys 并集仍含该键", {
    using namespace module::claude;
    test_support::TempDir d("xset2", kSyncTempDirPrefix);
    auto paths = makePaths(d);
    d.write(paths.settingsFile, R"({"env": {"MY_KEY": "user"}})");

    Profile p;
    p.model = "m-1";   // model 列
    p.modelsConfigJson = R"({"default": {"model": "m-1"}})";  // def 槽位
    p.extraSettingsJson = R"({"model": "json-model"})";
    auto r = settings_sync::takeOver(p, 8888, paths);
    DOCTEST_REQUIRE(r.ok);
    // 抑制：env 无 ANTHROPIC_MODEL；JSON 的 model 在顶层
    {
        const auto envWritten = readEnv(paths.settingsFile);
        DOCTEST_CHECK(envWritten.find("ANTHROPIC_MODEL") == envWritten.end());
    }
    DOCTEST_CHECK_EQ(topLevelString(paths.settingsFile, "model"),
                     std::string("json-model"));
    // managedKeys 并集仍含该键（buildProxyEnv 产出未变 → restore 语义不变）
    auto rd = readSettingsEnvForSync(paths);
    DOCTEST_REQUIRE(rd.ok);
    DOCTEST_CHECK(hasKeyEntry(rd.managedKeys, "ANTHROPIC_MODEL"));

    // restore：env 键按快照撤走；顶层 "model" 同样按 nonEnv 记账撤走
    //（live 原本无 model 键 → 删除）。键必须不存在（.empty() 口径三态混淆）
    DOCTEST_REQUIRE(settings_sync::restore(paths).ok);
    {
        auto doc = platform::json::parse(d.read(paths.settingsFile));
        DOCTEST_REQUIRE(doc.isOk());
        DOCTEST_CHECK_FALSE(doc.value().asObject().has("model"));
    }

    // 本轮 JSON 不含 model → 该键回到 buildProxyEnv 生成（m-1，槽位仍在）、
    // 不再被抑制（F-5 的另一半：抑制只发生在「本轮 JSON 含 model」的 apply
    // 步，不是持久状态——上一轮的抑制不残留，槽位键随 JSON 撤除自然恢复）
    p.extraSettingsJson = R"({"statusLine": "on"})";
    DOCTEST_REQUIRE(settings_sync::takeOver(p, 8888, paths).ok);
    DOCTEST_CHECK_EQ(readEnv(paths.settingsFile).at("ANTHROPIC_MODEL"),
                     std::string("m-1"));
    DOCTEST_CHECK(topLevelString(paths.settingsFile, "statusLine") ==
                  std::string("on"));
    // JSON 改回空串 → 无抑制状态残留，键照常生成
    p.extraSettingsJson = "";
    DOCTEST_REQUIRE(settings_sync::takeOver(p, 8888, paths).ok);
    DOCTEST_CHECK_EQ(readEnv(paths.settingsFile).at("ANTHROPIC_MODEL"),
                     std::string("m-1"));
    // restore：撤走（快照无原值 → 删除）
    DOCTEST_REQUIRE(settings_sync::restore(paths).ok);
    {
        const auto envFinal = readEnv(paths.settingsFile);
        DOCTEST_CHECK(envFinal.find("ANTHROPIC_MODEL") == envFinal.end());
        DOCTEST_CHECK(envFinal.find("MY_KEY") != envFinal.end());   // 用户键保留
    }
    return true;
})

// ③ restore：JSON 写入且快照无原值的键被删除、快照有原值的键恢复、sidecar 删除
REGISTER_TEST(sync_restore_extra_settings,
              "settings_sync: restore 按 nonEnvKeys/nonEnvSnapshot 恢复或删除顶层键", {
    using namespace module::claude;
    test_support::TempDir d("xset3", kSyncTempDirPrefix);
    auto paths = makePaths(d);
    d.write(paths.settingsFile,
            R"({"env": {}, "statusLine": "user-s", "permissions": {"p": 1}})");

    Profile p;
    p.extraSettingsJson = R"({"statusLine": "json-s", "hooks": {}})";
    DOCTEST_REQUIRE(settings_sync::takeOver(p, 8888, paths).ok);
    DOCTEST_CHECK_EQ(topLevelString(paths.settingsFile, "statusLine"),
                     std::string("json-s"));
    DOCTEST_CHECK(settings_sync::hasPendingSnapshot(paths));
    DOCTEST_REQUIRE(settings_sync::restore(paths).ok);
    // 快照有原值 → 恢复；快照无原值（原本不存在）→ 删除；sidecar 已删
    DOCTEST_CHECK_EQ(topLevelString(paths.settingsFile, "statusLine"),
                     std::string("user-s"));
    DOCTEST_CHECK_FALSE(settings_sync::hasPendingSnapshot(paths));
    // 用户自己的其他顶层键零触碰；hooks（快照无原值）必须不存在——
    // .empty() 口径把「键不存在 / 值为空串 / 文件解析失败」混为一谈
    auto doc = platform::json::parse(d.read(paths.settingsFile));
    DOCTEST_REQUIRE(doc.isOk());
    DOCTEST_CHECK_FALSE(doc.value().asObject().has("hooks"));
    DOCTEST_CHECK(doc.value().asObject().has("permissions"));
    return true;
})

// ④ merge-only 补拍（F-1 反例）：第 1 轮 JSON 无 statusLine、live 有手写值 →
//    不进账；第 2 轮 JSON 加 statusLine 热切换 re-takeover → 原值补拍 →
//    restore 恢复而非删除；且二次接管对已托管键不重拍
REGISTER_TEST(sync_takeover_merge_only_snapshot,
              "settings_sync: 二次接管 merge-only 补拍（新增键首拍、已托管键不重拍，F-1）", {
    using namespace module::claude;
    test_support::TempDir d("xset4", kSyncTempDirPrefix);
    auto paths = makePaths(d);
    d.write(paths.settingsFile,
            R"({"env": {}, "model": "user-model", "statusLine": "user-s"})");

    // 第 1 轮：JSON 只有 model（statusLine 不写入 → 不进 nonEnvKeys）
    Profile p;
    p.extraSettingsJson = R"({"model": "json-model"})";
    DOCTEST_REQUIRE(settings_sync::takeOver(p, 8888, paths).ok);
    {
        const auto keys = readSidecarNonEnvKeys(paths.sidecarFile);
        DOCTEST_CHECK(hasKeyEntry(keys, "model"));
        DOCTEST_CHECK(!hasKeyEntry(keys, "statusLine"));   // 未写入 → 未进账
        const auto snap = readSidecarNonEnvSnapshot(paths.sidecarFile);
        DOCTEST_CHECK_EQ(snap.at("model"), std::string("user-model"));   // 首拍
    }
    // 第 2 轮（热切换 re-takeover，sidecar 已存在）：JSON 加 statusLine
    p.extraSettingsJson = R"({"model": "json-model", "statusLine": "json-s"})";
    DOCTEST_REQUIRE(settings_sync::takeOver(p, 8888, paths).ok);
    {
        const auto keys = readSidecarNonEnvKeys(paths.sidecarFile);
        DOCTEST_CHECK(hasKeyEntry(keys, "statusLine"));
        const auto snap = readSidecarNonEnvSnapshot(paths.sidecarFile);
        // 补拍：statusLine 首次进账 → live 原值入快照（无此步 restore 会静默删它）
        DOCTEST_CHECK_EQ(snap.at("statusLine"), std::string("user-s"));
        // 不重拍：model 已在并集里，快照条目维持原值（live 现值是 JSON 托管值
        // "json-model"，重拍会把托管值当用户原值）
        DOCTEST_CHECK_EQ(snap.at("model"), std::string("user-model"));
    }
    // restore：两个键都恢复用户原值（F-1 反例的验收点）
    DOCTEST_REQUIRE(settings_sync::restore(paths).ok);
    DOCTEST_CHECK_EQ(topLevelString(paths.settingsFile, "model"),
                     std::string("user-model"));
    DOCTEST_CHECK_EQ(topLevelString(paths.settingsFile, "statusLine"),
                     std::string("user-s"));
    return true;
})

// ⑤ 旧形态 sidecar（无 nonEnv 新字段）→ restore 不误删任何非 env 键（D-9 空集守卫）
REGISTER_TEST(sync_restore_v1_sidecar_no_nonenv,
              "settings_sync: 旧形态 sidecar（无 nonEnv 字段）restore 不动非 env 键", {
    using namespace module::claude;
    test_support::TempDir d("xset5", kSyncTempDirPrefix);
    auto paths = makePaths(d);
    d.write(paths.settingsFile,
            R"({"env": {"MY_KEY": "user", "ANTHROPIC_BASE_URL": "http://127.0.0.1:8888", "ANTHROPIC_AUTH_TOKEN": "PROXY_MANAGED"}, "statusLine": "user-s"})");
    // 手工造 v1 形态 sidecar（只有 managedKeys + envSnapshot）
    d.write(paths.sidecarFile,
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

// ⑥ 片段叠加端到端（F-8）：绑定片段（片段与 profile JSON 各含独有键 + 同键）
// → 门面 §5.2① 装配生效 JSON（片段赢）→ 接管写入片段键且同键为片段值 →
// restore 全部撤走。走真门面（startProxy/stopProxy）钉住装配这一跳——装配漏传
// /传未生效化原文时必红（骨架照既有片段接管用例平移 JSON 域）
//
// 钥匙串隔离（merge-review R2）：本用例**装假 Keychain**（ScopedFakeKeychain，
// CPX-14 注入缝）。原写法 createProfile 会向**系统钥匙串**写测试密钥，清理
// 只有函数尾部那一次 deleteSecret——中间任一 DOCTEST_REQUIRE 失败就残留一条
// 真实条目（与 P0 钥匙串事故同款形状）。装假后端后本用例对真钥匙串零读写，
// 且末尾断言假后端里确实落过、也确实被清空（写链没被偷偷跳过）。
REGISTER_TEST(sync_takeover_fragment_json_end_to_end,
              "claude_service: 片段 JSON 叠加端到端（门面装配片段赢→接管写入→还原撤走，F-8）", {
    using namespace module::claude;
    ScopedFakeKeychain kc;   // 必须早于任何 createProfile（最早可能点）
    test_support::TestDb tdb(kDbPrefix, "fragjson");
    test_support::TempDir d("fragjson", kSyncTempDirPrefix);
    auto paths = makePaths(d);

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    svc.setSettingsSyncPaths(paths);
    svc.setSettingsTakeOverEnabled(true);

    auto frag = svc.createSharedConfig("片段J", R"({"statusLine": "frag-s", "hooks": {"a": 1}})");
    DOCTEST_REQUIRE(frag.isOk());
    auto a = svc.createProfile("绑JSON", "https://j.example.com",
                               "sk-ind-test-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    // 前置：密钥确实落到了（假）后端——否则末尾「清空」恒真、等于没走写链
    DOCTEST_REQUIRE_EQ(kc.fake.count(), static_cast<size_t>(1));
    Profile pa = a.value();
    pa.extraSettingsJson = R"({"statusLine": "profile-s", "permissions": {"p": 1}})";
    pa.sharedConfigId = frag.value().id;
    DOCTEST_REQUIRE(svc.updateProfile(pa).isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(pa.id).isOk());
    DOCTEST_REQUIRE(svc.startProxy("127.0.0.1", 0).isOk());
    {
        // 片段独有键 + profile 独有键都在；同键为片段值（片段赢，门面装配）
        DOCTEST_CHECK_EQ(topLevelString(paths.settingsFile, "statusLine"),
                         std::string("frag-s"));
        auto liveDoc = platform::json::parse(d.read(paths.settingsFile));
        DOCTEST_REQUIRE(liveDoc.isOk());
        DOCTEST_CHECK(liveDoc.value().asObject().get("hooks").isObject());
        DOCTEST_CHECK(liveDoc.value().asObject().get("permissions").isObject());
        // 片段键进 nonEnv 记账（还原撤走的依据）
        const auto keys = readSidecarNonEnvKeys(paths.sidecarFile);
        DOCTEST_CHECK(hasKeyEntry(keys, "statusLine"));
        DOCTEST_CHECK(hasKeyEntry(keys, "hooks"));
        DOCTEST_CHECK(hasKeyEntry(keys, "permissions"));
    }
    // stopProxy → restore：全部撤走（live 无原值 → 删除）
    svc.stopProxy();
    {
        // 键必须不存在（.empty() 口径把「键不存在 / 值为空串 / 文件解析失败」
        // 混为一谈，写坏文件也照样绿）
        auto doc = platform::json::parse(d.read(paths.settingsFile));
        DOCTEST_REQUIRE(doc.isOk());
        DOCTEST_CHECK_FALSE(doc.value().asObject().has("statusLine"));
        DOCTEST_CHECK_FALSE(doc.value().asObject().has("permissions"));
        DOCTEST_CHECK_FALSE(doc.value().asObject().has("hooks"));
    }
    platform::keychain::deleteSecret(kTestKeychainService,
                                     "claude:" + std::to_string(pa.id));
    // 假后端清空 = 清理链真的跑过（不是被 REQUIRE 跳过）
    DOCTEST_CHECK_EQ(kc.fake.count(), static_cast<size_t>(0));
    return true;
})

// ===== 非 env 托管键移除冲突告警（审计 CPX-12 的非 env 形态）=====
// 3.5 步陈旧非 env 键删除此前**无任何日志**——用户在 live settings.json 手写的
// 同名键被静默抹掉且毫不知情。本组钉住：①真冲突出警告（键名 + 「曾被接管
// 写入，本轮会移除 live 同名手写值」+ 如何避免）；②键在并集里但 root 无该键
// （陈旧清理正常路径）**零告警**（防噪音回归，必须有）。

// 警告是否提到某键（键名出现在任一条 warnings 文本里）
static bool warningsMention(const std::vector<std::string>& warns,
                            const char* key) {
    for (const auto& w : warns) {
        if (w.find(key) != std::string::npos) return true;
    }
    return false;
}

// ① 真冲突：第 1 轮 JSON 写 statusLine + hooks（两键进 nonEnvKeys 并集）→ 用户
//    在 live settings.json 手改同名键 → 第 2 轮 JSON 不再写它 → 3.5 步会移除
//    live 值，必须出告警且文案含键名与规避方式
REGISTER_TEST(sync_nonenv_removal_conflict_warns,
              "settings_sync: 非 env 托管键本轮不写且 live 有同名值 → 出冲突警告", {
    using namespace module::claude;
    test_support::TempDir d("nonenvconf", kSyncTempDirPrefix);
    auto paths = makePaths(d);
    d.write(paths.settingsFile, R"({"env": {"MY_KEY": "user"}})");

    // 第 1 轮：JSON 写 statusLine + hooks（两个键进 nonEnvKeys 并集）
    Profile p;
    p.extraSettingsJson = R"({"statusLine": "json-s", "hooks": {"a": 1}})";
    auto r1 = settings_sync::takeOver(p, 8888, paths);
    DOCTEST_REQUIRE(r1.ok);
    // 第 1 轮无移除 → 零噪音（无冲突时不得有任何新输出）
    DOCTEST_CHECK(r1.warnings.empty());
    {
        const auto keys = readSidecarNonEnvKeys(paths.sidecarFile);
        DOCTEST_REQUIRE(hasKeyEntry(keys, "statusLine"));
        DOCTEST_REQUIRE(hasKeyEntry(keys, "hooks"));
    }

    // 用户在 live settings.json 里手写同名键（接管后自己改）
    d.write(paths.settingsFile,
            R"({"env": {"MY_KEY": "user"}, "statusLine": "user-handwritten", "hooks": {"b": 2}})");

    // 第 2 轮：JSON 不再写这两个键 → 3.5 步将移除 live 值 → 必须告警
    p.extraSettingsJson = R"({"model": "json-model"})";
    auto r2 = settings_sync::takeOver(p, 8888, paths);
    DOCTEST_REQUIRE(r2.ok);
    // 两个键各报一次、合并进同一条（去重 + 稳定排序，不刷两条同义噪声）
    DOCTEST_REQUIRE_EQ(r2.warnings.size(), 1u);
    DOCTEST_CHECK(warningsMention(r2.warnings, "statusLine"));
    DOCTEST_CHECK(warningsMention(r2.warnings, "hooks"));
    // 文案要点：曾被接管写入 / 会移除 live 手写值 / 如何避免（改高级配置 JSON）
    const std::string& w = r2.warnings[0];
    DOCTEST_CHECK(w.find("曾被 DevDesk 接管写入") != std::string::npos);
    DOCTEST_CHECK(w.find("手写") != std::string::npos);
    DOCTEST_CHECK(w.find("高级配置 JSON") != std::string::npos);
    // 告警 ≠ 免删：本轮仍按陈旧清理移除（告警是告知，不改既有语义）
    {
        auto doc = platform::json::parse(d.read(paths.settingsFile));
        DOCTEST_REQUIRE(doc.isOk());
        DOCTEST_CHECK_FALSE(doc.value().asObject().has("statusLine"));
        DOCTEST_CHECK_FALSE(doc.value().asObject().has("hooks"));
    }
    return true;
})

// ② 防噪音：键在 nonEnvKeys 并集里（历史托管过）、本轮不写、但 live root 里
//    根本没有该键（用户已自行删掉）→ 陈旧清理的正常路径，静默跳过，不得报警。
//    这条不钉住，接管反复切几轮就会刷出无意义告警
REGISTER_TEST(sync_nonenv_removal_stale_absent_silent,
              "settings_sync: 托管键在并集里但 live 无该键（陈旧清理）→ 零告警", {
    using namespace module::claude;
    test_support::TempDir d("nonenvstale", kSyncTempDirPrefix);
    auto paths = makePaths(d);
    d.write(paths.settingsFile, R"({"env": {"MY_KEY": "user"}})");

    // 第 1 轮：JSON 写 statusLine（进并集）
    Profile p;
    p.extraSettingsJson = R"({"statusLine": "json-s"})";
    DOCTEST_REQUIRE(settings_sync::takeOver(p, 8888, paths).ok);
    DOCTEST_REQUIRE(
        hasKeyEntry(readSidecarNonEnvKeys(paths.sidecarFile), "statusLine"));

    // 用户自行把 statusLine 从 live settings.json 删了（键在并集里、root 无值）
    d.write(paths.settingsFile, R"({"env": {"MY_KEY": "user"}})");

    // 第 2 轮：JSON 不写它 → 无值可删 = 正常路径 → warnings 必须为空
    p.extraSettingsJson = R"({"model": "json-model"})";
    auto r2 = settings_sync::takeOver(p, 8888, paths);
    DOCTEST_REQUIRE(r2.ok);
    DOCTEST_CHECK(r2.warnings.empty());
    return true;
})
