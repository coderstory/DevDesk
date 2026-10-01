// tests/test_modules/test_claude_tools.cpp
// ClaudeTools 用例（agent M2 Task 8）：claude.list_profiles / claude.switch_profile
//（**破坏性**）/ claude.test_connection / claude.recent_logs 四工具的注册、出参
// 形状、参数校验与错误回喂。
//
// 错误回喂约定（spec §10 M1 统一，与 MarkdownTools 同款）：参数非法 / 目标不存在
// → execute 返回 Result ok + ToolResult.isError=true + content 含中文说明（错误
// 回喂 LLM 自我纠正）；Result::fail 只留给编程不变量破坏。
//
// 注册表清理由 ToolEnv 析构做（RAII）：断言 fail-fast 早退也不会把工具残留在
// 进程级 ToolRegistry 里（test_markdown_tools.cpp 同款约定）。
#include "doctest.h"
#include "test_support.h"

#include "claude_service.h"
#include "claude_tools.h"
#include "core/tool.h"
#include "platform/http_server.h"
#include "platform/json.h"

#include <barrier>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

// Keychain service：测试专用命名空间，不污染生产数据（同 test_claude_service.cpp）
constexpr const char* kTestKeychainService = "DevDeskTestClaudeTools";
constexpr const char* kDbPrefix = "devdesk_test_claude_tools_";

// 从进程级注册表按名取工具并执行（走与 AgentLoop 相同的 find→execute 路径）
core::Result<core::ToolResult> runToolName(const std::string& name,
                                           const std::string& argsJson) {
    auto found = core::ToolRegistry::instance().find(name);
    if (found.isFail()) {
        return core::Result<core::ToolResult>::fail(found.errorCode(),
                                                    found.errorMessage());
    }
    return found.value()->execute(core::ToolArgs{argsJson});
}

// 一次性环境：临时已 migrate 库（TestDb）+ 真实 ClaudeService + 真实 ClaudeTools。
// 析构顺序：~ToolEnv 体先 unregisterAll（注册表清干净）+ 清 Keychain 条目 →
// tools_ 析构 → svc_ 析构 → tdb 析构（删库文件）。
struct ToolEnv {
    test_support::TestDb tdb;
    std::unique_ptr<module::claude::ClaudeService> svc;
    std::unique_ptr<module::claude::ClaudeTools> tools;
    std::vector<int64_t> createdIds;

    explicit ToolEnv(const std::string& tag) : tdb(kDbPrefix, tag) {
        svc = std::make_unique<module::claude::ClaudeService>(
            tdb.handle(), kTestKeychainService);
        tools = std::make_unique<module::claude::ClaudeTools>(*svc);
    }
    ~ToolEnv() {
        tools->unregisterAll();
        for (const int64_t id : createdIds) {
            test_support::cleanupProfileKey(kTestKeychainService, id);
        }
    }
    ToolEnv(const ToolEnv&) = delete;
    ToolEnv& operator=(const ToolEnv&) = delete;

    // 建 profile 并登记待清 Keychain 条目。失败即抛：夹具没建成 = 后续断言必然
    // 连锁失真，与 test_support 的「构造失败即抛」同口径。
    module::claude::Profile create(const std::string& name,
                                   const std::string& baseUrl,
                                   const std::string& key,
                                   bool activateIfNoneActive = false) {
        auto r = svc->createProfile(name, baseUrl, key, "claude-sonnet-4-5",
                                    activateIfNoneActive, /*sharedConfigId=*/0);
        if (r.isFail()) {
            throw std::runtime_error("createProfile failed: " + r.errorMessage());
        }
        createdIds.push_back(r.value().id);
        return r.value();
    }
};

} // namespace

// ============ 1. 注册 / list_profiles / switch_profile ============

