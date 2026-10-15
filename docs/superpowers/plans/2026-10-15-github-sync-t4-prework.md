# GitHub Sync T4（同步编排）实施预案 — 前置设计（只读调研产物）

> 日期：2026-10-15 ｜ 作者：只读前置设计 agent（T3 合并前预研）
> 权威 spec：`docs/superpowers/specs/2026-10-15-github-sync-api-design.md`（决策表 + 审计补正 F1–F9）
> **基于 draft 现况**：
> - T2（`agent-ghapi`）：HEAD `b13b212`（2026-10-15），工作区干净。`src/platform/github_api.h` + `.cpp`（865 行）已齐。
> - T3（`agent-github-sync-ui`）：HEAD `de456cb`（2026-10-15）+ **大量未提交修改**（settings_module.cpp / github_sync_card.cpp / config / logic / git_error_dialog / local_git_probe 等在飞）。T4 开工时以合并进 dev 的版本为准，本文行号引的是 draft 现况。
> - T1（`agent-sqldump`）：提交 `47fe68a`（索引/触发器 DDL + 回环测试）**尚未合并进 dev**；dev 主树 `src/core/sql_dump.cpp` 现况仍无索引/触发器导出。T3 worktree 里 `sql_dump.cpp` 只 +2 行（include），F2 修复靠 T1 合并。
> ⚠ T2/T3 均为未合并 draft，API 若变以合并后为准；本文所有行号均需在合并后复核。

## 0. 最大发现（决定 T4 的形态）

**T3 draft 已经把大半 T4 编排写进了 `settings_module.cpp`**（后台线程 + 节流 + 首次方向 + 删除传播 + 库备份 + 409/详情弹窗 + harvest 收割模式全套已在）。T4 不是"从零写编排"，而是**差量补齐**：F1 内容级变更检测、F7 树列举 + 笔记拉取、F3 导入暂停/重载/回滚、F5 退出推送、F6 笔记备份、F9 体积上限、dirty 事件补齐、决策 #15 本地 commit、git_sync_service 删除。

---

## 1. sha 状态机（spec F1，决策 #19）

### 1.1 存储模型

每条同步对象（笔记 `notes/<名>` 与 `data/database.sql`）两条记录（app_config，`github_sync:` 前缀）：

| 键 | 值 | 写入时机 |
|---|---|---|
| `github_sync:sha:<相对路径>` | 上次同步成功后的**远端 sha**（PUT/DELETE 成功或拉取覆盖成功时） | T3 draft 已有 `recordSyncedNote`/`forgetSyncedNote`/`syncedNoteNames`（`github_sync_config.h:55-58`）；database.sql 的 sha 登记尚缺，需补 |
| `github_sync:hash:<相对路径>` | 上次同步成功时的**本地内容哈希**（FNV-1a 64，纯件可算，不引 OpenSSL 进 logic） | **T4 新增**，draft 无 |

不用 mtime（spec F1 明令）。

### 1.2 状态与转移表

状态 = f(记录, 本地现状, 远端现状)。纯件判定（进 `github_sync_logic`，表驱动单测）：

```
enum class NoteSyncState {
    InSync,          // 记录有 && 本地hash==记录 && 远端sha==记录
    New,             // 记录无 && 本地有            → 本地领先
    LocalModified,   // 记录有 && 本地hash≠记录 && 远端sha==记录
    RemoteModified,  // 记录有 && 本地hash==记录 && 远端sha≠记录
    Diverged,        // 记录有 && 本地hash≠记录 && 远端sha≠记录
    LocalDeleted,    // 记录有 && 本地无            → 删除传播
    RemoteNew,       // 记录无 && 本地无 && 远端有   → 仅 pull 场景有意义
};
```

| 状态 | 触发事件 | 动作 |
|---|---|---|
| InSync | 每轮同步扫描 | 跳过（**这是 T4 相对 draft 的主要收益**：draft 对每篇本地笔记无条件 PUT） |
| New | 本地新建笔记后同步 | `putFile(path, content, shaIfMatch=当前远端sha或空)` → 成功后 `recordSyncedNote` + `recordHash` |
| LocalModified | 笔记内容变化后同步 | 同 New（shaIfMatch=记录 sha；远端没动则不 409） |
| RemoteModified | 拉取时发现远端比记录新 | push 方向：先 GET 远端内容 → 以远端 sha 为 shaIfMatch PUT（覆盖远端）**或**提示冲突（409 路径已弹窗，可复用 `GitErrorDialogVariant::Conflict409`）；pull 方向：远端覆盖本地 |
| Diverged | 双方都动过 | 不自动推：走 409 弹窗（拉取后再推 + F6 清单确认） |
| LocalDeleted | 本地删文件后同步 | DELETE（draft `runGithubSyncPush` 内已有"安全栏"：文件还在盘上只是读失败 → 跳过，:911-918，保留）→ 成功 `forgetSyncedNote` + 删 hash |
| RemoteNew | pull 树里有本地没有的远端笔记 | 下载落到本地（T4 新增；保守策略照决策 #19——push 时"登记无的远端不动"） |

