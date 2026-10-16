# 决策 #15「本地 git 版本历史」实施预案（只读设计，不含工期 / 不含代码）

> 日期：2026-10-16 ｜ 类型：**设计预案**（未动代码，未构建，未跑测试）
> 依据：spec `docs/superpowers/specs/2026-10-15-github-sync-api-design.md` §2 决策 #15 + §2.1 F8 + §6 错误处理
> 现状：`src/module_settings/local_git_probe.{h,_macos.cpp,_win.cpp}`（探测，已落地）+ `core::paths` 的数据根 `.gitignore` 基建（**已存在**）
> 目标读者：醒来后要拍板「是否启动这张票 / 七个决策点各选哪条」的用户

## 0. 背景：这一票要补的是「半条已实现的决策」

决策 #15 原文：

> 探测到可用 git → best-effort 做本地版本历史（init + 每次 commit）；不可用不影响同步。**无 git 时在用户点「配置」按钮时弹一次警告**，之后仅在修改配置时再弹。

**拆成两半看现状**：
- **「无 git 警告」半条 —— 已实现**：`maybeWarnNoLocalGit`（`settings_module.cpp:668-676`）+ 探测缓存 `githubSyncLocalGitProbe_`（`:2112`）+ 配置变更置脏重探（`:1816/1822/1827`）。
- **「init + 每次 commit」半条 —— 未实现**：全仓 **零** `git init` / `git commit` 调用；`kLocalGitUnavailableHint`（`github_sync_logic.h:150`，="本地版本历史：不可用"）**定义了但零处消费**（grep 全仓只有定义行）——这半条的卡片灰字口是留好的，只是没接。

**关键发现：数据根的 git 基建已经在位**（决策 #9 删 `git_sync_service` 时没删它）：
- `core::paths::dataRootGitignoreFile()` = `appDataDir()/.gitignore`（`paths.cpp:45-51`）
- `core::paths::ensureDataRootGitignore()`（`paths.cpp:73-129`）——启动时调用（`main.cpp:618`），**幂等追加**必需屏蔽项，已屏蔽：`*.db` / `*.db-*`（WAL/shm）/ `*.db.backup-*` / `*.db.corrupt-*` / `config.json*` / `run-settings/` / `claude_settings_snapshot.json` / `logs/` / `pets/` / `!/.gitignore`
- `paths.h` 文件头原话：「刻意不用系统默认隐藏位置（数据要默认可见，**笔记目录要进 git**）」
- 设置页已在展示该 .gitignore 路径（`settings_ui_cards.cpp:582`）

→ **本票不是「从零造 git 支持」，是「接上一条早已铺好的线」**：数据根本来就是设计成 git 仓库的（.gitignore 的注释明写「数据根若被 `git add .` 扫走，密钥就跟着上远端」）。决策 #9 删的是**用 git 当同步传输**（那条已废），保留的是**数据根可以是个 git 仓库**。

**与 GitHub Sync 的关系（先说清楚，后文反复用）**：远端仓库用 Contents API 同步（决策 #1 零外部 git）；**本地 git 仓库是独立的、纯本地的，不配 remote、永不 push**。两者内容有重叠，但是两个不相干的仓库。

---

## ① commit 什么？（笔记目录？整个数据根？库 dump？）

### 推荐：**只 commit 笔记（`notes/`，即 `core::paths::markdownDir()`）**

**理由**：
1. **笔记是不可再生内容**——版本历史真正要救的就是手写笔记。todo/配置/皮肤都能重建或另有出处，笔记不能。
2. **零隐私面**：笔记是用户文本，不含密钥列。不依赖 `.gitignore` 写得对就能保证不泄密。
3. **体积有界、git 友好**：几百 KB 量级；纯文本 delta 压缩效果好。
4. **库另有兜底**：DB 已有自己的版本化备份轮转（`backups/devdesk-<ts>.db`，保 5 份，`backupLocalDb`）。DB 可恢复性已被覆盖，不需要再塞进 git。
5. **避免 dump 进 git 的反模式**：`database.sql` 每次同步**整体重写**（导出即全量快照），git 对「每次几乎全文重排的大文本」delta 效果极差 → 仓库按「dump 体积 × commit 次数」膨胀；且 dump 含密钥列（决策 #8），进 git 对象 = 新的本地密钥面。

