// tests/test_modules/test_agent_loop.cpp
// AgentLoop 主循环用例（agent M1 Task 4/5）：tool_use → 工具执行 → tool_result
// 回喂 → end_turn，断言消息累积（第二轮 messages 3 条）+ 三表落库（4 条消息）
// + run 状态/迭代数/token 累计；Task 5 追加状态机各分支（迭代上限 /
// stop_reason 受限 / LLM 失败 / 破坏性授权 0 与 1 / 工具失败回喂 / 32KB
// 截断 / cancel）+ tool_result 文本形状与 goal 转义两个契约钉子。
// LlmEndpoint 抽象使本用例零网络。
#include "doctest.h"
#include "test_support.h"
#include "module_agent/agent_loop.h"
#include "module_agent/agent_store.h"
#include "core/llm_endpoint.h"
#include "core/tool.h"
#include "platform/json.h"
#include <atomic>
#include <functional>

namespace {
using namespace module::agent;

// 脚本化 mock：按序吐响应，记录每次收到的 messages/tools/model
class MockLlm : public core::LlmEndpoint {
public:
    std::vector<core::LlmResponse> script;
    std::vector<std::vector<core::LlmMessage>> sentMessages;
    std::vector<std::string> sentModels;   // 每次 send 收到的 model（原样，不解析）
    // 分支 3：置上则 send 直接返回该失败（模拟网络超时），不消耗 script
    std::optional<core::Result<core::LlmResponse>> failResponse;
    // 分支 7：每次 send 入口触发（用来在首轮把 cancel 置位）
    std::function<void()> onSendHook;
    core::Result<core::LlmResponse> send(
        const std::vector<core::LlmMessage>& messages,
        const std::string& toolsJson, const std::string& systemPrompt,
        const std::string& model) override {
        (void)toolsJson; (void)systemPrompt;
        sentMessages.push_back(messages);
        sentModels.push_back(model);
        if (onSendHook) onSendHook();
        if (failResponse.has_value()) return *failResponse;
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
        // 分支 5：failMessage 非空 → 返回 Result::fail（工具异常语义）
        if (!failMessage.empty())
            return core::Result<core::ToolResult>::fail(failCode, failMessage);
        return core::Result<core::ToolResult>::ok(core::ToolResult{isError, resultJson});
    }
    std::string lastArgs; int calls = 0;
    // 可注入的返回体（默认 JSON 字符串字面量 "done"）
    std::string resultJson = R"("done")";
    bool isError = false;
    std::string failMessage;
    core::ErrorCode failCode = core::ErrorCode::ValidationError;
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

// 单 tool_use block 的响应（分支 1/4/5/6/7 共用）
core::LlmResponse toolUseResp(const char* id, const char* toolName) {
    return resp((std::string(R"([{"type":"tool_use","id":")") + id + R"(","name":")" +
                 toolName + R"(","input":{}}])").c_str(),
                "tool_use");
}

// 从一条 user 消息的 contentJson 里取第 idx 个 tool_result 的 text 字段
// （解析失败一律返回空串，断言侧再判——本 helper 只做取值不做断言）
std::string toolResultText(const std::string& contentJson, int idx) {
    auto arr = platform::json::parse(contentJson);
    if (arr.isFail() || !arr.value().isArray() || idx >= arr.value().size())
        return std::string();
    auto block = arr.value()[idx];
    if (!block.isObject()) return std::string();
    auto content = block.asObject().get("content");
    if (!content.isArray() || content.size() < 1) return std::string();
    auto textBlock = content[0];
    if (!textBlock.isObject()) return std::string();
    auto t = textBlock.asObject().get("text");
    return t.isString() ? t.asString() : std::string();
}

// 第 idx 个 tool_result 的 is_error 标志（缺块/非布尔 → false）
bool toolResultIsError(const std::string& contentJson, int idx) {
    auto arr = platform::json::parse(contentJson);
    if (arr.isFail() || !arr.value().isArray() || idx >= arr.value().size())
        return false;
    auto block = arr.value()[idx];
    if (!block.isObject()) return false;
    auto e = block.asObject().get("is_error");
    return e.isBool() ? e.asBool() : false;
}

