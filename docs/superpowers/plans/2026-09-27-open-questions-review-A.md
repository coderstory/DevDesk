# 存疑项审查 A（2026-09-27）

基线：worktree HEAD `dc727cd`（创建时的 master）。审查期间 master 前进到 `9dd9ee2`（docs-only：CLAUDE.md/HANDOFF 的 EUI rev 修正，`git diff --stat HEAD master` 仅这两份 md）→ 代码内容零差异。
方法：独立 worktree + ccache 热缓存；定向构建 `devdesk_tests`/`DevDesk`；定向过滤测试。**未跑全量套件、未启动 app**。复现程序在 `/tmp/revA/`（未入库，编译命令见各条）。

### 1. CP 片存疑①：`FileWatcher::stop()` 回调线程内调用（detach 后立即关两个句柄）
**结论：REFUTED（对象存活时）/ CONFIRMED（与析构并发时）**｜现网可达性：**不可达**（唯一消费方 markdown_service `:131 start`／`:143` 析构 stop，回调 `onWatchEvent:155-198` 从不 stop，测试里 stop 亦都在回调外）。
证据（`src/platform/file_watcher_win.cpp` 的 `joinWorker`/`runWorker`；拦截 kernel32 `__imp_*` 记录 (seq,tid,op,handle)；场景 a = 回调内 stop 且对象存活）：
```
  13 tid=37592 CloseHandle h=212   # stop(): 关 dirHandle
  14 tid=37592 CloseHandle h=216   # stop(): 关 stopEvent
  15 tid=37592 MARK:after-in-callback-stop
  17 tid=37592 CloseHandle h=228   # 分离后 worker 唯一动作=关自己的 ov.hEvent；无 read/wait 212/216
callbacks=1  active_after_stop=0  probe done (no crash)
# 场景 b（回调内 stop 后主线程析构）worker 从已释放 Impl 读到野句柄：
  18 tid=26768 WaitMulti(ov=228,stop=6579574)   # 6579574 非任何曾创建句柄
```
矛盾点：`file_watcher.h:75-76`「回调线程内调用安全（跳过自 join）」与 `:60`「析构自动 stop()：join 后台线程」不成立——该分支 detach 后 stop() 不 join，析构侧也 join 不到，worker 仍写 `impl_->running`／读 `stopEvent` → 并发析构即 UAF。旁证：场景 c（stop 返回后主线程析构、回调仍在跑）3/3 异常终止（`$?`=127，无 `probe done`）；该场景对任何回调 API 都属所有者被并发销毁的 UB，仅作旁证。
**最小修法**：回调线程分支只做 `running=false + SetEvent`（不 detach、不关句柄），两个 `CloseHandle` 统一挪进 `runWorker` 退出路径（判空幂等）；头文件注释改成真实契约。

### 2. CP 片存疑②：`Server::start()` 同实例 stop→start
**结论：REFUTED**（既有用例已覆盖，自建探针再证）｜现网可达性：非缺陷。
证据（`http_server.h:88/95`、`http_server.cpp:194-237`）：
```
./build/tests/devdesk_tests.exe http_server → Total: 7 / Passed: 7（含 "stop and start cycles"）
自建探针：start1=1 port=49167 → 200 pong → stop → isRunning=0、get-after-stop: refused
        → start2=1 port=49167 → 200 pong ；fixedport: start1=1 get1=OK start2=1 get2=OK
```
`tests/test_platform/test_http_server.cpp:264` 同实例 3 轮 start/stop + 真实 HTTP 200 断言，第 2/3 轮走 `bind_to_port`（首轮 port 0 后 `port_` 改成实绑端口）→ 两条分支都在测。残留：`running_` 是普通 bool，并发 start/stop（现无调用方）才是数据竞争。
**最小修法**：无需；防呆可把 `running_` 换 atomic。

### 3. CP 片存疑③：`json.cpp` 每次序列化的 2 次深拷贝
**结论：REFUTED（不值得改）**｜现网可达性：开销存在但可忽略。
证据（`json.cpp:100 implToMut` 树→yyjson 文档、逐串 `yyjson_mut_strncpy`；`:340 std::string out(text,len)` 写缓冲→string 再 free）：
```
env=12 x 64B (1.5KB)  bytes=1404   iters=20000   1.0 us/call
env=24 x 200B (6KB)   bytes=5760   iters=10000   2.7 us/call
env=40 x 1KB (44KB)   bytes=42352  iters= 2000  40.0 us/call
```
settings.json 实为 1–10KB ⇒ 单次 ≈1–3 µs，比「<1ms 不值得」门槛低 2–3 个数量级（两份拷贝成本已含在此数内）。
**最小修法**：不修（去第二份拷贝需自定义 allocator/增量写，收益为零）。

