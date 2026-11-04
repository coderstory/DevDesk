// tests/test_modules/test_agent_task_templates.cpp
// 出厂任务模板表（agent_task_templates.h）的定向用例：每张模板一条——字段完整可建任务
//（名称/提示词非空、触发类型合法、schedule 型 cron 非空且过 parseCron、manual 型 cron 留空），
// 且 goal 里引用的工具名全部在生产注册清单内（防臆造工具名——模板文案改出未注册工具即红）。
// 注册清单口径（need 219 预设批后）：registerAgentBuiltinTools 14 件 + AgentModule
// 同款工厂 app.now / app.notify / agent.runs + todo×6 / claude×3（各自模块批次类真
// 注册）+ schedule.runs / memory.stats / memory.health / mcp.health（预设批新工具，
// 生产工厂真注册，store 挂 TestDb 临时库——本文件只取「名字在注册清单」语义，
// 不触发执行）。
// 用例名前缀 agent_task_tpl_（--filter agent_task_tpl 可定向跑）。
#include "doctest.h"

#include "test_support.h"

#include "core/tool.h"
#include "module_agent/agent_builtin_tools.h"
#include "module_agent/agent_runs_tool.h"
#include "module_agent/agent_store.h"
#include "module_agent/agent_task_templates.h"
#include "module_agent/app_notify_tool.h"
#include "module_agent/app_now_tool.h"
#include "module_agent/cron.h"
#include "module_agent/task_draft_job.h"
#include "module_claude_proxy/claude_service.h"
#include "module_claude_proxy/claude_tools.h"
#include "module_mcp/mcp_health_tool.h"
#include "module_memory/agent_tools.h"
#include "module_schedule/schedule_runs_tool.h"
#include "module_schedule/schedule_store.h"
#include "module_todo/todo_service.h"
#include "module_todo/todo_tools.h"

#include <memory>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace {

std::set<std::string> registeredToolNames() {
    std::set<std::string> out;
    for (const auto& t : core::ToolRegistry::instance().listTools()) {
        out.insert(t->name());
    }
    return out;
}

// RAII：把本用例新注册的工具在退出（含 fail-fast 提前 return）时从进程级注册表摘除——
// ToolRegistry 不在 resetCaseGlobals 清单里，靠快照差集兜底（已注册的同名工具不动）
struct AgentToolRegistration {
    test_support::TestDb tdb;
    std::unique_ptr<module::todo::TodoService> todoSvc;
    std::unique_ptr<module::claude::ClaudeService> claudeSvc;
    std::unique_ptr<module::todo::TodoTools> todoTools;
    std::unique_ptr<module::claude::ClaudeTools> claudeTools;
    std::unique_ptr<module::schedule::ScheduleStore> scheduleStore;
    std::unique_ptr<module::agent::AgentStore> agentStore;
    std::vector<std::string> added;
    AgentToolRegistration() : tdb("agenttpl", "reg") {
        const auto before = registeredToolNames();
        module::agent::registerAgentBuiltinTools();
        core::ToolRegistry::instance().registerTool(
            std::make_shared<module::agent::NotifyTool>());
        core::ToolRegistry::instance().registerTool(module::agent::makeAppNowTool());
        // 预设批新工具：全部走生产工厂真注册（批次类先注册自家工具；store 挂
        // 临时库，注册阶段不触执行路径）
        todoSvc = std::make_unique<module::todo::TodoService>(tdb.handle());
        todoTools = std::make_unique<module::todo::TodoTools>(*todoSvc);
        todoTools->registerAll();
        claudeSvc = std::make_unique<module::claude::ClaudeService>(
            tdb.handle(), "DevDeskTestAgentTpl");
        claudeTools = std::make_unique<module::claude::ClaudeTools>(*claudeSvc);
        claudeTools->registerAll();
        scheduleStore =
            std::make_unique<module::schedule::ScheduleStore>(tdb.handle());
        core::ToolRegistry::instance().registerTool(
            module::schedule::makeScheduleRunsTool(*scheduleStore));
        agentStore =
            std::make_unique<module::agent::AgentStore>(tdb.handle());
        core::ToolRegistry::instance().registerTool(
            module::agent::makeAgentRunsTool(*agentStore));
        // memory 两工具路径注入（注册阶段不触文件系统，占位路径即可）
        core::ToolRegistry::instance().registerTool(
            module::memory::makeMemoryStatsTool("contract-tpl/proposals.db",
                                                "contract-tpl/usage_ledger.db"));
        core::ToolRegistry::instance().registerTool(
            module::memory::makeMemoryHealthTool());
        core::ToolRegistry::instance().registerTool(
            module::mcp::makeMcpHealthTool());
        const auto after = registeredToolNames();
        for (const auto& n : after) {
            if (before.count(n) == 0) added.push_back(n);
        }
    }
    ~AgentToolRegistration() {
        if (claudeTools) claudeTools->unregisterAll();
        if (todoTools) todoTools->unregisterAll();
        for (const auto& n : added) {
            core::ToolRegistry::instance().unregisterTool(n);
        }
    }
};

