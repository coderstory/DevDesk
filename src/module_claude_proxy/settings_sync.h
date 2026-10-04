// src/module_claude_proxy/settings_sync.h
// Claude Code settings.json 接管通道：DevDesk 对 settings.json 的唯一写入口
//（读-改-写 + 原子写 + sidecar 快照协议）。接管只动 env 键 + 生效高级设置 JSON
// 顶层键；还原按键级还原快照后删 sidecar（代理停止 = settings.json 永不指向
// 死代理）；sidecar 残留时 recoverPendingSnapshot 自动补还原，必须先于任何 takeOver。

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/sqlite_db.h"

namespace module::claude {

struct Profile;

namespace settings_sync {

// sidecar（接管前 settings.json 的 env 快照）的 app_config 键名
inline constexpr const char* kSidecarConfigKey = "claude.settings_snapshot";

// 注入点：生产用 defaultPaths()，测试传临时目录 + 临时库句柄，不碰真实 ~/.claude
struct Paths {
    std::filesystem::path settingsFile;  // ~/.claude/settings.json
    // sidecar 落点 = app_config 表的 kSidecarConfigKey 行。nullptr = 数据库未就绪：
    // 按「无快照」处理并 WARN，不静默当成没接管过（会让 restore 把用户 settings.json 留在代理形态）
    core::data::SqliteDb* db = nullptr;
    // 上层覆盖检测要读的文件（managed + project 层；user 层就是 settingsFile 自身，不列）
    std::vector<std::filesystem::path> overlayFiles;
};

Paths defaultPaths();

struct SyncResult {
    bool ok = false;
    std::string error;                  // 失败 / 中止原因（解析失败中止等）
    std::vector<std::string> warnings;  // 上层覆盖检测命中（UI 展示）
};

// 反向同步读侧：把 settings.json 的 env 读成「原文 JSON + sidecar 托管键集」，
// 供切 profile 前的反向同步使用。只读，不写任何文件、不碰 profile。
//   - 不存在 / 0 字节 / 无 env 键 → ok=true, present=false（no-op 成功）
//   - 读不出（IO/权限）/ 非法 JSON / env 非对象 → ok=false + error（调用方中止切换）
//     ——「读不出」与「不存在/空」是两种处置，不得合并
//   - 正常 → present=true + envJson + managedKeys（sidecar 缺失/损坏 → 空集 + WARN）
struct SettingsEnvRead {
    bool ok = true;
    bool present = false;
    std::string error;
    std::string envJson;
    std::vector<std::string> managedKeys;
};

SettingsEnvRead readSettingsEnvForSync(const Paths& paths);

// 接管。profile 可为默认构造（无激活 profile 时只写 BASE_URL + 占位凭据）。
// 生效高级设置 JSON 经 profile.extraSettingsJson 携带（片段赢），其写入键进
// sidecar nonEnvKeys / nonEnvSnapshot 记账，env 托管键照旧
SyncResult takeOver(const Profile& profile, int proxyPort,
                    const Paths& paths);

// 还原（写回后删 sidecar；解析失败 → fail 且保留 sidecar 供人工恢复）。无
// sidecar 但 settings.json 仍留本机代理形态键 → 最小清除；无该形态 → 真 no-op
SyncResult restore(const Paths& paths);

// sidecar 是否存在（设置页「接管状态」行数据源 + 崩溃补还原检测）
bool hasPendingSnapshot(const Paths& paths);

// 启动崩溃补还原：sidecar 残留 → 先还原（见文件头注释的调用时序要求）
SyncResult recoverPendingSnapshot(const Paths& paths);

} // namespace settings_sync
} // namespace module::claude