### 4. MD-08：索引「写点无锁」的并发不一致
**结论：CONFIRMED（已无头复现）**｜现网可达性：真实可触发，后果为短暂不一致（≤1 tick 自愈）。
证据（`markdown_service.cpp:200-206 rescan()`／`:155-198 onWatchEvent` 持 `rescanMu_`；写点 `:683 write`／`:711 create`／`:760 remove`／`:772 rename` 不持 → 撞 `rescanLocked` 的「先遍历磁盘攒 foundPaths（`:494-517`）→ 再 `SELECT path FROM markdown_index`（`:523`）→ 删不在 foundPaths 的行（`:543-548`）」）：
```
seed=3000 首次 rescan=11084ms   空转 rescan=702ms（= walk→SELECT 窗口量级）
并发 create: ok=1  文件在磁盘=1  在 markdown_index(listAll)=0  search 命中=0
pollChanged=1（下个 tick 自愈）  再次 rescan 后恢复=1
```
窗口是**遍历耗时量级**（~700ms/3000 篇），非微秒级 →「遍历中新建/改名」即触发，该笔记索引行+FTS 行被误删、列表/搜索 ≤1 tick 查不到；**无数据损坏**（索引+FTS 更新都在 `db_.transaction` 内并持 SQLite 全局 recursive_mutex，读者看不到半更新），自愈由 `pollChanged` 的 `disk != indexed`（`:1293`）保证 ≤5s。**升级面（未复现）**：`markdown_note_tags.note_id → markdown_index(id) ON DELETE CASCADE`（`migrations.h:47`）——被删行若有标签则级联删、重建走 AUTOINCREMENT 新 id → **不可恢复**；本次用无标签新笔记未覆盖。**相邻缺口**：遍历中途出错（`:500/:505` 的 `continue` 置 ec → `!ec` 结束循环，`:518` 仅 WARN）仍执行步骤 2 → foundPaths 残缺 → 成批误删。
**最小修法**：步骤 2 改「先取索引快照、再遍历磁盘」（或两者同事务）；遍历出错即 return 不剪枝。

### 5. TIN-12：`REGISTER_TEST` 静态计数 vs 运行输出总数
**结论：REFUTED**（无「注册被丢」；530/555/561/589 是不同树状态快照）｜文档口径：**当前应写 621**。
证据：
```
① 静态 621（两法一致）：行首 grep 621；awk 逐条抽取 621（已剔 doctest.h:137 宏定义 + 37 行注释）
② 54 个含 REGISTER_TEST 的文件全在 tests/CMakeLists.txt 显式源表（直接进 exe，静态初始化不丢）；
   唯二 #if defined(_WIN32) 守卫（test_claude_service.cpp:521、test_file_open.cpp:106）在 Windows 成立 → 全编入
③ 运行器 test_main.cpp:88 遍历 registry（doctest.h:68 vector+emplace_back，无去重无条件注册），只按描述子串过滤：
   filter http_server → Total 7 == 静态 7；filter markdown → Total 43，静态 44 的差 1 已逐条 diff 归因
   （theme_hierarchy 用例 body 含 "markdown"、描述不含 → 运行器正确排除，非丢用例）
④ [SKIP]（soak 门控等）计入 Total、单列 Skipped；⑤ git grep 计数：481cf11=587、dc727cd^2=615、dc727cd^1=621
```
**最小修法**：文档口径更新（不再硬编码，改「以无参运行输出的 `Total:` 为准」；本次只审不改 CLAUDE.md）。

