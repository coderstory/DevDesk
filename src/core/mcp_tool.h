// MCP 能力接缝（依赖反转）：core 定义工具抽象与注册表，各 module_* 实现并在
// onStart/onStop 注册/注销；module_mcp 只做协议与生命周期，不持有业务能力。
// 硬约束：core 不依赖 platform —— inputSchemaJson() 的合法性（能否解析为 JSON
// 对象）由 module_mcp 在组装 tools/list 时校验，单工具非法只跳过该工具。
#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/result.h"

namespace core::mcp {

struct ToolResult {
    bool isError = false;   // true = 工具业务失败；仍以文本形式回给客户端
    std::string text;       // 纯文本结果
};

// 工具接口：execute 在 HTTP worker 线程执行 —— 实现方自行保证线程安全、禁止
// 触 UI、长任务自行限时（stop/restart 会等待在途调用结束）。
class IMcpTool {
public:
    virtual ~IMcpTool() = default;
    virtual std::string name() const = 0;             // 全局唯一；内置工具一律下划线命名
    virtual std::string description() const = 0;      // 一句话说明
    virtual std::string inputSchemaJson() const = 0;  // JSON 对象文本（MCP inputSchema）
    // argumentsJson 恒为 JSON 对象文本；返回 fail 表示基础设施/实现错误（映射为
    // -32603），业务失败请返回 ToolResult{isError=true}
    virtual Result<ToolResult> execute(const std::string& argumentsJson) = 0;
};

// 注册表：单锁串行化，只保证查找/注册/注销线程安全；find 返回 shared_ptr 保持
// 生命周期，注销瞬间不会让在途 execute 悬垂。
class McpToolRegistry {
public:
    // name 只允许 1..128 个 [A-Za-z0-9_./-]；空指针 / 非法名 / 重名 → fail(ValidationError)
    Result<void> registerTool(std::shared_ptr<IMcpTool> tool);
    void unregisterTool(const std::string& name);

    // 不存在 → fail(NotFound)；返回 keep-alive
    Result<std::shared_ptr<IMcpTool>> find(const std::string& name) const;

    // 全部已注册工具快照，按 name 字典序（std::map 序遍历）
    std::vector<std::shared_ptr<IMcpTool>> list() const;
    std::size_t size() const;

private:
    std::map<std::string, std::shared_ptr<IMcpTool>> tools_;
    mutable std::mutex mutex_;
};

// 进程级单例访问器。永不析构持有：退出钩子里的 unregister 仍需访问 map/mutex，
// 不能写成函数局部 static（atexit LIFO 会先析构它）。
McpToolRegistry& globalMcpTools();

} // namespace core::mcp