// goal 里的「小写字母.小写字母」记号即工具名引用；常见文件扩展名（.md 草稿路径等）
// 不是工具名。扩展名清单刻意不含 log / h——那是 git.log 等真实工具的后缀段
std::vector<std::string> toolRefsIn(const std::string& goal) {
    // 下划线段并入工具名（claude.list_profiles 全名匹配；无下划线工具不受影响）
    static const std::regex re("[a-z]+\\.[a-z_]+");
    static const std::set<std::string> kFileExts = {
        "md", "txt", "html", "json", "py", "sh", "cpp"};
    std::vector<std::string> out;
    for (auto it = std::sregex_iterator(goal.begin(), goal.end(), re);
         it != std::sregex_iterator(); ++it) {
        const std::string tok = it->str();
        const auto dot = tok.rfind('.');
        if (dot != std::string::npos && kFileExts.count(tok.substr(dot + 1)) > 0) {
            continue;
        }
        out.push_back(tok);
    }
    return out;
}

} // namespace

// 表形：恰好 13 张（原 4 + 预设批 P-01..P-09）、名称互不相同、触发类型都是 kTriggerKeys 合法键
REGISTER_TEST(agent_task_tpl_table_shape,
              "出厂任务模板：13 张、名称唯一、触发类型合法", {
    DOCTEST_REQUIRE_EQ(module::agent::kTaskTemplateCount, 13);
    std::set<std::string> names;
    for (int i = 0; i < module::agent::kTaskTemplateCount; ++i) {
        const auto& tpl = module::agent::kTaskTemplates[i];
        DOCTEST_CHECK_MESSAGE(std::string(tpl.name).find("  ") == std::string::npos,
                              "模板名称不应为空");
        names.insert(tpl.name);
        bool known = false;
        for (int k = 0; k < module::agent::kTriggerCount; ++k) {
            if (std::string(tpl.triggerType) == module::agent::kTriggerKeys[k]) {
                known = true;
                break;
            }
        }
        DOCTEST_CHECK_MESSAGE(known,
                              std::string("非法触发类型: ") + tpl.triggerType);
    }
    DOCTEST_CHECK_EQ(names.size(),
                     static_cast<size_t>(module::agent::kTaskTemplateCount));
    return true;
})

// 模板一：日报草稿——每天 18:00 定时，git.log 汇总 + fs.write 落笔记目录
REGISTER_TEST(agent_task_tpl_daily_report,
              "出厂模板·日报草稿：cron 钉 0 18 * * * 且合法；工具引用全在注册清单", {
    AgentToolRegistration tools;
    const auto& tpl = module::agent::kTaskTemplates[0];
    DOCTEST_CHECK_EQ(tpl.name, std::string("日报草稿"));
    DOCTEST_CHECK_MESSAGE(tpl.goal != nullptr && tpl.goal[0] != '\0',
                          "提示词为空");
    DOCTEST_CHECK_EQ(tpl.triggerType, std::string("schedule"));
    DOCTEST_CHECK_EQ(tpl.cron, std::string("0 18 * * *"));
    DOCTEST_CHECK(module::agent::parseCron(tpl.cron).isOk());

    const auto refs = toolRefsIn(tpl.goal);
    DOCTEST_CHECK_MESSAGE(!refs.empty(), "提示词未引用任何工具");
    const auto registered = registeredToolNames();
    for (const auto& r : refs) {
        DOCTEST_CHECK_MESSAGE(registered.count(r) > 0,
                              "提示词引用了未注册工具: " + r);
    }
    return true;
})

