# DevDesk SQLite 数据库启动备份与损坏还原 — 设计文档

- 日期：2026-10-03
- 状态：定稿（与用户确认后实施）
- 分支：`feat/db-startup-backup`
- 关联代码：`src/core/db_backup.h` / `db_backup.cpp`、`src/platform/message_box.h` + `_win/_macos/_posix.cpp`、`src/main.cpp`

## 1. 需求

1. **启动成功后自动备份一次**：备份主库到同目录 `devdesk.db.backup-YYYYMMDD-HHMMSS`（**UTC** 时间戳——本地时间 DST 回拨时非单调，滚动修剪按字典序会误删新备份），滚动保留最多 5 份（超出删最旧），每进程只备份一次。
2. **启动时损坏检测**：打开数据库后立即 `PRAGMA quick_check`，非 `ok` 判损坏。此刻模块尚未写入，文件级操作安全。
3. **损坏时弹模态对话框**：
   - 【还原】：损坏文件改名隔离 `devdesk.db.corrupt-<时间戳>`（只留最近 1 份），拷贝最近备份到主库路径，重开数据库继续启动。
   - 【退出】：直接退出应用，不自动新建空库。
   - 无可用备份：提示「无备份可还原」，只给【退出】。
   - 还原失败（拷贝失败/备份本身也坏）：弹错误后走【退出】。
4. **失败策略**：自动备份失败（磁盘满/权限）→ `LOG_ERROR` + 继续启动，绝不阻断、不崩。

## 2. 分层设计（core 不碰 UI）

| 职责 | 层 | 落点 |
|---|---|---|
| quick_check / 备份 / 还原 / 滚动清理 | core 纯函数 | `core::dbmaint`（db_backup.cpp） |
| 阻塞对话框 / 退出决策 | app 层 | `main.cpp`（`handleDbCorruption` + `openAppDatabase` 循环） |
| 原生阻塞消息框 | platform | `platform::msgbox`（接口隔离 `_win/_macos/_posix`） |

`core::dbmaint` 接口：

- `quickCheckOnHandle(sqlite3*)`：已开连接跑 quick_check，调用方须持锁（`Database::quickCheck()` 内持 `db_.mutex()`）。
- `quickCheckOk(path, readOnlyOpen=false)`：独立连接（无 CREATE，绝不在检查里顺手造库）。判定口径：CORRUPT/NOTADB → `ok(false)`（损坏）；文件不存在与其余 sqlite 错误（BUSY/IOERR 等瞬态错误）→ fail（检查失败，调用方不得当损坏处理——主库路径瞬态错误跳过还原流程 LOG_ERROR 继续启动，备份校验路径按还原失败处理）。打开方式按路径区分：主库检测用 READWRITE（主库 WAL 模式，存在热 `-wal` 时只读连接会因 `-shm` 不可写而打开失败、误判）；备份校验用 READONLY（备份可能是用户拷贝来的/置了只读属性，READWRITE 会误拒还原）。
- `latestBackup(dbFile, exclude=nullopt)`：同目录 `<名>.backup-*` 字典序最大者；无 → nullopt。`exclude` 跳过指定备份（文件名比较）——损坏还原循环里排除本轮已尝试过的那份，使下一轮递补更早一份。
- `createBackup(dbFile, keep=5)`：`sqlite3_backup` 在线备份（step(-1) 循环 + BUSY/LOCKED 重试），失败删半成品；成功后按 POSIX 收紧 0600（内容含凭据页，与主库同保护级别），收紧失败仅 LOG_WARN 不回滚。
- `performStartupBackupOnce(dbFile)`：进程级 `std::atomic<bool>` 闩，首调备份、非首调 `ok(nullopt)` 跳过；失败 fail 由调用方 LOG_ERROR 后继续启动。
- `restoreFromBackup(dbFile, backup)`：删主库旧 `-wal/-shm` → rename 主库为 `.corrupt-<ts>` → 拷贝备份 → 清除落盘文件的只读位（copy 会连只读属性一起拷来）→ quick_check 校验（只读打开）；任一步失败把隔离文件改回原名复原现场（绝不让用户两头落空）。corrupt 滚动只留最近 1 份。
- `pruneFilesWithPrefix(dir, prefix, keep)`：文件名字典序 = 时间戳时间序，滚动删旧。
- `resetStartupBackupLatchForTests()`：测试缝，生产禁调。

## 3. 挂点与启动时序

```
dslAppConfig 静态初始化
 ├─ themeRestored → openAppDatabase()：open → quick_check → 损坏走 handleDbCorruption
 │    （损坏还原循环 ≤2 轮；还原成功重开，仍坏则退出）
 ├─ …getRouter()（模块 onStart，会写库）
 └─ initialized lambda 末尾 → performStartupBackupOnce（失败仅 LOG_ERROR）
EUI-NEO 建窗、进入 compose 循环
```