### 备选
- **B：`notes/` + `data/database.sql`**（= 完全镜像同步载荷）。Pro：本地历史与「同步了什么」一一对应，DB 状态也可从历史恢复。Con：按 dump 体积膨胀、密钥列（决策 #8）进本地 git 对象、且要**信任 .gitignore + 显式 pathspec 两道闸**都对。
- **C：整个数据根（靠 .gitignore 挡）**。Pro：连配置/皮肤都版本化。Con：隐私面最大、`logs/`/`pets/` 有噪音、必须完全信任 .gitignore；与「笔记要进 git」的原始意图相比是**超范围**。

> **取舍一句话**：A 是「够用且零新增隐私面」，B 是「完整但要接受膨胀 + 密钥进本地 git」，C 是「什么都存但风险最大」。**推荐 A**；若用户明确要「DB 也能回滚」，再升 B（并把「密钥列进本地 git 对象」写进 spec §10 风险表）。

**路径关系（决定 commit 相对路径怎么算）**：
- `appDataDir()` = `~/DevDesk`（macOS）/ `%USERPROFILE%/DevDesk`（Win），可被 `DEVDESK_DATA_ROOT` 覆盖
- `markdownDir()` = `appDataDir()/notes`（**可被 `DEVDESK_NOTES_DIR` 覆盖到数据根之外** —— 见 ③ 的边界处理）
- `sqlDumpFile()` = `appDataDir()/database.sql`；`sqliteDbFile()` = `appDataDir()/devdesk.db`

---

## ② commit 时机？（同步成功后？退出前？节流？）

### 推荐：**每次同步动作成功后，在同步后台线程里、网络 I/O 全部结束之后，commit 一次**

**理由**：
1. **commit 点 = 有意义的快照点**：push/pull 成功即代表「本地与远端对齐」的那个瞬间，正是版本历史想留的刻度。commit message 直接用同步摘要（如 `sync: 已推送 database.sql 与 3 篇笔记` / `sync: 已导入 database.sql；下载 2 篇笔记`）。
2. **节流自动继承**：同步本身已被 5 分钟节流（决策 #14，`throttleDue`），commit 搭同步的车 → 自动 ≤ 1 次/5 分钟，**不需要另设 commit 节流**。手动「立即同步」绕开节流 → 也产生一次 commit（用户显式要求，合理）。
3. **顺序天然安全**：commit 在同步的文件写盘 / 网络写之后（同线程顺序执行）→ 不会与同步的 `collectLocalNotes()` 读盘、笔记下载写盘并发（避开 git index 锁 vs 文件写竞争）。
4. **不碰退出路径**：退出补推（`runExitSyncPush`）**已是脆弱路径**（T4 审计 A1：退出真被阻塞）。v1 **不在退出路径加 git commit**，避免给退出加延迟。退出时若有未推改动，已有 `setUnsyncedNotice` 黄字信号（不依赖 git）。

### 备选
- **B：每次「笔记/待办写」都 commit** —— 太碎（几百次 commit），reject。
- **C：只在 push 成功后 commit（pull 不 commit）** —— pull 也改了本地状态（下载笔记 / 导入库），值得留刻度，reject。
- **D：退出前补一次 commit** —— 与 ④ 的「退出脆弱」冲突，v1 不做；若要做，必须复用 `waitOrDetachSyncThread` 的 5s 有界等待，不得延长退出。

**具体挂点**：
- **若 SyncEngine 抽取票（`2026-10-16-sync-engine-extraction-plan.md`）先落地** → 挂在 `GithubSyncEngine::run()` 的**成功收尾**（`SyncRunResult.ok == true` 且 action ∈ {Push, Pull, PullThenPush, FirstSync, ValidateFirstSync}）。
- **若本票先落地** → 挂在 `harvestGithubSync()` 的成功分支（`settings_module.cpp:1590-1625`，与「清 dirty / 记节流时点」同一处），在 UI 线程搬运时**异步**触发一次本地 commit（不阻塞 compose）。
- **两种落法都行**，但**只落一处**，且 commit 必须在「同步成功」判定之后 —— 它**不得改变** `SyncRunResult.ok`。

