// 只读应用统计工具 "app.stats"：直查 todo_tasks / markdown_index / agent_tasks
// 三表行数，给 LLM 一个应用全局状态快照。不挂模块生命周期，由 AgentModule 持有注册。
// 错误语义：查询失败 → Result ok + isError=true + 中文回喂；Result::fail 只留给编程不变量破坏
#pragma once

#include <memory>

#include "core/sqlite_db.h"
#include "core/tool.h"

namespace module::agent {

// 工厂：产出 "app.stats" 只读工具，无参数
std::shared_ptr<core::ITool> makeAppStatsTool(core::data::SqliteDb& db);

} // namespace module::agent
