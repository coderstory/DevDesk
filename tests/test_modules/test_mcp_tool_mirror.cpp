// need#381 ITool→IMcpTool 适配层单测（doc50 §4 落点 1-7、9；落点 8 = F-01 39 真工具
// diff 与「43 工具」断言归 MCP e2e probe mcp_e2e_probe.cpp，单测只验适配机制本身）。
// debt#416 合一后对外名 = tool_kit::mcpToolName(源名)（点号/短横→下划线）：源侧仍用
// 点号假名，target 侧一律按下划线名查，映射本身即用例覆盖点。
// target = core::mcp::globalMcpTools() 进程级单例：MirrorFixture 每用例收尾 unregister
// 本用例注册的全部名字，防跨用例污染（doc50 §4 落点 9）。
// 注意 shim 语义：CHECK 失败即 return，收尾清理只能靠 RAII，不写尾部清理语句。
#include "doctest.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/mcp_tool.h"
#include "core/tool.h"
#include "module_mcp/mcp_protocol.h"
#include "module_mcp/mcp_session_store.h"
#include "module_mcp/mcp_tool_mirror.h"
#include "platform/json.h"
#include "platform/tool_kit.h"

namespace {

constexpr const char* kGoodSchema =
    R"({"name":"mirror.fake","description":"fake desc",)"
    R"("input_schema":{"type":"object","properties":{}}})";

// 并发安全 FakeITool：execute 无状态（resultText_ 构造后只读、calls_ 原子），可共享
class FakeITool : public core::ITool {
public:
    FakeITool(std::string name, std::string schema, bool destructive = false)
        : name_(std::move(name)), schema_(std::move(schema)),
          destructive_(destructive) {}

    // 仅单线程用例调用（并发用例不得改）
    void setResultText(std::string text) { resultText_ = std::move(text); }

    std::string name() const override { return name_; }
    std::string schemaJson() const override { return schema_; }
    bool isDestructive() const override { return destructive_; }
    core::Result<core::ToolResult> execute(const core::ToolArgs&) override {
        ++calls_;
        return core::Result<core::ToolResult>::ok(
            core::ToolResult{false, resultText_});
    }

    int calls() const { return calls_.load(); }

private:
    std::string name_;
    std::string schema_;
    bool destructive_;
    std::string resultText_ = "fake-ok";
    std::atomic<int> calls_{0};
};

// F-03 捕参（仅单线程用例）：记录收到的 argumentsJson 原文
class RecordingFakeITool : public FakeITool {
public:
    using FakeITool::FakeITool;
    core::Result<core::ToolResult> execute(const core::ToolArgs& args) override {
        lastArgs = args.json;
        return FakeITool::execute(args);
    }
    std::string lastArgs;
};

// 直接注册进 MCP 端的原生工具（不经镜像）：F-05 内置无 annotations / F-07 重名占位
class StubMcpTool : public core::mcp::IMcpTool {
public:
    explicit StubMcpTool(std::string name) : name_(std::move(name)) {}
    std::string name() const override { return name_; }
    std::string description() const override { return "stub builtin"; }
    std::string inputSchemaJson() const override {
        return R"({"type":"object","properties":{}})";
    }
    core::Result<core::mcp::ToolResult> execute(const std::string&) override {
        return core::Result<core::mcp::ToolResult>::ok(
            core::mcp::ToolResult{false, "stub"});
    }

private:
    std::string name_;
};

// teardown 约定（doc50 §4 落点 9）：登记本用例注册过的名字，析构统一注销——
// 源名单逐名同时清 target 侧（sync 间接写入的镜像条目，防幽灵残留），目标
// 名单清直接注册项；两侧 unregister 均幂等
struct MirrorFixture {
    std::vector<std::string> sourceNames;
    std::vector<std::string> targetNames;

