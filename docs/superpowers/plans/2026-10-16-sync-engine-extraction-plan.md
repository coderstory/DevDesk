# SyncEngine 抽取实施预案（只读设计，不含工期 / 不含代码）

> 日期：2026-10-16 ｜ 类型：**设计预案**（未动代码，未构建，未跑测试）
> 依据：spec `docs/superpowers/specs/2026-10-15-github-sync-api-design.md`（§3 架构 / §7 文件面 / 决策 #15）
> 留痕：T4 审计 A11（`2026-10-16-github-sync-t4-audit-architecture.md`）+ T5 清理审计
> 代码基线：`src/module_settings/settings_module.cpp` 当前 **2161 行**（磁盘实测，非 2103）
> 目标读者：醒来后要拍板「是否启动这张票 / 以什么形态启动」的用户

## 0. 目标与非目标

**目标**：把「同步编排」（push / pull / firstsync / 节流驱动 / 备份 / 状态机跑批 / 结果汇总）从 `SettingsModule` 抽成一个**可单测的 `GithubSyncEngine`**（`module_settings` 内新文件），`SettingsModule` 只留 **UI 接线 + 后台线程 + 每帧收割 + 弹窗渲染**。

**判据（怎么算抽成功）**：
- 编排层的 `push` / `pull` / `firstSync` 能**不启动任何 UI、不建真线程、不拨真网**地单测（注入假 sender + 临时目录 + 内存库 + 假时钟）。
- `SettingsModule` 不再持有 `runGithubSync*` 那一整段（当前 `:841-1433` 的 ~590 行）。
- 行为**零变化**（同输入同输出；现有 24 条 `github_sync` 用例 + 卡片用例 + 全量套件保持全绿）。

**非目标**（本票不做，避免过度设计）：
- 不改线程模型（后台 `std::thread` + `githubSyncDone_` 原子位 + 每帧 `harvestGithubSync` 收割，照旧留在 SettingsModule）。
- 不改节流/退出的**决策**逻辑——`gsync::autoPushDue` / `exitPushDue` / `throttleDue` 已是纯件（`github_sync_logic.cpp`），且已单测；本票只搬编排，不搬这些纯件。
- 不动 `github_sync_logic`（纯件层已是对的，审计原话「接口深度是对的」）。
- 不评估工期，不改代码。

---

## ① 要抽的成员 / 方法全清单（file:line）

行号 = `src/module_settings/settings_module.cpp` 当前磁盘内容。

### A. 搬进引擎（MOVE → `github_sync_engine.cpp`）

| # | 成员 / 方法 | 行 | 性质 | 抽取时改写点 |
|---|---|---|---|---|
| M1 | `runGithubSyncAction(action, repoUrl, token, branch)` | `:841-931` | 动作分发（Test/Push/Pull/PullThenPush/ValidateFirstSync/FirstSync） | `this->` 成员 → `deps_.`；输出参数 + `staged_*` 写 → 返回 `SyncRunResult` |
| M2 | `runGithubSyncFirstSync(...)` | `:935-975` | 首次方向 | 同上；`summary/conflict/branchHint` 出参 → 结果字段 |
| M3 | `runGithubSyncPush(...)` | `:992-1254` | 推送（dump→分支→树→判态→PUT/DELETE） | `::core::appDatabase()` → `deps_.db`；`::core::paths::sqlDumpFile()` → `deps_.sqlDumpFile`；`githubSyncStore_` → `deps_.store`；`publishGithubSyncProgress` → `publish()`；`syncAborted()` → `aborted()` |
| M4 | `runGithubSyncPull(...)` | `:1259-1433` | 拉取（树→按 sha 判变→导入→下载） | 同上；`githubSyncStagedDbImported_.store(true)` → 结果字段 `dbImported=true` |
| M5 | `backupLocalDb(db, outName)` | `:1437-1464` | 备份本地库（wal_checkpoint + 拷贝 + prune） | `::core::paths::appDataDir()/sqliteDbFile()` → `deps_` 路径；`nowSec()` → `deps_.nowSec` |
| M6 | `backupNotesDir(outName)` | `:1468-1511` | 整目录备份笔记 | `::core::paths::markdownDir()` → `deps_.notesDir` |
| M7 | `collectLocalNotes()`（static） | `:1514-1534` | 本地笔记快照（名→内容） | 读 `deps_.notesDir`；**保持 static 语义**（纯读盘） |
| M8 | `stashOverwriteListForConflict(putNames)` | `:980-989` | 409 覆盖清单装配 | 不再写 `staged_`，改成**填结果字段** `overwriteFiles` |
| M9 | `readFileToString` / `writeFileFromString` | `:122-138` | 文件读写小工具 | 只被 M3/M4 用 → 随之下沉引擎 `.cpp` 匿名 namespace |
| M10 | `SyncAction` 枚举 | `:112` | 动作类型 | 移到引擎头（SettingsModule 与引擎共用；或独立小头） |

