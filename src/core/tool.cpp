// src/core/tool.cpp
#include "core/tool.h"

namespace core {

ToolRegistry& ToolRegistry::instance() {
    static ToolRegistry g;
    return g;
}

Result<void> ToolRegistry::registerTool(std::shared_ptr<ITool> tool) {
    if (!tool || tool->name().empty()) {
        return Result<void>::fail(ErrorCode::ValidationError, "工具为空或 name 为空");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tools_.find(tool->name());
    if (it != tools_.end()) {
        if (it->second == tool) return Result<void>::ok();   // 重复注册同一指针 = no-op
        return Result<void>::fail(ErrorCode::ValidationError,
                                  "工具名冲突: " + tool->name());
    }
    tools_.emplace(tool->name(), std::move(tool));
    return Result<void>::ok();
}

void ToolRegistry::unregisterTool(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    tools_.erase(name);
}

Result<std::shared_ptr<ITool>> ToolRegistry::find(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tools_.find(name);
    if (it == tools_.end()) {
        return Result<std::shared_ptr<ITool>>::fail(
            ErrorCode::NotFound, "工具未找到: " + name + "（模块已禁用？）");
    }
    return Result<std::shared_ptr<ITool>>::ok(it->second);   // keep-alive
}

std::string ToolRegistry::allSchemasJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string out = "[";
    bool first = true;
    for (const auto& [name, tool] : tools_) {
        if (!first) out += ",";
        out += tool->schemaJson();
        first = false;
    }
    out += "]";
    return out;
}

} // namespace core
