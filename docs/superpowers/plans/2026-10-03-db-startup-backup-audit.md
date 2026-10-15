# 独立审计报告：feat/db-startup-backup @ 7c48a03（vs 父 6365a68）

- 审计对象：git worktree `D:\project\gui\wt-backup-db`，分支 `feat/db-startup-backup`，commit `7c48a03`
- 审计员：独立审计（未参与实现）；只读审计，未改代码、未 commit
- 参照需求：启动自动备份（同目录滚动 5 份）+ quick_check 损坏检测 + 原生对话框【还原】/【退出】+ 还原失败/无备份走退出 + 备份失败仅 LOG_ERROR + core 分层纪律
- 计数：blocker 0 / major 1 / minor 7 / nit 4

---

## Findings

### [MAJOR-1] 静默失败面：failRestore 复原现场吞掉所有错误码，无任何日志
- 位置：`src/core/db_backup.cpp:267-272`（`failRestore` lambda：`fs::remove(dbFile, rmEc); fs::rename(corruptPath, dbFile, rnEc);`）
- 问题：还原流程"改名→拷贝→校验→失败复原"中，复原路径的两个错误码 `rmEc`/`rnEc` 完全未检查、未 LOG。若 `fs::rename(corruptPath, dbFile)` 失败（Windows 上目标被占用/权限问题），用户的主数据库文件停留在"已删除+未复原"或"被隔离"状态，而界面只显示调用方传入的 msg（"备份拷贝失败…"），用户与后续排障者都无从得知现场已破坏。这是六维中"静默失败面"最严重的一条：一次还原失败可能升级为数据文件下落不明。
- 建议：`failRestore` 内对 `rmEc`/`rnEc` 逐个 `LOG_ERROR`（带路径与 `ec.message()`），并把"复原也失败"的实情追加进返回给用户的 msg（例如"且恢复原库失败，原库已隔离为 <corruptPath>"）。不需要把复原失败变成独立流程——让失败可感知即可。

### [MINOR-1] 文档口径 + 正确性：quickCheckOk 用 SQLITE_OPEN_READWRITE，设计文档写"独立只读连接"
- 位置：`src/core/db_backup.cpp:83`（`openRaw(dbFile, SQLITE_OPEN_READWRITE)`）vs `docs/superpowers/specs/2026-10-03-db-startup-backup-design.md`
- 问题：doc/code 不一致。且 READWRITE 语义带来两个边界：a) 备份文件若被用户置只读属性，`copy_file`（Windows CopyFile 会复制只读属性）后的还原库 quickCheckOk 开库即失败 → 还原被误判失败，好备份被拒；b) 只读介质上的库无法复核。
- 建议：改 `SQLITE_OPEN_READONLY`（或文档改口径并说明理由）。quick_check 在只读连接上同样可用。同时还原后落盘的文件属性建议在 copy 后清掉只读位。

### [MINOR-2] 静默失败面：latestBackup 扫描失败被合并进"没有可用备份"文案
- 位置：`src/main.cpp:69-75`（`handleDbCorruption`：`backup.isFail()` 与"无备份"共用同一对话框文案"数据库文件损坏，且没有可用备份可还原"）
- 问题：目录列举失败（权限/IO）与真的没有备份对用户呈现完全相同，误导用户以为没有备份可救，实际上可能只是扫描失败。
- 建议：`backup.isFail()` 单独给文案（"备份目录无法访问"），并 LOG_ERROR 具体原因。

### [MINOR-3] 平台对称性：macOS osascript 运行期失败静默降级为 Cancel，不弹框不 LOG
- 位置：`src/platform/message_box_macos.cpp`（osascript exit != 0 → 返回 Cancel；只有 posix_spawn 本身失败才 LOG_WARN）
- 问题：无 GUI 会话（SSH/后台启动）、自动化权限被拒、osascript 被策略禁用时，osascript 运行期失败——用户看不到任何对话框，应用静默按"退出"处理，与"损坏必须弹原生对话框"的需求不符（Windows MessageBoxW 无此问题；POSIX stub 有 LOG_WARN 但 macOS 这条真实路径没有）。
- 建议：exit != 0 分支补 `LOG_ERROR`（含 stderr），并考虑此场景下的兜底文案（如 stderr 直写终端）。

