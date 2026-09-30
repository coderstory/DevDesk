// tests/test_modules/test_agent_loop.cpp
// AgentLoop 主循环用例（agent M1 Task 4）：tool_use → 工具执行 → tool_result
// 回喂 → end_turn，断言消息累积（第二轮 messages 3 条）+ 三表落库（4 条消息）
// + run 状态/迭代数/token 累计。LlmEndpoint 抽象使本用例零网络。
#include "doctest.h"
#include "test_support.h"
#include "module_agent/agent_loop.h"
#include "module_agent/agent_store.h"
#include "core/llm_endpoint.h"
#include "core/tool.h"
#include <atomic>

namespace {
using namespace module::agent;

// 脚本化 mock：按序吐响应，记录每次收到的 messages/tools/model
class MockLlm : public core::LlmEndpoint {
public:
    std::vector<core::LlmResponse> script;
    std::vector<std::vector<core::LlmMessage>> sentMessages;
    std::vector<std::string> sentModels;   // 每次 send 收到的 model（原样，不解析）
    core::Result<core::LlmResponse> send(
        const std::vector<core::LlmMessage>& messages,
        const std::string& toolsJson, const std::string& systemPrompt,
        const std::string& model) override {
        (void)toolsJson; (void)systemPrompt;
        sentMessages.push_back(messages);
        sentModels.push_back(model);
        if (cursor >= script.size())
            return core::Result<core::LlmResponse>::fail(core::ErrorCode::Internal, "script 耗尽");
        return core::Result<core::LlmResponse>::ok(script[cursor++]);
    }
    size_t cursor = 0;
};

class FakeTool : public core::ITool {
public:
    FakeTool(std::string n, bool destructive = false) : n_(std::move(n)), destructive_(destructive) {}
    std::string name() const override { return n_; }
    std::string schemaJson() const override { return R"({"name":")" + n_ + R"("})"; }
    bool isDestructive() const override { return destructive_; }
    core::Result<core::ToolResult> execute(const core::ToolArgs& a) override {
        lastArgs = a.json; ++calls;
        return core::Result<core::ToolResult>::ok(core::ToolResult{false, R"("done")"});
    }
    std::string lastArgs; int calls = 0;
private:
    std::string n_; bool destructive_;
};

core::LlmResponse resp(const char* contentJson, const char* stop) {
    core::LlmResponse r;
    r.stopReason = stop;
    r.contentJson = contentJson;
    r.inputTokens = 10; r.outputTokens = 5;
    return r;
}

struct Fixture {
    test_support::TestDb tdb{test_support::TestDb("agentloop", "fx")};
    // 注意：TestDb 构造失败即抛（TIN-06b 口径），用例内直接用
    AgentStore store{tdb.handle()};
    MockLlm llm;
    core::ToolRegistry& tools = core::ToolRegistry::instance();
    AgentLoop loop{llm, tools, store};
};
} // namespace