REGISTER_TEST(claude_tools_register_and_list_and_switch,
              "claude_tools: 注册四工具；list_profiles 活跃标记；switch_profile 破坏性 + 生效", {
    ToolEnv env("reglist");
    auto p1 = env.create("A", "https://api.anthropic.com", "sk-test-a",
                         /*activateIfNoneActive=*/true);
    DOCTEST_CHECK(p1.isActive);
    env.tools->registerAll();
    for (const char* n : {"claude.list_profiles", "claude.switch_profile",
                          "claude.test_connection", "claude.recent_logs"}) {
        auto found = core::ToolRegistry::instance().find(n);
        DOCTEST_REQUIRE(found.isOk());
        // schemaJson 契约（照 test_markdown_tools.cpp:239-246 先例）：schema 要真能
        // 进 Anthropic tools 数组——非空 + 声明的 name 与注册名一致 + input_schema /
        // required 两键齐（缺任一项请求侧会被上游拒或 LLM 看不到入参）
        const std::string schema = found.value()->schemaJson();
        DOCTEST_CHECK_FALSE(schema.empty());
        DOCTEST_CHECK(schema.find(std::string("\"name\":\"") + n + "\"") !=
                      std::string::npos);
        DOCTEST_CHECK(schema.find("\"input_schema\"") != std::string::npos);
        DOCTEST_CHECK(schema.find("\"required\"") != std::string::npos);
    }
    auto listed = runToolName("claude.list_profiles", "{}");
    DOCTEST_REQUIRE(listed.isOk());
    DOCTEST_CHECK_FALSE(listed.value().isError);
    DOCTEST_CHECK(listed.value().contentJson.find("\"active\":true") !=
                  std::string::npos);
    // 明文密钥绝不进工具出参（同 service 层约定）
    DOCTEST_CHECK(listed.value().contentJson.find("sk-test-a") ==
                  std::string::npos);

    auto p2 = env.create("B", "https://api.anthropic.com", "sk-test-b",
                         /*activateIfNoneActive=*/false);
    DOCTEST_CHECK_FALSE(p2.isActive);

    auto sw = core::ToolRegistry::instance().find("claude.switch_profile");
    DOCTEST_REQUIRE(sw.isOk());
    DOCTEST_CHECK(sw.value()->isDestructive());   // spec §5.3 授权闸的判据
    auto switched = runToolName("claude.switch_profile",
                                R"({"id":)" + std::to_string(p2.id) + "}");
    DOCTEST_REQUIRE(switched.isOk());
    DOCTEST_CHECK_FALSE(switched.value().isError);

    auto active = env.svc->getActiveProfile();
    DOCTEST_REQUIRE(active.isOk());
    DOCTEST_CHECK_EQ(active.value().id, p2.id);      // 真的切了

    // 活跃标记跟随切换：p2 = true、p1 = false（按 id 逐条取值，不依赖列表顺序）
    auto listed2 = runToolName("claude.list_profiles", "{}");
    DOCTEST_REQUIRE(listed2.isOk());
    auto parsed2 = platform::json::parse(listed2.value().contentJson);
    DOCTEST_REQUIRE(parsed2.isOk());
    auto arr2 = parsed2.value().asObject().get("profiles");
    DOCTEST_REQUIRE(arr2.isArray());
    DOCTEST_CHECK_EQ(arr2.size(), 2);
    bool p1Inactive = false;
    bool p2Active = false;
    for (int i = 0; i < arr2.size(); ++i) {
        auto row = arr2[i].asObject();
        if (row.get("id").asInt() == p1.id) {
            p1Inactive = !row.get("active").asBool();
        }
        if (row.get("id").asInt() == p2.id) {
            p2Active = row.get("active").asBool();
        }
    }
    DOCTEST_CHECK(p1Inactive);
    DOCTEST_CHECK(p2Active);

    env.tools->unregisterAll();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("claude.list_profiles").isFail());
    // 注销幂等（onStop 可能被重复触发）
    env.tools->unregisterAll();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("claude.switch_profile").isFail());
    return true;
})

// ============ 2. switch_profile 参数校验 / 错误回喂 ============