推送扫描顺序（保持 draft 的稳定性）：按文件名字典序 Put，再按文件名字典序 Delete（`github_sync_logic.cpp:59-75` 注释承诺的顺序不变）。

### 1.3 与 draft 的差量

- draft `diffNotes`（`github_sync_logic.h:52`）只做"本地有→PUT / 登记有本地无→DELETE"，**无内容比对** → 每轮全量 PUT。T4 扩签名（见 §2）传入本地 hash 与记录 hash，InSync 的文件不进变更集。
- database.sql 无独立状态机（它是整库覆盖，永远 PUT；冲突检测靠 shaIfMatch → 409，draft :846-852 已接）。sha 登记可省——每次 push 前都重新 GET 现值（draft :840-843 现做法，可保留）。

---

## 2. T4 函数清单（对齐 T2/T3 draft 真实 API，未发明）

### 2.1 `github_sync_logic.h/.cpp`（纯件，新函数）

```cpp
// 状态机（§1.2 表驱动）
enum class NoteSyncState { InSync, New, LocalModified, RemoteModified, Diverged, LocalDeleted, RemoteNew };
NoteSyncState classifyNote(bool hasRecord, bool localExists,
                           bool localHashMatches, bool remoteShaMatches);

// 内容哈希：FNV-1a 64bit，纯件、零依赖（不引 OpenSSL，避免 logic 依赖 platform）
std::string contentHash(const std::string& bytes);

// 变更集升级版：diffNotes 改为接收「本地名→(内容,hash)」与「登记名→(sha,hash)」，
// InSync 文件不产出 FileChange。旧 diffNotes(local, remote) 可保留为新函数的退化形态
//（或直接改签名 + 改 draft 调用点 settings_module.cpp:873 —— 二选一，倾向改签名，避免双轨）。
std::vector<FileChange> diffNotesByState(
    const std::map<std::string, NoteRecord>& local,      // NoteRecord{content, hash}
    const std::map<std::string, SyncedRecord>& synced,   // SyncedRecord{remoteSha, contentHash}
    const std::set<std::string>& remoteTreeNames);       // F7 树里的 notes/*.md（pull 用）

// F5：退出时是否值得推一次（决策：dirty 且距上次 commit ≥ 60s）
bool exitPushDue(int64_t lastCommitSec, int64_t nowSec);   // 常量 kExitPushMinGapSec = 60

// F4：skip 表常量从 settings_module.cpp:108 迁来（spec F4 要求"常量放 github_sync_logic，单测锁死"）
inline constexpr std::array<const char*, 2> kSkipTables = {"claude_call_logs", "claude_run_settings"};

// F6：笔记目录备份名 `notes-<ts>`；与库备份同 prune 规则（复用 pruneBackups，prefix="notes-"）
std::string notesBackupDirName(int64_t tsSec);
```

### 2.2 `github_api.h/.cpp`（T2 文件，新端点）

```cpp
// F7：拉取全树。返回 path→(sha,size)。truncated=true 时按需再取子树（先只做 truncated 报错，
// 分页子树按 spec F7「按需再取」可后补——dump+notes 仓库极小，truncated 实际不可达，报错即可）。
struct TreeEntry { std::string path; std::string sha; uint64_t size = 0; };
core::Result<std::vector<TreeEntry>> listTree(const std::string& branch, bool& truncated);
```

复用 T2 已有的 `doRequest` 重试/日志/base64 管线；新增一个 GET 端点，改动面小。

### 2.3 `settings_module.cpp`（编排，改 draft 现有函数 + 新增）

```cpp
// F3：导入前暂停后台写者（见 §4 接线）
void suspendBackgroundWritersForDbImport();
void resumeBackgroundWritersAfterDbImport();
// F3：导入失败回滚——导入前先 exportToString 存内存作回滚副本，失败时 importFromString(副本)。
// （不能用备份 .db 文件回写：连接开着时覆盖 db 文件不可靠；SQL 级重放才安全。）
// F3：导入成功后——重新 exec "PRAGMA foreign_keys=ON" + emit 全局重载事件（§4）
// F5：onStop / 进程退出钩子里：dirty && exitPushDue → 有界后台推（上限 5s，超时放弃+日志，
//     下次启动卡片提示「有未同步改动」——需要 `github_sync:unsynced_notice` 状态键）
// F6：409 确认清单——拉取覆盖前列出将被覆盖的本地未推文件（push 状态机里的
//     LocalModified/Diverged 名单），塞进 GitErrorDialogIn（`git_error_dialog.h` 需加字段）
```

