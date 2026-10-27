#include "mcp_tools_builtin.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/logging.h"
#include "mcp_protocol.h"

namespace module::mcp {

namespace {

#ifdef DEVDESK_VERSION
constexpr const char* kDevDeskVersion = DEVDESK_VERSION;
#else
constexpr const char* kDevDeskVersion = "dev";
#endif

#ifdef DEVDESK_BUILD_TYPE
constexpr const char* kDevDeskBuildType = DEVDESK_BUILD_TYPE;
#else
constexpr const char* kDevDeskBuildType = "unknown";
#endif

#ifdef DEVDESK_PLATFORM
constexpr const char* kDevDeskPlatform = DEVDESK_PLATFORM;
#else
constexpr const char* kDevDeskPlatform = "unknown";
#endif

// devdesk_server_info：无参数，返回固定顺序的 Markdown 列表。工具清单来自构造时
// 注入的注册表引用（模块隔离：不读其它模块数据、不读全局注册表）。
class ServerInfoTool final : public core::mcp::IMcpTool {
public:
    explicit ServerInfoTool(core::mcp::McpToolRegistry& registry)
        : registry_(registry) {}

    std::string name() const override { return "devdesk_server_info"; }

    std::string description() const override {
        return "查询 DevDesk 版本、构建类型、平台、支持的 MCP 协议版本与已注册工具列表";
    }

    std::string inputSchemaJson() const override {
        return R"({"type":"object","properties":{}})";
    }

    core::Result<core::mcp::ToolResult> execute(
        const std::string& /*argumentsJson*/) override {
        std::string text;
        text += "- DevDesk 版本：";
        text += kDevDeskVersion;
        text += "\n- 构建类型：";
        text += kDevDeskBuildType;
        text += "\n- 平台：";
        text += kDevDeskPlatform;
        text += "\n- 支持的 MCP 协议版本：";
        text += kMcpProtocolVersion;
        text += "、";
        text += kMcpProtocolVersionLegacy;

        const std::vector<std::shared_ptr<core::mcp::IMcpTool>> tools =
            registry_.list();
        text += "\n- 已注册工具：";
        text += std::to_string(tools.size());
        text += " 个";
        if (!tools.empty()) {
            text += "（";
            for (std::size_t i = 0; i < tools.size(); ++i) {
                if (i > 0) text += ", ";
                text += tools[i]->name();
            }
            text += "）";
        }

        return core::Result<core::mcp::ToolResult>::ok(
            core::mcp::ToolResult{false, std::move(text)});
    }

private:
    core::mcp::McpToolRegistry& registry_;
};

} // namespace

void registerBuiltinMcpTools(core::mcp::McpToolRegistry& registry,
                             Fetcher /*fetcher*/) {
    auto result =
        registry.registerTool(std::make_shared<ServerInfoTool>(registry));
    if (result.isFail()) {
        LOG_ERROR("MCP 内置工具注册失败: " + result.errorMessage());
    }
}

} // namespace module::mcp