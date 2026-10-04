// src/module_agent/tool_info.h
// 「可用工具」区块的数据源：从 ToolRegistry 枚举已注册工具，把 ITool::schemaJson()
// 解码成展示用 ToolInfo。schema 解析只在本文件（单一解码口径）；坏 schema 容错降级，
// 枚举是展示面，不因个别坏 schema 白屏
#pragma once

#include <string>
#include <vector>

namespace module::agent {

struct ToolInfo {
    std::string name;                 // 工具名，如 "md.search"；坏 schema → 空
    std::string description;          // 一行描述；缺失 → 空
    std::vector<std::string> params;  // input_schema.properties 的键（map 序）
};

// 解码单个 schemaJson。坏 JSON → 全空；缺 input_schema/properties → params 空
ToolInfo decodeToolInfo(const std::string& schemaJson);

// 枚举进程级 ToolRegistry 全部已注册工具，按注册表序（name 字典序）。空 name 的坏条目跳过
std::vector<ToolInfo> listRegisteredTools();

} // namespace module::agent
