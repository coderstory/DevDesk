// src/module_claude_proxy/settings_sync.h
// Claude Code settings.json 接管通道（provider-config Phase C，设计 §5.3）
//
// 接管协议（takeOver，代理启动 / 运行中切 profile 触发）：
//   1. 读现有 settings.json（不存在 → 视为空对象；解析失败 → **中止不写**——
//      宁可不让用户自动接入，不能弄坏用户文件）
//   2. 首份快照：接管前把原 env 对象完整存入 sidecar（DevDesk 数据目录的
//      claude_settings_snapshot.json，**不往 settings.json 塞标记字段**）；
//      sidecar 已存在则保留首份快照（热切换 / 重生成 token 不覆盖），
//      仅把新的托管键并入 managedKeys（防旧托管键漏还原）。
//      **污染守卫**：首份快照拍之前先扫现有 env，命中「本机代理形态」（BASE_URL
//      指向回环地址，或凭据键 == kProxyAuthPlaceholder）的键不进快照——它们是
//      上次残留 / 别的工具的托管值，不是用户原值（还原时不该被"恢复"），
//      该情况随 SyncResult.warnings 上报
//   3. 只动 env 键：buildProxyEnv() 产出的托管键写入/覆盖；**写前先删「上一轮
//      托管、本轮不再生成」的键**（如槽位被清空后的旧模型名——留着会被客户端
//      拿去请求新供应商；这些键仍留在 managedKeys 并集里，还原语义不变）；
//      用户自己的其他 env 键与所有非 env 顶层字段（permissions / hooks /
//      model 等）原样保留
//      （读-改-写，platform::json；注意序列化会统一 pretty 格式，字段值不变）
//   4. 原子写：同目录临时文件 + rename
//   5. 上层覆盖检测（§5.3）：managed（企业策略）/ project（cwd 的
//      .claude/settings.json + settings.local.json）层 env 含
//      ANTHROPIC_BASE_URL / AUTH_TOKEN / MODEL 等同名键 → LOG_WARN + 警告
//      信息随 SyncResult 返回（UI 展示留给 Phase D），**提示不代改**
//
// 还原协议（restore，代理停止 / 应用退出触发）：
//   - 按 sidecar 快照按键级还原：快照里有值 → 恢复原值；原本不存在的键 →
//     删除；其余一切不动；写回后删除 sidecar
//   - 代理停止 = settings.json 永不指向死代理
//
// 崩溃兜底：启动时 sidecar 残留（上次没走完还原）→ recoverPendingSnapshot()
// 自动补还原，必须先于任何 takeOver 调用（否则残留的托管键会被误快照为
// 用户原值）。
//
// 热切换：官方 settings-reference 已确认 env 键在「at startup, and again in
// the running session when a saved change alters the merged env」时重新加载，
// 即保存后运行中的 Claude Code 会话即生效，无需重启客户端（model settings
// 键的 /model 例外不影响 env 路径）。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace module::claude {

struct Profile;

namespace settings_sync {

// 路径注入点：生产用 defaultPaths()（core::paths），测试传临时目录，
// 绝不碰真实 ~/.claude
struct Paths {
    std::filesystem::path settingsFile;  // ~/.claude/settings.json
    std::filesystem::path sidecarFile;   // appDataDir()/claude_settings_snapshot.json
    // 上层覆盖检测要读的文件（managed + project 层；user 层就是 settingsFile 自身，不列）
    std::vector<std::filesystem::path> overlayFiles;
};

Paths defaultPaths();

struct SyncResult {
    bool ok = false;
    std::string error;                  // 失败 / 中止原因（解析失败中止等）
    std::vector<std::string> warnings;  // 上层覆盖检测命中（Phase D UI 展示）
};

// 反向同步读侧（设计 §3.3 G7/G8/G9）：把 settings.json 的 env 读成「原文 JSON
// + sidecar 托管键集」，供切 profile 前的反向同步（settings.json → profile）
// 使用。**只读，不写任何文件、不碰 profile**。
//   - 文件不存在 / 0 字节空文件 / 无 env 键 → ok=true, present=false
//     （no-op 成功：调用方照常完成本次切换，不清空任何 profile 字段）
//   - 读取失败 / 非法 JSON / 顶层非对象 / env 非对象 → ok=false + error
//     （含路径与原因；调用方据此中止本次切换 S-2）
//     ——与「不存在」是两种处置，不得合并
//   - 正常 → ok=true, present=true + envJson（env 对象原文）+ managedKeys
//     （sidecar 的 managedKeys 键集；sidecar 缺失 → 空集；sidecar 损坏 →
//     空集 + WARN，绝不因此失败）
struct SettingsEnvRead {
    bool ok = true;
    bool present = false;
    std::string error;
    std::string envJson;
    std::vector<std::string> managedKeys;
};

SettingsEnvRead readSettingsEnvForSync(const Paths& paths);

// 接管。profile 可为默认构造（无激活 profile 时只写 BASE_URL + 占位凭据）
SyncResult takeOver(const Profile& profile, int proxyPort, const Paths& paths);

// 还原（写回后删 sidecar；settings.json 解析失败 → fail 且保留 sidecar 供人工
// 恢复）。**无 sidecar 时不再无条件 no-op**：settings.json 里若仍留着本机代理
// 形态键（回环 BASE_URL / 占位凭据——sidecar 被清掉或与其它工具互抢的残留），
// 做最小清除（只删这些键，不按 active profile 重建 env）；无该形态 → 真 no-op
SyncResult restore(const Paths& paths);

// sidecar 是否存在（设置页「接管状态」行数据源 + 崩溃补还原检测）
bool hasPendingSnapshot(const Paths& paths);

// 启动崩溃补还原：sidecar 残留 → 先还原（见文件头注释的调用时序要求）
SyncResult recoverPendingSnapshot(const Paths& paths);

} // namespace settings_sync
} // namespace module::claude
