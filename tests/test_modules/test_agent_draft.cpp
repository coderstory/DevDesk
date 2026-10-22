// tests/test_modules/test_agent_draft.cpp
// 自然语言任务草稿层：prompt 组装 + 一次纯文本补全 + 解析校验降级矩阵。
//
// 跨文件前提：
//   - LlmEndpoint 是可注入的纯虚接口，本文件用**文件私有**的 MockLlm 顶替（
//     test_agent_loop.cpp 里那份是它自己的匿名命名空间副本，两边互不可见），
//     零网络、零凭据。
//   - prompt 里的工具名取自 core::ToolRegistry::instance().listTools()，用例因此
//     **不钉住工具名清单**（单测进程里别的用例可能注册过 FakeTool）—— 只钉
//     「出现了已注册工具里的某个名字」这一条不变量。
//   - parseTaskDraft 的 once 校验读真时钟，负例用 2000 年、正例用 2099 年，
//     结论不随运行时刻漂移。
// 用例名前缀 draft_（ASCII，--filter draft_ 可定向跑）
#include "doctest.h"

#include "core/llm_endpoint.h"
#include "core/result.h"
#include "core/tool.h"
#include "module_agent/agent_loop.h"   // currentTimePromptLine（prompt 时间行的唯一真源）
#include "module_agent/task_draft.h"
#include "module_agent/task_draft_parse.h"
#include "module_agent/trigger_config.h"
#include "platform/json.h"
#include "platform/utf8.h"

#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using module::agent::buildTaskDraftSystemPrompt;
using module::agent::parseTaskDraft;
using module::agent::requestTaskDraft;
using module::agent::TaskDraftResult;
using module::agent::triggerConfigProblem;

// 记录式 MockLlm（test_agent_loop.cpp 那份的精简副本）：单次响应 + 记录入参
class MockLlm : public core::LlmEndpoint {
public:
    core::Result<core::LlmResponse> reply = core::Result<core::LlmResponse>::fail(
        core::ErrorCode::Internal, "mock 未设置 reply");
    std::string sentToolsJson;
    std::string sentSystemPrompt;
    std::string sentModel;
    std::vector<core::LlmMessage> sentMessages;

    core::Result<core::LlmResponse> send(
        const std::vector<core::LlmMessage>& messages, const std::string& toolsJson,
        const std::string& systemPrompt, const std::string& model) override {
        sentMessages = messages;
        sentToolsJson = toolsJson;
        sentSystemPrompt = systemPrompt;
        sentModel = model;
        return reply;
    }
};

// 便捷构造：把若干 text 块 + 可选 tool_use 块拼成 content blocks 数组原文
std::string blocksJson(const std::vector<std::string>& texts) {
    std::string out = "[";
    for (size_t i = 0; i < texts.size(); ++i) {
        if (i > 0) out += ",";
        auto lit = platform::json::stringify(platform::json::makeString(texts[i]));
        if (lit.isFail()) return {};
        out += "{\"type\":\"text\",\"text\":" + lit.value() + "}";
    }
    out += "]";
    return out;
}

core::LlmResponse responseWith(const std::string& contentJson) {
    core::LlmResponse r;
    r.stopReason = "end_turn";
    r.contentJson = contentJson;
    return r;
}

// 在 text 块之间插一个 tool_use 块：草稿请求不该带 tools，但网关仍可能回灌历史
std::string blocksJsonWithToolUse(const std::vector<std::string>& texts) {
    std::string out = "[";
    for (size_t i = 0; i < texts.size(); ++i) {
        if (i > 0) out += ",";
        auto lit = platform::json::stringify(platform::json::makeString(texts[i]));
        if (lit.isFail()) return {};
        out += "{\"type\":\"text\",\"text\":" + lit.value() + "}";
        if (i == 0) {
            out +=
                ",{\"type\":\"tool_use\",\"name\":\"todo.list\",\"input\":"
                "{\"overdue\":true}}";
        }
    }
    out += "]";
    return out;
}