    core::Result<void> regSource(std::shared_ptr<core::ITool> tool) {
        std::string name = tool->name();
        auto r = core::ToolRegistry::instance().registerTool(std::move(tool));
        if (r.isOk()) sourceNames.push_back(std::move(name));
        return r;
    }
    core::Result<void> regTarget(std::shared_ptr<core::mcp::IMcpTool> tool) {
        std::string name = tool->name();
        auto r = core::mcp::globalMcpTools().registerTool(std::move(tool));
        if (r.isOk()) targetNames.push_back(std::move(name));
        return r;
    }
    ~MirrorFixture() {
        for (const auto& n : sourceNames) {
            core::ToolRegistry::instance().unregisterTool(n);
            // sync 镜像条目按对外名（下划线）落表，两侧名字不同名时都要清
            core::mcp::globalMcpTools().unregisterTool(n);  // 幂等
            core::mcp::globalMcpTools().unregisterTool(
                platform::tool_kit::mcpToolName(n));
        }
        for (const auto& n : targetNames) {
            core::mcp::globalMcpTools().unregisterTool(n);
        }
    }
};

platform::json::JsonValue parseBody(const std::string& body) {
    auto parsed = platform::json::parse(body);
    if (parsed.isFail()) return platform::json::JsonValue();
    return parsed.value();
}

// tools/list 响应（root.result.tools 数组）按 name 找 entry；缺 → 空 JsonValue
platform::json::JsonValue toolEntryByName(const platform::json::JsonValue& root,
                                          const std::string& name) {
    if (!root.isObject()) return platform::json::JsonValue();
    const auto result = root.asObject().get("result");
    if (!result.isObject()) return platform::json::JsonValue();
    const auto tools = result.asObject().get("tools");
    if (!tools.isArray()) return platform::json::JsonValue();
    for (int i = 0; i < tools.size(); ++i) {
        const auto entry = tools[i];
        if (entry.isObject() && entry.asObject().has("name") &&
            entry.asObject().get("name").isString() &&
            entry.asObject().get("name").asString() == name) {
            return entry;
        }
    }
    return platform::json::JsonValue();
}

} // namespace

// ---- 落点 1：镜像往返（F-02） ----
REGISTER_TEST(mcp_tool_mirror_roundtrip,
              "ToolMirror: 镜像往返 register→sync→unregister→sync→重注册恢复", {
    MirrorFixture fixture;
    auto a = std::make_shared<FakeITool>("mirror.rt.a", kGoodSchema);
    auto b = std::make_shared<FakeITool>("mirror.rt.b", kGoodSchema);
    auto c = std::make_shared<FakeITool>("mirror.rt.c", kGoodSchema);
    DOCTEST_CHECK(fixture.regSource(a).isOk());
    DOCTEST_CHECK(fixture.regSource(b).isOk());
    DOCTEST_CHECK(fixture.regSource(c).isOk());

    module::mcp::ToolMirror mirror(core::mcp::globalMcpTools());
    mirror.sync();
    // 对外名 = 源名点号转下划线（debt#416 唯一命名规则）
    DOCTEST_CHECK(core::mcp::globalMcpTools().find("mirror_rt_a").isOk());
    DOCTEST_CHECK(core::mcp::globalMcpTools().find("mirror_rt_b").isOk());
    DOCTEST_CHECK(core::mcp::globalMcpTools().find("mirror_rt_c").isOk());
    DOCTEST_CHECK(core::mcp::globalMcpTools().find("mirror.rt.a").isFail());

    // 镜像产物确为 AgentToolAdapter（name() 已下划线化），且经镜像 execute 可用
    auto found = core::mcp::globalMcpTools().find("mirror_rt_b");
    DOCTEST_REQUIRE(found.isOk());
    DOCTEST_CHECK_EQ(found.value()->name(), std::string("mirror_rt_b"));
    DOCTEST_CHECK(dynamic_cast<const module::mcp::AgentToolAdapter*>(
                      found.value().get()) != nullptr);
    auto executed = found.value()->execute("{}");
    DOCTEST_REQUIRE(executed.isOk());
    DOCTEST_CHECK_EQ(executed.value().text, std::string("fake-ok"));

    // 源注销 → sync → 镜像消失，其余保持
    core::ToolRegistry::instance().unregisterTool("mirror.rt.a");
    mirror.sync();
    DOCTEST_CHECK(core::mcp::globalMcpTools().find("mirror_rt_a").isFail());
    DOCTEST_CHECK(core::mcp::globalMcpTools().find("mirror_rt_b").isOk());
    DOCTEST_CHECK(core::mcp::globalMcpTools().find("mirror_rt_c").isOk());

    // 源重注册 → sync → 恢复
    DOCTEST_CHECK(fixture.regSource(a).isOk());
    mirror.sync();
    DOCTEST_CHECK(core::mcp::globalMcpTools().find("mirror_rt_a").isOk());
    return true;
});

