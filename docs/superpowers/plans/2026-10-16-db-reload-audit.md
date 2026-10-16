# 审计：`db.imported` 订阅重载（markdown / todo / agent 三模块）

- 对象：分支 `worktree-agent-db-reload`（worktree `/Users/coderstory/dev/gui/DevDesk/.claude/worktrees/agent-db-reload`，基 `c7694aa`），3 提交
  - `dbba921` fix(markdown): 订阅 db.imported 重载笔记列表与当前文档
  - `7387849` fix(todo): 订阅 db.imported 重载看板列与任务
  - `9d9396c` fix(agent): 订阅 db.imported 置脏重载任务与运行列表
- 审计者：独立只读 agent（非实现者），2026-10-16
- 手段：`git diff c7694aa..HEAD` 逐 hunk 精读（7 文件 +215/−0）+ 三模块 harvest 调用链常驻调用方核对（`router.cpp` 生命周期 / `settings_module.cpp` 同步序 / `event_bus.h` 派发契约 / `flushEditorIfIdle` 保存链）+ worktree 定向与全量复跑 + `ctest -L probe`
- 本审计为只读：**未执行变异回退**（本任务唯一写动作 = 落盘本文件），变异结论基于用例内**永久对照帧**的结构推断，已在下文标明

## 裁决：**放行（附 2 项建议修复）**

功能正确、线程模型成立、模块隔离与提交卫生干净、三用例内容级且带对照帧。放行不阻塞；但 **F1 / F2 两项建议在合并前顺手修**（各 1 行～数行），它们是本改动**声称要解决的同一类风险**在另一条路径上的残留。