---

## ③ 仓库位置？（数据根内 `.git`？与远端仓的关系？）

### 推荐：**独立本地仓，worktree = 数据根，`.git` 在 `appDataDir()/.git`；不配 remote，永不 push**

**理由**：
1. **`.gitignore` 基建已假设数据根是 worktree**（`ensureDataRootGitignore` 把 `.gitignore` 写在数据根；paths.h 明写「笔记目录要进 git」）。复用 = 零新增路径概念、零新增配置。
2. **与远端仓是两回事**：远端走 Contents API（决策 #1），本地仓**不配 remote、不 push、不 pull**。二者只是内容重叠，互不影响。
3. **不与同步枚举冲突**：同步只枚举 `markdownDir()` 下的**常规文件且 `.md` 结尾**（`collectLocalNotes:1524-1527`）→ `.git` 是目录，被跳过；`listTree` 走远端。`.git` 的存在**不改变**同步看到的内容。

### 备选
- **B：专用历史目录 `appDataDir()/history/`，独立 worktree，靠拷贝填充**。Pro：与同步的文件面彻底隔离。Con：存储翻倍、需要拷贝步骤、打破既有 .gitignore 假设、多一层「拷哪些」的规则。v1 不做。

### 边界：`DEVDESK_NOTES_DIR` 覆盖到数据根之外时
`markdownDir()` 可被 `DEVDESK_NOTES_DIR` 指到数据根外（`paths.cpp:133-135`）。此时：
- **推荐**：检测 `markdownDir()` 是否在 `appDataDir()` 之下；**不在则跳过本地历史**（记一次日志 + 卡片灰字）。理由：v1 只服务默认布局，不为一个覆盖场景引入「仓库开在哪」的分叉。
- 备选：把仓库开在 `markdownDir()` 的父目录。Con：父目录可能是 `$HOME` 之类，git 会扫到大量无关文件 —— reject。

**相关但已废弃的概念**：决策 #9 提到「用户机器上既有 `~/DevDesk/.git` 不自动删」。本票**复用**这个 `.git`（若已存在就 `add/commit`，不 `init`）；若不存在才 `init`。**不删、不重置**。

---

## ④ 与 GitHub Sync 的交互（本地 commit 会不会影响推送内容 / 冲突判定？）

### 结论：**不会**，前提是守住三条

1. **本地仓不配 remote、永不 push** → 本地 commit 完全不进远端，远端内容 / 冲突判定（409，靠 sha 比对）**零影响**。
2. **同步的变更判定用内容 hash（`contentHash`，FNV-1a），不是 git 状态** → git commit 不改工作区文件内容 → 笔记 hash 不变 → 判态结果不变。**同步对 git 无感知**。
3. **`.git/` 不进同步载荷**：同步只枚举 `notes/*.md`（常规文件）与 `data/database.sql`；`.git` 是目录，天然不被枚举。**`.git` 不会被上传到远端**。

### 唯一的竞争点与消解
- **风险**：`git add notes/` 与同步线程正在下载/写笔记文件并发 → 可能把半写的文件 add 进 index（或撞 index.lock）。
- **消解**：commit 与同步**同线程顺序执行**（见 ② 的挂点）——同步的文件写全部完成后才 commit。**不引入第二线程**。
- **次要**：`git add` 只加**显式 pathspec**（`git add -- notes/`），不加 `-A`，即使工作区里有别的未跟踪文件也不会被扫进去（同时是 ⑦ 的隐私闸）。

---

## ⑤ git 不可用 / 失败时的降级路径（绝不阻断同步）

### 推荐：**三层降级，commit 是「同步结果已定之后的 fire-and-forget」**

