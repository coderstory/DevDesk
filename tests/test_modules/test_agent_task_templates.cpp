// tests/test_modules/test_agent_task_templates.cpp
// 出厂任务模板表（agent_task_templates.h）的定向用例：每张模板一条——字段完整可建任务
//（名称/提示词非空、触发类型合法、schedule 型 cron 非空且过 parseCron、manual 型 cron 留空），
// 且 goal 里引用的工具名全部在生产注册清单内（防臆造工具名——模板文案改出未注册工具即红）。
// 注册清单口径：registerAgentBuiltinTools 的 14 件 + AgentModule::onStart 同款工厂的
// app.now / app.notify（模板当前只引用这三家；hot.list / net.fetch / app.stats 未被引用，
// 若将来模板引用它们，在本文件补注册后再加文案）。
// 用例名前缀 agent_task_tpl_（--filter agent_task_tpl 可定向跑）。
#include "doctest.h"

#include "core/tool.h"
#include "module_agent/agent_builtin_tools.h"
#include "module_agent/agent_task_templates.h"
#include "module_agent/app_notify_tool.h"
#include "module_agent/app_now_tool.h"
#include "module_agent/cron.h"
#include "module_agent/task_draft_job.h"

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
    std::vector<std::string> added;
    AgentToolRegistration() {
        const auto before = registeredToolNames();
        module::agent::registerAgentBuiltinTools();
        core::ToolRegistry::instance().registerTool(
            std::make_shared<module::agent::NotifyTool>());
        core::ToolRegistry::instance().registerTool(module::agent::makeAppNowTool());
        const auto after = registeredToolNames();
        for (const auto& n : after) {
            if (before.count(n) == 0) added.push_back(n);
        }
    }
    ~AgentToolRegistration() {
        for (const auto& n : added) {
            core::ToolRegistry::instance().unregisterTool(n);
        }
    }
};

// goal 里的「小写字母.小写字母」记号即工具名引用；常见文件扩展名（.md 草稿路径等）
// 不是工具名。扩展名清单刻意不含 log / h——那是 git.log 等真实工具的后缀段
std::vector<std::string> toolRefsIn(const std::string& goal) {
    static const std::regex re("[a-z]+\\.[a-z]+");
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

// 表形：恰好 4 张、名称互不相同、触发类型都是 kTriggerKeys 合法键
REGISTER_TEST(agent_task_tpl_table_shape,
              "出厂任务模板：4 张、名称唯一、触发类型合法", {
    DOCTEST_REQUIRE_EQ(module::agent::kTaskTemplateCount, 4);
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
    // 先钉表长再取下标：红态（表仍 3 张）在此 fail-fast 停住，不越界摸 kTaskTemplates[3]
    DOCTEST_REQUIRE_EQ(module::agent::kTaskTemplateCount, 4);
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