| # | 严重度 | 位置 | 问题 | 建议 |
|---|---|---|---|---|
| **F1** | **中** | `src/module_markdown/markdown_module.cpp:2665-2676` + `:2897-2909` + `:266/:268` | harvest **不判 mtime**，只要 `currentPath_` 非空就动当前笔记 → (a) 库里导入但该笔记文件未被 pull 时也弹「文件已被外部修改」**假冲突**，用户点「重载磁盘版本」会用未变的磁盘内容**替换缓冲、静默丢弃未保存编辑**；(b) `flushEditorIfIdle()` 排在 harvest **之后**且**不看 `conflictOpen_`**，`kAutoSaveDelayMs = 400`（`:3557`）→ 用户停键 400ms 即有未保存编辑时，同帧内先弹冲突、紧接着自动保存把本地旧缓冲写回磁盘，**把刚 pull 的内容盖掉**——正是 commit 声称解决的风险 | ① harvest 对当前笔记先比 `service_->fileMtime(currentPath_) > lastKnownMtime_`（复用 `checkCurrentNoteOnDisk:2990` 的口径），命中才走冲突/重载分支；② `flushEditorIfIdle()` 首行加 `if (conflictOpen_) return;`（一行，对既有磁盘冲突路径同样正确） |
| **F2** | **中低** | `src/module_markdown/markdown_module.cpp:2673` ← `src/module_settings/settings_module.cpp:1357`（广播）vs `:1405`（写笔记文件） | 同步序是「**先** DB 导入并广播 `kDbImported`、**后**才下载写 `notes/*.md`」（step ② 在 step ③ 之前），而 harvest 在 UI 线程下一帧就跑 → 当前笔记可能读到**导入前**内容并 `syncCurrentMtime()` 记下旧 mtime，只能靠 5s 磁盘轮询（`:2945`）补，列表刷新对、当前笔记「立即重载」的目标没达成。另 `writeFileFromString`（`settings_module.cpp:132-138`）是 `ios::trunc` 直写、**非原子**（无 temp+rename），harvest 若落在写窗口内会读到截断文件并把它当 `lastSavedText_`（`editorDirty_` 置 false），此后 Ctrl+S 会把截断内容写回 | ① sync 侧把笔记写盘挪到 DB 导入**之前**，或改 temp+rename 原子写；② harvest 侧按 mtime 判定（与 F1① 同一条修即可同时缓解） |
| **F3** | 低 | `src/module_markdown/markdown_module.cpp:2666-2667` | `exchange` 先于 `if (!service_)`：onStart 里订阅（`:210`）在 service_ 装配之前，DB 未就绪时到达的 `db.imported` 脏标记被消费后丢弃，之后 service_ 就绪也不会补一次重载 | 把 `if (!service_) return;` 提到 `exchange` 之前（或 service_ 就绪时补置脏）。影响面小（onStart 成功路径 service_ 必存在） |
| **F4** | 低（信息） | `markdown:243-245` / `todo:78-79` / `agent:386-387` | 「先关闸再退订」的闸门 `dbImportHandlersEnabled_` **本身也是被析构对象的成员**：handler 若在 `enabled.load()` 返回 true 后、`dirty.store()` 前被抢占、且模块对象在此期间析构，仍是 UAF。实际路径安全（`router.cpp:112/122` 切模块只调 onStart/onStop 不析构，对象活到进程退出），故闸门够用——但注释把结论说得比保证强 | 无需改码。若要硬保证需 shared_ptr 生命期令牌；建议注释改成「收窄窗口」而非「挡住」 |
| **F5** | 低 | `tests/test_markdown.cpp:428-471` | 用例依赖「`di1` 建立 5s 轮询基准后，`di2`/`di3` 两帧间隔 < 5s」（`markdown_module.cpp:3559` `kPollIntervalMs=5000`）。负载导致间隔 > 5s 时 `pollDiskChangesIfDue` 会 rescan 掉手工插入的 `imported.md` 行 → 假红 | 无头环境把轮询周期做成可注入缝（或断言前显式 `lastPollAt_` 前推），属机器负载敏感 flake 面，记录备查 |
| **F6** | 低（信息） | `src/module_agent/agent_module.cpp:1187` → `:913-944` | 收割置 `tasksDirty_` 后下一帧 `refreshData()` 对**每个任务**各发一次 `listRuns`（N+1）。大库导入后首帧有 N 次查询 | 既有路径（onPageEnter 同款），不新增风险，记录备查 |
| **F7** | 信息 | `src/module_agent/agent_module.h:122-125` | 新增 `dbImportedDirtyForTest()` 为 public 内联只读访问器，确实扩大了头文件公开面（装配层也 include 该头） | 无状态变更、无行为面，与既有 `runFallbackChecksForTest()`（同文件 `:119`）同口径，符合仓库惯例，可接受 |
| **F8** | 低（报告口径） | — | 实现方声称「全量 `1433/1427/0`」但**未注明 `DEVDESK_TEST_TMPDIR`**。本机不带该变量复跑 → `1433/1426/1`，唯一红为 `claude_service: runProfile 直连`（`test_claude_service.cpp:2991 settingsFile.is_absolute()`）。**根因是环境**：`tempBaseDir()`（`tests/doctest.h:131-137`）在 `DEVDESK_TEST_TMPDIR` 未设时返回**相对**路径 `build/testtmp` → `DataRootEnv` 把相对的 `DEVDESK_DATA_ROOT` 灌进 `appDataDir()` → run-settings 路径非绝对。带 `DEVDESK_TEST_TMPDIR=/tmp/devdesk-audit-dbreload` 时该例单独复跑绿、**全量复跑 `1433/1427/0/6`——与实现方声明逐字一致，声明已获证实** | 非回归（红点文件不在本 diff 内，且带绝对 tmpdir 即全绿）；但复跑口径应写成 `DEVDESK_TEST_TMPDIR=<绝对路径> …`（对齐 `2026-10-16-dirty-triggers-audit.md` 的既有写法），否则换个人复跑必见 1 红 |

## 逐项核查结论

| # | 核查点 | 结论 | 证据 |
|---|---|---|---|
| 1 | 线程安全 | ✅（附 F4 口径） | 见下 §1 |
| 2 | markdown 冲突分支 | ⚠️ **未真解除**（F1 / F2） | 见下 §2 |
| 3 | todo / agent 收割重入与每帧成本 | ✅ | 见下 §3 |
| 4 | 模块隔离 | ✅ | 见下 §4 |
| 5 | 测试有效性 | ✅（附 F5 / F7） | 见下 §5 |
| 6 | 回归面 | ✅（附 F8 口径） | 见下 §6 |
| 7 | 提交卫生 | ✅ | 见下 §7 |