> **M8 注意**：当前 `stashOverwriteListForConflict` 在**冲突分支**里被调用（M3 `:1133` / `:1148` / `:1189`），把清单写进锁内 `staged_`。抽后改为「把清单放进 `SyncRunResult.overwriteFiles`」，由 SettingsModule 的线程 lambda 在 join 后搬进 UI 侧 `githubSyncOverwriteFiles_`（现有搬运点 `harvestGithubSync:1573` 不变）。

### B. 留在 SettingsModule（STAY，UI 接线 / 线程 / 收割 / 弹窗）

| # | 成员 / 方法 | 行 | 为何留 |
|---|---|---|---|
| S1 | `startGithubSyncAction(action, manual)` | `:776-837` | 线程生命周期 + 前置校验（URL/Token/节流/落 keychain）+ 起线程。**线程 lambda 体改成构造引擎并调 `engine.run(...)`** |
| S2 | `harvestGithubSync()` | `:1546-1646` | 每帧收割（join → 搬结果 → 清 dirty / 记节流时点 / 落 first_sync_done / 弹窗分支）。**全部留在 UI 侧** |
| S3 | `waitOrDetachSyncThread()` | `:742-761` | 退出闸（5s 等待 / detach）。线程模型的一部分 |
| S4 | `runExitSyncPush()` | `:705-728` | 退出补推**决策**（`exitPushDue` + dirty + autoPush） |
| S5 | `maybeAutoGithubSync()` | `:650-658` | 自动同步**决策**（`autoPushDue`）→ 调 S1 |
| S6 | `markGithubSyncDirty()` / onStart 订阅循环 | `:661-665` / `:179-189` | EventBus 订阅 + dirty 置位 |
| S7 | `maybeRefreshLocalGitProbe()` / `maybeWarnNoLocalGit()` | `:681-698` / `:668-676` | 探测驱动 + 警告 toast（UI） |
| S8 | `publishGithubSyncProgress()` | `:1540-1543` | 进度槽的 UI 侧落点（作为 `deps.progress` 注入引擎） |
| S9 | `syncAborted()` | `:731-733` | 取消位读取（作为 `deps.aborted` 注入引擎） |
| S10 | `setGithubSyncError()` | `:764-773` | 前置校验失败的 UI 错误行 + 弹窗 |
| S11 | 卡片/弹窗/回执/重试一族 | `:566-646` / `:1649-1670` | 纯 UI（`applyGithubSyncEnabled` / `githubSyncPush/Pull` / `retryGithubSync` / `drawGithubSyncErrorDialog` …） |
| S12 | 全部 `githubSync*` 成员变量 | `:2067-2119` | 见下「成员归属」 |

### C. 成员归属（`:2067-2119` 逐个判）

**留 UI 侧**：`githubSyncCfg_` / `githubSyncTokenDraft_` / `githubSyncStatus_` / `githubSyncStatusError_` / `githubSyncThread_` / `githubSyncDone_` / `githubSyncMutex_` / `githubSyncProgressPending_` / `githubSyncProgress_` / `githubSyncLogs_` / `githubSyncError_` / `githubSyncResultSummary_` / `githubSyncResultOk_` / `githubSyncLastAction_` / `githubSyncLastManual_` / `githubSyncErrorDialog_` / `githubSyncDirty_` / `githubSyncLastCommitSec_` / `githubSyncRepoPublic_` / `githubSyncBranchHint_` / `githubSyncOverwriteFiles_` / `githubSyncLocalGitProbe_` / `githubSyncLocalGitProbeFuture_` / `githubSyncLocalGitProbeDirty_` / `githubSyncNoGitWarned_` / `githubSyncUnsyncedNotice_` / `githubSyncDeleteSkipHint_` / `githubSyncSubs_`。

