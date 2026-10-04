// src/module_todo/todo_tools.h
// Todo 模块的智能体工具集：把 TodoService 的任务操作暴露成 6 个 ITool，供
// module_agent 经 core::ToolRegistry 按名调用。
// 模块隔离：本文件只依赖 core/tool.h 与自身模块头，不依赖 module_agent——
// 双方都只依赖 core（依赖反转，见 src/core/tool.h 头注释）。
// 错误语义（spec §10 统一）：工具参数非法 / 目标不存在 → execute 返回
// Result ok + ToolResult.isError=true + 中文错误说明（错误回喂 LLM 自我纠正）；
// Result::fail 只留给编程不变量破坏（如自有拼接 JSON 校验失败）。
#pragma once

#include <memory>
#include <vector>

#include "core/tool.h"
#include "todo_service.h"

namespace module::todo {

// 6 个工具（todo.list / todo.create / todo.update / todo.move / todo.complete /
// todo.delete）的注册/注销批次口。onStart 在 TodoService 构造后 registerAll()，
// onStop 先 unregisterAll() 再放 service（模块开关插拔时注册表随之清理）。
class TodoTools {
public:
    explicit TodoTools(TodoService& svc);

    // 全部注册进 core::ToolRegistry。单条失败（name 冲突等）只记日志不中断
    //——onStart 没有错误回传通道，注册表冲突属于要修的装配 bug，日志留痕。
    void registerAll();

    // 全部从注册表注销（对未注册的名字是 no-op，可安全重复调用）
    void unregisterAll();

private:
    std::vector<std::shared_ptr<core::ITool>> tools_;
};

} // namespace module::todo