REGISTER_TEST(claude_tools_switch_profile_error_paths,
              "claude_tools: switch_profile 非法/缺失/不存在 id → toolError 且激活态不变", {
    ToolEnv env("switchbad");
    auto only = env.create("Only", "https://api.anthropic.com", "sk-test-only",
                           /*activateIfNoneActive=*/true);
    env.tools->registerAll();

    // 参数不是合法 JSON 对象
    auto badJson = runToolName("claude.switch_profile", "not-json");
    DOCTEST_REQUIRE(badJson.isOk());
    DOCTEST_CHECK(badJson.value().isError);

    // id 缺失
    auto missing = runToolName("claude.switch_profile", "{}");
    DOCTEST_REQUIRE(missing.isOk());
    DOCTEST_CHECK(missing.value().isError);

    // id 是字符串 / 浮点 / 布尔：一律参数非法，**且文案指向参数本身**——浮点若
    // 只靠 asInt() 归零会退化报成「id 不能为 0」，LLM 拿到的纠正方向是错的
    for (const char* args : {R"({"id":"1"})", R"({"id":1.5})", R"({"id":true})"}) {
        auto r = runToolName("claude.switch_profile", args);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_CHECK(r.value().isError);
        DOCTEST_CHECK(r.value().contentJson.find("必须是整数") !=
                      std::string::npos);
    }

    // id = 0 → 服务层 ValidationError（"id 不能为 0"）原样回喂
    auto zero = runToolName("claude.switch_profile", R"({"id":0})");
    DOCTEST_REQUIRE(zero.isOk());
    DOCTEST_CHECK(zero.value().isError);
    DOCTEST_CHECK(zero.value().contentJson.find("id 不能为 0") != std::string::npos);

    // id 不存在 → ProfileNotFound 原消息回喂（带 id，便于 LLM 纠正）
    auto ghost = runToolName("claude.switch_profile", R"({"id":99999})");
    DOCTEST_REQUIRE(ghost.isOk());
    DOCTEST_CHECK(ghost.value().isError);
    DOCTEST_CHECK(ghost.value().contentJson.find("99999") != std::string::npos);

    // 全程激活态未变
    auto active = env.svc->getActiveProfile();
    DOCTEST_REQUIRE(active.isOk());
    DOCTEST_CHECK_EQ(active.value().id, only.id);
    return true;
})

// ============ 3. recent_logs：默认 20 / clamp [1,100] / 出参形状 ============

REGISTER_TEST(claude_tools_recent_logs_limit_clamp_and_shape,
              "claude_tools: recent_logs 默认 20、clamp [1,100]、字段形状与排序", {
    ToolEnv env("recentlogs");
    auto p = env.create("Logs", "https://api.anthropic.com", "sk-test-logs",
                        /*activateIfNoneActive=*/true);
    env.tools->registerAll();

    // 105 条：越过 clamp 上界（100），上界才**有得可测**（只 3 条时 999 与 100
    // 返回值相同，断言恒真）
    const std::time_t base = 1700000000;
    for (int i = 0; i < 105; ++i) {
        module::claude::ClaudeService::CallLogEntry e;
        e.profileId = p.id;
        e.method = "POST";
        e.path = "/v1/messages";
        e.statusCode = (i == 104) ? 429 : 200;
        e.latencyMs = i;
        e.error = (i == 104) ? std::string("rate limited") : std::string();
        e.startedAt = base + i;
        DOCTEST_REQUIRE(env.svc->logCall(e).isOk());
    }

    auto parseLogs = [](const core::ToolResult& r) {
        auto v = platform::json::parse(r.contentJson);
        if (v.isFail()) return platform::json::JsonValue();
        return v.value().asObject().get("logs");
    };

    // 默认 20 + 排序（started_at DESC → 最新在前）+ 字段形状
    auto def = runToolName("claude.recent_logs", "{}");
    DOCTEST_REQUIRE(def.isOk());
    DOCTEST_CHECK_FALSE(def.value().isError);
    auto defLogs = parseLogs(def.value());
    DOCTEST_REQUIRE(defLogs.isArray());
    DOCTEST_CHECK_EQ(defLogs.size(), 20);
    auto newest = defLogs[0].asObject();
    DOCTEST_CHECK(newest.get("id").asInt() > 0);
    DOCTEST_CHECK_EQ(newest.get("statusCode").asInt(), 429);
    DOCTEST_CHECK_EQ(newest.get("latencyMs").asInt(), 104);
    DOCTEST_CHECK_EQ(newest.get("startedAt").asInt(),
                     static_cast<int64_t>(base + 104));
    DOCTEST_CHECK_EQ(newest.get("error").asString(), std::string("rate limited"));

    // 下界 clamp：0 / 负数 → 1
    for (const char* args : {R"({"limit":0})", R"({"limit":-5})"}) {
        auto r = runToolName("claude.recent_logs", args);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_CHECK_FALSE(r.value().isError);
        auto logs = parseLogs(r.value());
        DOCTEST_REQUIRE(logs.isArray());
        DOCTEST_CHECK_EQ(logs.size(), 1);
    }

    // 上界 clamp：999 → 100（105 条在库）
    auto big = runToolName("claude.recent_logs", R"({"limit":999})");
    DOCTEST_REQUIRE(big.isOk());
    DOCTEST_CHECK_FALSE(big.value().isError);
    auto bigLogs = parseLogs(big.value());
    DOCTEST_REQUIRE(bigLogs.isArray());
    DOCTEST_CHECK_EQ(bigLogs.size(), 100);

    // 区间内取值原样生效
    auto exact = runToolName("claude.recent_logs", R"({"limit":7})");
    DOCTEST_REQUIRE(exact.isOk());
    DOCTEST_CHECK_EQ(parseLogs(exact.value()).size(), 7);

    // limit 非整数（字符串 / 浮点）→ 参数非法回喂，不是被 clamp 静默吞成 1 条
    for (const char* args : {R"({"limit":"10"})", R"({"limit":2.5})"}) {
        auto r = runToolName("claude.recent_logs", args);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_CHECK(r.value().isError);
    }
    return true;
})