// 第 idx 个 **text** block 的 text 字段（goal 消息是裸 text block，
// 没有 content 包裹层，故与 toolResultText 分开取）
std::string textBlockText(const std::string& contentJson, int idx) {
    auto arr = platform::json::parse(contentJson);
    if (arr.isFail() || !arr.value().isArray() || idx >= arr.value().size())
        return std::string();
    auto block = arr.value()[idx];
    if (!block.isObject()) return std::string();
    auto t = block.asObject().get("text");
    return t.isString() ? t.asString() : std::string();
}

// 进程级 ToolRegistry 单例 + 借用栈对象：注册与注销必须成对。本仓库的
// DOCTEST_CHECK 失败即 `return false`（doctest.h 头注释），手动写在用例
// 末尾的 unregisterTool 会被跳过——栈上 FakeTool 随之析构，注册表里就留下
// 悬垂裸指针，下一个用例的 allSchemasJson()/find() 一碰就 SIGBUS（RED 期
// 实测整个 exe 崩在 exit 138）。改用 RAII，注销由析构保证一定发生。
class ToolReg {
public:
    ToolReg(core::ToolRegistry& reg, core::ITool* tool)
        : reg_(reg), name_(tool->name()) {
        ok_ = reg_.registerTool(
                  std::shared_ptr<core::ITool>(tool, [](core::ITool*){})).isOk();
    }
    ~ToolReg() { if (ok_) reg_.unregisterTool(name_); }
    ToolReg(const ToolReg&) = delete;
    ToolReg& operator=(const ToolReg&) = delete;
    bool ok() const { return ok_; }
private:
    core::ToolRegistry& reg_;
    std::string name_;
    bool ok_ = false;
};

struct Fixture {
    test_support::TestDb tdb{test_support::TestDb("agentloop", "fx")};
    // 注意：TestDb 构造失败即抛（TIN-06b 口径），用例内直接用
    AgentStore store{tdb.handle()};
    MockLlm llm;
    core::ToolRegistry& tools = core::ToolRegistry::instance();
    AgentLoop loop{llm, tools, store};
};

// 建 task 行 + 填好 AgentTask（分支用例共用；createTask 失败即 return false）
bool setupTask(Fixture& fx, AgentTask& task, const std::string& goal,
               int maxIterations, bool allowDestructive) {
    auto taskId = fx.store.createTask("t", goal, "manual", "default",
                                      maxIterations, allowDestructive);
    if (!taskId.isOk()) return false;
    task.id = taskId.value();
    task.goal = goal;
    task.modelSlot = "default";
    task.maxIterations = maxIterations;
    task.allowDestructive = allowDestructive;
    return true;
}

// 取该 task 的唯一一条 run 行；查不到/多条 → 返回 false
bool onlyRun(Fixture& fx, const AgentTask& task, AgentRunRow& out) {
    auto runs = fx.store.listRuns(task.id, 10);
    if (runs.isFail() || runs.value().size() != 1) return false;
    out = runs.value()[0];
    return true;
}

} // namespace