// ---- 落点 2：并发 sync + execute（≥3 线程 ×100 轮） ----
REGISTER_TEST(mcp_tool_mirror_concurrency,
              "ToolMirror: 并发 sync+execute 3 线程×100 轮无死锁结果一致", {
    MirrorFixture fixture;
    auto tool = std::make_shared<FakeITool>("mirror.concurrent", kGoodSchema);
    DOCTEST_CHECK(fixture.regSource(tool).isOk());
    module::mcp::ToolMirror mirror(core::mcp::globalMcpTools());
    mirror.sync();  // 先注册齐，worker 里 find 恒成功

    std::atomic<int> failures{0};
    constexpr int kRounds = 100;
    auto worker = [&]() {
        for (int i = 0; i < kRounds; ++i) {
            mirror.sync();
            auto found = core::mcp::globalMcpTools().find("mirror_concurrent");
            if (found.isFail()) {
                ++failures;
                continue;
            }
            auto r = found.value()->execute("{}");
            if (r.isFail() || r.value().text != "fake-ok") ++failures;
        }
    };
    std::thread t1(worker);
    std::thread t2(worker);
    std::thread t3(worker);
    t1.join();
    t2.join();
    t3.join();
    DOCTEST_CHECK_EQ(failures.load(), 0);
    DOCTEST_CHECK(core::mcp::globalMcpTools().find("mirror_concurrent").isOk());
    return true;
});

// ---- 落点 3：源 register/unregister 与 sync 竞态交错 ----
REGISTER_TEST(mcp_tool_mirror_race,
              "ToolMirror: 源注册注销与 sync 竞态交错 100 轮无悬垂无崩溃", {
    MirrorFixture fixture;
    auto tool = std::make_shared<FakeITool>("mirror.race", kGoodSchema);
    DOCTEST_CHECK(fixture.regSource(tool).isOk());
    module::mcp::ToolMirror mirror(core::mcp::globalMcpTools());

    std::atomic<int> failures{0};
    std::atomic<bool> done{false};
    std::thread writer([&]() {
        for (int i = 0; i < 100; ++i) {
            core::ToolRegistry::instance().unregisterTool("mirror.race");
            mirror.sync();
            if (core::ToolRegistry::instance().registerTool(tool).isFail()) {
                ++failures;
            }
            mirror.sync();
        }
        done.store(true);
    });
    while (!done.load()) {
        mirror.sync();
        auto found = core::mcp::globalMcpTools().find("mirror_race");
        if (found.isOk()) {
            // keep-alive：即便此刻源已注销，adapter 持源引用不悬垂
            auto r = found.value()->execute("{}");
            if (r.isFail()) ++failures;
        }
    }
    writer.join();
    mirror.sync();
    DOCTEST_CHECK_EQ(failures.load(), 0);
    DOCTEST_CHECK(core::ToolRegistry::instance().find("mirror.race").isOk());
    DOCTEST_CHECK(core::mcp::globalMcpTools().find("mirror_race").isOk());
    return true;
});