| 层 | 触发 | 行为 | 已有/新增 |
|---|---|---|---|
| L1 | 探测不可用（`githubSyncLocalGitProbe_ == 0`） | **整段跳过**本地历史；不改同步结果 | 已有：探测 + `maybeWarnNoLocalGit` toast |
| L2 | git 可用但某条命令失败（init/add/commit 非零退出） | `LOG_WARN` + 卡片灰字 `kLocalGitUnavailableHint`（"本地版本历史：不可用"）；**同步结果仍 ok=true** | 新增：消费 `kLocalGitUnavailableHint`（现零消费） |
| L3 | git 命令超时（如 `git add` 卡住） | 按 deadline `SIGKILL`/`TerminateProcess` 收尸，记 WARN，放弃本次 commit | 新增：runner 自带超时（复用探测的 fork/exec+deadline 模式） |

**硬约束**：
- commit 的成败**不得改变** `SyncRunResult.ok` / `summary`；不得让同步因 git 失败而报错、弹窗、重试。
- commit **不阻塞**同步线程的收尾（它本就在网络 I/O 之后，纯本地磁盘操作，毫秒级；仍设超时上界）。
- **不新增退出路径依赖**（见 ②）。

---

## ⑥ 平台适配（复用 `local_git_probe` 探测结果缓存）

### 复用点
- **探测结果缓存**：`githubSyncLocalGitProbe_`（`settings_module.cpp:2112`，atomic int：-1 未知/在途、0 不可用、1 可用）。**只在 == 1 时才执行 git**。
- **探测的语义耦合（关键安全点）**：macOS 探测**故意不执行 `/usr/bin/git`**（会弹 CLT 安装 GUI），而是先 `xcode-select -p` 再查文件存在性（`local_git_probe_macos.cpp:93-98`）。→ **探测返回 1 蕴含「CLT 已装」**，因此后续**执行** git 不会触发安装 GUI。这是「先探测再执行」的安全前提，runner 必须**只**在探测为 1 时跑。

### 新增：git 执行器（探测只查存在性，历史必须真跑 git）
- **新平台拆文件**：`local_git_runner.h` + `local_git_runner_macos.cpp` / `local_git_runner_win.cpp`，CMake 按平台选编（照 `CMakeLists.txt:20-25` 的 `local_git_probe` 模式）。
- **接口草案**（平台无关头，业务零平台宏）：
  ```cpp
  // src/module_settings/local_git_runner.h
  namespace module::settings {
  struct GitRunResult { bool ok = false; int exitCode = -1; std::string outputTail; };
  // 在 repoDir 执行 git <args>；带 timeoutMs 上界（到点杀进程）；失败/超时 → ok=false。
  // 只在 localGitAvailable()==true 时调用（探测保证 CLT/git 就绪，不弹安装 GUI）。
  GitRunResult runGit(const std::filesystem::path& repoDir,
                      const std::vector<std::string>& args, int timeoutMs = 5000);
  }
  ```
- **macOS 实现**：`fork/exec` git + 非阻塞读 + deadline + `SIGKILL` 收尸（直接照搬 `local_git_probe_macos.cpp:28-86` 的结构，含 A8 那条 deadline 判定顺序的教训——`n>0` 分支的 `continue` 不得跳过 deadline 判定）。
- **Windows 实现**：`CreateProcessW` + `CREATE_NO_WINDOW`（不闪控制台）+ 带超时 `WaitForSingleObject`；git 路径来自探测的已知位置。
- **探测需要小扩展**：现在 `localGitAvailable()` 只返回 bool，不返回**路径**；runner 需要知道用哪个 git。**推荐**新增 `localGitPath()`（返回解析到的 git 可执行路径，空=不可用），`localGitAvailable()` 改为 `!localGitPath().empty()`；或 runner 自行按同样规则再解析一次。**前者更干净**（单一来源）。

### v1 命令集（最小）
`git init`（仅 `.git` 不存在时）→ `git add -- <显式路径>` → `git commit -m <msg>`。可选 `git status --porcelain` 判空提交（无改动则跳过，避免空 commit 报错）。**全部 best-effort**。

---

## ⑦ 隐私面（哪些文件绝不入库）