// ============ 4. test_connection（本地 mock 上游，端口 0 由 OS 分配）============

REGISTER_TEST(claude_tools_test_connection_ok_and_error_paths,
              "claude_tools: test_connection 打 mock 上游 + 缺密钥/不存在 id → toolError", {
    platform::http::Server upstream("127.0.0.1", 0);
    int hitCount = 0;
    upstream.route("GET", "/v1/models",
                   [&](const platform::http::RequestCtx& ctx,
                       platform::http::ResponseWriter& w) {
                       (void)ctx;
                       ++hitCount;
                       w.setStatus(200);
                       w.setHeader("Content-Type", "application/json");
                       w.write(R"({"data":[{"id":"claude-sonnet-4-5"}]})");
                       w.end();
                   });
    DOCTEST_REQUIRE(upstream.start());
    const std::string baseUrl =
        "http://127.0.0.1:" + std::to_string(upstream.actualPort());

    ToolEnv env("testconn");
    auto p = env.create("Conn", baseUrl, "sk-test-conn",
                        /*activateIfNoneActive=*/true);
    env.tools->registerAll();
    // bind 后立刻可用，给一点余量（同 test_claude_service 同款 mock 用例）
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto parseObj = [](const core::ToolResult& r) {
        auto v = platform::json::parse(r.contentJson);
        if (v.isFail()) return platform::json::JsonValue();
        return v.value();
    };

    // 缺 id → 用激活 profile；200 → ok=true
    auto r = runToolName("claude.test_connection", "{}");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);
    auto v = parseObj(r.value());
    DOCTEST_REQUIRE(v.isObject());
    DOCTEST_CHECK_EQ(v.asObject().get("statusCode").asInt(), 200);
    DOCTEST_CHECK(v.asObject().get("ok").asBool());
    DOCTEST_CHECK_EQ(hitCount, 1);

    // 显式 id 等价
    auto r2 = runToolName("claude.test_connection",
                          R"({"id":)" + std::to_string(p.id) + "}");
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK_FALSE(r2.value().isError);
    DOCTEST_CHECK_EQ(parseObj(r2.value()).asObject().get("statusCode").asInt(), 200);
    DOCTEST_CHECK_EQ(hitCount, 2);

    // id 不存在 → toolError（不发请求）
    auto ghost = runToolName("claude.test_connection", R"({"id":99999})");
    DOCTEST_REQUIRE(ghost.isOk());
    DOCTEST_CHECK(ghost.value().isError);
    DOCTEST_CHECK(ghost.value().contentJson.find("99999") != std::string::npos);
    DOCTEST_CHECK_EQ(hitCount, 2);

    // id 非 int → toolError（不发请求）
    auto badId = runToolName("claude.test_connection", R"({"id":"x"})");
    DOCTEST_REQUIRE(badId.isOk());
    DOCTEST_CHECK(badId.value().isError);
    DOCTEST_CHECK_EQ(hitCount, 2);

    // Keychain 里没有密钥 → loadApiKey fail → toolError（计划书：任一步 fail 即错）
    test_support::cleanupProfileKey(kTestKeychainService, p.id);
    auto noKey = runToolName("claude.test_connection",
                             R"({"id":)" + std::to_string(p.id) + "}");
    DOCTEST_REQUIRE(noKey.isOk());
    DOCTEST_CHECK(noKey.value().isError);
    DOCTEST_CHECK_EQ(hitCount, 2);

    upstream.stop();
    return true;
})