bool hasWarningContaining(const TaskDraftResult& r, const std::string& needle) {
    for (const auto& w : r.draft.warnings) {
        if (w.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool hasNeedContaining(const TaskDraftResult& r, const std::string& needle) {
    for (const auto& s : r.needsUserInput) {
        if (s.find(needle) != std::string::npos) return true;
    }
    return false;
}

const char* kScheduleJson =
    R"({"goal":"调用 hot.list 取热榜，有新增条目时调用 app.notify 通知我。",)"
    R"("contextNote":"只关注科技榜","triggerType":"schedule","cron":"0 8 * * *",)"
    R"("onceLocal":"","maxIterations":10,"allowDestructive":false,"needsUserInput":[]})";

} // namespace

// ---- 正常路径：schedule / once 都能填出可落库的触发配置 ----

REGISTER_TEST(draft_schedule_fills_trigger,
              "草稿: 合法 schedule 草稿填入 triggerType/cron 且 triggerConfigProblem 为空", {
    auto r = parseTaskDraft(kScheduleJson, {});
    DOCTEST_REQUIRE(r.isOk());
    const TaskDraftResult& v = r.value();
    DOCTEST_CHECK(v.goalFilled);
    DOCTEST_CHECK(v.triggerFilled);
    DOCTEST_CHECK_EQ(v.draft.goal,
                     std::string("调用 hot.list 取热榜，有新增条目时调用 app.notify 通知我。"));
    DOCTEST_CHECK_EQ(v.draft.contextNote, std::string("只关注科技榜"));
    DOCTEST_CHECK_EQ(v.draft.triggerType, std::string("schedule"));
    DOCTEST_CHECK_EQ(v.draft.triggerConfig, std::string("{\"cron\":\"0 8 * * *\"}"));
    DOCTEST_CHECK(triggerConfigProblem("schedule", v.draft.triggerConfig).empty());
    DOCTEST_CHECK(v.draft.warnings.empty());
    return true;
})

REGISTER_TEST(draft_once_fills_trigger,
              "草稿: 合法 once 草稿把 onceLocal 解析成 once_at（2099 年恒为未来）", {
    const std::string json =
        R"({"goal":"到点后通知我该交房租了。","contextNote":"","triggerType":"once",)"
        R"("cron":"","onceLocal":"2099-03-04 08:30","maxIterations":10,)"
        R"("allowDestructive":false,"needsUserInput":[]})";
    auto r = parseTaskDraft(json, {});
    DOCTEST_REQUIRE(r.isOk());
    const TaskDraftResult& v = r.value();
    DOCTEST_CHECK(v.triggerFilled);
    DOCTEST_CHECK_EQ(v.draft.triggerType, std::string("once"));
    DOCTEST_CHECK(triggerConfigProblem("once", v.draft.triggerConfig).empty());
    auto cfg = module::agent::decodeTriggerConfig(v.draft.triggerConfig);
    DOCTEST_REQUIRE(cfg.isOk());
    DOCTEST_REQUIRE(cfg.value().onceAt.has_value());
    // 编码进去的 once_at 必须与「同一个文本再解析一次」完全一致（不引第二套时区逻辑）
    auto reparsed = module::agent::parseOnceAtLocalText("2099-03-04 08:30");
    DOCTEST_REQUIRE(reparsed.isOk());
    DOCTEST_CHECK_EQ(*cfg.value().onceAt, reparsed.value());
    DOCTEST_CHECK_EQ(module::agent::formatOnceAtText(*cfg.value().onceAt),
                     std::string("2099-03-04 08:30"));
    return true;
})

REGISTER_TEST(draft_manual_daily_resident_filled,
              "草稿: manual / daily / resident 不消费 trigger_config，triggerConfig 恒空且算已填", {
    for (const char* type : {"manual", "daily", "resident"}) {
        const std::string json = std::string(R"({"goal":"列一下今天的待办。",)"
                                           R"("contextNote":"","triggerType":")") +
                                 type +
                                 R"(","cron":"","onceLocal":"","maxIterations":10,)"
                                 R"("allowDestructive":false,"needsUserInput":[]})";
        auto r = parseTaskDraft(json, {});
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_CHECK(r.value().triggerFilled);
        DOCTEST_CHECK_EQ(r.value().draft.triggerType, std::string(type));
        DOCTEST_CHECK(r.value().draft.triggerConfig.empty());
        DOCTEST_CHECK(triggerConfigProblem(type, "").empty());
    }
    return true;
})

REGISTER_TEST(draft_event_with_events_filled,
              "草稿: event 型给了非空 events 才算填入（与 saveTaskDialog 的口径同源）", {
    const std::string json =
        R"({"goal":"新建待办后给我发通知。","contextNote":"","triggerType":"event",)"
        R"("cron":"","onceLocal":"","events":["todo.created"],"maxIterations":10,)"
        R"("allowDestructive":false,"needsUserInput":[]})";
    auto r = parseTaskDraft(json, {});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(r.value().triggerFilled);
    DOCTEST_CHECK_EQ(r.value().draft.triggerType, std::string("event"));
    DOCTEST_CHECK(triggerConfigProblem("event", r.value().draft.triggerConfig).empty());
    return true;
})

// ---- once 的四类坏输入：缺失 / 非法格式 / 已过去 → 降级 manual + 追问时间 ----

REGISTER_TEST(draft_once_missing_time_asks,
              "草稿: once 缺 onceLocal → triggerFilled=false + needsUserInput 含「时间」", {
    const std::string json =
        R"({"goal":"到点提醒我。","contextNote":"","triggerType":"once","cron":"",)"
        R"("onceLocal":"","maxIterations":10,"allowDestructive":false,)"
        R"("needsUserInput":[]})";
    auto r = parseTaskDraft(json, {});
    DOCTEST_REQUIRE(r.isOk());
    const TaskDraftResult& v = r.value();
    DOCTEST_CHECK(v.goalFilled);
    DOCTEST_CHECK(!v.triggerFilled);
    DOCTEST_CHECK_EQ(v.draft.triggerType, std::string("manual"));
    DOCTEST_CHECK(v.draft.triggerConfig.empty());
    DOCTEST_CHECK(hasNeedContaining(v, "时间"));
    DOCTEST_CHECK(hasWarningContaining(v, "手动"));
    return true;
})

REGISTER_TEST(draft_once_past_time_asks,
              "草稿: once 给过去时刻（2000 年）→ 降级 manual + 追问时间（错过的闹钟永不会响）", {
    const std::string json =
        R"({"goal":"到点提醒我。","contextNote":"","triggerType":"once","cron":"",)"
        R"("onceLocal":"2000-01-01 08:00","maxIterations":10,)"
        R"("allowDestructive":false,"needsUserInput":[]})";
    auto r = parseTaskDraft(json, {});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(!r.value().triggerFilled);
    DOCTEST_CHECK_EQ(r.value().draft.triggerType, std::string("manual"));
    DOCTEST_CHECK(hasNeedContaining(r.value(), "时间"));
    DOCTEST_CHECK(hasWarningContaining(r.value(), "晚于当前时间"));
    return true;
})

REGISTER_TEST(draft_once_bad_format_asks,
              "草稿: once 的 onceLocal 格式非法 → 降级 manual + 追问时间", {
    for (const char* bad : {"明天八点", "2099-13-04 08:30", "2099-02-30 08:30",
                            "2099-03-04 8:30"}) {
        const std::string json =
            std::string(R"({"goal":"到点提醒我。","contextNote":"","triggerType":)"
                        R"("once","cron":"","onceLocal":")") +
            bad +
            R"(","maxIterations":10,"allowDestructive":false,"needsUserInput":[]})";
        auto r = parseTaskDraft(json, {});
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_CHECK(!r.value().triggerFilled);
        DOCTEST_CHECK_EQ(r.value().draft.triggerType, std::string("manual"));
        DOCTEST_CHECK(hasNeedContaining(r.value(), "时间"));
    }
    return true;
})

