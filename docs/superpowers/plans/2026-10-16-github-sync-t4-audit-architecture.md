# T4 审计（lens = 软件架构师 / 静默失败与过程轴）

> 日期：2026-10-16 ｜ 审计对象：`worktree-agent-github-sync-t4`（HEAD `7982d4c`）
> 审计范围（仅本 4 提交，另一半由另一 agent 审）：
> `9ca6fb6` M3 F6 ｜ `be20148` M4 F3 ｜ `c5098bd` L1 F5/F8/finding 9/8 ｜ `7982d4c` 决策 #9 删除
> 复跑：`DEVDESK_TEST_TMPDIR=/tmp/t4-audit-arch bash scripts/build-test-run.sh --no-run --filter github_sync`
> → `Total: 24 / Passed: 24 / Failed: 0` + 6 道静态守卫自检全过（本次审计唯一实跑证据；全量 1410/1397/0 的声明**未独立复现**）

## 1. findings

| # | 位置 | severity | 问题 | 影响 | 建议 |
|---|---|---|---|---|---|
| A1 | `settings_module.cpp:700-712` + `:214` | **高（必修）** | 「5s 上限」只界住等通知，**不界住退出**：5s 到点后 `onStop` 的 `githubSyncThread_.join()` 无超时，进程仍要等整轮推送跑完 | spec F5 明写「不阻塞退出」被违反。断网时单请求 `kRequestTimeoutMs=30000`×3 次尝试 + 1/3/9s 退避 ×（PUT database.sql + 每篇笔记）→ 退出可卡数十秒至分钟级，用户只能强杀 | 退出补推的线程不 join（detach + 原子取消位），或 onStop 对该线程设 5s 有界 join；至少把「退出等待」与「onStop 收束」拆成两个上限 |
| A2 | `github_sync_config.cpp:302-306` | **中（必修）** | `db_->exec("PRAGMA integrity_check")` 走 `sqlite3_exec(..., NULL callback)`（`sqlite_db.cpp:344`）→ 结果行被丢弃，**rc 恒 OK**；只有语句本身执行不了才报错 | 名为「校验」实则零校验，损坏库照样静默放行；日志分支写了却永不触发 = 假保证（spec F3 要求「索引校验」） | 用 `prepare/step` 读首行，非 `ok` 即 `LOG_ERROR` + 报错；或删掉这句与它的日志，别留假校验 |
| A3 | `settings_module.cpp:1414-1427` | 中 | 清「未同步」提示（与 `githubSyncDirty_`/`LastCommitSec_`）挂在 `if (githubSyncResultOk_)`，**Test 动作也走这条**（`settings_module.cpp:802-813` → `stagedOk_=true`） | 点一次「测试连接」即抹掉 F5 黄字「有未同步改动」（且顺带清 dirty，自动推送不再触发）——静默丢状态。dirty 部分系 T3 遗留，F5 把提示键又挂上同一错误作用域 | 该块按 `githubSyncLastAction_ != SyncAction::Test` 收窄；至少提示键只在 Push/Pull/FirstSync 成功时清 |
| A4 | `claude_module.cpp:324-325`（注释）+ `:311-315` | 中 | 注释断言「退订后不再进入」不成立：`event_bus.h:73-99` 在锁内**拷贝** handler、锁外调用，退订后不再校验存活 | 导入线程 emit `db.import_end` 与 UI 线程 `onStop`（模块开关）竞争时，late handler 会调 `startQuotaRefresh()` → 模块已停但刷新线程复活；且 handler 与 `onStart/onStop` 同时改 `quotaRefresher_/quotaStore_` 两个 unique_ptr（`started_` 是普通 `bool`，`:1018`）→ 数据竞争，窗口窄、症状静默 | handler 内先查 `started_`（改 atomic）再动作；或给这组成员一把锁。注释要么改对、要么删 |
| A5 | `github_sync_config.cpp:307`（emit）+ `todo_module.cpp:55`/`markdown_module.cpp:224`/`agent_module.cpp:381` | 中 | `kDbImported` **零订阅者**（全仓 grep：仅测试订阅）。todo/markdown 只在 `onStart` 装载、均未覆写 `onPageEnter` → 拉取导入后全程读旧快照；markdown 编辑器缓冲还停在导入前内容 | spec F3「导入后全局重载」实际未落地（只有 settings 自己的 `githubSyncCfg_` 重载）。用户拉取后切到待办看到旧列表（写操作打向已不存在的行），回笔记页 Ctrl+S 会用旧缓冲盖掉刚拉下来的内容。缺口只在 `2026-10-15-github-sync-t4-prework.md:144` 记过，**backlog 未登记** | 立刻登记 backlog（含影响面）；下票给 markdown/todo/agent 各加「原子脏标记 + compose 收割」，或抽一个共用小 helper |
| A6 | `git_error_dialog.cpp:133-150` | 低 | 覆盖清单无条数上限，每帧重建 N 个 text 节点 | 几百篇未推笔记时弹窗每帧构造几百节点（immediate-mode），卡顿 | 清单截断（如 20 条 + 「等 N 项」）或分页 |
| A7 | `settings_module.cpp:1246-1257` | 低 | 拉取摘要只报笔记备份目录，T3 原有的库备份名（`backups/devdesk-<ts>.db`）不再出现 | 库被覆盖后用户找不到库备份（备份确实在，只是不可见） | 摘要同时给两个路径（`sqlImported` 为真时必给库备份名） |
| A8 | `local_git_probe_macos.cpp:52-70` | 低 | `n > 0` 分支 `continue` 跳过 `waitpid` **与 deadline 判定**；子进程若持续吐数据则循环无上界 | 对 `xcode-select -p` 属理论风险（输出一行）；换成别的命令即真无界 | 把 deadline 判定提到 `continue` 之前 |
| A9 | `src/core/sql_dump.h:46` / `sql_dump.cpp:355` | 低 | F3 改走 `importDatabase` 后 `importFromFile` 生产/测试**零调用者**（grep 全仓） | 死公开 API（与决策 #9 对 `gitRepoDir` 的口径不一致） | 下批顺手删（或写明保留理由） |
| A10 | spec `2026-10-15-github-sync-api-design.md:48` vs `github_sync_config.cpp:288-311` | 低 | spec F3 写「导入失败 → **从备份回滚**」，实现改为「靠 `importFromString` 事务原子性」（提交信息已论证，等价且更省），spec 文本未同步 | 文档口径与实现不一致，后人读 spec 会以为缺一条回滚路径 | spec F3 行补一句「失败回滚由事务原子性承担（`sql_dump_import_rollback` 钉死），不另做副本」 |
| A11 | `settings_module.cpp:1968` 行 | 低中（架构） | SettingsModule 仍是「设置页 UI + 整套同步引擎」单类：全文件 1968 行、314 处 `githubSync*`，push/pull/备份/探测/退出钩子全在私有区 | 纯规则已抽到 `github_sync_logic`（157 行）+ store/card/dialog/probe 四个文件，分层是对的；但编排只能通过模块类测（订阅方配对、退出路径均无测试可挂），下个同步特性继续往这个 2000 行文件里加 | 不阻塞合并；下票动同步功能时抽 `GithubSyncEngine`（注入 api/progress 槽），别现在为拆而拆 |
| A12 | `paths.h:59` | 低（文档） | 删除 `gitRepoDir` 后，相邻注释「SQL dump 文件路径（仓库根目录下的 database.sql）」里的「仓库根目录」已无所指（实现是 `appDataDir()/database.sql`） | 语义漂移，后人误以为仍有 git 仓库根概念 | 改成「数据根下的 database.sql」 |