**其中 `staged_*` 系列**（`githubSyncStagedLogs_/Summary_/Ok_/Conflict_/PublicRepo_/BranchHint_/OverwriteFiles_/DeleteSkipHint_/DbImported_`，`:2079-2094`）：**语义降级**——它们现在承接「引擎 → UI」的搬运。抽后 `runGithubSyncAction` 返回 `SyncRunResult`，线程 lambda 在锁内把这 9 个字段一次性搬进 `staged_*`（或直接搬进 UI 成员），**`staged_*` 成员可保留**（改动最小）或随迁移删减。**推荐保留**：搬运层是线程安全边界，不动它风险最小。

**注入引擎的**：`GithubSyncStore*`（`deps.store`）/ `core::appDatabase()`（`deps.db`）/ 三个路径 / 时钟 / 进度槽 / 取消位 / api 工厂。

---

## ② 引擎接口草案（构造依赖注入）

对齐既有缝风格：`agent_module.h:94-101` 的 `struct Deps`（测试传 fake、生产传空回退）+ `local_git_probe.h` 的平台拆文件（这里不需要拆平台，引擎零平台宏）。

```cpp
// src/module_settings/github_sync_engine.h（新增）
#pragma once
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/result.h"
#include "core/sqlite_db.h"
#include "github_sync_config.h"       // GithubSyncStore（登记 / 导入 / 配置）
#include "platform/github_api.h"      // GithubApi / ApiLogEntry

namespace module::settings {

// 同步动作（原 settings_module.cpp 匿名 namespace 的 SyncAction，移到这里共用）。
enum class SyncAction { None, Push, Pull, Test, PullThenPush, FirstSync, ValidateFirstSync };

// 一次动作的结果：引擎产出、UI 侧消费；零 UI 类型依赖（无 eui / 无卡片结构）。
struct SyncRunResult {
    bool ok = false;
    std::string summary;
    bool conflict = false;                     // 409 → UI 走冲突弹窗变体
    bool publicRepo = false;                   // 测试连接发现 public 仓 → 黄字
    std::string branchHint;                    // 分支被自动创建提示
    std::vector<std::string> overwriteFiles;   // 409 覆盖清单（原 stashOverwriteListForConflict）
    std::string deleteSkipHint;                // 删除传播安全栏跳过提示（原 staged_）
    bool dbImported = false;                   // 本轮导入过库 → UI 重载配置缓存
    std::vector<platform::github::ApiLogEntry> logs;  // 弹窗日志源（原 api.recentLog()）
};

// 引擎依赖（构造注入）。生产传真实实现；测试传 fake —— 对齐 agent_module.h 的 Deps 风格。
struct SyncEngineDeps {
    core::data::SqliteDb* db = nullptr;     // appDatabase；nullptr → 运行期报「数据库未就绪」
    GithubSyncStore* store = nullptr;       // 登记 / 导入 / 配置缓存重载

    // 时钟缝（nullptr = 生产真实系统时间；测试注入假时钟驱动节流/备份名）。
    std::function<int64_t()> nowSec;
    // 进度槽（引擎各阶段调；UI 侧在锁内落 pending）。nullptr = 丢弃。
    std::function<void(const std::string&)> progress;
    // 取消位（退出闸置位后各阶段边界收束）。nullptr = 永不中止。
    std::function<bool()> aborted;

    // 路径缝（空 = 生产 core::paths；测试注入临时目录）。
    std::filesystem::path notesDir;      // 空 → core::paths::markdownDir()
    std::filesystem::path appDataDir;    // 空 → core::paths::appDataDir()
    std::filesystem::path sqlDumpFile;   // 空 → core::paths::sqlDumpFile()

    // api 工厂（测试注入假 sender 的 GithubApi；生产按 token/owner/repo 建真 api）。
    // 空 → 生产默认实现。
    std::function<std::unique_ptr<platform::github::GithubApi>(
        const std::string& token, const std::string& owner,
        const std::string& repo)> apiFactory;
};

class GithubSyncEngine {
public:
    explicit GithubSyncEngine(SyncEngineDeps deps);

    // 执行一个动作（同步阻塞；调用方在后台线程跑）。repoUrl 解析失败 → 结果 ok=false。
    SyncRunResult run(SyncAction action, const std::string& repoUrl,
                      const std::string& token, const std::string& branch);

    // 分步编排（run 按 action 分发到这些；单测可直接调，绕过动作分发）。
    SyncRunResult push(platform::github::GithubApi& api, const std::string& branch);
    SyncRunResult pull(platform::github::GithubApi& api, const std::string& branch);
    SyncRunResult firstSync(platform::github::GithubApi& api, const std::string& branch);

private:
    SyncEngineDeps deps_;
    void publish(const std::string& text);
    bool aborted() const;
    int64_t now() const;
    std::filesystem::path notesDir() const;    // 空回退 core::paths
    // 内部：备份（库 / 笔记目录）、本地笔记快照（M5/M6/M7）
    core::Result<void> backupLocalDb(std::string& outName);
    core::Result<void> backupNotesDir(std::string& outName);
    std::map<std::string, std::string> collectLocalNotes() const;
};

} // namespace module::settings
```