// ---- schedule / triggerType / event 的降级 ----

REGISTER_TEST(draft_bad_cron_downgrades,
              "草稿: cron 段数错或字段越界 → 降级 manual + warnings", {
    for (const char* bad : {"0 8 * *", "99 8 * * *", "0 25 * * *", "0 8 * * abc"}) {
        const std::string json =
            std::string(R"({"goal":"每天八点查热榜。","contextNote":"","triggerType":)"
                        R"("schedule","cron":")") +
            bad +
            R"(","onceLocal":"","maxIterations":10,"allowDestructive":false,)"
            R"("needsUserInput":[]})";
        auto r = parseTaskDraft(json, {});
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_CHECK(!r.value().triggerFilled);
        DOCTEST_CHECK_EQ(r.value().draft.triggerType, std::string("manual"));
        DOCTEST_CHECK(r.value().draft.triggerConfig.empty());
        DOCTEST_CHECK(hasWarningContaining(r.value(), "cron"));
        DOCTEST_CHECK(hasNeedContaining(r.value(), "cron"));
    }
    return true;
})

REGISTER_TEST(draft_schedule_missing_cron_downgrades,
              "草稿: schedule 缺 cron → 降级 manual + 追问表达式", {
    const std::string json =
        R"({"goal":"每天八点查热榜。","contextNote":"","triggerType":"schedule",)"
        R"("cron":"","onceLocal":"","maxIterations":10,"allowDestructive":false,)"
        R"("needsUserInput":[]})";
    auto r = parseTaskDraft(json, {});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(!r.value().triggerFilled);
    DOCTEST_CHECK_EQ(r.value().draft.triggerType, std::string("manual"));
    DOCTEST_CHECK(hasNeedContaining(r.value(), "cron"));
    return true;
})

