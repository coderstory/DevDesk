// src/core/tool.h
// 智能体工具调用中介（依赖反转）：core 定义抽象，各 module_* 实现并注册，module_agent 经
// ToolRegistry 按名调用。agent 与具体模块互不依赖，双方都只依赖 core（模块隔离铁律）
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/result.h"

namespace core {

// 工具参数：原始 JSON 串（Anthropic tool_use.input）。由 ITool 实现方自行用 platform::json 解析
struct ToolArgs {
    std::string json;
};

struct ToolResult {
    bool isError = false;       // true = 回喂 LLM 时标 is_error（Anthropic tool_result）
    std::string contentJson;    // JSON 串（建议 {"content": ...} 或直接值）
};

// 工具接口：每个内置工具实现一个。name() 全局唯一；schemaJson() 返回 Anthropic tools 定义片段，
// 由 AgentLoop 在请求里带上；execute() 同步执行、返回 Result fail 而非抛异常
class ITool {
public:
    virtual ~ITool() = default;
    virtual std::string name() const = 0;           // 如 "todo.create"
    virtual std::string schemaJson() const = 0;     // Anthropic tool 定义 JSON
    // 破坏性工具（不可逆副作用）覆写返回 true；AgentLoop 在 allowDestructive=0 时遇之直接终止
    // run（不挂起确认）。默认 false = 只读/可逆写
    virtual bool isDestructive() const { return false; }
    virtual Result<ToolResult> execute(const ToolArgs& args) = 0;
};

// 注册表（进程级单例）：module_* 在 onStart register()、onStop unregister()，AgentLoop 经 find()
// 取用。单例永不析构（退出钩子 stopAll → onStop → unregisterTool 仍需访问它）。线程安全：单锁串行化
class ToolRegistry {
public:
    static ToolRegistry& instance();

    // 注册：name 冲突 → fail（ValidationError）。重复注册同一指针 = no-op。
    Result<void> registerTool(std::shared_ptr<ITool> tool);
    void unregisterTool(const std::string& name);

    // 查找返回 shared_ptr（keep-alive）：模块运行时可插拔，裸指针在 find 与 execute 之间被
    // unregister 会 use-after-free。不存在 → fail(NotFound)
    Result<std::shared_ptr<ITool>> find(const std::string& name) const;

    // 全部工具的 schemaJson（拼成 Anthropic tools 数组）
    std::string allSchemasJson() const;

    // 只读枚举：全部已注册工具的 shared_ptr 快照（keep-alive 同 find）。map 序（name 字典序），
    // 供 agent 页「可用工具」展示
    std::vector<std::shared_ptr<ITool>> listTools() const;

private:
    ToolRegistry() = default;
    std::map<std::string, std::shared_ptr<ITool>> tools_;
    // 单锁：register/unregister/find/allSchemasJson 全经此 mutex 串行化（主线程写/AgentLoop 线程读）
    mutable std::mutex mutex_;
};

} // namespace core
