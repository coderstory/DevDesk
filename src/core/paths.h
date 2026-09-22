// src/core/paths.h
#pragma once

#include <filesystem>
#include <string>

namespace core::paths {

// 应用数据根目录
//   macOS:   ~/Library/Application Support/DevDesk/
//   Windows: %APPDATA%/DevDesk/
std::filesystem::path appDataDir();

// 配置文件（config.json）
std::filesystem::path configFile();

// SQLite 数据库文件
std::filesystem::path sqliteDbFile();

// Markdown 笔记根目录
std::filesystem::path markdownDir();

// 桌宠皮肤根目录（module_pet；皮肤 = <petsDir>/<皮肤名>/pet.json + 帧序列）
// 环境变量 DEVDESK_PETS_DIR 可覆盖（测试隔离用，与 markdownDir 同款机制）
std::filesystem::path petsDir();

// 日志目录
std::filesystem::path logsDir();

// 主日志文件
std::filesystem::path logFile();

// Claude Code 用户级 settings.json（provider-config Phase C 接管通道的写入目标）
//   Windows: %USERPROFILE%\.claude\settings.json
//   macOS:   ~/.claude/settings.json
// 环境变量 DEVDESK_CLAUDE_CONFIG_DIR 可覆盖 .claude 目录（测试隔离用；
// 也是 spec 开放问题 3「自定义 .claude 目录」的预留入口）
std::filesystem::path claudeSettingsFile();

// Claude Code 企业托管 managed-settings.json（上层覆盖检测用，只读不写）
//   Windows: C:\ProgramData\ClaudeCode\managed-settings.json
//   macOS:   /Library/Application Support/ClaudeCode/managed-settings.json
//   POSIX:   /etc/claude-code/managed-settings.json
std::filesystem::path claudeManagedSettingsFile();

// 确保所有目录存在
void ensureDirectoriesExist();

} // namespace core::paths