// ---- 落点 4：F-04③ 映射五形态（string/对象/数组/非法 JSON/超长截断） ----
REGISTER_TEST(mcp_tool_mirror_map_result_text,
              "ToolMirror: mapResultText 五形态 string/对象/数组/非法 JSON/超长截断", {
    auto tool = std::make_shared<FakeITool>("mirror.map", kGoodSchema);
    module::mcp::AgentToolAdapter adapter(tool);

    // ① JSON string → 取值
    tool->setResultText("\"hello\"");
    auto r1 = adapter.execute("{}");
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_CHECK_EQ(r1.value().text, std::string("hello"));

    // ② 对象 / ③ 数组 → stringify（与 parse→stringify 自洽比对，不硬编码键序）
    for (const char* json : {R"({"a":1,"b":"值"})", R"([1,"x",2])"}) {
        auto parsed = platform::json::parse(json);
        DOCTEST_REQUIRE(parsed.isOk());
        auto expected = platform::json::stringify(parsed.value());
        DOCTEST_REQUIRE(expected.isOk());
        tool->setResultText(json);
        auto r = adapter.execute("{}");
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_CHECK_EQ(r.value().text, expected.value());
    }

    // ④ 非法 JSON → 原文
    tool->setResultText("not-json{{{");
    auto r4 = adapter.execute("{}");
    DOCTEST_REQUIRE(r4.isOk());
    DOCTEST_CHECK_EQ(r4.value().text, std::string("not-json{{{"));

    // ⑤ 超长截断：32KB 字节上限 + \n[truncated] 标记（切点 ASCII，无多字节回退）
    const std::string kMarker = "\n[truncated]";
    tool->setResultText("\"" + std::string(40000, 'x') + "\"");
    auto r5 = adapter.execute("{}");
    DOCTEST_REQUIRE(r5.isOk());
    DOCTEST_CHECK_EQ(r5.value().text.size(),
                     std::size_t(32 * 1024) + kMarker.size());
    DOCTEST_CHECK(r5.value().text.compare(
                      r5.value().text.size() - kMarker.size(), kMarker.size(),
                      kMarker) == 0);
    return true;
});

// ---- 落点 5：F-03 捕参逐字节一致（无参/带参） ----
REGISTER_TEST(mcp_tool_mirror_capture_args,
              "ToolMirror: RecordingFakeITool 捕参与透传原文逐字节一致", {
    auto tool = std::make_shared<RecordingFakeITool>("mirror.capture",
                                                     kGoodSchema);
    module::mcp::AgentToolAdapter adapter(tool);
    const std::string payloads[] = {
        "{}",
        "",
        R"({"k":"值\"引号","n":[1,2,{"deep":true}]})",
    };
    for (const auto& payload : payloads) {
        auto r = adapter.execute(payload);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_CHECK_EQ(tool->lastArgs, payload);
    }
    DOCTEST_CHECK_EQ(tool->calls(), 3);
    return true;
});

// ---- 落点 6：F-05 annotations 分支（协议组装行） ----
REGISTER_TEST(mcp_tool_mirror_annotations_branch,
              "ToolMirror: annotations 分支 destructive 带 hint/plain 与内置无键", {
    MirrorFixture fixture;
    DOCTEST_CHECK(
        fixture.regSource(
            std::make_shared<FakeITool>("mirror.dest", kGoodSchema, true))
            .isOk());
    DOCTEST_CHECK(
        fixture.regSource(
            std::make_shared<FakeITool>("mirror.plain", kGoodSchema, false))
            .isOk());
    DOCTEST_CHECK(
        fixture.regTarget(std::make_shared<StubMcpTool>("mirror_stub_builtin"))
            .isOk());

    module::mcp::ToolMirror mirror(core::mcp::globalMcpTools());
    mirror.sync();

    module::mcp::McpSessionStore sessions;
    module::mcp::McpProtocol protocol(sessions, core::mcp::globalMcpTools(),
                                      "mirror-test");
    std::string sessionId;
    auto init = protocol.handlePost(
        R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})", "", "",
        sessionId);
    DOCTEST_REQUIRE_EQ(init.status, 200);
    std::string listNewSession;
    auto list = protocol.handlePost(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", sessionId, "",
        listNewSession);
    DOCTEST_REQUIRE_EQ(list.status, 200);
    const auto root = parseBody(list.body);
    DOCTEST_REQUIRE(root.isObject());

    // 破坏性 → annotations.destructiveHint=true + readOnlyHint=false
    const auto dest = toolEntryByName(root, "mirror_dest");
    DOCTEST_REQUIRE(dest.isObject());
    DOCTEST_CHECK(dest.asObject().has("annotations"));
    const auto annotations = dest.asObject().get("annotations");
    DOCTEST_REQUIRE(annotations.isObject());
    DOCTEST_CHECK(annotations.asObject().get("destructiveHint").isBool() &&
                  annotations.asObject().get("destructiveHint").asBool());
    DOCTEST_CHECK(annotations.asObject().get("readOnlyHint").isBool() &&
                  !annotations.asObject().get("readOnlyHint").asBool());

    // 非破坏镜像 / 原生内置 → 无 annotations 键（spec 可选省略）
    const auto plain = toolEntryByName(root, "mirror_plain");
    DOCTEST_REQUIRE(plain.isObject());
    DOCTEST_CHECK_FALSE(plain.asObject().has("annotations"));
    const auto stub = toolEntryByName(root, "mirror_stub_builtin");
    DOCTEST_REQUIRE(stub.isObject());
    DOCTEST_CHECK_FALSE(stub.asObject().has("annotations"));
    return true;
});