## 2. 逐核查点结论（含「已核」）

1. **M4 F3「事务原子性即回滚机制」声明 —— 成立**。`SqliteDb::transaction`（`sqlite_db.cpp:369-405`）真 BEGIN/COMMIT/ROLLBACK 且**全程持 db 递归锁**（导入期间其他线程连读都被挡住）；`importFromString`（`sql_dump.cpp:325-353`）在 OFF 与复位之间**无任何早退路径**，复位恒执行；`sql_dump_import_rollback` 用例的坏句落在 dump 尾部（stripTransactionShell 保留 COMMIT 之后尾巴）→ 仍在外层事务内 → 回滚覆盖全部 DROP/CREATE/INSERT，是**真断言**。`importDatabase` 的 `db_==nullptr` 早退在 emit 之前，不产生「只 begin 不 end」。唯一缺陷是 A2（校验无效），与「不另做内存副本」的判断无关。
2. **M4 事件序 —— begin/end 恒发成立**（`github_sync_config.cpp:293/310`，中间无 return）；订阅方 `claude_module` 的 stop/start 各自幂等且 start 建新实例（旧实例 stopAll 后不可重启），正常路径**不会永久停摆**。残余风险：①A4 的跨线程竞争；②`importFromString` 若挂死（无 watchdog）→ end 永不来 → 余量刷新静默停摆且无任何 UI 迹象（低概率、无兜底）。
3. **M4 已知缺口 —— 属实且影响面比提交信息写的大**（见 A5）：广播点确已就位（`github_sync_config.cpp:307`），但 `kDbImported` **零订阅者**；todo/markdown 不覆写 `onPageEnter`，故不是「下次进页就刷新」而是「本会话一直旧」。
4. **L1 F5 —— atexit 顺序正确、退出阻塞不成立**。顺序已核：`armExitHook` 在 `assembleAndStart` 末尾注册（`app_assembly.cpp:73`），F5 钩子在 `main.cpp:257` 注册（更晚）→ LIFO 先跑，模块与库都活着；模块 static 析构在其后；钩子目标 `onStart` 挂 / `onStop`+析构双摘（`settings_module.cpp:174/215/150`），悬垂指针面已堵。`githubSyncDone_` 无陈旧真值（`startGithubSyncAction` 先 `harvestGithubSync()`，`settings_module.cpp:728/1379-1381` 会复位），5s 轮询不会假绿。**但**：A1（退出真被阻塞）、A3（提示被 Test 清掉）、以及「5s 放弃后推送其实还在跑 → 成功也留着未同步黄字」的误报（下次成功动作才清）。**「未运行时验证」风险面**：`runExitSyncPush` 本体（atexit 顺序、5s 等待、提示落键、线程收束）**没有任何自动化测试**，只有 `exitPushDue` 边界 + 提示键往返；本批也没留下真机退出验证的证据。
5. **L1 F8 —— fork/exec 实现基本干净**。已核：exec 失败 `_exit(127)` → 与「未安装」同判（符合 F8 不弹 GUI 的要求）；正常路径靠 `waitpid(WNOHANG)` 必收尸、超时路径 `SIGKILL` + 阻塞收尸 → 无僵尸；父进程只调 async-signal-safe 调用（dup2/open/close/execl/_exit）；Windows 侧是纯文件系统检查（`local_git_probe_win.cpp`），无子进程、无需超时。缺陷仅 A8。缓存失效时机：`githubSyncLocalGitProbeDirty_` 首探 + 仓库/分支/Token 变更置脏（`settings_module.cpp:173/1626/1631/1637`）——语义上这三项都不影响「本机有无 git」，重探属无害冗余（原是为「改配置时弹一次警告」服务），不算缺陷。`onStop` 会 `wait()` 在途 future（`:217-220`），不悬垂。
6. **7982d4c 删除 —— 安全**。全仓 grep（src/ tests/ CMakeLists/ 文档）：`git_sync_service`/`GitSyncService` 与 `gitRepoDir` 在代码中**零引用**，仅历史 spec/审计文档提及（保留为沿革）。`git_sync_service.h` 曾加的 `<filesystem>` include 随文件一起删，无残引用；`paths.h` 只删声明、`sqlDumpFile()` 保留且仍被 push 侧使用（`settings_module.cpp:907`）。仅 A12 的注释措辞残留。
7. **架构 lens**：见 A11。`github_sync_logic` 的接口深度是**对的**（纯件、无 Ui/网络/DB、可穷举单测）；问题不在它，而在编排仍留在 SettingsModule 私有区。
8. **行数/功能比**：`9ca6fb6` +254/-50（F6 清单+整目录备份+prune 扩展，7 文件）、`be20148` +123/-6（F3 事件+暂停+重载）、`c5098bd` +349/-16（F5+F8+两个 finding，13 文件）、`7982d4c` -542（纯删）。**未见超需求实现**：唯一「多出来」的是 A9 的死 API（不是本批新增，是本批留下的）与 A6 的无界清单渲染。无膨胀迹象。
9. **文档口径**：F5/F6/F8 对码一致（F5 的「不阻塞退出」除外 → A1；F6 的清单+笔记备份逐条落实，含 5 份独立计数与「先备份后覆盖、失败即中止」；F8 的「只服务本地版本历史、缓存、配置变更失效」落实）；F9 常量 `kMaxDumpRawBytes = 73'000'000` 与 spec「≈73MB」一致（F9 属本分支但不在本次 4 提交内）；F3 口径不一致见 A10。commit message 依据**基本成立**：事务原子性、事件序、prune 语义、atexit LIFO 均有代码/测试支撑；「全量 7 个失败为主树既有」**本次未复现**（只跑了 filter，24/24 绿），按声明记录、不作实证。

## 3. 裁决

**带病合并**。必修 2 项：**A1**（F5 违反 spec「不阻塞退出」，真机症状是退出卡死）、**A2**（integrity_check 是假校验，一行可修）。必修登记 1 项：**A5**（`db.imported` 零订阅者 → 登记 backlog + 开票）。A3/A4 建议同批修（各 ~3 行）；A6–A12 登记即可。

> 未跑过的东西（诚实边界）：全量套件、真机退出补推、真机拉取后跨模块刷新——**均未验证**；本次仅 `--filter github_sync` 24/24 + 6 道静态守卫自检。