## 1 · 线程安全 — ✅

- **三处 handler 只置原子、不摸 UI 线程成员**：`markdown:210-214` / `todo:49-53` / `agent:349-353` 的 lambda 体都只有 `enabled.load(acquire)` + `dirty.store(release)`，无任何 UI 成员读写。✅
- **收割全在 UI 线程**：`markdown:266`（compose 首行）、`todo:664`、`agent:1187` 三处均在 `compose()` 内；`compose` 由 `Router::compose`（`router.cpp:95-102`）在主线程调。✅
- **memory order 恰当**：`dirty` 用 release 存 / acq_rel 交换，`enabled` 用 release/acquire。对**纯标志位**而言 relaxed 已足够（DB 数据可见性由 `SqliteDb` 的 `recursive_mutex` + 语句层保证，不靠这个原子传递）；此处用 release/acquire 是**保守过头但正确**，且 `sqlite_db.h:120-124` 的既有注释已把「版本号别省成 relaxed」的口径立住，本改动与之一致。**不是缺陷**。✅
- **onStop「先关闸再退订」顺序正确且必要**：`event_bus.h:86-104` 的 `emit` 先把存活 handler **拷出**锁外再逐个调用 → 仅靠 token 退订拦不住在途那一发，闸门是必要的第二道。三处顺序均为 `enabled.store(false)` → `dbImportedSub_ = {}`。✅
- **成员声明序正确**（退订先于原子析构）：`markdown:3566-3568` / `todo:872-874` / `agent_module.h:201-203` 中 `dbImportedSub_` 均声明在两个原子**之后** → 析构逆序时先退订再销毁原子。✅

## 2 · markdown 冲突分支 — ⚠️ 未真解除

`harvestDbImport`（`markdown_module.cpp:2665-2676`）在 `editorDirty_` 时只置 `conflictOpen_ = true`，走既有 toast 二选一（`drawToast:2480-2505`：action「重载磁盘版本」→ `resolveConflict(true)`；✕ → `resolveConflict(false)`）。两条残留：

1. **假冲突 → 静默丢编辑**。harvest 不看 mtime，只要 `currentPath_` 非空就进冲突/重载分支。但 `settings_module.cpp:1300-1320` 的 `sqlImportPlanned`（DB 变了）与 `actions`（笔记文件要不要下）是**独立**的两件事——DB 变了而当前笔记文件没被 pull 时，harvest 仍弹「文件已被外部修改」。用户点「重载磁盘版本」→ `reloadCurrentNote(false)` 用**未变**的磁盘内容替换缓冲并置 `editorDirty_ = false`（`:3072-3073`）→ **未保存编辑被静默丢弃**。commit 说「与磁盘轮询同口径」，但 `checkCurrentNoteOnDisk:2990` 是先比 `m.value() > lastKnownMtime_` 再动手，harvest 没有这道比较 → **口径并不相同**。
   同一根因的另一面：`editorDirty_` 为假时 harvest **无条件** `reloadCurrentNote(true)`，于是每次库导入都会对当前笔记弹一次「检测到外部修改，已自动重载」（`:3081`）并复位光标 / 撤销基线（`:3066-3075`），即使文件根本没变。
2. **400ms 自动保存竞态，commit 声称的风险仍在**。compose 顺序是 `harvestDbImport()`（`:266`）→ `flushEditorIfIdle()`（`:268`），而 `flushEditorIfIdle`（`:2897-2909`）与 `saveEditor`（`:2912`）**都不看 `conflictOpen_`**（全仓 `conflictOpen_` 命中点：`2476/2492/2499` toast 渲染、`2671/2996` 置位、`2969` 轮询守卫、`3087/3497` 复位——**保存链一个都没有**）。`kAutoSaveDelayMs = 400`（`:3557`）。故「有未保存编辑 + 用户停键 ≥400ms」时：同帧内先弹冲突 toast，紧接着自动保存把**本地旧缓冲**写回磁盘，覆盖刚 pull 下来的内容；用户随后点「重载磁盘版本」读回的是自己刚写回去的本地内容——**选择被静默否定**。
   诚实界定：该洞在既有磁盘轮询路径同样存在（轮询也在 `flushEditorIfIdle` 之后跑），本改动**没有引入**它；但本改动的卖点正是堵这条路，而它没堵住 → 按「声称要解决的同一风险的另一条路径」记 F1。

