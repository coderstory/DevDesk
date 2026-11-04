// agent.runs：智能体任务运行历史只读查询工具（need 219 F-02）。
// 数据源 = AgentStore::listRunsFiltered（agent_runs JOIN agent_tasks），零新存储；
// AgentModule 自注册（app_stats_tool 同款模式）。status 缺省排除 running =
// 自指排除（工具调用本身发生在 running run 内，排除即防自指噪音）。
#pragma once

#include <memory>

#include "core/tool.h"
#include "module_agent/agent_store.h"

namespace module::agent {

std::shared_ptr<core::ITool> makeAgentRunsTool(AgentStore& store);

} // namespace module::agent