备份挂在 `initialized` lambda 末尾（模块 onStart 完成后、窗口创建前）：此刻主库已含本次启动的 schema migration 与模块初始化写入，备份内容最完整；且不在 compose 内重复触发（每帧跑），进程级闩再兜底防重。

刻意不放 `app_assembly/app_sidebar` 的 compose 路径：那两个文件编进 `devdesk_tests`，测试跑 compose 会误碰真实数据根。

## 4. 关键决策记录

### 4.1 损坏对话框用平台原生阻塞消息框，不用 EUI-NEO dialog

损坏检测发生在 `dslAppConfig` 静态初始化期间（`themeRestored` → `openAppDatabase`），此刻 EUI-NEO 窗口、compose 循环都不存在——EUI-NEO 的 dialog 组件是 compose 树声明式（需要运行中的帧循环），时序上不可能用于启动期。EUI-NEO 也无 `onStarted/firstFrame` 类钩子（grep 确认）。故新增 `platform::msgbox::show(title, message, withCancel)`：

- Windows：`MessageBoxW`（UTF-8→宽字符；`MB_ICONERROR | MB_OKCANCEL|MB_OK`、`MB_SETFOREGROUND|MB_TOPMOST`——启动早期弹框必须抢前台）
- macOS：`posix_spawn` osascript `display dialog`，pipe 读 stdout 解析 `button returned:`，stderr 另走管道；用户取消（stderr "User canceled"）→ Cancel；osascript 运行期失败（无 GUI 会话/权限被拒）→ LOG_ERROR 后 Cancel 兜底，不静默
- Linux：无统一原生阻塞框，stub 返回 Cancel + LOG_WARN（诚实降级——调用方对 Cancel 的语义就是退出，安全缺省）

### 4.2 退出用 `std::_Exit(1)`，不用 exit/abort

退出点在 `dslAppConfig` 静态初始化内，`atexit`（`armExitHook` 等）会摸到尚未构造的 Router/modules；`abort` 语义错误（不是崩溃）。日志逐行 flush（logging.cpp 每行 cout.flush + 文件 flush），`_Exit` 前无日志丢失。仅在用户决策退出/还原失败路径使用；`open` 失败且非损坏（权限等）维持原 LOG_FATAL + abort 语义。

### 4.3 还原用文件拷贝、备份用 sqlite3_backup

- 备份在线进行、允许并发写（`sqlite3_backup` 语义），防 -wal 丢失页。
- 还原是文件级操作，前置是「全部连接已关闭」；quick_check 在模块写入前做，该前置天然成立。Windows 上有连接开着 rename 会失败，还原前 `cand.reset()` 显式关连接。
- 损坏库隔离不删（改名保留），给用户/支持留排查材料；只滚动删旧到 1 份。
- 还原失败复原现场：隔离文件改回原名，用户至少拿得回损坏原件。复原本身也可能失败（目标被占用/权限）：remove/rename 逐项 LOG_ERROR（含路径与 ec.message()），并把「原损坏文件已保留为 <corrupt 路径>」的实情追加进弹给用户的文案——绝不让主库下落不明静默发生。

### 4.4 备份/隔离文件的 gitignore

数据根 `.gitignore` 追加 `*.db.backup-*` / `*.db.corrupt-*`（既有 `*.db-*` 匹配不到这两种后缀），防止真实数据根下残片被提交。

## 5. 测试计划（tests/test_core/test_db_backup.cpp，自制 doctest）

| 用例 | 覆盖 |
|---|---|
| create 产可读库 | 备份文件 sqlite3 可开、sqlite_master 有表 |
| once 闩 | reset 后首调 has_value、次调 nullopt、目录备份计数 1 |
| prune 7→5 | 7 份假备份滚动删 2，最新 5 份在 |
| createBackup 存量 5 → 5 | 全链滚动，最旧假备份被顶掉 |
| quick_check 判坏 | 垃圾文件 false、正常库 true |
| 还原流程 | 数据在（marker 表读回）、corrupt 只留 1 份（两轮后仍 1） |
| 无备份 | latestBackup nullopt |
| 多备份取最新 | 字典序最大 |
| 备份失败不炸 | 不存在库 createBackup/once → fail 不抛 |
| 坏备份还原 | 还原 fail、主库文件仍在（现场复原） |

main.cpp 的 UI 决策路径（弹框文案、退出）不进单测——`devdesk_tests` 不编 main.cpp，与既有 `openAppDatabase` 口径一致。

## 6. 验收

- `bash scripts/build-test-run.sh --filter backup` 定向通过
- `--filter file_perms` 复验 gitignore 新行
- 静态守卫（module isolation 等）脚本内自动跑