**关键设计点**：
1. **引擎不持有线程**。`run()` 是**同步阻塞**调用；线程仍由 `SettingsModule::startGithubSyncAction` 拥有。引擎是「一次动作 = 一个对象」或可复用对象，绝不跨线程共享。
2. **假 api 走 `setSender`**（`github_api.h:32/38` 已有注入点，spec §4 明写「发送器与 sleep 均可注入」）。`apiFactory` 缝只是让引擎不自己 `new` 真 api；测试工厂里建 `GithubApi` 后 `setSender(fake)` 即零真网。**不需要给 GithubApi 造虚接口**。
3. **进度 / 取消 / 时钟走 `std::function` 缝**，与 `agent_module.h:100` 的 `Scheduler::NowFn schedulerNow` 同风格。
4. **路径走显式字段 + 空回退 core::paths**，测试可注临时目录（对齐 `DataRootEnv` 测试基建）。
5. `push/pull/firstSync` 返回 `SyncRunResult`（而非出参），`firstSync`/`PullThenPush` 内部组合它们——语义更清晰，单测也能直接打这三步。

---

## ③ 迁移顺序（每步可编译可测）

**原则**：每步一个原子 commit，每步结束必须 **`bash scripts/build-test-run.sh --filter github_sync` 绿 + 卡片用例绿 + 全量套件绿**；任一步红 → `git revert` 该 commit 回退（不硬改）。

| 步 | 动作 | 编译产物变化 | 测试判据 |
|---|---|---|---|
| **T1** | 新增 `github_sync_engine.h` + `.cpp`；把 `SyncAction`（M10）、`readFileToString`/`writeFileFromString`（M9）、7 个编排方法（M1–M8）**逐字搬运**进引擎，`this->成员` → `deps_.`、出参 + `staged_*` 写 → `SyncRunResult`。SettingsModule 的线程 lambda（S1 内 `:826-833`）改成构造 `SyncEngineDeps`（真 store/db/paths/时钟/进度槽/取消位）→ `GithubSyncEngine::run(...)` → 锁内把结果搬进 `staged_*`。CMakeLists 注册新 `.cpp`。 | +`github_sync_engine.cpp`（~720 行）/ +`.h`（~130 行）；`settings_module.cpp` 减 ~680 行 | `--filter github_sync` 24/24；卡片用例全绿；全量套件 `Failed: 0`。**行为零变化**（逐字搬运，无逻辑编辑） |
| **T2** | 新增 `tests/test_modules/test_github_sync_engine.cpp`：`FakeSender` 注入 + `InMemoryDb`/`ScopedAppDatabase` + 临时目录（`DataRootEnv`）+ 假时钟。覆盖见 ⑤。CMakeLists 注册测试。 | +测试文件（~350–450 行） | 新用例全绿；**并做变异验证**：故意把 `pushActionForState` 的某分支改错，新用例必红（证明断言非恒真） |
| **T3** | 清理 `settings_module.cpp` 中已成死代码的 include / 私有辅助；`nowSec()`（`:115`）若引擎侧自带则 UI 侧保留（退出/节流仍用）。**只删声明，不改逻辑**。 | `settings_module.cpp` 再减 ~20 行 | 全量套件绿 |
| **T4（可选，后续票）** | 若引擎 `.cpp` 超 ~800 行，把 push/pull 各拆一个 TU（`github_sync_engine_push.cpp` 等）。**本票不做**（避免为拆而拆）。 | — | — |

**为何 T1 是「逐字搬运」而非「顺手重构」**：抽取的唯一目标是**开测试缝**，不是改行为。任何顺带的逻辑「改进」都会让「行为零变化」的判据失效，也会把回归风险混进重构 commit。T1 只允许三类改动：① `this->X` → `deps_.X`；② 出参 / `staged_` 写 → 返回值；③ 把 `::core::paths::*` / `::core::appDatabase()` 换 `deps_.*`（带空回退）。

