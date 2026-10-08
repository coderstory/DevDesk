// src/core/paths.h
#pragma once

#include <filesystem>
#include <string>

namespace core::paths {

// 应用数据根目录 —— **全部应用数据的唯一根**（审计 CP-04）
//   macOS:   ~/DevDesk/
//   Windows: %USERPROFILE%/DevDesk/
//   Linux:   ~/DevDesk/
// 环境变量 DEVDESK_DATA_ROOT 已设且非空 → 该目录。
// 为什么不用系统惯例位置（2026-09-28 决策）：数据根要**默认可见**——macOS 的
// ~/Library/Application Support、Windows 的 %APPDATA% 在 Finder / 资源管理器里
// 默认是隐藏的，用户看不见自己有哪些数据；且笔记目录后续要做成 git 仓库
// （用户会自己去 commit / 推远端），放在用户主目录下一级最顺手。
// 代价（已接受）：改动前落在旧位置（macOS ~/Library/Application Support/DevDesk、
// Windows %APPDATA%/DevDesk）的既有数据**不会自动搬**——本版不做迁移，新根从零
// 开始；要保留旧数据只能手动拷贝（旧位置的文件原样留在原处，本应用不读也不删）。
// 覆盖只有这一个入口：配置 / 笔记 / 宠物 / 数据库 / 日志 / 视图状态等**全部**
// 应用数据路径都经本函数派生（旧形态是每个路径各自读自己的环境变量，隔离缝
// 散落三层且无中心入口，漏设一个就静默读写真实用户目录）。
// 不随数据根迁移的两个例外（刻意为之，见各自注释）：executableDir（随包
// 资源，随可执行文件走）与 claudeSettingsFile（Claude Code 自己的用户级
// 配置，是外部集成点而不是本应用的数据）。
std::filesystem::path appDataDir();

// 平台默认数据根（paths_win.cpp / paths_macos.cpp / paths_posix.cpp 实现）。
// **不是**业务入口——业务代码一律走 appDataDir()（它已经是「环境变量覆盖 +
// 平台默认」的唯一入口，见上）；这里只供 paths.cpp 回落与测试对照
// （断言「没设 DEVDESK_DATA_ROOT 时落真实默认根」，即区分「没设」与「设了」）
std::filesystem::path platformDefaultAppDataDir();

// 当前可执行文件所在目录（随包资源解析用：exe 旁 assets/ 等）
// 只返回目录本身、不含可执行文件名；不做存在性保证。
// 失败（平台 API 不可用 / 路径解析失败）返回 **空 path** —— 绝不拿 cwd 或
// 其它目录冒充；调用方必须自行判空并跳过该候选（空路径参与拼接会退化成
// 相对 cwd 的路径，等于把 cwd 依赖偷偷加回来）
std::filesystem::path executableDir();

// 配置文件（config.json）
// 覆盖优先级（审计 CP-04）：DEVDESK_CONFIG_PATH（整文件路径）> DEVDESK_DATA_ROOT
// （<根>/config.json）> 平台默认根
std::filesystem::path configFile();

// SQLite 数据库文件
std::filesystem::path sqliteDbFile();

// Markdown 笔记根目录
// 覆盖优先级：DEVDESK_NOTES_DIR > DEVDESK_DATA_ROOT（<根>/notes）> 平台默认根
std::filesystem::path markdownDir();

// 桌宠皮肤根目录（module_pet；皮肤 = <petsDir>/<皮肤名>/pet.json + 帧序列）
// 覆盖优先级：DEVDESK_PETS_DIR > DEVDESK_DATA_ROOT（<根>/pets）> 平台默认根。
// 注意：本函数只管「皮肤写在哪」；「皮肤能从哪儿被发现」是 module_pet 的
// skinRootCandidates()（数据根 petsDir + 安装目录 assets/pets，纯派生、
// 不读任何环境变量——旧的 DEVDESK_PETS_DIR 产品行为分支已删，审计 CP-04）
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
// **刻意不随 DEVDESK_DATA_ROOT 迁移**（审计 CP-04）：写的是 Claude Code 自己
// 的配置，属于外部集成点而不是本应用的数据——数据根覆盖时把接管目标改到
// <根>/.claude 会让该功能失去意义（Claude Code 读不到）。隔离测试这一路时
// 仍须走 DEVDESK_CLAUDE_CONFIG_DIR。
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