REGISTER_TEST(draft_unknown_trigger_type_downgrades,
              "草稿: 未知 triggerType / 缺 triggerType → 降级 manual + warnings", {
    const std::string unknown =
        R"({"goal":"每天八点查热榜。","contextNote":"","triggerType":"weekly",)"
        R"("cron":"","onceLocal":"","maxIterations":10,"allowDestructive":false,)"
        R"("needsUserInput":[]})";
    auto r1 = parseTaskDraft(unknown, {});
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_CHECK(!r1.value().triggerFilled);
    DOCTEST_CHECK_EQ(r1.value().draft.triggerType, std::string("manual"));
    DOCTEST_CHECK(hasWarningContaining(r1.value(), "weekly"));

    const std::string missing =
        R"({"goal":"每天八点查热榜。","contextNote":"","cron":"",)"
        R"("onceLocal":"","maxIterations":10,"allowDestructive":false,)"
        R"("needsUserInput":[]})";
    auto r2 = parseTaskDraft(missing, {});
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK(!r2.value().triggerFilled);
    DOCTEST_CHECK_EQ(r2.value().draft.triggerType, std::string("manual"));
    DOCTEST_CHECK(hasWarningContaining(r2.value(), "没有指定触发方式"));
    return true;
})

REGISTER_TEST(draft_event_missing_events_downgrades,
              "草稿: event 型没给 events → 降级 manual + 追问事件（全不勾 = 永不触发的死配置）", {
    const std::string json =
        R"({"goal":"新建待办后通知我。","contextNote":"","triggerType":"event",)"
        R"("cron":"","onceLocal":"","events":[],"maxIterations":10,)"
        R"("allowDestructive":false,"needsUserInput":[]})";
    auto r = parseTaskDraft(json, {});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(!r.value().triggerFilled);
    DOCTEST_CHECK_EQ(r.value().draft.triggerType, std::string("manual"));
    DOCTEST_CHECK(hasNeedContaining(r.value(), "事件"));
    return true;
})

// ---- 宽容路径：goal 空 / 触发坏，都只是一条 warnings，不整体失败 ----

REGISTER_TEST(draft_tolerant_goal_only,
              "草稿: 只有 goal、触发降级 → 仍返回 ok（goalFilled=true，triggerFilled=false）", {
    const std::string json =
        R"({"goal":"把我今天所有的逾期待办列出来。","contextNote":"",)"
        R"("triggerType":"weekly","cron":"","onceLocal":"","maxIterations":10,)"
        R"("allowDestructive":false,"needsUserInput":[]})";
    auto r = parseTaskDraft(json, {});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(r.value().goalFilled);
    DOCTEST_CHECK(!r.value().triggerFilled);
    DOCTEST_CHECK_EQ(r.value().draft.goal,
                     std::string("把我今天所有的逾期待办列出来。"));
    DOCTEST_CHECK(!r.value().draft.warnings.empty());
    DOCTEST_CHECK(hasNeedContaining(r.value(), "触发方式"));
    return true;
})

