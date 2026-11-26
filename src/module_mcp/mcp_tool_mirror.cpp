#include "module_mcp/mcp_tool_mirror.h"

#include <cstddef>
#include <utility>
#include <vector>

#include "core/logging.h"
#include "platform/json.h"
#include "platform/tool_kit.h"
#include "platform/utf8.h"

namespace module::mcp {

namespace {

// 单条 tool_result text 硬上限，与 agent_loop.cpp:50 kMaxToolResultBytes 同值
constexpr std::size_t kMaxToolResultBytes = 32 * 1024;
constexpr const char* kTruncatedMarker = "\n[truncated]";

// contentJson → text：与 agent_loop.cpp:78 toolResultText 同款形态（D-05 落定：语义
// 与 AgentLoop 调用一致）。一处偏离：stringify fail 不 abort（HTTP worker 禁 abort），
// 降级原文 + LOG_WARN。先 UTF-8 归一再按字节上限截断（切点回退字符边界）。
std::string mapResultText(const std::string& contentJson) {
    std::string text;
    if (!contentJson.empty()) {
        auto cv = platform::json::parse(contentJson);
        if (cv.isOk() && cv.value().isString()) {
            text = cv.value().asString();
        } else if (cv.isOk()) {
            auto s = platform::json::stringify(cv.value());
            if (s.isFail()) {
                LOG_WARN("MCP: 工具结果文本序列化失败，降级原文回传");
                text = contentJson;
            } else {
                text = s.value();
            }
        } else {
            text = contentJson;
        }
    }
    if (!platform::utf8::isValid(text)) {
        LOG_WARN("MCP: 工具文本含非法 UTF-8 字节，已替换为 U+FFFD（" +
                 std::to_string(text.size()) + " 字节）");
        text = platform::utf8::sanitize(text);
    }
    if (text.size() > kMaxToolResultBytes) {
        // 切点可能落在多字节字符中间 → 回退到字符边界（agent_loop.cpp:104 同款）
        std::size_t cut = kMaxToolResultBytes;
        while (cut > 0 &&
               (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        text = text.substr(0, cut) + kTruncatedMarker;
    }
    return text;
}

} // namespace

AgentToolAdapter::AgentToolAdapter(std::shared_ptr<core::ITool> source)
    : source_(std::move(source)) {
    // schemaJson 静态（ITool 契约），构造期 parse 一次缓存；非法即 F-06 交 ToolMirror 跳过
    auto schema = platform::json::parse(source_->schemaJson());
    if (schema.isFail() || !schema.value().isObject()) {
        return;
    }
    const auto description = schema.value().asObject().get("description");
    if (description.isString()) {
        description_ = description.asString();
    }
    const auto inputSchema = schema.value().asObject().get("input_schema");
    if (inputSchema.isObject()) {
        auto text = platform::json::stringify(inputSchema);
        if (text.isOk()) {
            inputSchemaText_ = text.value();
        }
    }
}

std::string AgentToolAdapter::name() const {
    // MCP 对外面唯一命名规则 = 点号/短横换下划线（platform::tool_kit::mcpToolName
    // 单点实现）；ToolMirror 的注销名必须走同一映射，两处各改一半会留孤儿注册。
    return platform::tool_kit::mcpToolName(source_->name());
}

std::string AgentToolAdapter::description() const { return description_; }

std::string AgentToolAdapter::inputSchemaJson() const {
    return inputSchemaText_;
}

bool AgentToolAdapter::isDestructive() const { return source_->isDestructive(); }

std::string AgentToolAdapter::annotationsSuffix() const {
    if (!isDestructive()) {
        return "";
    }
    return R"(,"annotations":{"destructiveHint":true,"readOnlyHint":false})";
}

core::Result<core::mcp::ToolResult> AgentToolAdapter::execute(
    const std::string& argumentsJson) {
    // argumentsJson 原文透传（不 parse/改写）；fail 原样透传（协议层映射 -32603）
    auto r = source_->execute(core::ToolArgs{argumentsJson});
    if (r.isFail()) {
        return core::Result<core::mcp::ToolResult>::fail(r.errorCode(),
                                                         r.errorMessage());
    }
    const core::ToolResult& tr = r.value();
    return core::Result<core::mcp::ToolResult>::ok(
        core::mcp::ToolResult{tr.isError, mapResultText(tr.contentJson)});
}

ToolMirror::ToolMirror(core::mcp::McpToolRegistry& target) : target_(target) {}

void ToolMirror::sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    // 源快照（keep-alive，map 序）；与 mirrored_ 双向集合差 O(n)
    std::map<std::string, std::shared_ptr<core::ITool>> next;
    for (const auto& tool : core::ToolRegistry::instance().listTools()) {
        if (!tool) continue;
        const std::string& name = tool->name();
        auto known = mirrored_.find(name);
        // 新名字，或同名新实例（背靠背 unregister+register 同名重建、中间无
        // POST——settings_module stop/start 是现实触发面）：先注销旧镜像再重建
        // adapter（同名新实例靠指针比对重建，符合 v4 自愈契约）
        if (known == mirrored_.end() || known->second != tool) {
            if (known != mirrored_.end()) {
                target_.unregisterTool(platform::tool_kit::mcpToolName(name));
            }
            auto adapter = std::make_shared<AgentToolAdapter>(tool);
            if (!adapter->valid()) {
                // F-06：schema 静态非法，不重试；名字仍入 mirrored_ 防刷日志
                LOG_WARN("MCP 镜像跳过 schema 非法的工具: " + name);
            } else if (auto reg = target_.registerTool(adapter); reg.isFail()) {
                // F-07：重名/非法名拒注，不重试；自愈=源侧 unregister/re-register
                LOG_WARN("MCP 镜像注册失败 " + name + ": " + reg.errorMessage());
            }
        }
        next.emplace(name, tool);
    }
    // 消失名：注销镜像并移除条目（源侧 unregister/re-register 依此自愈）
    for (const auto& entry : mirrored_) {
        if (next.find(entry.first) == next.end()) {
            target_.unregisterTool(platform::tool_kit::mcpToolName(entry.first));
        }
    }
    mirrored_ = std::move(next);
}

} // namespace module::mcp
