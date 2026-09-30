// src/module_agent/app_stats_tool.h
// AppStatsTool（agent M1 Task 10）：只读应用统计工具 "app.stats"——直查
// todo_tasks / markdown_index / agent_tasks 三表行数，给 LLM 一个应用全局
// 状态快照（agent 决定动哪些工具前的背景信息）。
// 不挂模块生命周期：Task 12 由 AgentModule 持有注册（本文件只提供工厂）。
//
// 错误语义（spec §10 M1 统一）：查询失败 → execute 返回 Result ok +
// ToolResult.isError=true + 中文说明回喂；Result::fail 只留给编程不变量破坏。
#pragma once

#include <memory>

#include "core/sqlite_db.h"
#include "core/tool.h"

namespace module::agent {

// 工厂：产出 "app.stats" 只读工具。无参数（input_schema 空对象）。
std::shared_ptr<core::ITool> makeAppStatsTool(core::data::SqliteDb& db);

} // namespace module::agent
