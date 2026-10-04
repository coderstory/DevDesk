// src/module_claude_proxy/claude_tools.h
// Claude 模块的智能体工具集（claude.list_profiles / switch_profile / test_connection /
// recent_logs），供 module_agent 经 core::ToolRegistry 调用；switch_profile 破坏性（allowDestructive=0 时终止 run）。
// 线程约定：agent 工作线程执行，与 UI 共用 ClaudeService；写方法 setActiveProfile 自带互斥锁，其余只调纯读方法。
// 错误语义：参数非法/目标不存在/调用失败 → execute 返回 ok + isError=true + 中文错误；Result::fail 只留给不变量破坏。
#pragma once

#include <memory>
#include <vector>

#include "claude_service.h"
#include "core/tool.h"

namespace module::claude {

// 四个工具的注册/注销口；onStart 注册、onStop 先注销再收尾。
class ClaudeTools {
public:
    explicit ClaudeTools(ClaudeService& svc);

    // 全部注册进 core::ToolRegistry。单条失败只记日志不中断（无错误回传通道，日志留痕）。
    void registerAll();

    // 全部从注册表注销（对未注册的名字是 no-op，可安全重复调用）
    void unregisterAll();

private:
    std::vector<std::shared_ptr<core::ITool>> tools_;
};

} // namespace module::claude
