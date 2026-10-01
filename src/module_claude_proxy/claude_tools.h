// src/module_claude_proxy/claude_tools.h
// Claude 模块的智能体工具集，供 module_agent 经 core::ToolRegistry 按名调用
//（agent M2 Task 8，工具清单见 spec §7）：
//   claude.list_profiles    只读：全部 Profile + 哪一个是激活
//   claude.switch_profile   **破坏性**（isDestructive 已覆写为 true）：切激活
//                           Profile；代理运行中还会同步接管 settings.json，
//                           AgentLoop 在 task.allowDestructive=0 时遇之直接终止
//                           run（spec §5.3：后台自动化不挂起等确认）
//   claude.test_connection  只读（发一次测试请求，spec §5.3）：GET /v1/models
//   claude.recent_logs      只读：最近代理调用日志
// 模块隔离：本文件只依赖 core/tool.h 与自身模块头，不依赖 module_agent。
//
// 线程安全（spec §10 线程模型 / Review Focus 5）：工具由 agent 工作线程执行，
// 而同一 ClaudeService 实例也被 UI 线程使用。写方法 setActiveProfile 自带服务内
// 互斥锁（claude_service.h 的 switchMutex_）；本文件其余三个工具只调纯读方法
//（listProfiles / getActiveProfile / getProfile / loadApiKey / recentCalls），
// 按 claude_service.h 既有裁定随调用方 SqliteDb recursive_mutex 串行化——服务层
// 只为 setActiveProfile 加锁，读方法零改动。
//
// 错误语义（spec §10 M1 统一，与 MarkdownTools 同款）：工具参数非法 / 目标不存在
// / 服务调用失败 → execute 返回 Result ok + ToolResult.isError=true + 中文错误
// 说明（错误回喂 LLM 自我纠正）；Result::fail 只留给编程不变量破坏。
#pragma once

#include <memory>
#include <vector>

#include "claude_service.h"
#include "core/tool.h"

namespace module::claude {

// 四个工具（list_profiles / switch_profile / test_connection / recent_logs）的
// 注册/注销批次口。onStart 在 ClaudeService 就绪后 registerAll()；onStop 先
// unregisterAll()（模块开关插拔时注册表随之清理，工具不活过收尾）。
class ClaudeTools {
public:
    explicit ClaudeTools(ClaudeService& svc);

    // 全部注册进 core::ToolRegistry。单条失败（name 冲突等）只记日志不中断
    //——onStart 没有错误回传通道，注册表冲突属于要修的装配 bug，日志留痕。
    void registerAll();

    // 全部从注册表注销（对未注册的名字是 no-op，可安全重复调用）
    void unregisterAll();

private:
    std::vector<std::shared_ptr<core::ITool>> tools_;
};

} // namespace module::claude
