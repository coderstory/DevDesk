// ToolRegistry 枚举 + schema 解码实现（契约见 tool_info.h）
#include "module_agent/tool_info.h"

#include <map>
#include <utility>

#include "core/tool.h"
#include "platform/json.h"

namespace module::agent {

namespace {

namespace pj = platform::json;

} // namespace

ToolInfo decodeToolInfo(const std::string& schemaJson) {
    ToolInfo info;
    auto parsed = pj::parse(schemaJson);
    if (parsed.isFail() || !parsed.value().isObject()) {
        return info;   // 坏 schema：全空，调用方跳过
    }
    pj::JsonObject obj = parsed.value().asObject();
    pj::JsonValue name = obj.get("name");
    if (name.isString()) info.name = name.asString();
    pj::JsonValue desc = obj.get("description");
    if (desc.isString()) {
        // 描述进工具卡是 wrap(false) 单行元素：换行摊平
        std::string d = desc.asString();
        for (char& c : d) {
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        }
        info.description = std::move(d);
    }
    pj::JsonValue schema = obj.get("input_schema");
    if (!schema.isObject()) return info;
    pj::JsonValue props = schema.asObject().get("properties");
    if (!props.isObject()) return info;
    // 键序 = 底层 map（字母序），稳定性够展示用
    for (const auto& key : props.asObject().keys()) {
        info.params.push_back(key);
    }
    return info;
}

std::vector<ToolInfo> listRegisteredTools() {
    std::vector<ToolInfo> out;
    // listTools() 返回 shared_ptr 快照：解码期间模块开关不会把工具从脚下滑走
    for (const auto& tool : core::ToolRegistry::instance().listTools()) {
        ToolInfo info = decodeToolInfo(tool->schemaJson());
        if (info.name.empty()) info.name = tool->name();   // schema 坏但 name() 活着
        if (info.name.empty()) continue;
        out.push_back(std::move(info));
    }
    return out;
}

} // namespace module::agent
