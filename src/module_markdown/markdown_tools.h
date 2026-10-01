// src/module_markdown/markdown_tools.h
// Markdown 模块的智能体工具集，供 module_agent 经 core::ToolRegistry 按名调用：
//   M1 Task 9：md.search / md.read 两个只读工具；
//   M2 Task 7：md.create（写但可逆）/ md.list_tags（只读）/ md.delete（**破坏性**，
//   isDestructive() 已覆写为 true —— AgentLoop 在 task.allowDestructive=0 时
//   遇之直接终止 run，spec §5.3）。
// 模块隔离：本文件只依赖 core/tool.h 与自身模块头，不依赖 module_agent。
//
// 错误语义（spec §10 M1 统一）：工具参数非法 / 目标不存在 → execute 返回
// Result ok + ToolResult.isError=true + 中文错误说明（错误回喂 LLM 自我纠正）；
// Result::fail 只留给编程不变量破坏。
#pragma once

#include <memory>
#include <vector>

#include "core/tool.h"
#include "markdown_service.h"

namespace module::markdown {

// 五个工具（md.search / md.read / md.create / md.list_tags / md.delete）的注册/
// 注销批次口。onStart 在 MarkdownService 构造后 registerAll()，onStop 先
// unregisterAll()（模块开关插拔时注册表随之清理）。
class MarkdownTools {
public:
    explicit MarkdownTools(MarkdownService& svc);

    // 全部注册进 core::ToolRegistry。单条失败（name 冲突等）只记日志不中断
    //——onStart 没有错误回传通道，注册表冲突属于要修的装配 bug，日志留痕。
    void registerAll();

    // 全部从注册表注销（对未注册的名字是 no-op，可安全重复调用）
    void unregisterAll();

private:
    std::vector<std::shared_ptr<core::ITool>> tools_;
};

} // namespace module::markdown
