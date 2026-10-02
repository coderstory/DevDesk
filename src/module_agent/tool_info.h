// src/module_agent/tool_info.h
// 「可用工具」区块的数据源（2026-10-02 用户反馈④「tool 怎么调用也不知道」）：
// 从 ToolRegistry 枚举已注册工具，把 ITool::schemaJson()（Anthropic 工具定义：
// name/description/input_schema）解码成展示用 ToolInfo。
//
// 单一解码口径（trigger_config / message_blocks 同款）：schema 解析只在本文件，
// 渲染层不做裸 parse。坏 schema 容错降级（name 尽力取，取不到空 name 由
// listRegisteredTools 跳过），枚举是展示面，不因个别坏 schema 白屏。
#pragma once

#include <string>
#include <vector>

namespace module::agent {

struct ToolInfo {
    std::string name;                 // 工具名，如 "md.search"；坏 schema → 空
    std::string description;          // 一行描述；缺失 → 空
    std::vector<std::string> params;  // input_schema.properties 的键（map 序）
};

// 解码单个 schemaJson。坏 JSON → 全空 ToolInfo；缺 input_schema / properties
// → params 空。不报错（展示面容错降级）。
ToolInfo decodeToolInfo(const std::string& schemaJson);

// 枚举进程级 ToolRegistry 的全部已注册工具（跨模块：md.* / claude.* /
// todo.* / app.* 等），按注册表序（name 字典序）。空 name 的坏条目跳过。
std::vector<ToolInfo> listRegisteredTools();

} // namespace module::agent
