// agent_builtin_tools.h：智能体内置工具注册入口（UAT6 特性批）。
// 集中注册 fs.* / bash.* / git.* 共 14 件工具进 ToolRegistry，与既有
// app.now / app.stats / app.notify / net.fetch / hot.list 同口径。
// 工具全部为无状态 standalone 实现，registerAgentBuiltinTools() 由
// AgentModule::onStart() 调用一行，onStop() 不再显式注销（进程级单例
// ToolRegistry 永不析构——与既有工具同口径）。
#pragma once

namespace module::agent {

// 注册全部 14 件内置工具进 ToolRegistry。名称冲突属装配 bug，日志留痕不中断（与
// app.stats / hot.list 同口径）
void registerAgentBuiltinTools();

} // namespace module::agent