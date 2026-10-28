// src/module_memory/hook/hooks_settings.h
// Claude Code 侧 hooks 配置写入/还原（S3-C）。走 DevDesk 既有 settings 管理通道
//（claude_service 接管同模式：DevDesk 是 settings.json 的终局 owner，见 CLAUDE.md；
// 写的是键级最小 diff，不是整文件重写）。
// 契约：
//   - 只动 hooks.UserPromptSubmit 下带本模块命令的那一条；用户手工加的其它 hook 一律不碰；
//   - 还原 = 摘除命令等于 ours 的条目（键级幂等），无需 sidecar 快照——我们精确知道
//     自己加了什么；被用户改过的条目（命令不同）原样保留；
//   - 测试隔离：settingsFile 经 DEVDESK_CLAUDE_CONFIG_DIR 覆盖（paths 契约），单测
//     永远不碰用户真实 ~/.claude/settings.json。
#pragma once

#include <filesystem>
#include <string>

#include "core/result.h"

namespace module::memory::hook {

// 注入 Claude Code 的 hook 命令行："<exe>" memory hook（exe 带引号防路径空格）
std::string buildHookCommand(const std::string& exePath);

// 在 settings JSON 文本上安装/摘除（纯函数，单测钉）。changed 出参可空。
// 安装形态：hooks.UserPromptSubmit[].hooks[] 追加
//   {"type":"command","command":<cmd>,"timeout":10}
//（UserPromptSubmit 缺失则逐层补建；已存在同命令条目 → no-op）
core::Result<std::string> applyHookEntry(const std::string& settingsJson,
                                         const std::string& hookCommand,
                                         bool* changed);
// 摘除：过滤 UserPromptSubmit[].hooks[] 中 command == cmd 的条目；空组逐层回收。
// 文件本无该命令 → 原样返回（changed=false）
core::Result<std::string> removeHookEntry(const std::string& settingsJson,
                                          const std::string& hookCommand,
                                          bool* changed);

// 文件级编排：install = 读-改-原子写（幂等）；restore = 读-摘-原子写（文件缺失
// = no-op 成功：还原路径绝不报错挡停机）
core::Result<void> installHookFile(const std::filesystem::path& settingsFile,
                                   const std::string& hookCommand);
core::Result<void> restoreHookFile(const std::filesystem::path& settingsFile,
                                   const std::string& hookCommand);

}  // namespace module::memory::hook
