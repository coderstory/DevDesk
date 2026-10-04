// src/core/paths.h
#pragma once

#include <filesystem>
#include <string>

#include "result.h"

namespace core::paths {

// 应用数据根目录——全部应用数据的唯一根。macOS/Linux ~/DevDesk/，Windows %USERPROFILE%/DevDesk/；
// DEVDESK_DATA_ROOT 已设且非空则用它。不用系统惯例位置：数据根要默认可见（~/Library、%APPDATA%
// 在文件管理器里默认隐藏），且笔记目录要做成 git 仓库，放主目录下一级最顺手。代价（已接受）：
// 旧位置数据不自动迁移。全部应用数据路径都经本函数派生（唯一覆盖入口）。例外：executableDir
// 与 claudeSettingsFile 不随数据根迁移（见各自注释）
std::filesystem::path appDataDir();

// 平台默认数据根（paths_*.cpp 实现）。不是业务入口——业务走 appDataDir()；只供 paths.cpp 回落
// 与测试对照（断言没设 DEVDESK_DATA_ROOT 时落真实默认根）
std::filesystem::path platformDefaultAppDataDir();

// 当前可执行文件所在目录（随包资源解析用），只返回目录不含文件名，不做存在性保证。
// 失败返回空 path——绝不拿 cwd 冒充；调用方须判空跳过（空路径拼接会退化成相对 cwd）
std::filesystem::path executableDir();

// SQLite 数据库文件
std::filesystem::path sqliteDbFile();

// 数据根内置的 .gitignore（数据根本身的忽略规则）。数据根要进 git 仓库，而凭据迁进 SQLite 后
// devdesk.db / -wal 里有凭据页（WAL 默认 0644），一次 `git add .` 就带走。本文件与 file_perms.h
// 是两道互补闸：一道防误提交，一道防同机他人直读。路径可经 DEVDESK_GITIGNORE_PATH 覆盖（测试隔离）
std::filesystem::path dataRootGitignoreFile();

// 在数据根写入/校正 .gitignore：幂等追加，只补缺失必需项，齐全时一字节不动，绝不覆盖用户规则。
// 写失败返回 Result（调用方记日志不阻断启动）
core::Result<void> ensureDataRootGitignore();

// Markdown 笔记根目录。覆盖优先级：DEVDESK_NOTES_DIR > DEVDESK_DATA_ROOT（<根>/notes）> 平台默认根
std::filesystem::path markdownDir();

// 桌宠皮肤根目录（皮肤 = <petsDir>/<皮肤名>/pet.json + 帧序列）。覆盖优先级：DEVDESK_PETS_DIR >
// DEVDESK_DATA_ROOT（<根>/pets）> 平台默认根。本函数只管「皮肤写在哪」；「能从哪儿发现」是
// module_pet 的 skinRootCandidates()（纯派生，不读环境变量）
std::filesystem::path petsDir();

// 日志目录
std::filesystem::path logsDir();

// 主日志文件
std::filesystem::path logFile();

// Claude Code 用户级 settings.json（接管通道写入目标）。Windows %USERPROFILE%\.claude\settings.json，
// macOS ~/.claude/settings.json。DEVDESK_CLAUDE_CONFIG_DIR 可覆盖 .claude 目录（测试隔离用）。
// 刻意不随 DEVDESK_DATA_ROOT 迁移：写的是 Claude Code 自己的配置，数据根覆盖会把接管目标改到
// <根>/.claude 使功能失去意义（Claude Code 读不到）
std::filesystem::path claudeSettingsFile();

// DEVDESK_CLAUDE_CONFIG_DIR 覆盖分支：已设 → <env>/settings.json；未设 → 空 path，
// 调用方走各平台 home 默认分支
std::filesystem::path claudeConfigDirOverride();

// Claude Code 企业托管 managed-settings.json（上层覆盖检测用，只读不写）。
// Windows C:\ProgramData\ClaudeCode\managed-settings.json；macOS /Library/Application Support/
// ClaudeCode/managed-settings.json；POSIX /etc/claude-code/managed-settings.json
std::filesystem::path claudeManagedSettingsFile();

// 确保所有目录存在
void ensureDirectoriesExist();

} // namespace core::paths