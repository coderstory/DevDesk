#include "core/mcp_tool.h"

#include <utility>

namespace core::mcp {

namespace {

constexpr std::size_t kMaxToolNameLength = 128;

// 字符集白名单：字母/数字/下划线/点/连字符。点号允许是给「显式点号命名」留的
// 口子；内置工具仍一律下划线命名（规避客户端对点号的规范化）。
bool isValidToolName(const std::string& name) {
    if (name.empty() || name.size() > kMaxToolNameLength) return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                        c == '-';
        if (!ok) return false;
    }
    return true;
}

} // namespace

McpToolRegistry& globalMcpTools() {
    // 永不析构持有：注册表是进程级登记表，没有「必须释放」的语义。函数局部
    // static 的析构可能先于 atexit 里的 unregisterTool 执行（map/mutex 悬垂）。
    static auto& g = *new McpToolRegistry();
    return g;
}

Result<void> McpToolRegistry::registerTool(std::shared_ptr<IMcpTool> tool) {
    if (!tool) {
        return Result<void>::fail(ErrorCode::ValidationError, "MCP 工具为空指针");
    }
    const std::string name = tool->name();
    if (!isValidToolName(name)) {
        return Result<void>::fail(
            ErrorCode::ValidationError,
            "MCP 工具名非法: \"" + name + "\"（要求 1..128 个 A-Z a-z 0-9 _ . -）");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (tools_.find(name) != tools_.end()) {
        return Result<void>::fail(ErrorCode::ValidationError,
                                  "MCP 工具名冲突: " + name);
    }
    tools_.emplace(name, std::move(tool));
    return Result<void>::ok();
}

void McpToolRegistry::unregisterTool(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    tools_.erase(name);
}

Result<std::shared_ptr<IMcpTool>> McpToolRegistry::find(
    const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tools_.find(name);
    if (it == tools_.end()) {
        return Result<std::shared_ptr<IMcpTool>>::fail(
            ErrorCode::NotFound, "MCP 工具未找到: " + name);
    }
    return Result<std::shared_ptr<IMcpTool>>::ok(it->second);
}

std::vector<std::shared_ptr<IMcpTool>> McpToolRegistry::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<IMcpTool>> out;
    out.reserve(tools_.size());
    for (const auto& entry : tools_) {
        out.push_back(entry.second);
    }
    return out;
}

std::size_t McpToolRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tools_.size();
}

} // namespace core::mcp