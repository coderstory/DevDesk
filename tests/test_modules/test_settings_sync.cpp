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

#include "claude_service.h"
#include "core/database.h"
#include "core/module_interface.h"
#include "core/router.h"
#include "platform/json.h"
#include "platform/keychain.h"
#include "provider_config.h"
#include "settings_sync.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// REGISTER_TEST 宏实参不能有裸逗号（Phase A 报告的坑），模板类型统一用别名
using EnvMap = std::map<std::string, std::string>;

constexpr const char* kTestKeychainService = "DevDeskTestSettingsSync";

// RAII 临时目录
struct TempDir {
    std::filesystem::path path;

    explicit TempDir(const std::string& tag) {
        static int counter = 0;
        ++counter;
        path = test_temp::tempPath(
            "devdesk_test_sync_" + tag + "_" + std::to_string(counter));
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
        if (ec) throw std::runtime_error("temp dir create failed");
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    std::filesystem::path file(const char* name) const { return path / name; }

    void write(const std::filesystem::path& f, const std::string& content) const {
        std::ofstream o(f, std::ios::binary | std::ios::trunc);
        o << content;
    }
    std::string read(const std::filesystem::path& f) const {
        std::ifstream i(f, std::ios::binary);
        std::ostringstream ss;
        ss << i.rdbuf();
        return ss.str();
    }
    bool exists(const std::filesystem::path& f) const {
        std::error_code ec;
        return std::filesystem::exists(f, ec) && !ec;
    }
};

module::claude::settings_sync::Paths makePaths(const TempDir& d) {
    module::claude::settings_sync::Paths p;
    p.settingsFile = d.file("settings.json");
    p.sidecarFile = d.file("snapshot.json");
    return p;
}

// 读 settings.json 的 env 块为 map（不存在 / 无 env → 空表）
EnvMap readEnv(const std::filesystem::path& f) {
    EnvMap out;
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

std::filesystem::path tempDbPath(const std::string& tag) {
    static int counter = 0;
    ++counter;
    return test_temp::tempPath("devdesk_test_sync_db_" + tag + "_" +
                               std::to_string(counter) + ".db");
}

// RAII：已 migrate 的临时数据库
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
            throw std::runtime_error("Database::open failed");
        }
        db = std::make_unique<core::Database>(std::move(r.unsafeValue()));
        if (db->migrate().isFail()) {
            throw std::runtime_error("migrate failed");
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
    const auto env = buildProxyEnv(modelsJson, "legacy-model",
                                   /*extraEnvJson=*/"", 8888);

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

    // models_config 空 + 旧 model 列 → fallback 填 default 槽位
    const auto legacy = buildProxyEnv("", "claude-sonnet-4-5", "", 8888);
    DOCTEST_CHECK_EQ(legacy.at("ANTHROPIC_MODEL"), std::string("claude-sonnet-4-5"));

    // 恒定写入占位凭据（无「空 token 跳过」分支了）
    const auto noTok = buildProxyEnv("", "", "", 8888);
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
    const auto env = buildProxyEnv("", "", extra, 9999);

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
    TempDir d("roundtrip");
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
    TempDir d("retake");
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
    TempDir d("missing");
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
    TempDir d("parsefail");
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
    TempDir d("pollute");
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
    TempDir d("residual");
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
    TempDir d("noop");
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
    TempDir d2("noop2");
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
    TempDir d("overlay");
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

// ---- 8. 崩溃补还原 + 原子性（无 tmp 残留）----
REGISTER_TEST(sync_crash_recovery_and_atomic, "settings_sync: 崩溃补还原与原子写残留", {
    using namespace module::claude;
    TempDir d("crash");
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
    TestDb tdb("wiring");
    TempDir d("wiring");
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
        const int port = static_cast<int>(r.unsafeValue());
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
    TestDb tdb("warn");
    TempDir d("warn");
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
    TestDb tdb("tofail");
    TempDir d("tofail");
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
    TestDb tdb("exit");
    TempDir d("exit");
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

    // 模拟退出序列（main.cpp atexit → Router::stopAll）
    std::unique_ptr<::core::Module> mod(create_claude_module());
    ::core::Router router;
    router.registerModule(mod.get());
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
    TestDb tdb("delact");
    TempDir d("delact");
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
    TestDb tdb("off");
    TempDir d("off");
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
    TempDir d("revnoop");
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
    TempDir d("revbad");
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

REGISTER_TEST(sync_reverse_read_env_and_sidecar,
              "settings_sync: 反向读——env 原文与 sidecar 托管键（reverse）", {
    using module::claude::settings_sync::readSettingsEnvForSync;
    TempDir d("revok");
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

// ---- 15. 共享配置片段：合并优先级 + 接管（托管键集含片段键）+ 还原 ----
// 优先级（低→高）：profile 自身 extra_env → 共享片段 extra_env → 代理托管键；
// 片段在这批只做数据层/服务层，UI 留下批，故这里全走纯函数与 takeOver 直调。

REGISTER_TEST(sync_build_proxy_env_shared_config,
              "settings_sync: 共享片段覆盖 profile env，托管键仍最高", {
    using module::claude::buildProxyEnv;

    const std::string profileEnv =
        R"({"MY_KEY": "from-profile", "PROFILE_ONLY": "1"})";
    const std::string sharedEnv =
        R"({"MY_KEY": "from-shared", "SHARED_ONLY": "1"})";

    // 同键 → 片段赢；各自独有键都在
    const auto env = buildProxyEnv("", "", profileEnv, 8888, sharedEnv);
    DOCTEST_CHECK_EQ(env.at("MY_KEY"), std::string("from-shared"));
    DOCTEST_CHECK_EQ(env.at("PROFILE_ONLY"), std::string("1"));
    DOCTEST_CHECK_EQ(env.at("SHARED_ONLY"), std::string("1"));
    // 托管键赢两者（片段里塞 BASE_URL/token 也不生效）
    DOCTEST_CHECK_EQ(env.at("ANTHROPIC_BASE_URL"),
                     std::string("http://127.0.0.1:8888"));
    DOCTEST_CHECK_EQ(env.at("ANTHROPIC_AUTH_TOKEN"),
                     std::string("PROXY_MANAGED"));

    // 托管键（模型槽位）赢片段：default 槽位与片段同键 → 槽位值生效
    const auto slotWins = buildProxyEnv(
        R"({"default": {"model": "slot-model"}})", "", profileEnv, 8888,
        R"({"ANTHROPIC_MODEL": "shared-model"})");
    DOCTEST_CHECK_EQ(slotWins.at("ANTHROPIC_MODEL"), std::string("slot-model"));

    // 未绑定片段（空串）→ 与改动前逐字一致
    const auto noShared = buildProxyEnv("", "", profileEnv, 8888);
    DOCTEST_CHECK_EQ(noShared.at("MY_KEY"), std::string("from-profile"));
    DOCTEST_CHECK(noShared.find("SHARED_ONLY") == noShared.end());

    // 片段 JSON 非法 → 读路径容错整体忽略（不炸、不部分生效）
    const auto badShared = buildProxyEnv("", "", profileEnv, 8888, "{bad-json");
    DOCTEST_CHECK_EQ(badShared.at("MY_KEY"), std::string("from-profile"));
    return true;
})

REGISTER_TEST(sync_takeover_with_shared_config,
              "settings_sync: 接管写合并 env，片段键进托管集随还原撤走", {
    using namespace module::claude;
    TempDir d("shared");
    auto paths = makePaths(d);
    d.write(paths.settingsFile, R"({"env": {"MY_KEY": "user"}})");

    Profile p;
    p.model = "m-1";
    p.extraEnvJson = R"({"MY_KEY": "profile", "PROFILE_ONLY": "1"})";
    const std::string sharedEnv =
        R"({"MY_KEY": "shared", "SHARED_ONLY": "1"})";

    auto r = settings_sync::takeOver(p, 8888, paths, sharedEnv);
    DOCTEST_REQUIRE(r.ok);
    {
        const auto env = readEnv(paths.settingsFile);
        DOCTEST_CHECK_EQ(env.at("MY_KEY"), std::string("shared"));  // 片段赢
        DOCTEST_CHECK_EQ(env.at("SHARED_ONLY"), std::string("1"));
        DOCTEST_CHECK_EQ(env.at("PROFILE_ONLY"), std::string("1"));
        DOCTEST_CHECK_EQ(env.at("ANTHROPIC_MODEL"), std::string("m-1"));
        // settings.json 里不留片段痕迹（只有合并后的扁平 env，无出处标记）
        DOCTEST_CHECK(d.read(paths.settingsFile).find("shared_config") ==
                      std::string::npos);
    }
    // 片段键进 sidecar 托管键集：还原时随快照撤走（活文件不留片段键）
    {
        auto rd = readSettingsEnvForSync(paths);
        DOCTEST_REQUIRE(rd.ok);
        DOCTEST_CHECK(hasKeyEntry(rd.managedKeys, "SHARED_ONLY"));
        DOCTEST_CHECK(hasKeyEntry(rd.managedKeys, "MY_KEY"));
    }
    DOCTEST_REQUIRE(settings_sync::restore(paths).ok);
    {
        const auto env = readEnv(paths.settingsFile);
        DOCTEST_CHECK_EQ(env.at("MY_KEY"), std::string("user"));  // 快照原值恢复
        DOCTEST_CHECK(env.find("SHARED_ONLY") == env.end());   // 原本不存在 → 删
        DOCTEST_CHECK(env.find("PROFILE_ONLY") == env.end());
        DOCTEST_CHECK(env.find("ANTHROPIC_MODEL") == env.end());
    }

    // 解绑后重接管（同一路径、片段参数改空）：片段键从 live 撤走，同名键回落
    // profile 自身值（不写也不清从未删过 profile 行里的原值）
    DOCTEST_REQUIRE(settings_sync::takeOver(p, 8888, paths).ok);
    {
        const auto env = readEnv(paths.settingsFile);
        DOCTEST_CHECK_EQ(env.at("MY_KEY"), std::string("profile"));
        DOCTEST_CHECK(env.find("SHARED_ONLY") == env.end());
        DOCTEST_CHECK_EQ(env.at("PROFILE_ONLY"), std::string("1"));
    }
    DOCTEST_REQUIRE(settings_sync::restore(paths).ok);
    return true;
})

// ---- 16. 服务级 G10：片段键在 live 被改 → 切换时不写也不清 ----
// 场景刻意把 sidecar 拿掉（模拟与其它工具互抢 / 手工清理）：此时 G1 无托管键集
// 可依据，只有 G10 能挡住片段键被反向收编 —— 这正是「按绑定关系取键集」的证明。

REGISTER_TEST(sync_service_g10_frozen_key_not_captured,
              "claude_service: G10 绑定片段键不被反向同步收编", {
    using namespace module::claude;
    TestDb tdb("g10svc");
    TempDir d("g10svc");
    auto paths = makePaths(d);

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    svc.setSettingsSyncPaths(paths);
    svc.setSettingsTakeOverEnabled(true);

    auto frag = svc.createSharedConfig("片段S", R"({"MY_KEY": "from-shared"})");
    DOCTEST_REQUIRE(frag.isOk());
    auto a = svc.createProfile("绑片段", "https://a.example.com",
                               "sk-ind-test-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    Profile pa = a.value();
    pa.extraEnvJson = R"({"MY_KEY": "from-profile"})";  // 被片段覆盖，行里保留
    pa.sharedConfigId = frag.value().id;
    DOCTEST_REQUIRE(svc.updateProfile(pa).isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(pa.id).isOk());
    DOCTEST_REQUIRE(svc.startProxy("127.0.0.1", 0).isOk());
    DOCTEST_CHECK_EQ(readEnv(paths.settingsFile).at("MY_KEY"),
                     std::string("from-shared"));  // 片段值进 live

    // 拿掉 sidecar + 用户在 live 手工改片段键（其余字段的缺席清空是既有 G5 语义，
    // 与本守卫无关，这里只看片段键的处置）
    std::error_code ec;
    std::filesystem::remove(paths.sidecarFile, ec);
    d.write(paths.settingsFile, R"({"env": {"MY_KEY": "live-edited"}})");

    auto b = svc.createProfile("不绑", "https://b.example.com",
                               "sk-ind-test-fedcba9876543210", "");
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(b.value().id).isOk());

    // 不写：A 行的 MY_KEY 仍是 profile 原值（没被 live-edited 覆盖）
    auto ga = svc.getProfile(pa.id);
    DOCTEST_REQUIRE(ga.isOk());
    auto parsed = parseExtraEnv(ga.value().extraEnvJson);
    DOCTEST_REQUIRE(parsed.isOk());
    DOCTEST_CHECK_EQ(parsed.value().at("MY_KEY"), std::string("from-profile"));
    // 不清：冻结键仍在（缺席清空不适用于它）
    DOCTEST_CHECK_EQ(parsed.value().size(), 1u);

    svc.stopProxy();
    platform::keychain::deleteSecret(kTestKeychainService,
                                     "claude:" + std::to_string(pa.id));
    platform::keychain::deleteSecret(kTestKeychainService,
                                     "claude:" + std::to_string(b.value().id));
    return true;
})

// ---- 17. 服务级：删片段 → 同事务解绑 + 代理在跑则立即 re-takeover ----

REGISTER_TEST(sync_service_shared_config_delete_retakeover,
              "claude_service: 删片段当场解绑并重写 live（回落 profile 值）", {
    using namespace module::claude;
    TestDb tdb("fragdel");
    TempDir d("fragdel");
    auto paths = makePaths(d);

    ClaudeService svc(tdb.handle(), kTestKeychainService);
    svc.setSettingsSyncPaths(paths);
    svc.setSettingsTakeOverEnabled(true);

    auto frag = svc.createSharedConfig("片段S", R"({"MY_KEY": "from-shared"})");
    DOCTEST_REQUIRE(frag.isOk());
    auto a = svc.createProfile("绑片段", "https://a.example.com",
                               "sk-ind-test-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    Profile pa = a.value();
    pa.extraEnvJson = R"({"MY_KEY": "from-profile"})";
    pa.sharedConfigId = frag.value().id;
    DOCTEST_REQUIRE(svc.updateProfile(pa).isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(pa.id).isOk());
    DOCTEST_REQUIRE(svc.startProxy("127.0.0.1", 0).isOk());
    DOCTEST_CHECK_EQ(readEnv(paths.settingsFile).at("MY_KEY"),
                     std::string("from-shared"));

    // 删片段：同事务解绑 + 代理在跑 → 当场 re-takeover（片段键撤走、同名键回落
    // profile 自身值）；「改了不生效」不会被留到下次切换
    DOCTEST_REQUIRE(svc.deleteSharedConfig(frag.value().id).isOk());
    DOCTEST_CHECK_EQ(readEnv(paths.settingsFile).at("MY_KEY"),
                     std::string("from-profile"));
    auto ga = svc.getProfile(pa.id);
    DOCTEST_REQUIRE(ga.isOk());
    DOCTEST_CHECK_EQ(ga.value().sharedConfigId, 0);

    svc.stopProxy();
    platform::keychain::deleteSecret(kTestKeychainService,
                                     "claude:" + std::to_string(pa.id));
    return true;
})