### 6. HANDOFF §9.4.2：`readSettingsEnvForSync` 空文件 vs 读失败不可区分
**结论：CONFIRMED**｜现网可达性：需环境条件（ACL/句柄占用导致读失败），Windows 可复现。
证据（`src/module_claude_proxy/settings_sync.cpp:537-542` 注释自陈；`:46 readTextFile` 两形态都返回空串；`:40 fileExists` 同文件可用但未用）：
```
if (!doc.exists || !doc.obj.has("env")) return out;   // G7/G9 → no-op
// 注：readTextFile 对「存在但读不出」（IO/权限）也返回空串，该形态下与空文件同处置
```
G8 要求「存在但读不出 → 中止本次切换」，实际降级为 no-op 成功 → 切换照常继续、失去真相源校验。
**最小修法**：空串后补一次 `std::filesystem::file_size(p, ec)`：`size>0` → 判读失败走 G8；`size==0` → 空文件走 G9。约 5 行、单文件、无接口变化；需补新分支用例（现无用例覆盖该形态）。

### 7. HANDOFF §9.4.3：move-assign 用 `sqlite3_close`；`columnCount()` 无锁
**结论：CONFIRMED（两条均属实）但均无现网可达性**（死路径 / 死代码）。
证据：
```
sqlite_db.cpp:235  if (handle_) sqlite3_close(handle_);   // 析构 :218-226 已改 close_v2
唯一调用点 = database.cpp:21（Database::operator=(Database&&)）；生产只有 move ctor（main.cpp:93）、
tests/ 无 move-assign → 死路径
sqlite_db.cpp:194  columnCount() { return sqlite3_column_count(stmt_); }  // 无锁；全仓 0 调用点
（grep columnCount src/ tests/：todo_module.cpp:91 系同名局部参数，非本法）
```
move-assign 一旦启用，目标连接有未 finalize 语句时 `sqlite3_close` 返回 BUSY → 句柄静默泄漏（与 L5 同根因）。实测 `sqlite3_threadsafe=1`（3.53.4，serialized）→ 偶发越锁 finalize/close 不破坏连接状态。
**最小修法**：move-assign 改 `close_v2`（1 行）；`columnCount` 补锁或删（1 行）；同类：`~Statement()`/`Statement::operator=(&&)` 的 finalize 亦无锁（低危）。

### 8. PAB-11：脚本 `-D` 是否真影响最终 exe
**结论：REFUTED**（仅配置层不一致，产物不受影响）｜现网可达性：无产物影响。
证据（本次 configure 即用脚本那套 `-D`，故 build.ninja/产物即真相）：
```
CMakeCache.txt: ZLIB_LIBRARY:UNINITIALIZED=C:/msys64/mingw64/lib/libz.dll.a   ← -D 生效于 cache
build.ninja:    grep -c "libz.dll.a" = 0                                      ← 无目标链接它
build.ninja:127 LINK_LIBRARIES = ... libpng-bundled-build/libpng16d.a  _eui_neo_build/libeui_zlib.a ...
                （libeui_zlib.a 由 EUI-NEO/3rd/zlib-1.3.1/*.c 现场编译；链上 libz.a×4 来自 curl 静态闭包）
objdump -p build/DevDesk.exe | grep "DLL Name" → 17 个全系统 DLL，zlib/libz 命中 0
```
`png_static` 走 EUI 自带 `eui_zlib`（build.ninja:1568 的 order-only 即 `..._target_eui_zlib`），全仓无 `find_package(ZLIB)`/`ZLIB::` 消费者 → -D 只污染 cache，不进链接线、不改 exe。
**最小修法**：删 `scripts/build-test-run.sh:101-102` 的 `-DZLIB_*`（一致性清理）；`-DOPENSSL_ROOT_DIR` 已被 `Dependencies.cmake` 自动探测覆盖，可留。

## 需要人工 / 未取证
1. 条目 4 的**标签级联**分支未复现（需「被误删行已有标签」：rename 撞 rescan 窗口，或 walk 出错路径）——建议单独补用例。
2. 条目 1 落地时须同步改 `file_watcher.h:60/75-76` 注释，把「对象须活到回调返回」写成显式消费方约束；场景 c（3/3 异常终止）不作独立结论。
3. 条目 5 的 621 以本 worktree 构建为准；主会话跑完全量后应把 `Total:` 回填文档、替换 530/589 两处硬编码。
4. 未跑全量套件、未启动 app；EUI-NEO 工作树在构建期间被他会话推进（rev 269a8c4）——条目 8 的链接线证据取自 build.ninja 内目标，与该 rev 无耦合。