// ---- 落点 7：F-06 坏 schema 注入（不进镜像、不拖垮其余） ----
REGISTER_TEST(mcp_tool_mirror_bad_schema_skip,
              "ToolMirror: 坏 schema 注入跳过（坏 JSON/非对象/缺 input_schema）", {
    MirrorFixture fixture;
    DOCTEST_CHECK(
        fixture.regSource(std::make_shared<FakeITool>("mirror.bad1", "{not json"))
            .isOk());
    DOCTEST_CHECK(
        fixture.regSource(std::make_shared<FakeITool>("mirror.bad2", "[]"))
            .isOk());
    DOCTEST_CHECK(
        fixture.regSource(std::make_shared<FakeITool>(
                              "mirror.bad3",
                              R"({"name":"mirror.bad3","description":"缺 input_schema"})"))
            .isOk());
    DOCTEST_CHECK(
        fixture.regSource(std::make_shared<FakeITool>("mirror.good", kGoodSchema))
            .isOk());

    module::mcp::ToolMirror mirror(core::mcp::globalMcpTools());
    mirror.sync();
    DOCTEST_CHECK(core::mcp::globalMcpTools().find("mirror_bad1").isFail());
    DOCTEST_CHECK(core::mcp::globalMcpTools().find("mirror_bad2").isFail());
    DOCTEST_CHECK(core::mcp::globalMcpTools().find("mirror_bad3").isFail());
    DOCTEST_CHECK(core::mcp::globalMcpTools().find("mirror_good").isOk());
    return true;
});

// ---- F-07：重名拒注不重试，原名语义保持（占位名 = 源名下划线后的对外名） ----
REGISTER_TEST(mcp_tool_mirror_name_clash_no_retry,
              "ToolMirror: 重名拒注不重试，原名工具语义保持", {
    MirrorFixture fixture;
    DOCTEST_CHECK(
        fixture.regTarget(std::make_shared<StubMcpTool>("mirror_clash"))
            .isOk());
    DOCTEST_CHECK(
        fixture.regSource(std::make_shared<FakeITool>("mirror.clash", kGoodSchema))
            .isOk());

    module::mcp::ToolMirror mirror(core::mcp::globalMcpTools());
    mirror.sync();
    mirror.sync();  // 第二轮不得重复注册（名字已记入 mirrored_）

    auto found = core::mcp::globalMcpTools().find("mirror_clash");
    DOCTEST_REQUIRE(found.isOk());
    // 原生 stub 保持原位（适配器被拒注丢弃）
    DOCTEST_CHECK(dynamic_cast<const module::mcp::AgentToolAdapter*>(
                      found.value().get()) == nullptr);
    auto r = found.value()->execute("{}");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value().text, std::string("stub"));
    return true;
});