REGISTER_TEST(agent_loop_happy_path_tool_then_end, "agent_loop: tool_use→工具执行→回喂→end_turn，消息累积与落库", {
    Fixture fx;
    FakeTool tool("t.hello");
    // 注册表借用栈对象：deleter 空、由 ToolReg 析构注销，勿用 make_shared
    ToolReg reg0(fx.tools, &tool);
    DOCTEST_REQUIRE(reg0.ok());
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

// ===== Task 5：状态机各分支 =====

// 分支 1 迭代上限：模型永远要调工具 → 循环必须被 maxIterations 收口，
// 记 limited 而不是无限跑（后台自动化无人盯着，卡死就是永久占着 worker）。
REGISTER_TEST(agent_loop_iteration_cap_limited, "agent_loop: 迭代到 maxIterations → run limited（收口不失控）", {
    Fixture fx;
    FakeTool tool("t.cap");
    ToolReg reg1(fx.tools, &tool);
    DOCTEST_REQUIRE(reg1.ok());
    AgentTask task;
    DOCTEST_REQUIRE(setupTask(fx, task, "一直调工具", 3, false));
    // 永远 tool_use：只有迭代上限能收口（script 给 8 条防万一）
    fx.llm.script = { toolUseResp("c1", "t.cap"), toolUseResp("c2", "t.cap"),
                      toolUseResp("c3", "t.cap"), toolUseResp("c4", "t.cap") };
    std::atomic<bool> cancel{false};
    auto runId = fx.loop.run(task, cancel);
    DOCTEST_REQUIRE(runId.isOk());
    DOCTEST_REQUIRE_EQ(fx.llm.sentMessages.size(), 3u);   // 恰好 3 轮，第 4 轮没发
    DOCTEST_CHECK_EQ(tool.calls, 3);
    AgentRunRow row;
    DOCTEST_REQUIRE(onlyRun(fx, task, row));
    DOCTEST_CHECK_EQ(row.status, std::string("limited"));
    DOCTEST_CHECK_EQ(row.iterations, 3);
    DOCTEST_REQUIRE(row.error.has_value());
    DOCTEST_CHECK(row.error->find("迭代上限") != std::string::npos);
    return true;
})

// 分支 2 stop_reason 受限：max_tokens 是「被截断」而非「模型答完了」，
// 记 limited 让 UI 能提示续跑（与 refusal→failed 互不重叠）。
REGISTER_TEST(agent_loop_max_tokens_limited, "agent_loop: stop_reason=max_tokens → run limited", {
    Fixture fx;
    AgentTask task;
    DOCTEST_REQUIRE(setupTask(fx, task, "写长文", 10, false));
    fx.llm.script = { resp(R"([{"type":"text","text":"写到一半被截断"}])", "max_tokens") };
    std::atomic<bool> cancel{false};
    auto runId = fx.loop.run(task, cancel);
    DOCTEST_REQUIRE(runId.isOk());
    DOCTEST_REQUIRE_EQ(fx.llm.sentMessages.size(), 1u);
    AgentRunRow row;
    DOCTEST_REQUIRE(onlyRun(fx, task, row));
    DOCTEST_CHECK_EQ(row.status, std::string("limited"));
    DOCTEST_REQUIRE(row.error.has_value());
    DOCTEST_CHECK(row.error->find("max_tokens") != std::string::npos);
    return true;
})

// 分支 3 LLM 失败：网络异常 → run failed + 错误原文入库，且**只留 goal
// 一条**消息（assistant 响应根本没拿到，不能凭空补一条）。
REGISTER_TEST(agent_loop_llm_failure_is_failed, "agent_loop: LLM 请求失败 → run failed 且只落 goal 一条", {
    Fixture fx;
    AgentTask task;
    DOCTEST_REQUIRE(setupTask(fx, task, "打个招呼", 10, false));
    fx.llm.failResponse =
        core::Result<core::LlmResponse>::fail(core::ErrorCode::NetworkTimeout, "请求超时");
    std::atomic<bool> cancel{false};
    auto runId = fx.loop.run(task, cancel);
    DOCTEST_REQUIRE(runId.isOk());
    AgentRunRow row;
    DOCTEST_REQUIRE(onlyRun(fx, task, row));
    DOCTEST_CHECK_EQ(row.status, std::string("failed"));
    DOCTEST_REQUIRE(row.error.has_value());
    DOCTEST_CHECK(row.error->find("超时") != std::string::npos);
    auto msgs = fx.store.listMessages(runId.value());
    DOCTEST_REQUIRE(msgs.isOk());
    DOCTEST_REQUIRE_EQ(msgs.value().size(), 1u);   // 只有 goal
    DOCTEST_CHECK_EQ(msgs.value()[0].role, std::string("user"));
    return true;
})

// 分支 4a 破坏性未授权：先拒后不调——工具**一次都不能执行**（spec §5.3：
// allow_destructive=0 时不挂起、不弹窗，直接 fail run）。
REGISTER_TEST(agent_loop_destructive_denied, "agent_loop: 破坏性工具未授权 → run failed 且工具零调用", {
    Fixture fx;
    FakeTool tool("t.del", /*destructive=*/true);
    ToolReg reg2(fx.tools, &tool);
    DOCTEST_REQUIRE(reg2.ok());
    AgentTask task;
    DOCTEST_REQUIRE(setupTask(fx, task, "删掉待办", 10, /*allowDestructive=*/false));
    fx.llm.script = { toolUseResp("d1", "t.del"), resp(R"([{"type":"text","text":"好"}])", "end_turn") };
    std::atomic<bool> cancel{false};
    auto runId = fx.loop.run(task, cancel);
    DOCTEST_REQUIRE(runId.isOk());
    DOCTEST_CHECK_EQ(tool.calls, 0);              // 先拒后不调
    DOCTEST_REQUIRE_EQ(fx.llm.sentMessages.size(), 1u);   // 不回喂、不进第二轮
    AgentRunRow row;
    DOCTEST_REQUIRE(onlyRun(fx, task, row));
    DOCTEST_CHECK_EQ(row.status, std::string("failed"));
    DOCTEST_REQUIRE(row.error.has_value());
    DOCTEST_CHECK(row.error->find("allow_destructive") != std::string::npos);
    return true;
})

// 分支 4b 破坏性已授权：同一个工具在 allow_destructive=1 下必须真被执行，
// 且 run 正常走到 end_turn completed（授权开关不能把工具永久禁掉）。
REGISTER_TEST(agent_loop_destructive_allowed, "agent_loop: allow_destructive=1 → 破坏性工具执行且 run completed", {
    Fixture fx;
    FakeTool tool("t.del2", /*destructive=*/true);
    ToolReg reg3(fx.tools, &tool);
    DOCTEST_REQUIRE(reg3.ok());
    AgentTask task;
    DOCTEST_REQUIRE(setupTask(fx, task, "删掉待办", 10, /*allowDestructive=*/true));
    fx.llm.script = { toolUseResp("d2", "t.del2"), resp(R"([{"type":"text","text":"已删除"}])", "end_turn") };
    std::atomic<bool> cancel{false};
    auto runId = fx.loop.run(task, cancel);
    DOCTEST_REQUIRE(runId.isOk());
    DOCTEST_CHECK_EQ(tool.calls, 1);
    DOCTEST_REQUIRE_EQ(fx.llm.sentMessages.size(), 2u);
    AgentRunRow row;
    DOCTEST_REQUIRE(onlyRun(fx, task, row));
    DOCTEST_CHECK_EQ(row.status, std::string("completed"));
    DOCTEST_CHECK_EQ(row.iterations, 2);
    return true;
})

// 分支 5 工具失败回喂：工具 execute 失败**不中止 run**，把原因以
// is_error=true 的 tool_result 回喂让 LLM 自纠（spec §10 工具异常语义）。
REGISTER_TEST(agent_loop_tool_failure_refeeds, "agent_loop: 工具 execute 失败 → is_error 回喂且不中止 run", {
    Fixture fx;
    FakeTool tool("t.bad");
    tool.failMessage = "参数错";
    tool.failCode = core::ErrorCode::ValidationError;
    ToolReg reg4(fx.tools, &tool);
    DOCTEST_REQUIRE(reg4.ok());
    AgentTask task;
    DOCTEST_REQUIRE(setupTask(fx, task, "调工具", 10, false));
    fx.llm.script = { toolUseResp("b1", "t.bad"), resp(R"([{"type":"text","text":"改对了"}])", "end_turn") };
    std::atomic<bool> cancel{false};
    auto runId = fx.loop.run(task, cancel);
    DOCTEST_REQUIRE(runId.isOk());
    DOCTEST_CHECK_EQ(tool.calls, 1);
    // 不中止：第二轮照发，最终 completed
    DOCTEST_REQUIRE_EQ(fx.llm.sentMessages.size(), 2u);
    const std::string& fed = fx.llm.sentMessages[1][2].contentJson;
    DOCTEST_CHECK(toolResultIsError(fed, 0));
    DOCTEST_CHECK(toolResultText(fed, 0).find("参数错") != std::string::npos);
    AgentRunRow row;
    DOCTEST_REQUIRE(onlyRun(fx, task, row));
    DOCTEST_CHECK_EQ(row.status, std::string("completed"));
    DOCTEST_CHECK_EQ(row.iterations, 2);
    return true;
})

// 分支 6 tool_result 32KB 硬上限（spec §10 L3c）：单 result 超 32KB 截断
// + 末尾 [truncated] 标记。防 md.read 长笔记 + 多迭代烧爆上下文致上游 400。
REGISTER_TEST(agent_loop_tool_result_truncated, "agent_loop: tool_result 超 32KB → 截断 + [truncated] 标记", {
    Fixture fx;
    FakeTool tool("t.big");
    tool.resultJson = "\"" + std::string(40 * 1024, 'A') + "\"";   // 40KB 的 JSON 字符串
    ToolReg reg5(fx.tools, &tool);
    DOCTEST_REQUIRE(reg5.ok());
    AgentTask task;
    DOCTEST_REQUIRE(setupTask(fx, task, "读长笔记", 10, false));
    fx.llm.script = { toolUseResp("g1", "t.big"), resp(R"([{"type":"text","text":"读完了"}])", "end_turn") };
    std::atomic<bool> cancel{false};
    auto runId = fx.loop.run(task, cancel);
    DOCTEST_REQUIRE(runId.isOk());
    const std::string text = toolResultText(fx.llm.sentMessages[1][2].contentJson, 0);
    // 上限 32KB + 标记余量（标记本身不算进 32KB 配额）
    DOCTEST_CHECK(text.size() <= 32u * 1024u + 32u);
    DOCTEST_CHECK(text.find("[truncated]") != std::string::npos);
    DOCTEST_CHECK(text.size() > 32u * 1024u);       // 确实截了（不是原样 40KB）
    DOCTEST_CHECK(text.compare(0, 16, "AAAAAAAAAAAAAAAA") == 0);   // 头部原样保留
    DOCTEST_CHECK(!toolResultIsError(fx.llm.sentMessages[1][2].contentJson, 0));
    AgentRunRow row;
    DOCTEST_REQUIRE(onlyRun(fx, task, row));
    DOCTEST_CHECK_EQ(row.status, std::string("completed"));
    return true;
})

// 分支 7 cancel：首轮请求发出后用户置位 cancel → 第二轮前中止，记
// cancelled（不是 failed——用户主动停的，UI 不该报成出错）。
REGISTER_TEST(agent_loop_cancel_stops_second_round, "agent_loop: cancel 置位 → 第二轮前中止且 run cancelled", {
    Fixture fx;
    FakeTool tool("t.cancel");
    ToolReg reg6(fx.tools, &tool);
    DOCTEST_REQUIRE(reg6.ok());
    AgentTask task;
    DOCTEST_REQUIRE(setupTask(fx, task, "长任务", 10, false));
    fx.llm.script = { toolUseResp("x1", "t.cancel"), resp(R"([{"type":"text","text":"不该到这"}])", "end_turn") };
    std::atomic<bool> cancel{false};
    // 在 send 入口置位：首轮请求发出、第一轮工具跑完后，循环顶部即中止
    fx.llm.onSendHook = [&cancel]() { cancel.store(true); };
    auto runId = fx.loop.run(task, cancel);
    DOCTEST_REQUIRE(runId.isOk());
    DOCTEST_CHECK_EQ(fx.llm.sentMessages.size(), 1u);   // 第二轮没发
    DOCTEST_CHECK_EQ(fx.llm.cursor, 1u);               // 第二个脚本响应没被取用
    AgentRunRow row;
    DOCTEST_REQUIRE(onlyRun(fx, task, row));
    DOCTEST_CHECK_EQ(row.status, std::string("cancelled"));
    return true;
})

// ===== Task 5 携带裁定 =====

// 裁定 1：contentJson 解析成 JSON **字符串**时，text 块用该字符串的值原样
// （工具回 "done" → 模型看到 done，不是带引号的 "done"；回 "" → 真空串，
// 不是 "\"\""）。非字符串（对象/数组/数字/布尔）保持 stringify(值)。
// 同一条 response 三个 tool_use 块，一次跑完三种形状。
REGISTER_TEST(agent_loop_tool_result_text_shape, "agent_loop: tool_result text 形状（字符串原样/空串/对象 stringify）", {
    Fixture fx;
    FakeTool tStr("t.shape.str"), tEmpty("t.shape.empty"), tObj("t.shape.obj");
    tEmpty.resultJson = "\"\"";                 // JSON 空字符串
    tObj.resultJson = R"({"k":1})";             // 对象
    ToolReg reg7(fx.tools, &tStr);
    DOCTEST_REQUIRE(reg7.ok());
    ToolReg reg8(fx.tools, &tEmpty);
    DOCTEST_REQUIRE(reg8.ok());
    ToolReg reg9(fx.tools, &tObj);
    DOCTEST_REQUIRE(reg9.ok());
    AgentTask task;
    DOCTEST_REQUIRE(setupTask(fx, task, "试三种返回", 10, false));
    fx.llm.script = {
        resp(R"([{"type":"tool_use","id":"s1","name":"t.shape.str","input":{}},
                 {"type":"tool_use","id":"s2","name":"t.shape.empty","input":{}},
                 {"type":"tool_use","id":"s3","name":"t.shape.obj","input":{}}])", "tool_use"),
        resp(R"([{"type":"text","text":"都收到了"}])", "end_turn"),
    };
    std::atomic<bool> cancel{false};
    auto runId = fx.loop.run(task, cancel);
    DOCTEST_REQUIRE(runId.isOk());
    DOCTEST_REQUIRE_EQ(fx.llm.sentMessages.size(), 2u);
    const std::string& fed = fx.llm.sentMessages[1][2].contentJson;   // goal+asst1+3×tool_result
    // JSON 字符串 → 值原样，不二次 stringify
    DOCTEST_CHECK_EQ(toolResultText(fed, 0), std::string("done"));
    // JSON 空字符串 → 真空串
    DOCTEST_CHECK_EQ(toolResultText(fed, 1), std::string());
    // 对象 → stringify(值)
    DOCTEST_CHECK_EQ(toolResultText(fed, 2), std::string(R"({"k":1})"));
    // 多块 → tool_call_id 留 NULL（见 Task 4 决策）
    auto msgs = fx.store.listMessages(runId.value());
    DOCTEST_REQUIRE(msgs.isOk());
    DOCTEST_REQUIRE_EQ(msgs.value().size(), 4u);   // goal+asst1+3×tool_result(一条)+asst2
    DOCTEST_CHECK(!msgs.value()[2].toolCallId.has_value());
    AgentRunRow row;
    DOCTEST_REQUIRE(onlyRun(fx, task, row));
    DOCTEST_CHECK_EQ(row.status, std::string("completed"));
    return true;
})

// 裁定 1 的另一半：工具查不到（模块被拔）那条 is_error 回喂也必须是同一
// 形状（text block 数组）。模型收到的 content 形状必须统一——一处是裸
// 字符串、一处是 blocks 数组，上游 schema 校验与 LLM 解析都得多担一份心。
// 同一条 response 里混一个正常工具 + 一个不存在的工具，一次钉两种形状。
REGISTER_TEST(agent_loop_tool_notfound_same_shape, "agent_loop: 工具未找到的 is_error 回喂与正常回喂同形状（text block 数组）", {
    Fixture fx;
    FakeTool tool("t.found");
    ToolReg regA(fx.tools, &tool);
    DOCTEST_REQUIRE(regA.ok());
    AgentTask task;
    DOCTEST_REQUIRE(setupTask(fx, task, "调两个工具", 10, false));
    fx.llm.script = {
        resp(R"([{"type":"tool_use","id":"f1","name":"t.found","input":{}},
                 {"type":"tool_use","id":"f2","name":"t.absent","input":{}}])", "tool_use"),
        resp(R"([{"type":"text","text":"换个工具"}])", "end_turn"),
    };
    std::atomic<bool> cancel{false};
    auto runId = fx.loop.run(task, cancel);
    DOCTEST_REQUIRE(runId.isOk());
    // 工具查不到**不中止** run（spec §1 L3a：让 LLM 自纠或换工具）
    DOCTEST_CHECK_EQ(tool.calls, 1);
    DOCTEST_REQUIRE_EQ(fx.llm.sentMessages.size(), 2u);
    const std::string& fed = fx.llm.sentMessages[1][2].contentJson;
    // 两个块都是 text block 数组（toolResultText 只在数组形状下才取得到值）
    DOCTEST_CHECK_EQ(toolResultText(fed, 0), std::string("done"));
    DOCTEST_CHECK(toolResultText(fed, 1).find("未找到") != std::string::npos);
    DOCTEST_CHECK(!toolResultIsError(fed, 0));
    DOCTEST_CHECK(toolResultIsError(fed, 1));       // 未找到 → is_error=true
    AgentRunRow row;
    DOCTEST_REQUIRE(onlyRun(fx, task, row));
    DOCTEST_CHECK_EQ(row.status, std::string("completed"));
    return true;
})
// ===== M2 Task 1：资源上限 =====

