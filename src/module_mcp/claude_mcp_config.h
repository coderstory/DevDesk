// src/module_mcp/claude_mcp_config.h
// Claude Code 全局 MCP 配置（~/.claude.json 顶层 mcpServers）读写。
//
// 口径（用户手册 §3.8）：DevDesk 的 MCP 是**单一 HTTP 服务**，客户端只需配一条
// server 地址 —— 故这里按服务级一条注册（key = devdesk），不按工具逐个注册。
//
// 安全边界（~/.claude.json 实测 105KB+，含 Claude Code 自身大量状态）：
//   - 只动 mcpServers 下 devdesk 这一个键，其余键与全部其他顶层字段原样保留；
//   - 写路径 = 读全量 → 改键 → 备份 → 原子写（core::atomicWriteFile）；
//   - 绝不新建文件（文件不存在 = 未检测到 Claude Code，由调用方置灰）；
//   - 不调 claude CLI（可能不存在 / 版本差异 / 会改动其他行为）。
#pragma once

#include <filesystem>
#include <string>

#include "core/result.h"

namespace module::mcp::cc {

// 配置 key：Claude Code 侧看到的 server 名（工具前缀 mcp__devdesk__* 同源）
inline constexpr const char* kServerName = "devdesk";

// 文件级状态快照：每次现读，不跨会话缓存
struct ConfigState {
    bool filePresent = false;  // ~/.claude.json 是否存在
    bool installed = false;    // mcpServers.devdesk 是否存在
    std::string error;         // 读取/解析失败原因（空 = 正常）
};

ConfigState readConfigState(const std::filesystem::path& file);

// 纯函数（单测钉，不碰磁盘）：
// 安装 —— 幂等判据 = mcpServers 是否已有 kServerName；已有则原样返回（changed=false），
// 不覆盖已有条目（端口变了由「卸载再安装」收敛，见 manifest 残余风险）。
core::Result<std::string> applyServerEntry(const std::string& jsonText,
                                           const std::string& url,
                                           bool* changed);
// 卸载 —— 只摘 mcpServers.kServerName；其余 server 与其他顶层字段原样。
// 本就没有该键 → 原样返回（changed=false）
core::Result<std::string> removeServerEntry(const std::string& jsonText,
                                            bool* changed);

// 文件级编排：文件必须已存在（缺失 = fail，绝不新建）；有改动时先备份到
// <file>.bak-devdesk（备份失败即整体失败，不写目标文件）
core::Result<void> installServerFile(const std::filesystem::path& file,
                                     const std::string& url);
core::Result<void> removeServerFile(const std::filesystem::path& file);

// 备份路径 = <file>.bak-devdesk
std::filesystem::path backupFile(const std::filesystem::path& file);

} // namespace module::mcp::cc
