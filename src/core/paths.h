// src/core/paths.h
#pragma once

#include <filesystem>
#include <string>

namespace core::paths {

// 应用数据根目录
//   macOS:   ~/Library/Application Support/DevDesk/
//   Windows: %APPDATA%/DevDesk/
std::filesystem::path appDataDir();

// 当前可执行文件所在目录（随包资源解析用：exe 旁 assets/ 等）
// 只返回目录本身、不含可执行文件名；不做存在性保证。
// 失败（平台 API 不可用 / 路径解析失败）返回 **空 path** —— 绝不拿 cwd 或
// 其它目录冒充；调用方必须自行判空并跳过该候选（空路径参与拼接会退化成
// 相对 cwd 的路径，等于把 cwd 依赖偷偷加回来）
std::filesystem::path executableDir();

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

// DEVDESK_CLAUDE_CONFIG_DIR 覆盖分支（审计 CP-09：三平台 paths_*.cpp 逐字
// 相同实现的下沉）：环境变量已设 → 返回 <env>/settings.json；
// 未设（或设了空串以外含义不变）→ 返回空 path，调用方走各平台 home 默认分支
std::filesystem::path claudeConfigDirOverride();

// Claude Code 企业托管 managed-settings.json（上层覆盖检测用，只读不写）
//   Windows: C:\ProgramData\ClaudeCode\managed-settings.json
//   macOS:   /Library/Application Support/ClaudeCode/managed-settings.json
//   POSIX:   /etc/claude-code/managed-settings.json
std::filesystem::path claudeManagedSettingsFile();

// 确保所有目录存在
void ensureDirectoriesExist();

} // namespace core::paths