REGISTER_TEST(agent_loop_happy_path_tool_then_end, "agent_loop: tool_use→工具执行→回喂→end_turn，消息累积与落库", {
    Fixture fx;
    FakeTool tool("t.hello");
    DOCTEST_REQUIRE(fx.tools.registerTool(std::shared_ptr<core::ITool>(&tool, [](core::ITool*){})).isOk());
    // 注册表借用栈对象：deleter 空、结尾注销，勿用 make_shared
    auto taskId = fx.store.createTask("t", "打招呼", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    AgentTask task; task.id = taskId.value(); task.goal = "打招呼"; task.modelSlot = "default";

    fx.llm.script = {
        resp(R"([{"type":"text","text":"调工具"},{"type":"tool_use","id":"tu1","name":"t.hello","input":{"x":1}}])", "tool_use"),
        resp(R"([{"type":"text","text":"完成"}])", "end_turn"),
    };
    std::atomic<bool> cancel{false};
    auto runId = fx.loop.run(task, cancel);
    DOCTEST_REQUIRE(runId.isOk());
    DOCTEST_CHECK_EQ(tool.calls, 1);
    DOCTEST_CHECK_EQ(tool.lastArgs, std::string(R"({"x":1})"));
    // 累积：首条 user goal → assistant1 → user(tool_result) → assistant2
    DOCTEST_REQUIRE(fx.llm.sentMessages.size() == 2);
    DOCTEST_CHECK_EQ(fx.llm.sentMessages[0].size(), 1u);           // goal
    DOCTEST_CHECK_EQ(fx.llm.sentMessages[1].size(), 3u);           // goal+asst1+tool_result
    DOCTEST_CHECK(fx.llm.sentMessages[1][2].contentJson.find("tool_result") != std::string::npos);
    DOCTEST_CHECK(fx.llm.sentMessages[1][2].contentJson.find("tu1") != std::string::npos);
    // 落库：4 条消息
    auto msgs = fx.store.listMessages(runId.value());
    DOCTEST_REQUIRE(msgs.isOk());
    DOCTEST_REQUIRE_EQ(msgs.value().size(), 4u);
    // 落库的 seq/role 顺序 = 累积顺序（审计契约，spec §6 agent_messages.seq）
    DOCTEST_CHECK_EQ(msgs.value()[0].seq, 0);
    DOCTEST_CHECK_EQ(msgs.value()[0].role, std::string("user"));
    DOCTEST_CHECK(msgs.value()[0].contentJson.find("打招呼") != std::string::npos);
    DOCTEST_CHECK(!msgs.value()[0].toolCallId.has_value());
    DOCTEST_CHECK_EQ(msgs.value()[1].seq, 1);
    DOCTEST_CHECK_EQ(msgs.value()[1].role, std::string("assistant"));
    DOCTEST_CHECK_EQ(msgs.value()[2].seq, 2);
    DOCTEST_CHECK_EQ(msgs.value()[2].role, std::string("user"));
    DOCTEST_REQUIRE(msgs.value()[2].toolCallId.has_value());
    DOCTEST_CHECK_EQ(msgs.value()[2].toolCallId.value(), std::string("tu1"));
    DOCTEST_CHECK_EQ(msgs.value()[3].seq, 3);
    DOCTEST_CHECK_EQ(msgs.value()[3].role, std::string("assistant"));
    auto runs = fx.store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("completed"));
    DOCTEST_CHECK_EQ(runs.value()[0].iterations, 2);
    DOCTEST_CHECK_EQ(runs.value()[0].inputTokens.value_or(0), 20);  // 10+10 累计
    DOCTEST_CHECK_EQ(runs.value()[0].outputTokens.value_or(0), 10);
    // model 原样传 task.modelSlot（槽位→model id 由 LlmClient 解析，loop 不解析）
    DOCTEST_REQUIRE_EQ(fx.llm.sentModels.size(), 2u);
    for (const auto& m : fx.llm.sentModels) {
        DOCTEST_CHECK_EQ(m, std::string(task.modelSlot));
    }
    fx.tools.unregisterTool("t.hello");
    return true;
})

// I1：completed 的唯一定义是 end_turn。refusal（Anthropic 拒答）等非 end_turn 的
// stop_reason 不能默认记成功——否则「模型没给出终局回复」的 run 在 UI 上看着
// 跟正常跑完一样。
REGISTER_TEST(agent_loop_refusal_is_failed, "agent_loop: stop_reason=refusal → run failed（不记 completed）", {
    Fixture fx;
    auto taskId = fx.store.createTask("t", "打招呼", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    AgentTask task; task.id = taskId.value(); task.goal = "打招呼"; task.modelSlot = "default";

    fx.llm.script = { resp(R"([{"type":"text","text":"我不做这个"}])", "refusal") };
    std::atomic<bool> cancel{false};
    auto runId = fx.loop.run(task, cancel);
    DOCTEST_REQUIRE(runId.isOk());
    auto runs = fx.store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_REQUIRE_EQ(runs.value().size(), 1u);
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("failed"));
    DOCTEST_REQUIRE(runs.value()[0].error.has_value());
    DOCTEST_CHECK(runs.value()[0].error->find("stop_reason=refusal") != std::string::npos);
    return true;
})