---

## ④ 抽取后各文件预估行数

| 文件 | 现在 | 抽取后（估） | 说明 |
|---|---|---|---|
| `settings_module.cpp` | 2161 | **~1480–1500** | 减 ~680（M1–M8 方法体）+ ~20（M9/M10），加回 ~15（线程 lambda 构造引擎） |
| `github_sync_engine.h`（新） | — | **~120–140** | Deps / Result / 类声明 + 契约注释 |
| `github_sync_engine.cpp`（新） | — | **~700–740** | M1–M9 搬运体 + 引擎胶水（publish/now/路径回退） |
| `github_sync_logic.cpp/.h` | 217 / 152 | 不变 | 纯件层不动 |
| `github_sync_config.cpp/.h` | 405 / 88 | 不变 | store 不动 |
| `github_sync_card.cpp` / `git_error_dialog.cpp` | 355 / 272 | 不变 | UI 不动 |
| **合计（同步相关）** | ~3650 | **~3700–3740** | 净增 ~50–90 行 = 接口头 + 胶水的成本；换来 680 行编排可单测 |

> 数字会随实现漂移，以落地后 `wc -l` 为准。**判据不是行数**，是「`runGithubSync*` 不再出现在 `settings_module.cpp`」+「引擎单测可跑」。

---

## ⑤ 需要的测试缝隙与用例清单

**编排层零覆盖是已知欠账**（T4 审计逐条核查点 7 + 核查点 4 明写「`runExitSyncPush` 本体没有任何自动化测试」）。当前 `tests/test_modules/` 里：
- `test_github_sync_logic.cpp`（18812 B）：只测纯件（状态机 / hash / prune / 节流）。
- `test_github_sync_card.cpp`（30116 B）：只测卡片/弹窗**无头 compose**（注入 lambda，零真线程）。
- `test_settings.cpp`：只经 `create_settings_module()` 黑盒驱动（`clickSettingsTab` + `compose`），**打不到 `runGithubSyncPush/Pull` 内部**（要真网络）。

→ **`push`/`pull`/`firstSync`/备份的编排逻辑，当前零用例**。抽取的直接收益就在这里。

### 需要的缝隙（引擎 `SyncEngineDeps` 已覆盖）
1. **假 api**：`apiFactory` 里建 `GithubApi` + `setSender(fake)`。fake 按 `(method, path)` 回预设响应（`getRepo`/`listTree`/`getFile`/`putFile`/`deleteFile`/`createRef`）。**用例结束必须 `setSleepFunction(nullptr)` 复位**（`github_api.h:38-39` 静态缝，污染同进程后续用例）。
2. **临时目录**：`notesDir` / `appDataDir` / `sqlDumpFile` 注临时目录（`DataRootEnv` 基建）。
3. **内存库**：`InMemoryDb` + `ScopedAppDatabase`（测试基建，见 TIN-06 迁移）。
4. **假时钟**：`nowSec` 注入递增计数器（驱动备份名 / 节流判定）。
5. **进度 / 取消槽**：捕获字符串序列断言阶段文案；`aborted` 返回 true 断言「阶段边界收束」。

### 用例清单（建议）

**push（M3）**
- 新笔记（无登记 + 本地有）→ `putFile` 一次，`shaIfMatch` 为空；登记写 `recordSyncedNote`。
- 本地改（登记 hash≠本地）→ `putFile` 带**登记 sha** 作 `shaIfMatch`。
- 删除传播（登记有 + 本地无 + 远端未变）→ `deleteFile` + `forgetSyncedNote`。
- **删除安全栏**（登记有 + 本地读失败但文件仍在磁盘）→ 不删远端、`deleteSkipHint` 非空（现 T4 A5 类静默面）。
- **409 冲突**（Diverged / LocalDeleted+远端变）→ `conflict=true` + `overwriteFiles` 含 `data/database.sql` + Put 集；**断言不推任何笔记**。
- dump 超限（>73MB）→ `ok=false` + summary 含「过大」。
- **断言阶段边界查 `aborted()`**：`aborted` 恒 true → 各步早退、不调 api。