REGISTER_TEST(draft_empty_goal_only_warns,
              "草稿: goal 缺失/空串 → goalFilled=false + warnings，触发仍照常校验", {
    for (const char* goalJson : {R"("")", R"("  ")"}) {
        const std::string json =
            std::string(R"({"goal":)") + goalJson +
            R"(,"contextNote":"只做只读查询","triggerType":"manual","cron":"",)"
            R"("onceLocal":"","maxIterations":10,"allowDestructive":false,)"
            R"("needsUserInput":[]})";
        auto r = parseTaskDraft(json, {});
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_CHECK(!r.value().goalFilled);
        DOCTEST_CHECK(r.value().draft.goal.empty());
        DOCTEST_CHECK(hasWarningContaining(r.value(), "任务目标"));
        DOCTEST_CHECK(r.value().triggerFilled);   // 触发与 goal 互不牵连
        // W4 C3：contextNote 已移除（task_draft.cpp 注释——执行记忆并入
        // systemPrompt 单字段，不再从草稿解析）；载荷里的 contextNote 丢弃为默认空。
        DOCTEST_CHECK(r.value().draft.contextNote.empty());
    }
    return true;
})

REGISTER_TEST(draft_keeps_model_needs_user_input,
              "草稿: 模型自报的 needsUserInput 原样保留，与宿主补的追问去重合并", {
    const std::string json =
        R"({"goal":"到点通知我。","contextNote":"","triggerType":"once","cron":"",)"
        R"("onceLocal":"","maxIterations":10,"allowDestructive":false,)"
        R"("needsUserInput":["请指定闹钟时间（YYYY-MM-DD HH:MM）","用哪个日历？"]})";
    auto r = parseTaskDraft(json, {});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_REQUIRE(r.value().needsUserInput.size() == 2u);
    DOCTEST_CHECK_EQ(r.value().needsUserInput[0],
                     std::string("请指定闹钟时间（YYYY-MM-DD HH:MM）"));
    DOCTEST_CHECK_EQ(r.value().needsUserInput[1], std::string("用哪个日历？"));
    return true;
})

// ---- 唯一会 fail 的分支 ----

REGISTER_TEST(draft_non_json_fails,
              "草稿: 空串 / 纯文本 / 坏 JSON / 非对象顶层 → parseTaskDraft fail", {
    for (const char* bad : {"",
                            "好的，我来帮你创建一个每天八点执行的任务。",
                            R"({"goal": "没闭合)",
                            R"(["not","an","object"])",
                            "42"}) {
        auto r = parseTaskDraft(bad, {});
        DOCTEST_CHECK(r.isFail());
        DOCTEST_CHECK(r.errorMessage().find("JSON") != std::string::npos);
    }
    return true;
})

REGISTER_TEST(draft_strip_fence_and_prose,
              "草稿: ```json 围栏 / JSON 前后废话 都能剥出对象（容错解析）", {
    const std::string fenced = std::string("```json\n") + kScheduleJson + "\n```";
    auto r1 = parseTaskDraft(fenced, {});
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_CHECK(r1.value().triggerFilled);
    DOCTEST_CHECK_EQ(r1.value().draft.triggerType, std::string("schedule"));

    const std::string plainFence = "```\n" + std::string(kScheduleJson) + "\n```";
    auto r2 = parseTaskDraft(plainFence, {});
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK(r2.value().triggerFilled);

    const std::string prose =
        "好的，这是配置：\n" + std::string(kScheduleJson) + "\n以上，请确认。";
    auto r3 = parseTaskDraft(prose, {});
    DOCTEST_REQUIRE(r3.isOk());
    DOCTEST_CHECK(r3.value().triggerFilled);
    DOCTEST_CHECK_EQ(r3.value().draft.triggerConfig, std::string("{\"cron\":\"0 8 * * *\"}"));
    return true;
})

