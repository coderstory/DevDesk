#pragma once

#include <filesystem>
#include <string>

#include "result.h"

namespace core::paths {

// 应用数据唯一根：macOS/Linux ~/DevDesk/、Windows %USERPROFILE%/DevDesk/，DEVDESK_DATA_ROOT 非空则用；
// 刻意不用系统默认隐藏位置（数据要默认可见，笔记目录要进 git）。全部应用数据路径经此派生，
// 例外：executableDir 与 claudeSettingsFile 不随数据根迁移（见各自注释）
std::filesystem::path appDataDir();

// 平台默认数据根（paths_*.cpp 实现），非业务入口——业务走 appDataDir()；供回落与测试对照
std::filesystem::path platformDefaultAppDataDir();

// 当前可执行文件所在目录（随包资源解析用），只返回目录不含文件名，不做存在性保证。
// 失败返回空 path——绝不拿 cwd 冒充；调用方须判空跳过（空路径拼接会退化成相对 cwd）
std::filesystem::path executableDir();

// SQLite 数据库文件
std::filesystem::path sqliteDbFile();

// 数据根内置的 .gitignore：db/-wal 有凭据页（WAL 默认 0644），防 `git add .` 误提交；
// 与 file_perms.h 两道互补闸（防误提交 / 防同机他人直读）。路径可经 DEVDESK_GITIGNORE_PATH 覆盖
std::filesystem::path dataRootGitignoreFile();

// 在数据根写入/校正 .gitignore：幂等追加，只补缺失必需项，齐全时一字节不动，绝不覆盖用户规则。
// 写失败返回 Result（调用方记日志不阻断启动）
core::Result<void> ensureDataRootGitignore();

// Markdown 笔记根目录。覆盖优先级：DEVDESK_NOTES_DIR > DEVDESK_DATA_ROOT（<根>/notes）> 平台默认根
std::filesystem::path markdownDir();

// 日志目录
std::filesystem::path logsDir();

// 主日志文件
std::filesystem::path logFile();

// Claude Code 用户级 settings.json（接管通道写入目标）：Windows %USERPROFILE%\.claude\、macOS ~/.claude/。
// DEVDESK_CLAUDE_CONFIG_DIR 可覆盖（测试隔离）；刻意不随 DEVDESK_DATA_ROOT 迁移（改了 Claude Code 读不到）
std::filesystem::path claudeSettingsFile();

// DEVDESK_CLAUDE_CONFIG_DIR 覆盖分支：已设 → <env>/settings.json；未设 → 空 path，
// 调用方走各平台 home 默认分支
std::filesystem::path claudeConfigDirOverride();

// Claude Code 企业托管 managed-settings.json（上层覆盖检测用，只读不写）。Windows
// C:\ProgramData\ClaudeCode\；macOS /Library/Application Support/ClaudeCode/；POSIX /etc/claude-code/
std::filesystem::path claudeManagedSettingsFile();

// GitHub Sync 相关路径
// SQL dump 文件路径（仓库根目录下的 database.sql）
std::filesystem::path sqlDumpFile();

// 确保所有目录存在
void ensureDirectoriesExist();

} // namespace core::paths