**无测试覆盖该分支的实际行为**：新用例 `md_module_db_import_reloads_list` 全程 `currentPath_` 为空（只写 `a.md` 未打开），`editorDirty_` 分支与 `reloadCurrentNote` 分支**一行都没走到**。两条推演结论（假冲突丢编辑 / 自动保存竞态）为**读码推演，未实机验证**。

## 3 · todo / agent 收割重入与每帧成本 — ✅

- **无重入**。`reloadAll()`（`todo_module.cpp:88-118`）只做 `listColumns` / `listAllTasks` / `pruneScrolls` / `refreshOverdueCount` / `refreshSearch`，**不 emit、不进 compose** → 不会「compose 内触发重载 → 再次 compose」。markdown 侧 `refreshList` / `reloadCurrentNote` 同样不 emit（`kMdSaved` 只在 `saveEditor:2939` 发，不在重载路径）→ 无重入。agent 侧收割只置 `tasksDirty_`，**不在 compose 内同步重查**，重查发生在下一帧的 `refreshData()`（`:913`）→ 更无重入。✅
- **复用既有路径正确**：agent 置 `tasksDirty_` 恰好命中 `refreshData` 的既有失效判据（`:914` 清位 → 重查 `tasks_` / `latestRuns_` / `taskRuns_`），且 `runSummaries_` 由 `store_->writeVersion()` 判脏（`:894`）——`importFromString` 走 `db.exec()`（`sql_dump.cpp:348-354`）会 `writeVersion_->fetch_add`（`sqlite_db.cpp:353`）→ 汇总缓存**确实**失效。commit 声称的三个缓存都被覆盖。✅
- **每帧成本可忽略**：三处各一次 `atomic::exchange`（无锁、无分支外的额外工作），markdown 多一次函数调用。✅（收割后首帧的 N+1 见 F6，属既有路径。）

## 4 · 模块隔离 — ✅

- 三个源文件的新增依赖只有 `::core::event::bus()`（`core/event_bus.h`），**该头三文件本就 include**（`todo_module.cpp:10`、`agent_module.h:20`，markdown 因 `kMdSaved` 早已引入）。零兄弟模块 include、零 link 变化。✅
- `bash scripts/check-module-isolation.sh` → 退出 0（含「module_agent 零兄弟 include 且工具方不回依赖」）。✅
- 与 `event_bus.h:10-11` 的契约一致：发布方（settings）与订阅方（三模块）都只 include 本 core 头。✅

## 5 · 测试有效性 — ✅

- **断言内容级 + 永久对照帧**。三用例结构一致：先造数据**不发事件** → 断言新数据**不可见**（对照帧）→ `emit(kDbImported)` → 断言**可见**。
  - `test_markdown.cpp:466-470`：直插 `markdown_index` 行（**刻意不动文件系统**，注释说明磁盘变化会被 watcher/轮询抢先刷新、掩掉本改动）→ `di2` 对照 `!treeHasNote("imported.md")` → emit → `di3` `treeHasNote(...)`。断言的是 UI 树行文本，非内部计数器。✅
  - `test_todo_ui.cpp:288-295`：`env.svc->createTask` **直调 service**（`kTodoCreated` 由**模块**在 `todo_module.cpp:193` 发，直调 service 不发）→ 对照 `todo.col.0.card.0.bg == nullptr` → emit → 非空。✅
  - `test_agent_ui.cpp:246-256`：`AgentStore::createTask` 直写库 → 对照 `agent.task.<id> == nullptr` → emit → **断言 `dbImportedDirtyForTest()` 已置位** → compose → 任务可见 → **断言标记已复位**。三层断言（handler 置位 / 收割可见 / 收割清位）比另两例更强。✅