### 已被 `.gitignore` 屏蔽（`paths.cpp:56-67`，`ensureDataRootGitignore` 幂等维护）
| 项 | 原因 |
|---|---|
| `*.db` | 主库（凭据入库后密钥列在里面） |
| `*.db-*` | WAL/shm 伴生（会复制凭据页） |
| `*.db.backup-*` / `*.db.corrupt-*` | 启动备份 / 损坏隔离残骸（含主库全页） |
| `config.json*` | 旧配置残骸 |
| `run-settings/` | **每会话临时 settings（含明文 API key）** |
| `claude_settings_snapshot.json` | 接管前 settings.json 原 env 快照（可能含真 token） |
| `logs/` | 日志（含 profile 名与路径） |
| `pets/` | 桌宠皮肤（本机资产） |

### 本票必须继续保证
- **显式 pathspec 提交**：`git add -- notes/`（可选 `data/database.sql`），**永不** `git add -A` / `git add .`。这是项目自己的纪律（CLAUDE.md 交付流水线「禁 git add -A」），也是这里的**第一道隐私闸**；`.gitignore` 是第二道。
- **绝不上远端的项**：`.git/` 本身（同步不枚举目录）、`run-settings/*.json`、`claude_settings_snapshot.json`、任何 `*.db*`。
- **token**：从不落数据根（在 keychain，`platform::keychain` service=`DevDesk/github`）→ 天然不入库。
- **若选备选 B（commit dump）**：密钥列（决策 #8）进本地 git 对象 —— **必须在 spec §10 风险表补一行**，并告知用户「本地 `.git/objects` 与远端同等暴露」。**选推荐方案 A 则零新增密钥面**。

---

## 拍板清单（用户醒来直接逐条选）

| # | 决策点 | **推荐** | 备选 | 影响 |
|---|---|---|---|---|
| 1 | **commit 什么** | **只 `notes/`** | B：`notes/`+`database.sql`；C：整个数据根 | 隐私面 / 仓库膨胀 / DB 可回滚性 |
| 2 | **commit 时机** | **每次同步成功（push/pull/firstSync），同线程、网络 I/O 之后** | B：也在退出前补一次；C：每次写都 commit | 快照粒度 / 退出延迟 |
| 3 | **仓库位置** | **数据根 `.git`（复用既有 .gitignore 基建）** | B：专用 `history/` 目录 | 隔离度 / 存储 / 与同步枚举的关系 |
| 4 | **`DEVDESK_NOTES_DIR` 指到数据根外** | **跳过本地历史**（记日志 + 灰字） | B：仓库开在 notes 父目录 | 覆盖场景支持度 |
| 5 | **本地仓是否配 remote** | **永不配、永不 push** | —（不建议） | 是否污染同步 / 冲突判定 |
| 6 | **v1 命令集** | **init + add + commit（+ 可选 status 判空）** | 更小：init+add+commit 不判空 | 空提交处理 |
| 7 | **退出路径是否 commit** | **否**（退出已脆弱，见 T4 A1） | B：有界等待内补一次 | 退出延迟风险 |
| 8 | **失败降级口径** | **三层（L1 跳过 / L2 WARN+灰字 / L3 超时放弃），绝不改同步结果** | — | 同步健壮性 |

**启动前还需确认的一件事**：本票与 SyncEngine 抽取票（`2026-10-16-sync-engine-extraction-plan.md`）的**先后**——决定 commit 挂点落在引擎 `run()` 尾部还是 `harvestGithubSync` 成功分支（见 ②「具体挂点」）。**两票都能独立先落地**，但挂点只落一处。

---

## 未验证边界（诚实声明）

- 本预案**只读代码 + 落盘本文件**，**未构建、未跑测试、未执行任何 git 命令**。
- 「探测返回 1 蕴含 CLT 已装」是**读 `local_git_probe_macos.cpp` 源码的推断**（`xcode-select -p` 成功 + `<root>/usr/bin/git` 存在），**未在真机验证**「CLT 已装时执行 `/usr/bin/git` 不弹 GUI」。
- 「`git add notes/` 不会与同步并发」依赖 ② 的**同线程顺序**约定——这是设计约束，落地时必须由代码结构保证，不是现状事实。
- 「`.git/` 不被同步枚举」是读 `collectLocalNotes`（只取 `.md` 常规文件）的推断，**未在真机跑过**「数据根有 `.git` 时同步仍正常」。