### [MINOR-4] 正确性/UX：第二轮还原对话框仍拿同一个刚验证失败的备份再问一次
- 位置：`src/main.cpp:110-149`（`for (int round = 0; round < 2 && opened == nullptr; ++round)` + round 1 的 `handleDbCorruption`）
- 问题：还原成功（校验通过）但重开仍 quick_check 失败时，round 1 再次弹还原对话框，`latestBackup` 返回的还是同一个刚被验证过的备份——用户选"还原"会重复同一操作，随后循环耗尽显示"还原后的数据库仍无法通过完整性检查"。逻辑不致崩溃（.corrupt 隔离兜底在），但第二轮的"还原"选项没有意义。
- 建议：round≥1 时对话框只给【退出】，或在 handleDbCorruption 加参数跳过已尝试过的备份。

### [MINOR-5] 正确性：quick_check 的瞬时 IO/锁错误会被判"损坏"进还原流程
- 位置：`src/core/db_backup.cpp:83-105`（`quickCheckOnHandle`：CORRUPT/NOTADB → ok(false)，其余错 → fail）+ `src/main.cpp:115`（`qc.isFail() || !qc.value()` → 一律按损坏处理）
- 问题：`quickCheckOk` fail（如 BUSY、磁盘 IO 抖动）与 `value()==false` 在 main.cpp 被合并为同一条"损坏"路径，可能对健康库触发隔离+还原。有 .corrupt 隔离兜底、且启动期无并发写者，实际概率低，但语义上 fail≠损坏。
- 建议：`handleDbCorruption` 只在 `qc.value()==false` 时进入；`isFail()` 单独提示"完整性检查无法执行"并走退出。

### [MINOR-6] 测试有效性：损坏注入只覆盖 NOTADB 垃圾，未测"页头合法但内容烂"
- 位置：`tests/test_core/test_db_backup.cpp:22`（`writeGarbageFile` 只写文本垃圾）+ `:147/:178/:207/:279` 各用例
- 问题：测试注释宣称覆盖"页头合法但内容烂"的真损坏场景，实际所有损坏注入都是 NOTADB（开库即失败）。真页损坏（quick_check 返回非 ok 行）路径没有用例。另未覆盖：备份文件本身损坏时 createBackup 的表现（test 279 覆盖的是还原侧）、WAL 未 checkpoint 时备份内容完整性。
- 建议：加一个"合法空库 + 篡改某页字节"用例让 quick_check 返回非 ok；补 WAL 场景（写数据不 checkpoint → createBackup → 还原后数据在）。

### [MINOR-7] 测试有效性/环境：测试依赖 %TEMP% 可写，本沙箱即被拒（ec=13）
- 位置：`tests/doctest.h:124`（tempPath 走 %TEMP%）
- 问题：非代码缺陷，但说明测试对 TEMP 环境有硬依赖；在受限执行环境（如本审计沙箱）下整套 doctest 无法原地跑，验证需重定向 TEMP 到 worktree 内。属可接受的既有测试基建约定，记录为环境提示而非代码问题。
- 建议：无需改码；如将来 CI 受限，可在 testmain 里允许 DSH_TEST_TMPDIR 覆盖。

### [NIT-1] `posix_spawn_file_actions_init` 等返回值未检查
- 位置：`src/platform/message_box_macos.cpp`（spawn 前的 file_actions 系列）
- 建议：失败时 LOG_WARN 并走 Cancel，与 spawn 失败同路径。

### [NIT-2] `sqlite3_backup_step` 的 `rc == SQLITE_OK` 分支为不可达死码
- 位置：`src/core/db_backup.cpp:~192-205`（step(-1) 只会返回 SQLITE_DONE 或错误码；OK 分支永假）
- 建议：删除或改为断言注释，避免误导后续读者。

### [NIT-3] test restore 用例残留未使用变量
- 位置：`tests/test_core/test_db_backup.cpp` restore 用例中的 `ok`/`rows` 局部变量（声明后未参与断言）
- 建议：删除或纳入断言。

### [NIT-4] 时间戳字典序在 DST 回拨时理论非单调
- 位置：`src/core/db_backup.cpp`（`timestampForFileSuffix` 本地时间 YYYYMMDD-HHMMSS）
- 问题：`latestBackup` 取字典序最大，本地时间回拨（DST/手动改钟）时新备份名可能小于旧备份 → 还原取到"次新"备份。本地日期-时间格式同小时级跳变基本安全，仅记录。
- 建议：无需改；如要绝对单调可存 Unix 秒。

