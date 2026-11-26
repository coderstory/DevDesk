// need#381 ITool→IMcpTool 适配层：AgentToolAdapter（薄适配器）+ ToolMirror（惰性 diff 镜像）。
// 源 = core::ToolRegistry 单例（agent 侧 ITool），目标 = core::mcp::McpToolRegistry（MCP 端口）；
// 两侧注册表都无变更通知钩子 → 触发点在 McpHttpHandler::handlePost 入口（POST 是 tools/list
// 与 tools/call 唯一入口），每 POST 一次 O(n) 集合差，空转零成本。core 零改动（依赖反转沿用
// tool.h 中介口径）。不抛异常，错误统一 Result。
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "core/mcp_tool.h"
#include "core/result.h"
#include "core/tool.h"

namespace module::mcp {

// ITool → IMcpTool 薄适配器：schema 构造期 parse 一次缓存（description/inputSchemaText），
// execute 原文透传（AgentLoop 同款同步语义）。纯转发无状态（缓存构造期只读），
// HTTP worker 多线程并发安全。
class AgentToolAdapter : public core::mcp::IMcpTool {
public:
    // source 快照拷贝持 keep-alive（与两 registry find 契约同款：注销不悬垂）
    explicit AgentToolAdapter(std::shared_ptr<core::ITool> source);

    std::string name() const override;
    std::string description() const override;
    std::string inputSchemaJson() const override;
    core::Result<core::mcp::ToolResult> execute(
        const std::string& argumentsJson) override;

    // 破坏性标记透传自源 ITool。契约：协议层服务端闸与在途上限按它分流，
    // 适配层不另判。
    bool isDestructive() const override;

    // F-05：isDestructive()=true → R"(,"annotations":{"destructiveHint":true,
    // "readOnlyHint":false})"，否则空串。spec annotations 可选，省略=合法；
    // 组装点 mcp_protocol.cpp tools/list 经 dynamic_cast 到本类型取值。
    std::string annotationsSuffix() const;

    // 无效 = schemaJson parse fail / 非 object / 无（或非法）input_schema → ToolMirror 跳过
    bool valid() const { return !inputSchemaText_.empty(); }

private:
    std::shared_ptr<core::ITool> source_;
    std::string description_;      // schema.description，缺失→空串
    std::string inputSchemaText_;  // schema.input_schema stringify；空 = 无效（F-06）
};

// 镜像同步器：源快照 listTools() 与 mirrored_ 集合差 → 增删 adapter。自身单 mutex
//（与两 registry 同款单锁风格），HTTP worker 多线程并发 sync 安全。
// 命名契约：对外名 = tool_kit::mcpToolName(源名)，须对源名单射；两个源名撞同一对外名
// 时后到者被注册表拒注（F-07 仅日志），注销按对外名执行、不区分归属——现存工具集无撞名。
class ToolMirror {
public:
    // target = MCP 端注册表（生产 = globalMcpTools()，测试可局部）；source 单例自取
    explicit ToolMirror(core::mcp::McpToolRegistry& target);

    void sync();

private:
    core::mcp::McpToolRegistry& target_;
    // 名字 → 源工具 keep-alive（指针兼作实例指纹：同名新实例靠指针比对注销重建，
    // 覆盖背靠背 unregister+register 无 POST 间隙）。key 用源侧智能体面名（如
    // todo.list），注册/注销对外名一律经 tool_kit::mcpToolName 转下划线。registerTool
    // fail（F-07 重名/非法名）与 schema 非法（F-06）的名字也记入：不重试防每请求刷
    // WARN；自愈路径 = 源工具 unregister/re-register，不依赖服务重启。
    std::map<std::string, std::shared_ptr<core::ITool>> mirrored_;
    std::mutex mutex_;
};

} // namespace module::mcp
