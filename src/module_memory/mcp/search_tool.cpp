// src/module_memory/mcp/search_tool.cpp
#include "search_tool.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "core/logging.h"
#include "module_memory/panel/settings_store.h"
#include "platform/json.h"
#include "platform/utf8.h"

namespace module::memory::mcp {

namespace {

using platform::json::JsonObject;
using platform::json::JsonValue;

// MCP 是主动查询通道，超时放宽（hook 的 800ms 是「不阻断用户提问」特例，不共用）
constexpr int kMcpSearchTimeoutMs = 3000;
constexpr int kDefaultLimit = 5;
constexpr int kMaxLimit = 10;
// 输出硬顶（码点数）：limit ≤10 × 单条 4KB 分段的极端和仍可控，只防超长段落
// 烧爆客户端上下文；固定值不参数化（web_fetch 截断同口径）。
constexpr std::size_t kMaxOutputChars = 20000;

std::string schemaJson() {
    return R"({"type":"object","properties":{"query":{"type":"string","description":"检索词，用自然语言描述要找的记忆"},"limit":{"type":"integer","description":"返回条数上限，默认 5，范围 1..10"}},"required":["query"]})";
}

// 按码点数截断，不砍多字节序列中段；truncatedOut = 是否发生截断。
// module 隔离禁 include module_mcp，web_fetch 的同名函数不可共享（见方案卡 D 系）。
std::string truncateByCharsUtf8(const std::string& s, std::size_t maxChars,
                                bool* truncatedOut) {
    std::size_t chars = 0;
    std::size_t i = 0;
    while (i < s.size() && chars < maxChars) {
        const auto lead = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if (lead >= 0xF0) {
            len = 4;
        } else if (lead >= 0xE0) {
            len = 3;
        } else if (lead >= 0xC0) {
            len = 2;
        }
        if (i + len > s.size()) len = 1;  // 容错：sanitize 后不应出现
        ++chars;
        i += len;
    }
    *truncatedOut = i < s.size();
    return s.substr(0, i);
}

core::mcp::ToolResult okToolResult(std::string text) {
    return core::mcp::ToolResult{false, platform::utf8::sanitize(std::move(text))};
}

core::mcp::ToolResult errToolResult(std::string text) {
    return core::mcp::ToolResult{true, platform::utf8::sanitize(std::move(text))};
}

}  // namespace

MemorySearchTool::MemorySearchTool(
    std::shared_ptr<gateway::IMemoryRetriever> retriever)
    : retriever_(std::move(retriever)) {}

std::string MemorySearchTool::name() const { return kMemorySearchToolName; }

std::string MemorySearchTool::description() const {
    return "检索 DevDesk 本机长期记忆（构建约定、环境事实、项目结论等已沉淀条目），"
           "返回按相关度融合的记忆段落";
}

std::string MemorySearchTool::inputSchemaJson() const { return schemaJson(); }

core::Result<core::mcp::ToolResult> MemorySearchTool::execute(
    const std::string& argumentsJson) {
    auto parsed = platform::json::parse(argumentsJson);
    if (parsed.isFail() || !parsed.value().isObject()) {
        return core::Result<core::mcp::ToolResult>::ok(
            errToolResult("参数必须是 JSON 对象"));
    }
    const JsonObject args = parsed.value().asObject();
    const JsonValue queryValue = args.get("query");
    if (!queryValue.isString() || queryValue.asString().empty()) {
        return core::Result<core::mcp::ToolResult>::ok(
            errToolResult("缺少 query（必须是非空字符串）"));
    }
    int limit = kDefaultLimit;
    const JsonValue limitValue = args.get("limit");
    if (limitValue.isNumber()) {
        // JsonImpl 的 Int/Double 字段互斥写入（json.cpp valToImpl）：Int 形态只写
        // intVal、Double 形态只写 dblVal，且公开 API 不区分两形态（type 都映射
        // T_Number）。故 asInt()!=0 ⟺ Int 非零值可直取；asInt()==0 时（Int 0 或
        // 任意 Double）走 asDouble 验整值——Int 0 的 dblVal 恒 0.0 恰为真值，歧义
        // 无影响。非整值（3.7）/超范围按 PRD 忽略取默认，钳制在 int64/double 域
        // 完成后再收窄，不经 impl-defined 截断。
        const std::int64_t raw = limitValue.asInt();
        const double d = limitValue.asDouble();
        if (raw != 0) {
            std::int64_t clamped = raw;
            if (clamped < 1) clamped = 1;
            if (clamped > kMaxLimit) clamped = kMaxLimit;
            limit = static_cast<int>(clamped);
        } else if (std::floor(d) == d) {
            double clamped = d < 1.0 ? 1.0 : d;
            if (clamped > static_cast<double>(kMaxLimit)) {
                clamped = static_cast<double>(kMaxLimit);
            }
            limit = static_cast<int>(clamped);
        }
    }
    if (limit < 1) limit = 1;
    if (limit > kMaxLimit) limit = kMaxLimit;

    const auto outcome =
        retriever_->search(queryValue.asString(), limit, kMcpSearchTimeoutMs);
    if (outcome.isFail()) {
        if (outcome.errorCode() == core::ErrorCode::NetworkTimeout) {
            return core::Result<core::mcp::ToolResult>::ok(errToolResult(
                "记忆检索超时（3 秒）。引擎可能正在启动或负载高，稍后重试"));
        }
        return core::Result<core::mcp::ToolResult>::ok(errToolResult(
            "记忆引擎未运行或连接失败：" + outcome.errorMessage() +
            "。可在 DevDesk「记忆」页主控台启动引擎后重试"));
    }

    // telemem 实测唯一形态 = 单 hit 融合段落信封，原样回传不二次加工；
    // mem0 风格数组形态多 hit 时以换行连接。
    std::string text;
    const auto& hits = outcome.value();
    for (std::size_t i = 0; i < hits.size(); ++i) {
        if (i > 0) text += "\n";
        text += hits[i].text;
    }
    if (text.empty()) {
        return core::Result<core::mcp::ToolResult>::ok(
            okToolResult("未命中任何记忆"));
    }
    bool truncated = false;
    text = truncateByCharsUtf8(text, kMaxOutputChars, &truncated);
    if (truncated) {
        text += "（已截断到 " + std::to_string(kMaxOutputChars) + " 字符）";
    }
    return core::Result<core::mcp::ToolResult>::ok(okToolResult(std::move(text)));
}

core::Result<void> registerSearchTool(core::mcp::McpToolRegistry& registry) {
    auto tool = std::make_shared<MemorySearchTool>(
        std::make_shared<gateway::TelememHttpRetriever>());
    return registry.registerTool(std::move(tool));
}

void unregisterSearchTool(core::mcp::McpToolRegistry& registry) {
    registry.unregisterTool(kMemorySearchToolName);
}

core::Result<void> setSearchEnabled(core::mcp::McpToolRegistry& registry,
                                    bool on) {
    auto settings = panel::loadPanelSettings();
    settings.mcpSearchEnabled = on;
    const auto saved = panel::savePanelSettings(settings);
    if (saved.isFail()) {
        return saved;  // 写盘失败：注册态不动（不变式 注册 ⟺ mcpSearchEnabled）
    }
    if (!on) {
        unregisterSearchTool(registry);
        return core::Result<void>::ok();
    }
    auto registered = registerSearchTool(registry);
    if (registered.isFail()) {
        // 重名/冲突 = 理论态：只 LOG_ERROR 不致命，服务与其余工具不受影响
        LOG_ERROR("memory_search 注册失败: " + registered.errorMessage());
    }
    return core::Result<void>::ok();
}

bool searchEnabledBySetting() { return panel::loadPanelSettings().mcpSearchEnabled; }

}  // namespace module::memory::mcp
