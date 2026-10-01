// src/core/tool.h
// 智能体工具调用中介（依赖反转）：core 定义抽象，各 module_* 实现并注册，
// module_agent 经 ToolRegistry 按名调用。agent 不依赖任何具体模块，
// 具体模块也不依赖 agent —— 双方都只依赖 core。符合 spec §4.2 模块隔离铁律。
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/result.h"

namespace core {

// 工具参数：原始 JSON 串（Anthropic tool_use.input 的 JSON 文本）。
// 由 ITool 实现方自行用 platform::json 解析；core 不依赖 platform::json。
struct ToolArgs {
    std::string json;
};

struct ToolResult {
    bool isError = false;       // true = 回喂 LLM 时标 is_error（Anthropic tool_result）
    std::string contentJson;    // JSON 串（建议 {"content": ...} 或直接值）
};

// 工具接口：每个内置工具实现一个。name() 全局唯一（注册时冲突拒）。
// schemaJson() 返回 Anthropic tools 定义片段（name/description/input_schema），
// 由 AgentLoop 在请求里带上。execute() 同步执行，返回结果回喂 LLM。
// execute() 应返回 Result fail 而非抛异常（见 §10 工具异常语义）。
class ITool {
public:
    virtual ~ITool() = default;
    virtual std::string name() const = 0;           // 如 "todo.create"
    virtual std::string schemaJson() const = 0;     // Anthropic tool 定义 JSON
    // 破坏性工具（删数据 / 切配置等不可逆副作用）覆写返回 true。AgentLoop 在
    // task.allowDestructive=0 时遇破坏性工具直接终止 run（spec §5.3：不挂起确认）。
    // 默认 false = 只读 / 写但可逆。M2 的 todo.delete / md.delete 等再覆写。
    virtual bool isDestructive() const { return false; }
    virtual Result<ToolResult> execute(const ToolArgs& args) = 0;
};

// 注册表（进程级单例，同 ClaudeService::global() 模式）。module_* 在 onStart
// 调 register()，onStop 调 unregister()。AgentLoop 经 find() 取用。
// 线程安全：读写各一把锁（onStart/onStop 主线程写，AgentLoop 工作线程读）。
class ToolRegistry {
public:
    static ToolRegistry& instance();

    // 注册：name 冲突 → fail（ValidationError）。重复注册同一指针 = no-op。
    Result<void> registerTool(std::shared_ptr<ITool> tool);
    void unregisterTool(const std::string& name);

    // 查找：返回 shared_ptr（keep-alive）——模块运行时可插拔（设置页模块开关），
    // 裸指针在 find 与 execute 之间被 unregister 会 use-after-free。
    // 调用方持有 shared_ptr 期间 unregister 不悬垂。不存在 → fail（NotFound）。
    Result<std::shared_ptr<ITool>> find(const std::string& name) const;

    // 全部工具的 schemaJson（拼成 Anthropic tools 数组）
    std::string allSchemasJson() const;

private:
    ToolRegistry() = default;
    std::map<std::string, std::shared_ptr<ITool>> tools_;
    // 单锁：register/unregister/find/allSchemasJson 全部经这把 mutex 串行化
    //（onStart/onStop 主线程写，AgentLoop 工作线程读；见 §10 线程模型）
    mutable std::mutex mutex_;
};

} // namespace core