// spec §5.2：token 累计（input+output）硬上限 200K。单轮响应即破限也必须停
//（检查点在 keepAssistant 落库之后、下一轮工具执行之前——破限轮的 assistant
// 消息与用量照实落库（spec §10 审计），落库完立即判停，不再发第二轮）。
REGISTER_TEST(agent_loop_token_cap_limits_run,
              "agent_loop: token 累计超 200K → limited + error 说明", {
    Fixture fx;
    auto taskId = fx.store.createTask("t", "烧token", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    AgentTask task; task.id = taskId.value(); task.goal = "烧token"; task.modelSlot = "default";
    core::LlmResponse big = resp(R"([{"type":"text","text":"big"}])", "tool_use");
    big.inputTokens = 150000; big.outputTokens = 60000;    // 累计 21 万 > 200K
    core::LlmResponse next = resp(R"([{"type":"text","text":"more"}])", "end_turn");
    next.inputTokens = 10; next.outputTokens = 5;
    fx.llm.script = {big, next};   // 第二条必须永远发不出去
    std::atomic<bool> cancel{false};
    auto runId = fx.loop.run(task, cancel);
    DOCTEST_REQUIRE(runId.isOk());
    DOCTEST_REQUIRE_EQ(fx.llm.sentMessages.size(), 1u);    // 破限后不再发第二轮
    AgentRunRow run;
    DOCTEST_REQUIRE(onlyRun(fx, task, run));
    DOCTEST_CHECK_EQ(run.status, std::string("limited"));
    DOCTEST_REQUIRE(run.error.has_value());
    DOCTEST_CHECK(run.error->find("200K") != std::string::npos);
    DOCTEST_CHECK_EQ(run.inputTokens.value_or(0), 150000); // 用量照实落库
    DOCTEST_CHECK_EQ(run.outputTokens.value_or(0), 60000);
    // spec §10 审计：检查点在 keepAssistant **之后**——破限轮的 assistant 消息
    // 必须已落库（若判定挪到落库之前，这里只剩 goal 一条）
    auto msgs = fx.store.listMessages(runId.value());
    DOCTEST_REQUIRE(msgs.isOk());
    DOCTEST_REQUIRE_EQ(msgs.value().size(), 2u);           // goal + 破限轮 assistant
    DOCTEST_CHECK_EQ(msgs.value()[1].role, std::string("assistant"));
    return true;
})

// spec §5.2：单 run 墙钟 = max_iterations × 请求超时(60s)，硬上限 30 分钟。
// 注入假时钟：每次 send 前推进 10 分钟 → 第二轮顶部即破 min(10×60s, 30min)
// = 600s 预算（预算取 min，迭代上限 10 轮远未到）。
// 与 brief 的唯一偏差：脚本响应用**真 tool_use 轮**（toolUseResp）而非 text 块 +
// stop_reason=tool_use——后者命中「stop_reason=tool_use 但响应里没有 tool_use
// block」分支，第一轮就 run failed 收尾，永远回不到循环顶部的墙钟检查点，
// 用例恒红且测不到目标行为（agent_loop.cpp 该分支已有独立用例覆盖）。
REGISTER_TEST(agent_loop_wall_clock_cap_limits_run,
              "agent_loop: 墙钟超预算 → limited（时钟注入零等待）", {
    Fixture fx;
    auto taskId = fx.store.createTask("t", "磨洋工", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    AgentTask task; task.id = taskId.value(); task.goal = "磨洋工"; task.modelSlot = "default";
    int64_t fakeNowMs = 0;
    AgentLoop loop{fx.llm, fx.tools, fx.store,
                   [&fakeNowMs] { return fakeNowMs; }};   // 注入时钟（新形参）
    fx.llm.onSendHook = [&fakeNowMs] { fakeNowMs += 10 * 60 * 1000; };  // 每轮 +10min
    // 工具名不存在 → is_error 回喂、run 继续（不引入注册表依赖，只烧轮次）
    core::LlmResponse tu = toolUseResp("w1", "t.ghost");
    tu.inputTokens = 1; tu.outputTokens = 1;   // token 累计远不触顶，只钉墙钟
    fx.llm.script = {tu, tu, tu, tu};   // 4 条远小于迭代上限 10
    std::atomic<bool> cancel{false};
    auto runId = loop.run(task, cancel);
    DOCTEST_REQUIRE(runId.isOk());
    AgentRunRow run;
    DOCTEST_REQUIRE(onlyRun(fx, task, run));
    DOCTEST_CHECK_EQ(run.status, std::string("limited"));
    DOCTEST_REQUIRE(run.error.has_value());
    DOCTEST_CHECK(run.error->find("墙钟") != std::string::npos);
    // 钉死「预算取 min」这一支（评审补）：maxIterations=10 → 预算 600s 紧于
    // 30 分钟硬上限，第 2 轮循环顶即停 = 恰好 1 次 send。若 min 退化成取 max
    // （1800s），要 3 轮才停且报 1800s —— 下面两条断言会红（原断言集对它假绿）。
    DOCTEST_CHECK(run.error->find("600s") != std::string::npos);
    DOCTEST_CHECK_EQ(fx.llm.sentMessages.size(), 1u);
    return true;
})

// spec §5.2 的另一支：30 分钟**硬上限**本身。maxIterations=50（loop 内 clamp
// 上界）→ 预算 = min(50×60s, 30min) = 1800s，由硬上限决定而非迭代数。
// 同样注入假时钟：每次 send 前 +10 分钟，3 轮后（第 4 次循环顶）累计 1800s 破限；
// 脚本给 4 条、迭代上限 50 远未到 —— 只有硬上限能让它停。
REGISTER_TEST(agent_loop_wall_clock_hard_cap_30min_limits_run,
              "agent_loop: 墙钟 30 分钟硬上限 → limited 1800s（时钟注入）", {
    Fixture fx;
    auto taskId = fx.store.createTask("t", "磨洋工", "manual", "default", 50, false);
    DOCTEST_REQUIRE(taskId.isOk());
    AgentTask task; task.id = taskId.value(); task.goal = "磨洋工"; task.modelSlot = "default";
    task.maxIterations = 50;
    int64_t fakeNowMs = 0;
    AgentLoop loop{fx.llm, fx.tools, fx.store,
                   [&fakeNowMs] { return fakeNowMs; }};
    fx.llm.onSendHook = [&fakeNowMs] { fakeNowMs += 10 * 60 * 1000; };  // 每轮 +10min
    core::LlmResponse tu = toolUseResp("h1", "t.ghost");   // 工具不存在 → is_error 回喂继续
    tu.inputTokens = 1; tu.outputTokens = 1;               // token 远不触顶，只钉墙钟
    fx.llm.script = {tu, tu, tu, tu};
    std::atomic<bool> cancel{false};
    auto runId = loop.run(task, cancel);
    DOCTEST_REQUIRE(runId.isOk());
    AgentRunRow run;
    DOCTEST_REQUIRE(onlyRun(fx, task, run));
    DOCTEST_CHECK_EQ(run.status, std::string("limited"));
    DOCTEST_REQUIRE(run.error.has_value());
    DOCTEST_CHECK(run.error->find("1800s") != std::string::npos);   // 硬上限 30 分钟
    DOCTEST_CHECK_EQ(fx.llm.sentMessages.size(), 3u);   // 3 轮后才破（不是第 1 轮）
    return true;
})

// 裁定 2：goal 含引号 / 反斜杠 / 换行时，首条 user 消息仍必须是合法 JSON
// 且 text 块与原文逐字节相同（每个 run 都走这条路径，转义坏了全盘皆输）。
REGISTER_TEST(agent_loop_goal_escaping, "agent_loop: goal 含引号/反斜杠/换行 → 首条消息仍合法且原文逐字保真", {
    Fixture fx;
    const std::string goal = "他说\"做完了\"\\路径\n第二行";
    AgentTask task;
    DOCTEST_REQUIRE(setupTask(fx, task, goal, 10, false));
    fx.llm.script = { resp(R"([{"type":"text","text":"收到"}])", "end_turn") };
    std::atomic<bool> cancel{false};
    auto runId = fx.loop.run(task, cancel);
    DOCTEST_REQUIRE(runId.isOk());
    DOCTEST_REQUIRE_EQ(fx.llm.sentMessages.size(), 1u);
    DOCTEST_REQUIRE_EQ(fx.llm.sentMessages[0].size(), 1u);
    const std::string& first = fx.llm.sentMessages[0][0].contentJson;
    auto parsed = platform::json::parse(first);
    DOCTEST_REQUIRE(parsed.isOk());                       // 转义坏了这里就红
    DOCTEST_REQUIRE(parsed.value().isArray());
    DOCTEST_REQUIRE_EQ(parsed.value().size(), 1);
    auto block = parsed.value()[0];
    DOCTEST_REQUIRE(block.isObject());
    DOCTEST_CHECK_EQ(block.asObject().get("type").asString(), std::string("text"));
    DOCTEST_CHECK_EQ(block.asObject().get("text").asString(), goal);   // 逐字节
    // 落库那份同样保真
    auto msgs = fx.store.listMessages(runId.value());
    DOCTEST_REQUIRE(msgs.isOk());
    DOCTEST_REQUIRE_EQ(msgs.value().size(), 2u);
    DOCTEST_CHECK_EQ(textBlockText(msgs.value()[0].contentJson, 0), goal);
    return true;
})