// ---- goal 侧的两道体检 ----

REGISTER_TEST(draft_unknown_tool_warns,
              "草稿: goal 引用不在工具表里的点分 token → warnings 含「未知工具」（空表则跳过）", {
    const std::string json =
        R"({"goal":"先调用 todo.create 建一条待办，再用 hot.list 看榜单。",)"
        R"("contextNote":"","triggerType":"manual","cron":"","onceLocal":"",)"
        R"("maxIterations":10,"allowDestructive":false,"needsUserInput":[]})";
    // 空表 = 跳过工具校验（本层不该在没有工具的环境里乱报警）
    auto skipped = parseTaskDraft(json, {});
    DOCTEST_REQUIRE(skipped.isOk());
    DOCTEST_CHECK(!hasWarningContaining(skipped.value(), "未知工具"));

    auto r = parseTaskDraft(json, {"todo.create", "hot.list"});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(r.value().goalFilled);
    DOCTEST_CHECK(!hasWarningContaining(r.value(), "未知工具"));

    auto bad = parseTaskDraft(json, {"todo.create"});
    DOCTEST_REQUIRE(bad.isOk());
    DOCTEST_CHECK(hasWarningContaining(bad.value(), "未知工具"));
    DOCTEST_CHECK(hasWarningContaining(bad.value(), "hot.list"));
    return true;
})

REGISTER_TEST(draft_long_goal_truncated_on_utf8_boundary,
              "草稿: goal 超长 → UTF-8 边界安全截断且不破坏字符边界 + warnings", {
    // 1500 个 3 字节汉字 = 4500 字节 > 4000 上限；截点必落在字符中间
    std::string goal;
    while (goal.size() < 4500) goal += "每";
    const std::string json =
        R"({"goal":")" + goal +
        R"(","contextNote":"","triggerType":"manual","cron":"","onceLocal":"",)"
        R"("maxIterations":10,"allowDestructive":false,"needsUserInput":[]})";
    auto r = parseTaskDraft(json, {});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(r.value().goalFilled);
    DOCTEST_CHECK(r.value().draft.goal.size() <= 4000u);
    DOCTEST_CHECK(!r.value().draft.goal.empty());
    DOCTEST_CHECK(hasWarningContaining(r.value(), "过长"));
    // 截断点必须落在字符边界上：切在半字里会让 UI 渲染出乱码（上游也会拒）
    DOCTEST_CHECK(platform::utf8::isValid(r.value().draft.goal));
    return true;
})

// ---- prompt 组装 ----

REGISTER_TEST(draft_prompt_contains_time_triggers_tools,
              "草稿: system prompt 含当前本地时间行、六种触发类型、工具名与 JSON-only schema", {
    // 2026-03-04 08:30 本地（mktime 走本机时区，格式串两侧对称故断言只钉住形态）
    std::tm local{};
    local.tm_year = 2026 - 1900;
    local.tm_mon = 2;
    local.tm_mday = 4;
    local.tm_hour = 8;
    local.tm_min = 30;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    const std::time_t now = std::mktime(&local);

    const std::string p =
        buildTaskDraftSystemPrompt(now, {"todo.list", "hot.list", "app.notify"});
    DOCTEST_CHECK(p.find(module::agent::currentTimePromptLine(now)) != std::string::npos);
    DOCTEST_CHECK(p.find("当前本地时间：") != std::string::npos);
    for (const char* type : {"manual", "schedule", "resident", "event", "daily",
                             "once"}) {
        DOCTEST_CHECK(p.find(std::string("- ") + type + "：") != std::string::npos);
    }
    DOCTEST_CHECK(p.find("todo.list、hot.list、app.notify") != std::string::npos);
    DOCTEST_CHECK(p.find("JSON") != std::string::npos);
    DOCTEST_CHECK(p.find("needsUserInput") != std::string::npos);
    DOCTEST_CHECK(p.find("分 时 日 月 周") != std::string::npos);
    DOCTEST_CHECK(p.find("allowDestructive") != std::string::npos);
    // 空工具表不得留成空白让人猜
    const std::string empty =
        buildTaskDraftSystemPrompt(now, {});
    DOCTEST_CHECK(empty.find("没有可用工具") != std::string::npos);
    return true;
})

