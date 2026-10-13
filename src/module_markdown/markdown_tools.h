// Markdown 智能体工具集（md.search / md.read / md.create / md.list_tags / md.delete）。
// md.delete 破坏性（isDestructive()=true）：allowDestructive=0 时 AgentLoop 终止 run。
// 错误语义：参数非法 / 目标不存在 → Result ok + isError + 中文说明（回喂 LLM）；
// Result::fail 只留给编程不变量破坏。模块隔离：只依赖 core/tool.h 与自身模块头。
#pragma once

#include <memory>
#include <vector>

#include "core/tool.h"
#include "markdown_service.h"

namespace module::markdown {

// 五个工具的注册/注销批次口：onStart 构造 service 后 registerAll()，onStop 先
// unregisterAll()（模块插拔时注册表随之清理）。
class MarkdownTools {
public:
    explicit MarkdownTools(MarkdownService& svc);

// 全部注册进 core::ToolRegistry。单条失败只记日志（onStart 无错误回传通道）。
    void registerAll();

// 全部注销（对未注册的名字是 no-op，可安全重复调用）
    void unregisterAll();

private:
    std::vector<std::shared_ptr<core::ITool>> tools_;
};

} // namespace module::markdown