---

## 3. 节流 / 首次方向 / 删除传播 / 备份的落点

| 项 | spec 决策 | draft 现况 | T4 差量 |
|---|---|---|---|
| 5 分钟节流 | #14/#17 | **已实现**：`gsync::throttleDue`（`github_sync_logic.cpp:9-13`，含时钟回拨保护）；`maybeAutoGithubSync`（settings_module.cpp:607-612）；手动绕开 `startGithubSyncAction:648-650` | 无（单测已在 `test_github_sync_card.cpp:432`） |
| 首次方向 | #16 | **已实现**：`firstSyncChoice`（logic.cpp:77-81）+ `runGithubSyncFirstSync`（settings_module.cpp:763）+ `isFirstSyncDone` 状态键（config.h:47-49） | 无 |
| 删除传播 | #19 | **半实现**：push 侧 DELETE 已有（settings_module.cpp:916-943，含安全栏）；但变更集基于"登记有本地无"，无 F1 内容判定 | 接 §1 状态机；pull 侧"远端已删的本地保留"语义随笔记拉取（§5）一起补 |
| 备份 5 份 | #11/F6 | **半实现**：`backupLocalDb`（settings_module.cpp:987-1015，wal_checkpoint + 整文件拷 + prune）只保**库**；pruneBackups 纯件已可复用 | F6：笔记整目录 → `backups/notes-<ts>/`（prune prefix=`notes-`，独立计数）；409 确认框列覆盖清单 |

---

## 4. 导入前后暂停/重载（F3）— 接线点与跨模块约束

**要暂停谁**：
1. **QuotaRefresher**（`src/module_claude_proxy/quota/quota_refresh.h:43`，真后台线程，60s 一轮写 QuotaStore —— 同一个 `appDatabase()`）。已有现成 API：`stopAll()`（幂等、有界等待）+ `start()`（幂等）。
2. **同步自身**：已由单线程闸（`startGithubSyncAction:644` joinable 检查）天然互斥，无需额外处理。
3. **ClaudeService 代理**：不暂停。import 是单事务（`importFromFile` 事务包裹），读侧要么见旧态要么见新态，短暂 SQLITE_BUSY 由现有连接互斥锁（memory: 单 sqlite 连接并发纪律）吸收。spec F3 的"暂停后台任务"清单只点名余量刷新与同步自身。

**🔴 跨模块依赖红线**：`module_settings` 不可依赖 `module_claude_proxy`（模块隔离，`scripts/check-module-isolation.sh` 守卫）。**接线走 EventBus**（`src/core/event_bus.h`）：
- 新事件 `db.import_begin` / `db.import_end`（常量进 event_bus.h）；`module_claude_proxy` 的 `claude_module.cpp` 订阅 → `stopAll()` / `start()`。QuotaRefresher 生命周期归 claude_module 管，settings 只广播。
- 导入成功后重载：`requestUiUpdate()`（已用）只刷 UI 帧，**不够**——需 emit `db.imported`，各模块（markdown/todo/claude/settings/agent）订阅后做 onPageEnter 语义的强制重载（缓存丢弃）。settings 自己的 `githubSyncStore_` 也要重建（指向的表被 DROP 重建过）。
- 导入成功后 settings 侧 `PRAGMA foreign_keys=ON` 重执行 + `db.integrity_check`（spec F3）。

**回滚**：导入失败 → `importFromString(导入前内存副本)` → 成功：报错但数据无恙；连回滚也失败：报错 + 指引去 `backups/devdesk-<ts>.db`（人工兜底，日志 WARN）。

---

## 5. 冲突预警：T3 draft 里 T4 要动的函数（逐个，draft 行号）