// ============ 5. setActiveProfile 服务内锁（并发烟测）============

// Review Focus 5：agent 工作线程（claude.switch_profile 工具）与 UI 线程可同时进
// setActiveProfile（内含 DB 事务 + settings.json 接管文件写 + warnings_ 内存态）。
// 两条线程经 barrier 对齐后各打 25 次切换，断言：不崩不挂、终态「恰好一个激活」
// 且是两者之一。
//
// **变异验证（本仓无 TSan/ASan，用变异代替插桩）**：把 claude_service.cpp 里那行
// lock_guard 整行删掉重编后，本用例 5/5 全部 SIGSEGV（缺锁 = 两线程并发进同一
// SQLite 事务的真实数据竞争）；带锁版本连跑 20 次 = 20/20 绿。故本用例是「缺锁
// 会红」的真钉子，而非恒真断言。用例内无任何计时断言（本仓禁用时间敏感 flaky）。
REGISTER_TEST(claude_tools_switch_profile_concurrent_smoke,
              "claude_tools: 双线程并发 setActiveProfile 不崩不挂、终态唯一激活", {
    ToolEnv env("concurrent");
    auto a = env.create("A", "https://api.anthropic.com", "sk-test-ca",
                        /*activateIfNoneActive=*/true);
    auto b = env.create("B", "https://api.anthropic.com", "sk-test-cb",
                        /*activateIfNoneActive=*/false);

    constexpr int kIterations = 25;
    std::barrier<> startLine(2);
    // 每线程只写自己槽位、join 之后主线程才读（评审微修：原实现的 firstErr 由两
    // worker 无同步共写 = 用例自身的数据竞争——本用例正是在测锁，测具不该自带
    // race；ok/err 分槽后全部跨线程可见性都由 join 建立）
    int okPerSlot[2] = {0, 0};
    std::string errPerSlot[2];

    auto worker = [&](int slot, int64_t id) {
        startLine.arrive_and_wait();
        for (int i = 0; i < kIterations; ++i) {
            auto r = env.svc->setActiveProfile(id);
            if (r.isOk()) {
                okPerSlot[slot] += 1;
            } else if (errPerSlot[slot].empty()) {
                errPerSlot[slot] = r.errorMessage();
            }
        }
    };
    std::thread t1(worker, 0, a.id);
    std::thread t2(worker, 1, b.id);
    t1.join();
    t2.join();

    DOCTEST_CHECK_MESSAGE(okPerSlot[0] == kIterations, errPerSlot[0]);
    DOCTEST_CHECK_MESSAGE(okPerSlot[1] == kIterations, errPerSlot[1]);

    // 终态自洽：激活行恰好一条，且是 A / B 之一
    auto list = env.svc->listProfiles();
    DOCTEST_REQUIRE(list.isOk());
    int activeRows = 0;
    for (const auto& row : list.value()) {
        if (row.isActive) {
            ++activeRows;
            DOCTEST_CHECK(row.id == a.id || row.id == b.id);
        }
    }
    DOCTEST_CHECK_EQ(activeRows, 1);
    auto active = env.svc->getActiveProfile();
    DOCTEST_REQUIRE(active.isOk());
    DOCTEST_CHECK(active.value().id == a.id || active.value().id == b.id);
    return true;
})