---

## 回归面结论（六维之一，无独立 finding）
- `openAppDatabase` 时序：themeRestored(503) 与 initialized(589) 双调用由 `g_startupBackupDone` 闩保护，main.cpp:267/288 两处调用点拿同一静态，不重复备份。
- `Database::quickCheck` 持 `db_.mutex()`，与既有 Statement 互斥约定一致，无新增死锁面。
- `.gitignore` 追加两行到 `kRequiredGitignoreLines`（`*.db.backup-*`/`*.db.corrupt-*`），test_file_perms 有断言，不影响现有 646 用例。
- `std::_Exit(1)` 用在 dslAppConfig 静态初始化窗口内，已核实 logging.cpp 每行 flush，无丢日志；此点位退出不经过任何已构造静态的析构，跳过 atexit 是刻意的，安全成立。
- 分层纪律 OK：db_backup.cpp 仅 include core 头（file_perms/logging/time），msgbox 在 platform，main.cpp 属 app 层可用 platform。

## 总结论

**可合并（先修建议）**：blocker 0、major 1（≤2），满足既定合并门槛。MAJOR-1（failRestore 吞错误）建议合并前顺手修——改动小（两行 LOG + 文案追加），收益是消除唯一的数据丢失静默路径；其余 minor/nit 可入后续修复任务。
- 验证实测：`devdesk_tests.exe db_backup`（TEMP 重定向至 worktree build/testtmp，MinGW DLL 入 PATH）**10/10 全过**；build-test-run.sh 内嵌跑挂（10/10 FAIL）是脚本环境问题（bash 会话下 TEMP/PATH 未正确传给测试进程），非被审计代码缺陷，建议修复任务里顺带核实脚本对测试进程环境的传递。

---

# 复查（fix commit 3745005，2026-10）

复查范围：MAJOR-1 / MINOR-A（只读备份误拒）/ MINOR-B（瞬态错误误判损坏）三条；其余 findings 不在本轮。实测 `devdesk_tests.exe db_backup`：**12/12 全过**（含 2 个新用例）。

- **MAJOR-1 通过**：db_backup.cpp failRestore 现对 rmEc/rnEc 逐项 LOG_ERROR（含路径+ec.message()），并把"半还原主库未清理"与"原损坏文件已保留为 <corruptPath>"追加进返回文案，文案经 main.cpp handleDbCorruption 弹给用户——静默路径消除。
- **MINOR-A 通过**：quickCheckOk 加 readOnlyOpen 参数（默认 false）；还原校验走 SQLITE_OPEN_READONLY；主库路径保持 READWRITE 且头文件/设计文档写明 WAL 热 -wal 时只读连接因 -shm 不可写会误判的理由（成立）；copy 后 `perm_options::add owner_write` 清只读位（失败 WARN，校验兜底）。设计文档同步更新，doc/code 口径一致。新用例 db_backup_restore_accepts_readonly_backup 真测行为：置只读备份→还原成功→落盘主库可写位恢复→READWRITE 可开。
- **MINOR-B 通过**：quickCheckOnHandle 判定口径不变（CORRUPT/NOTADB→ok(false)，其余→fail），调用方语义修正——main.cpp open 成功后 chk.isFail() → LOG_ERROR 后带库继续启动（不进还原）；还原校验路径 verify.isFail() → 走 failRestore（还原失败退出），fail/ok(false) 分开报错。附带变更：open 失败复核路径 qc.isFail() 从原"并入损坏"改为 LOG_FATAL+abort——open 已失败且瞬时错误下无可继续的库，abort 合理非回归。新用例 db_backup_busy_lock_is_not_corruption 用 rollback-journal 库+BEGIN EXCLUSIVE+busy_timeout(0) 注入真 BUSY，断言 isFail 且解锁后同文件 ok(true)——测行为非实现。main.cpp 分支本身无单测（UI 路径，沿用既有口径，可接受）。

## 复查总结论

**修复合格，可合并。** 三条修复均语义正确、有真实行为测试覆盖、文档口径同步；无新增 blocker/major。