| file:line | 函数/对象 | T4 动作 | 风险 |
|---|---|---|---|
| settings_module.cpp:108 | `kGithubSyncSkipTables` | 迁到 github_sync_logic（F4） | S |
| :615-621 | `markGithubSyncDirty` | 事件源补齐（见下） | S |
| src/core/event_bus.h:27-29 | 事件常量表 | 缺 `md.deleted` / `todo.updated` / `todo.deleted`（决策 #20 要求"笔记删除 + 待办增删改"都置 dirty，现只有 md.saved/todo.created/todo.completed）；**发布方也要补 emit 点** | S |
| :805-944 | `runGithubSyncPush` | 接状态机变更集（:873 `diffNotes` 调用点换 `diffNotesByState`）；F9 体积上限检查（dump > ~73MB 原始 → 明确报错，现无任何检查）；决策 #15 本地 git best-effort commit（probe 已有 :156，commit 动作缺） | M |
| :945-985 | `runGithubSyncPull` | **重写大半**：F7 listTree + 笔记拉取合并（现只导 database.sql）+ F3 暂停/回滚/重载 + F6 笔记备份 | L |
| :987-1015 | `backupLocalDb` | 加笔记目录备份（F6） | S |
| :1049-1095 | `harvestGithubSync` | 导入成功后的重载触发（emit `db.imported`） | S |
| :194-198 | `onStop` | F5 退出推送（另接 main.cpp 退出钩子；注意 onStop 也被 LiveToggle 模块停用触发，"退出推送"要区分进程退出 vs 模块停用——用进程退出钩子而非 onStop） | M |
| github_sync_logic.h:52 | `diffNotes` | 改签名/换新函数 | M |
| github_sync_config.h:55-58 | sha 登记三方法 | 加 hash 键读写（或合成一个 `recordNote(fileName, sha, hash)`） | S |
| git_error_dialog.h | `GitErrorDialogIn` | F6 覆盖清单字段（列表 + 文案区） | S |
| src/core/git_sync_service.{h,cpp} | 旧 git 遗留 | 决策 #9：**删除**（draft 里还活着，`src/core/CMakeLists.txt` 同步摘除；`paths::sqlDumpFile()` 保留改落 appData 已由 draft 处理——合并后复核） | S |

---

## 6. 测试计划（编排层，表驱动 + 假发送器/假时钟，零真网）

1. **状态机转移表**：`classifyNote` 7 态 × 布尔组合穷举（含记录缺失边界）。
2. **contentHash**：FNV-1a 已知向量（空串/单字节/长串）+ 稳定性。
3. **diffNotesByState**：InSync 不产出；New/LocalModified/LocalDeleted 顺序稳定；RemoteNew 只在 pull 侧。
4. **throttle/exitPushDue**：假时钟边界（恰 5min/恰 60s/时钟回拨）。
5. **F6**：notes 目录备份与库备份独立计数、各自保 5 份删最旧（临时目录断言，复用 `github_sync_logic_backup_prune` 用例 :467 的模式）。
6. **F7**：假发送器回 trees JSON → 条目筛选 notes/**.md + data/database.sql；`truncated=true` → 明确报错。
7. **pull 合并**：远端新增落盘 / 远端更新覆盖 / 远端已删本地保留 / database.sql 导入失败 → 回滚副本内容恢复（注入坏 dump）。
8. **F3**：`db.import_begin/end` 事件 → 假订阅者收到；导入失败不 emit imported。
9. **F4**：常量锁死用例（skip 表逐项断言，防手滑扩表）。
10. **F5**：dirty + 距上次 commit 65s → 触发；35s → 不触发；5s 超时放弃 → 通知键置位。
11. **F9**：构造 >100MB base64 前的原始体积判定（注入小常量上限做单测）。
12. 变异验证：每条新断言用"回退实现"证明会红（项目口径：恒真假绿重写）。

## 7. 工期预估（按复杂度分票）

| 票 | 内容 | 量 |
|---|---|---|
| T4-S1 | F4 常量迁移 + 单测锁死 | S |
| T4-S2 | dirty 事件补齐（event_bus 常量 + md/todo 发布方 emit + 订阅） | S |
| T4-S3 | F9 体积上限 + git_sync_service 删除（决策 #9） | S |
| T4-M1 | F1 状态机：contentHash + classifyNote + diffNotesByState + config 键 + push 接线 + 表驱动单测 | M |
| T4-M2 | F5 退出推送（退出钩子 + 60s 判定 + 5s 上限 + 通知键） | M |
| T4-M3 | 决策 #15 本地 git best-effort commit（probe 已有，补 init/commit 调用，失败只记日志） | M |
| T4-M4 | T2 文件加 `listTree`（F7）+ 假发送器单测 | M |
| T4-L1 | pull 重写：F7 笔记拉取合并 + F3 暂停/回滚/重载（EventBus 接线 + claude_module 订阅）+ F6 笔记备份与 409 覆盖清单 | L |

关键路径：T4-M1 → T4-M4 → T4-L1。**前置依赖**：T1（47fe68a）必须先合并——pull 导入依赖索引/触发器 DDL，否则 schema 静默退化（spec F2）。T2/T3 合并后所有行号复核一遍（尤其 settings_module.cpp 是在飞 draft）。

## 8. 最大风险

**T3 draft 与 T4 差量在同文件（settings_module.cpp）上叠写**：T3 还在飞（该 worktree 有未提交修改），T4 的 L1 票要重写 `runGithubSyncPull` 主体——若 T3 合并前就按 draft 行号开工，合并即冲突。**纪律：T4 开工以 dev 上合并后的 T3 为基线重读一遍本预案的行号表，且 T4 与 T3 不得并行派单写同一文件。**