// 模板二：发版 Changelog——手动触发（manual 型 cron 必须留空），按最近两个 tag 区间出草稿
REGISTER_TEST(agent_task_tpl_release_changelog,
              "出厂模板·发版 Changelog：manual + cron 留空；工具引用全在注册清单", {
    AgentToolRegistration tools;
    const auto& tpl = module::agent::kTaskTemplates[1];
    DOCTEST_CHECK_EQ(tpl.name, std::string("发版 Changelog"));
    DOCTEST_CHECK_MESSAGE(tpl.goal != nullptr && tpl.goal[0] != '\0',
                          "提示词为空");
    DOCTEST_CHECK_EQ(tpl.triggerType, std::string("manual"));
    DOCTEST_CHECK_EQ(tpl.cron, std::string(""));

    const auto refs = toolRefsIn(tpl.goal);
    DOCTEST_CHECK_MESSAGE(!refs.empty(), "提示词未引用任何工具");
    const auto registered = registeredToolNames();
    for (const auto& r : refs) {
        DOCTEST_CHECK_MESSAGE(registered.count(r) > 0,
                              "提示词引用了未注册工具: " + r);
    }
    return true;
})

// 模板三：测试环境巡检——每天 9:00 定时，bash.run curl 探测 + 异常 app.notify 置顶提醒
REGISTER_TEST(agent_task_tpl_env_inspect,
              "出厂模板·测试环境巡检：cron 钉 0 9 * * * 且合法；工具引用全在注册清单", {
    AgentToolRegistration tools;
    const auto& tpl = module::agent::kTaskTemplates[2];
    DOCTEST_CHECK_EQ(tpl.name, std::string("测试环境巡检"));
    DOCTEST_CHECK_MESSAGE(tpl.goal != nullptr && tpl.goal[0] != '\0',
                          "提示词为空");
    DOCTEST_CHECK_EQ(tpl.triggerType, std::string("schedule"));
    DOCTEST_CHECK_EQ(tpl.cron, std::string("0 9 * * *"));
    DOCTEST_CHECK(module::agent::parseCron(tpl.cron).isOk());

    const auto refs = toolRefsIn(tpl.goal);
    DOCTEST_CHECK_MESSAGE(!refs.empty(), "提示词未引用任何工具");
    const auto registered = registeredToolNames();
    for (const auto& r : refs) {
        DOCTEST_CHECK_MESSAGE(registered.count(r) > 0,
                              "提示词引用了未注册工具: " + r);
    }
    return true;
})

