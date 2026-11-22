// src/module_mcp/mcp_config_common.h
// MCP 宿主配置读-改-写公共编排（need:380 批C：claude/zcode 双副本收敛）。
#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "core/result.h"

namespace module::mcp::config_common {

// 备份文件路径（<file>.bak-devdesk）；测试直接引用此符号（原 cc/zc 各一份，need:380 批C 收敛）
std::filesystem::path backupFile(const std::filesystem::path& file);

// 读-改-写公共编排。文件必须已存在（绝不新建）；有改动才备份 + 原子写。
// productName 用于 FileNotFound 文案（"未检测到 <X> 配置 ..."）与备份提示（"<X> 配置备份"）。
core::Result<void> rewriteExisting(
    const std::filesystem::path& file,
    const std::function<core::Result<std::string>(const std::string&, bool*)>& mutate,
    const char* label, const char* productName);

}  // namespace module::mcp::config_common