- **变异验证可信度**：**未实跑回退**（只读约束）。但对照帧正是变异验证的**永久形态**——去掉收割后，对照帧（`di2` / 无广播帧）行为不变，而后置断言必红；且三例都确认了「不广播就不刷新」，排除 compose 本身会重查的可能。agent 例尤其可信：若无收割，`dbImportedDirty_` 仍为 true（会被断言到），但 compose 不会置 `tasksDirty_`；而 `refreshData` 的另两条门（`uiPolling_` / `prevAlive`，`:851`）在无 run 的用例里恒假（否则对照帧就红了）→ 必红。**结论：变异声明可信。**
- **未覆盖面**：markdown 例不覆盖 `currentPath_` 非空分支（见 §2）；无 `onStop` 后 emit 的「闸门生效」用例；无「service_ 为空时脏标记」用例（F3）。均属可补而非阻塞。

## 6 · 回归面 — ✅（附 F8 口径）

| 复跑 | 命令 | 结果 |
|---|---|---|
| 定向 markdown | `./build/tests/devdesk_tests markdown` | `Total 94 / Passed 94 / Failed 0` |
| 定向 todo_ui | `./build/tests/devdesk_tests todo_ui` | `Total 54 / Passed 54 / Failed 0` |
| 定向 agent | `./build/tests/devdesk_tests agent` | `Total 93 / Passed 93 / Failed 0` |
| 三新用例 | `./build/tests/devdesk_tests "db.import"`（过滤词匹配**描述**而非用例名） | `Total 3 / Passed 3 / Failed 0` |
| probe 门禁 | `(cd build && ctest -L probe)` | `100% tests passed out of 7` |
| 全量（无 `DEVDESK_TEST_TMPDIR`） | `./build/tests/devdesk_tests` | `Total 1433 / Passed 1426 / Failed 1 / Skipped 6` —— 唯一红为环境性的 `claude_service: runProfile 直连`（见 F8） |
| 全量（带绝对 tmpdir） | `DEVDESK_TEST_TMPDIR=/tmp/devdesk-audit-dbreload ./build/tests/devdesk_tests` | **`Total 1433 / Passed 1427 / Failed 0 / Skipped 6`** —— 与实现方声明逐字一致 |

`Total 1433` = 基 `1430` + 本次 3 例，与分支增量自洽；`Skipped 6` = 平台条件跳过（keychain ×2 / junction / soak ×3 等），非失败。**三条新用例全绿、probe 7/7 全绿、定向三组全绿、全量带绝对 tmpdir `1433/1427/0`。**

## 7 · 提交卫生 — ✅

| 提交 | 文件 | 越界 |
|---|---|---|
| `dbba921` | `src/module_markdown/markdown_module.cpp` + `tests/test_modules/test_markdown.cpp` | 无 |
| `7387849` | `src/module_todo/todo_module.cpp` + `tests/test_modules/test_todo_ui.cpp` | 无 |
| `9d9396c` | `src/module_agent/agent_module.cpp` + `agent_module.h` + `tests/test_modules/test_agent_ui.cpp` | 无 |

`git diff c7694aa..HEAD --stat` = 7 文件 +215/−0：**纯新增、零删除**，无 spec / backlog / CLAUDE.md / 无关重构 / 格式噪声。worktree `git status --porcelain` 干净、HEAD 停在 `9d9396c`（复跑对象即被审提交）。三提交各只动本模块 + 对应测试，**零跨模块夹带**。✅

## 附：本审计未做的事

- **未实跑变异回退**（只读约束）→ §5 的变异结论是结构推断，非实测。
- **未实机验证** markdown 冲突分支的两条推演（F1 / F2 的 (b) 条）→ 读码推演，未构造「打开笔记 + 置 dirty + emit + 等 400ms」的实机时序。
- **未在 Windows 复跑** → F8 的相对路径根因（`tests/doctest.h:136`）平台无关，Windows 侧同样适用，未另验。