// 模板四：变更雷达晨报——工作日 9:00 定时，git.status + git.log 巡查登记仓库三类风险
REGISTER_TEST(agent_task_tpl_change_radar,
              "出厂模板·变更雷达晨报：cron 钉 0 9 * * 1-5 且合法；git.status/git.log 锚点；工具引用全在注册清单", {
    AgentToolRegistration tools;
    // 先钉表长再取下标：红态（表变短）在此 fail-fast 停住，不越界摸 kTaskTemplates[3]
    DOCTEST_REQUIRE_EQ(module::agent::kTaskTemplateCount, 13);
    const auto& tpl = module::agent::kTaskTemplates[3];
    DOCTEST_CHECK_EQ(tpl.name, std::string("变更雷达晨报"));
    DOCTEST_CHECK_MESSAGE(tpl.goal != nullptr && tpl.goal[0] != '\0',
                          "提示词为空");
    DOCTEST_CHECK_EQ(tpl.triggerType, std::string("schedule"));
    DOCTEST_CHECK_EQ(tpl.cron, std::string("0 9 * * 1-5"));
    DOCTEST_CHECK(module::agent::parseCron(tpl.cron).isOk());

    // 提示词锚点（防臆造）：巡查动作锚已注册的 git.status / git.log（git.log 无
    // since 参数，近 24h 靠 n=50 拉取后按提交时间筛——文案与工具实参面一致）；
    // 仓库清单占位语必须在场——模板不自动探测仓库布局（需-6 边界口径）
    const std::string goal(tpl.goal);
    DOCTEST_CHECK_MESSAGE(goal.find("git.status") != std::string::npos,
                          "提示词缺少 git.status 锚点");
    DOCTEST_CHECK_MESSAGE(goal.find("git.log") != std::string::npos,
                          "提示词缺少 git.log 锚点");
    DOCTEST_CHECK_MESSAGE(goal.find("仓库根路径列表") != std::string::npos,
                          "提示词缺少仓库清单占位语");
    DOCTEST_CHECK_MESSAGE(goal.find("用户首次使用时补全") != std::string::npos,
                          "提示词缺少占位补全说明");

    const auto refs = toolRefsIn(tpl.goal);
    DOCTEST_CHECK_MESSAGE(!refs.empty(), "提示词未引用任何工具");
    const auto registered = registeredToolNames();
    for (const auto& r : refs) {
        DOCTEST_CHECK_MESSAGE(registered.count(r) > 0,
                              "提示词引用了未注册工具: " + r);
    }
    return true;
})

// 预设批（need 219 P-01..P-09，下标 4..12）：九张逐张过闸——字段完整、cron 合法、
// 工具引用全在注册清单；P-09 一键系统晨检另钉锚点（五类检查工具全引用 + 单条通知
// 口径 + 全正常不通知 + cron 钉 09:00）
REGISTER_TEST(agent_task_tpl_preset_batch,
              "出厂模板·预设批 P-01..P-09：cron 合法、工具引用全在注册清单、晨检锚点", {
    AgentToolRegistration tools;
    DOCTEST_REQUIRE_EQ(module::agent::kTaskTemplateCount, 13);
    const auto registered = registeredToolNames();
    for (int i = 4; i < module::agent::kTaskTemplateCount; ++i) {
        const auto& tpl = module::agent::kTaskTemplates[i];
        DOCTEST_CHECK_MESSAGE(tpl.name != nullptr && tpl.name[0] != '\0',
                              "预设批模板名为空");
        DOCTEST_CHECK_MESSAGE(tpl.goal != nullptr && tpl.goal[0] != '\0',
                              std::string("预设批提示词为空: ") + tpl.name);
        if (std::string(tpl.triggerType) == "schedule") {
            DOCTEST_CHECK_MESSAGE(module::agent::parseCron(tpl.cron).isOk(),
                                  std::string("cron 非法: ") + tpl.cron);
        } else {
            DOCTEST_CHECK_EQ(tpl.cron, std::string(""));
        }
        const auto refs = toolRefsIn(tpl.goal);
        DOCTEST_CHECK_MESSAGE(!refs.empty(),
                              std::string("提示词未引用任何工具: ") + tpl.name);
        for (const auto& r : refs) {
            DOCTEST_CHECK_MESSAGE(registered.count(r) > 0,
                                  "提示词引用了未注册工具: " + r);
        }
    }
    // P-09（下标 12）锚点
    DOCTEST_CHECK_EQ(module::agent::kTaskTemplates[12].cron,
                     std::string("0 9 * * *"));
    const std::string p9(module::agent::kTaskTemplates[12].goal);
    for (const char* toolName : {"memory.health", "mcp.health", "schedule.runs",
                                 "agent.runs", "todo.list", "app.notify",
                                 "fs.write"}) {
        DOCTEST_CHECK_MESSAGE(p9.find(toolName) != std::string::npos,
                              std::string("晨检提示词缺少工具锚点: ") + toolName);
    }
    DOCTEST_CHECK_MESSAGE(p9.find("只发一条") != std::string::npos,
                          "晨检提示词缺少单条通知口径");
    DOCTEST_CHECK_MESSAGE(p9.find("不发通知") != std::string::npos,
                          "晨检提示词缺少全正常不通知口径");
    return true;
})