// ---- requestTaskDraft：一次纯文本补全 ----

REGISTER_TEST(draft_request_sends_prompt_and_joins_text_blocks,
              "草稿: requestTaskDraft 走纯文本补全（toolsJson 空）、systemPrompt 带时间行、"
              "多个 text 块按序拼接（夹着的 tool_use 不混入）", {
    MockLlm llm;
    llm.reply = core::Result<core::LlmResponse>::ok(
        responseWith(blocksJsonWithToolUse({"第一段草稿", " 第二段补充"})));
    const std::time_t now = std::time(nullptr);

    auto r = requestTaskDraft(llm, "每天八点帮我盯热榜", now, "default");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value(), std::string("第一段草稿 第二段补充"));

    DOCTEST_CHECK(llm.sentToolsJson.empty());   // 不声明 tools = 纯文本补全
    DOCTEST_CHECK_EQ(llm.sentModel, std::string("default"));
    DOCTEST_REQUIRE(llm.sentMessages.size() == 1u);
    DOCTEST_CHECK_EQ(llm.sentMessages[0].role, std::string("user"));
    // user 消息 = 单个 text block；含引号的中文描述必须被转义成合法 blocks 数组
    const std::string content = llm.sentMessages[0].contentJson;
    auto parsed = platform::json::parse(content);
    DOCTEST_REQUIRE(parsed.isOk());
    DOCTEST_REQUIRE(parsed.value().isArray());
    DOCTEST_REQUIRE(parsed.value().size() == 1);
    DOCTEST_CHECK(parsed.value()[0].isObject());
    auto block = parsed.value()[0].asObject();
    DOCTEST_CHECK_EQ(block.get("type").asString(), std::string("text"));
    DOCTEST_CHECK_EQ(block.get("text").asString(),
                     std::string("每天八点帮我盯热榜"));

    DOCTEST_CHECK(llm.sentSystemPrompt.find(module::agent::currentTimePromptLine(now)) !=
                  std::string::npos);
    return true;
})

REGISTER_TEST(draft_request_prompt_lists_registered_tools,
              "草稿: requestTaskDraft 的 system prompt 列出了注册表里的工具名", {
    class NoopTool : public core::ITool {
    public:
        std::string name() const override { return "draft.noop.tool"; }
        std::string schemaJson() const override { return "{}"; }
        core::Result<core::ToolResult> execute(const core::ToolArgs&) override {
            return core::Result<core::ToolResult>::ok(core::ToolResult{});
        }
    };
    auto reg = core::ToolRegistry::instance().registerTool(std::make_shared<NoopTool>());
    DOCTEST_REQUIRE(reg.isOk());
    struct Unreg {
        ~Unreg() { core::ToolRegistry::instance().unregisterTool("draft.noop.tool"); }
    } unreg;

    MockLlm llm;
    llm.reply = core::Result<core::LlmResponse>::ok(
        responseWith(blocksJson({std::string(kScheduleJson)})));
    auto r = requestTaskDraft(llm, "每天八点帮我盯热榜", std::time(nullptr), "sonnet");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(llm.sentSystemPrompt.find("draft.noop.tool") != std::string::npos);
    return true;
})

REGISTER_TEST(draft_request_fails_on_llm_error,
              "草稿: 端点失败原样上抛（不做静默降级）", {
    MockLlm llm;
    llm.reply = core::Result<core::LlmResponse>::fail(
        core::ErrorCode::NetworkTimeout, "上游超时");
    auto r = requestTaskDraft(llm, "随便什么", std::time(nullptr), "default");
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK_EQ(r.errorMessage(), std::string("上游超时"));
    return true;
})

