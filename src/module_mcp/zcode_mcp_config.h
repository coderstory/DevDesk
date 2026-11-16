// src/module_mcp/zcode_mcp_config.h
// ZCode 全局 MCP 配置（~/.zcode/cli/config.json 顶层 mcp.servers）读写（need279）。
//
// 与 claude_mcp_config（cc）同构镜像：DevDesk 的 MCP 是**单一 HTTP 服务**，按
// 服务级一条注册（key = devdesk）；全量 DOM 读改写只动一键、绝不新建文件、
// 备份 + 原子写（core::atomicWriteFile）。Claude 端先例见 claude_mcp_config.h。
//
// 差异（need279 PRD §8）：
//   - 两级键 mcp.servers：缺失补建层级（文件必须已存在不变）；存在但非对象拒写；
//   - 安装三分判定 = 无键可装 / 同址幂等 no-op / 异址冲突不覆盖 fail（中文引导）。
//
// 安全边界（~/.zcode/cli/config.json 同时承载 plugins/hooks/skills 与任意未知键，
// schema 严格）：只动 mcp.servers 下 devdesk 这一个键，其余键与全部其他顶层字段
// 原样保留；不调外部 CLI（本机无 zcode 命令）。
#pragma once

#include <filesystem>
#include <string>

#include "core/result.h"

namespace module::mcp::zc {

// 配置 key：ZCode 侧看到的 server 名（与 cc::kServerName 同源，工具前缀
// mcp__devdesk__* 同一服务）
inline constexpr const char* kServerName = "devdesk";

// 文件级状态快照：每次现读，不跨会话缓存
struct ConfigState {
    bool filePresent = false;  // config.json 是否存在
    bool installed = false;    // mcp.servers.devdesk 键是否存在（含异址冲突态）
    std::string installedUrl;  // 已装条目 url（未装 / 缺 url / 非串 = 空）
    std::string error;         // 读取/解析失败原因（空 = 正常）
};

ConfigState readConfigState(const std::filesystem::path& file);

// 纯函数（单测钉，不碰磁盘）：
// 安装三分判定 —— 无键=写入 {"type":"http","url","enabled":true}（changed=true）；
// 有键且 url 相同=幂等 no-op（原文本返回，changed=false，不覆盖）；
// 有键且 url 不同/缺失/非串=冲突 fail（ErrorCode::Conflict，中文引导先移除再安装）
core::Result<std::string> applyServerEntry(const std::string& jsonText,
                                           const std::string& url,
                                           bool* changed);
// 卸载 —— 只摘 mcp.servers.kServerName；其余 server 与其他顶层字段原样。
// 本就没有该键（含 mcp/servers 缺失）→ 原样返回（changed=false）
core::Result<std::string> removeServerEntry(const std::string& jsonText,
                                            bool* changed);

// 文件级编排：文件必须已存在（缺失 = fail，绝不新建）；有改动时先备份到
// <file>.bak-devdesk（备份失败即整体失败，不写目标文件）
core::Result<void> installServerFile(const std::filesystem::path& file,
                                     const std::string& url);
core::Result<void> removeServerFile(const std::filesystem::path& file);

// 备份路径 = <file>.bak-devdesk（D4 裁决：对齐 Claude 端 .bak-devdesk 后缀族）
std::filesystem::path backupFile(const std::filesystem::path& file);

} // namespace module::mcp::zc