**pull（M4）**
- 远端 sql sha ≠ 登记 → 下载 + 导入 + `dbImported=true` + 登记新 sha。
- 远端 sql sha == 登记 → **跳过导入**（省一次全库重建）。
- 笔记远端变过 → 下载覆盖 + 登记。
- 远端已删笔记 → `KeepLocal` + 清登记（保守语义）。
- **备份先于覆盖**：`backupNotesDir` 失败 → 中止、不导入（断言 api.getFile 未被调）。
- 备份保留 5 份（临时目录断言 prune）。

**firstSync（M2）**
- 远端有 sql → `PullThenPush`（先 pull 再 push）。
- 远端无 sql（NotFound）→ `Push`。
- 探测出错（非 NotFound 的 fail）→ `Fail`，不动作。

**备份（M5/M6）**
- `backupLocalDb` 名格式 `devdesk-<ts>.db`；prune 保 5。
- `backupNotesDir` 只拷 `.md`；笔记目录不存在 → ok（无可备份）。

**harvest 搬运（S2，仍走 test_settings 黑盒）**
- 补一条：pull 导入后 `githubSyncStagedDbImported_` → `githubSyncCfg_` 重载（现无覆盖）。

### 变异验证（必做）
至少挑 1 条：把 `pushActionForState`（`github_sync_logic.cpp:141`）的 `LocalDeleted` 分支从 `Delete` 改成 `Skip`，断言 push 删除传播用例**变红**——证明新用例真钉住了行为，不是恒真。

---

## ⑥ 风险与回退点

| # | 风险 | 影响 | 缓解 / 回退 |
|---|---|---|---|
| R1 | **逐字搬运引入语义漂移**（把某成员读改成 `deps_` 时改错对象，或漏搬一个 `staged_` 字段） | 同步行为悄悄变（如 dirty 不清、覆盖清单空） | T1 只允许三类机械改动（见 ③ 原则）；**全量套件 + 24 条 github_sync 用例必须绿**；搬运后逐字段对照 `SyncRunResult` ↔ 原 `staged_*` 做一次人工清单核对 |
| R2 | **对象生命周期**：引擎持 `GithubSyncStore*` / `SqliteDb*` 裸指针，若线程 detach 后 store 已析构 → 悬垂 | 退出期崩溃（SQLite 事务性不脏库） | 生命周期**与现状一致**：线程仍捕获 `this`；`onStop`/`harvest` 先 join 再 `store_.reset()`（`settings_module.cpp:219/231`）；`waitOrDetachSyncThread` 的 detach 面**不被本票放大**（引擎是局部对象，随 lambda 结束析构，不额外延长裸指针寿命） |
| R3 | **`dbImported` 的原子性**：`githubSyncStagedDbImported_`（`:2089`，atomic）抽成 `SyncRunResult.dbImported`（普通 bool）后，跨线程可见性靠 `githubSyncDone_` 的 release/acquire + join | 理论上可能读不到导入标记 → 配置缓存不重载 | 结果对象由**线程内构造、join 后读取**（join 即 happens-before），无需原子；harvest 侧搬运照旧（`:1581`）。加一条 pull-import 用例钉死 |
| R4 | **静态缝污染**：引擎测试用 `GithubApi::setSender/setSleepFunction`（静态），用例漏复位 | 污染同进程后续用例（假网/假 sleep 泄漏） | 测试文件头写死「每用例结束复位」；或 `ScopedGithubApiSeam` RAII 夹具（推荐） |
| R5 | **T1 是纯搬运大 diff**（~700 行移动），审计时难判「有没有偷偷改逻辑」 | 审计成本高、回归混入 | commit message 写明「逐字搬运，零逻辑编辑」；审计用「移动前后 AST/行配对」法（照 T5 审计 `fa80dfa` 的双法验证：词法器剥注释比对 + git diff 行配对） |

**回退点**：T1/T2/T3 各一个原子 commit；任一步红 → `git revert <sha>`。**T1 是唯一动 `settings_module.cpp` 结构的一步，回退它即回到「编排在模块类里」的现状，无残留**。

---

## 附：为什么现在抽（T4 审计 A11 原话）

> 「不阻塞合并；下票动同步功能时抽 `GithubSyncEngine`（注入 api/progress 槽），别现在为拆而拆。」

**触发条件**：下一次要动同步功能（加特性 / 修 bug）时启动本票。当前 `settings_module.cpp` 2161 行里 `githubSync*` 出现 **314 处**，编排全在私有区——纯规则已抽干净（`github_sync_logic` 157 行 + store/card/dialog/probe 四文件），**唯一剩下的就是编排**，而它恰恰是零覆盖的那块。