REGISTER_TEST(draft_request_fails_on_empty_response,
              "草稿: 无 text 块的响应（只有 tool_use / 空数组）→ fail「模型未返回内容」", {
    const std::string onlyToolUse =
        R"([{"type":"tool_use","name":"todo.list","input":{}}])";
    const std::vector<std::string> emptyish = {"", "[]", onlyToolUse};
    for (const std::string& content : emptyish) {
        MockLlm llm;
        llm.reply =
            core::Result<core::LlmResponse>::ok(responseWith(content));
        auto r = requestTaskDraft(llm, "随便什么", std::time(nullptr), "default");
        DOCTEST_CHECK(r.isFail());
        DOCTEST_CHECK_EQ(r.errorMessage(), std::string("模型未返回内容"));
    }
    return true;
})

REGISTER_TEST(draft_request_then_parse_roundtrip,
              "草稿: requestTaskDraft 拿到的原文能直接喂 parseTaskDraft（两层可组合）", {
    MockLlm llm;
    const std::string json =
        R"({"goal":"到点提醒我开会。","contextNote":"","triggerType":"once",)"
        R"("cron":"","onceLocal":"2099-01-02 08:00","maxIterations":10,)"
        R"("allowDestructive":false,"needsUserInput":[]})";
    llm.reply = core::Result<core::LlmResponse>::ok(
        responseWith(blocksJson({"思考中……", json})));
    auto raw = requestTaskDraft(llm, "明早八点提醒我开会", std::time(nullptr), "default");
    DOCTEST_REQUIRE(raw.isOk());
    auto parsed = parseTaskDraft(raw.value(), {"app.notify"});
    DOCTEST_REQUIRE(parsed.isOk());
    DOCTEST_CHECK(parsed.value().goalFilled);
    DOCTEST_CHECK(parsed.value().triggerFilled);
    DOCTEST_CHECK_EQ(parsed.value().draft.triggerType, std::string("once"));
    return true;
})

// ---- 搬移的时间纯函数（task_draft_parse.h）行为不变量 ----

REGISTER_TEST(draft_once_parse_strict_format,
              "草稿: parseOnceAtLocalText 严格格式/字段范围/不存在日期/DST 判定", {
    DOCTEST_CHECK(module::agent::parseOnceAtLocalText("2099-03-04 08:30").isOk());
    DOCTEST_CHECK(module::agent::parseOnceAtLocalText("  2099-03-04 08:30  ").isOk());
    for (const char* bad : {"2099-3-04 08:30", "2099-03-04 08:3", "2099/03/04 08:30",
                            "abcd-03-04 08:30", "2099-13-04 08:30",
                            "2099-02-30 08:30", "2099-03-04 24:00", "2099-03-04 08:60",
                            "0000-03-04 08:30"}) {
        DOCTEST_CHECK(module::agent::parseOnceAtLocalText(bad).isFail());
    }
    return true;
})

REGISTER_TEST(draft_default_once_text_is_now_plus_hour,
              "草稿: defaultOnceAtText(now) = now+1h 取整到分钟（时钟由调用方注入）", {
    const std::time_t base = std::time(nullptr) / 60 * 60;   // 对齐到整分
    const std::string text = module::agent::defaultOnceAtText(
        static_cast<int64_t>(base));
    auto parsed = module::agent::parseOnceAtLocalText(text);
    DOCTEST_REQUIRE(parsed.isOk());
    DOCTEST_CHECK_EQ(parsed.value(), static_cast<int64_t>(base) + 3600);
    DOCTEST_CHECK_EQ(text.size(), 16u);
    return true;
})

REGISTER_TEST(draft_trim_copy_is_ascii_whitespace_only,
              "草稿: trimCopy 只裁 ASCII 空白（全角空格保留）", {
    DOCTEST_CHECK_EQ(module::agent::trimCopy("  a b \r\n"),
                     std::string("a b"));
    DOCTEST_CHECK_EQ(module::agent::trimCopy("\t\t"), std::string(""));
    // U+3000 是三字节，不在裁剪集合里
    const std::string fullWidth = "\xE3\x80\x80x\xE3\x80\x80";
    DOCTEST_CHECK_EQ(module::agent::trimCopy(fullWidth), fullWidth);
    return true;
})