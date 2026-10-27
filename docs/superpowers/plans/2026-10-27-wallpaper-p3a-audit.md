# 壁纸模块 P3a（Windows 平台引擎层）独立审计（2026-10-27）

- 审计对象：worktree `D:/project/gui/wt-wp-verify`，分支 `verify/wallpaper-module` @ `eec88dbb`（工作树干净）
- 审计者：独立子代理，未参与本轮改动（实现者为 worktree-agent-* 会话，独立性自检通过）；只读，唯一写为本文件
- 范围：`src/module_wallpaper/platform/*`、`wallpaper_service.*` 装配 diff（`3a8f2df5..HEAD`）、`tests/test_modules/test_wallpaper_platform.cpp`、`tests/probes/wallpaper_desktop_probe.cpp`
- 结论：**PASS（带 findings）**——无阻断项；2 Medium / 7 Low / 2 Info

## 一、OVERLAPPED 修复专项核查（eec88dbb，5 点）

| 点 | 结论 |
|---|---|
| ① 句柄所有权链 | **自洽，已核**。readHandle 由控制线程 Duplicate（mpv_controller.cpp:366-373）并独占 close（:256-259，且在 reader.join 之后）；读线程只用值拷贝、只关自己的 event（:239），绝不碰 readPipe/readHandle。 |
| ② 写超时排空序列 | **无竞态缺陷，已核**（mpv_controller.cpp:191-198）。CancelIoEx→GetOverlappedResult(bWait=TRUE) 必然返回：IRP 或被取消、或已先完成，进程死亡断管是第三重兜底；返回值有意丢弃（dropped），event 在 IRP 确定完成后才 CloseHandle，无 use-after-free。存在"超时与完成赛跑"误杀窗口（写实际已送达仍 TerminateProcess），属契约声明的保守取舍，记 Info F-9。 |
| ③ 读线程 join 有界性 | **已核，全路径可达**。读线程唤醒有三重独立机制：进程死断管 / CancelIoEx(pipe,nullptr) 按文件对象撤全部 IRP / CancelIoEx(readHandle,nullptr)；Terminate→CancelIoEx→join 顺序在 shutdownProcess 所有调用路径（run 尾、Stop 命令、spawn 四个失败点 :360/:369/:379/:385）均成立；reader 线程创建失败分支已先清 readHandle 再收尾（:377-381）。 |
| ④ 写超时后 OVERLAPPED 并发 | **已核无共享**。写 OVERLAPPED 是 writeBounded 局部量且出作用域前必被排空（完成路径 bWait=FALSE 但事件已 WAIT_OBJECT_0；超时路径 bWait=TRUE 排空）；读 OVERLAPPED 只存在于读线程。同一文件对象（pipe/readHandle 重复句柄）上各自持独立 OVERLAPPED 并发 I/O 合法；写侧全程单写者（仅控制线程，契约 :167 成立——handleCommand/spawn/shutdownProcess 全在 worker 线程）。 |
| ⑤ stop 梯子失败分支 | **已核无跳过**。quit 写超时→writeBounded 内就地 Terminate（:193）；宽限未退→Terminate+500ms（:248-251）；Terminate 本身失败时 join 仍经 CancelIoEx 有界返回，进程残留由 Job kill-on-close 兜底（:268-272）。唯一残余风险：Job 创建/Assign 失败仅 LOG_WARN（:337-346）且 Terminate 又失败 → 进程带窗残留，概率极低（Info F-11）。梯子总时延 300+800+500ms < 3s 与注释、探针 §5 口径一致。 |

## 二、六维度

1. **正确性**：核心修复自洽（上表）；新增 finding F-3（事件复用依赖内核隐语义）、F-8（陈旧错误码）、F-1（崩溃无检测）。未发现阻断缺陷。
2. **回归面（P1 门控与生命周期幂等）**：已核。工厂门控完好（wallpaper_module.cpp:76-79 `supported()` 否则 nullptr；capability win=true / macos=false / posix=false，src/platform/CMakeLists.txt 按平台登记）；默认关由 test_module_gating 覆盖；service start/stop 幂等有测试且断言 stop < 3s（test_wallpaper_core.cpp:683-688）；host start/stop 幂等（thread_.joinable 守卫）。1 Low（F-2 hostStarted_ 锁外写）。
3. **平台对称性**：已核，未发现问题。`_WIN32` 业务代码零命中（grep 证），Win32 只在 mpv_controller.cpp / wallpaper_host_win.cpp，CMake 按平台选源并在模块级 CMakeLists 登记（交叉编译守卫口径）；非 Windows stub 行为有测试（start 必败 + 中文原因，test_wallpaper_platform.cpp:34-51）；mpv_player.cpp 双平台共用委托。
4. **静默失败面**：已核。错误链完整：recordError（中文）→ takeError → tickLoop 250ms 轮询 → snapshot.playbackError + LOG_WARN；扫描异常不杀线程、scanning 兜底复位。缺口：mpv 意外死亡延迟暴露可达分钟级（F-1）、spawn 异步失败后 playerRunning_ 假位无恢复路径（F-4）。
5. **测试有效性（变异视角）**：纯逻辑断言变异有效（路径优先级含 Release 禁用兜底、参数表逐项、JSON 转义、time-pos 负样本、命令行转义——回退实现均会变红）；服务装配测试缺 "stop" 调用断言（F-7）；**OVERLAPPED 修复无自动化回归网**：Windows 单测只测空态，回退 eec88dbb 全部单测保持绿，唯一防线是需真机+mpv+视频的手动探针（F-0，本批最重）。修复类核对：无"先红后绿"证据（P3a 修复前从未跑过探针，属验证前设计改写，可接受但应留痕）。
6. **文档口径**：已核，无矛盾。spec §6 P3 摘要含图片层/watchers/全屏，本批只交付 WorkerW+mpv——handoff（docs/handoff/2026-10-27-wallpaper-module-handoff.md §3）已显式重划 Batch 3b，service.h:13-15 明确"不得对外宣称自动让路已生效"；wallpaper_service.h:48 注释"待 P3 解码接入"与 3b 口径差一词，不值 finding。开发兜底路径、非 Windows nullptr/capability false 与 handoff §6 决策一致。

## 三、Findings（severity 按难维护程度定）

### F-0 [Medium] OVERLAPPED 死锁修复缺自动化回归网
- file: `tests/test_modules/test_wallpaper_platform.cpp:52-62` + `tests/probes/wallpaper_desktop_probe.cpp:199-218`
- 问题：Windows 单测对 MpvController 只验证空态（不 spawn、不连管道）；回退 eec88dbb（改回同步句柄读写）后全部单测保持绿。死锁防线唯一回归手段是 desktop probe 的 §3 pause（挂住读）→§4 seek（并发写）序列，而 probe 需要真机桌面 + mpv.exe + fixture 视频，且死锁表现为 hang 而非断言失败。
- 影响：后续任何人重构 IPC 层都可能静默重新引入"用户桌面挂死"，只能靠现场反馈发现，排障成本极高。
- 建议：①把 probe 列为 P3a 收口必须项并在 progress.md 留"修复前 hang / 修复后 PASS"证据；②补一个注入型最小 IPC 测试（占位 exe 挂住管道一端 + 断言 writeBounded 有界返回），或至少给 probe §5 显式断言 stop 耗时上限。

### F-1 [Medium] mpv 意外退出无检测，黑屏/冻结帧延迟暴露
- file: `src/module_wallpaper/platform/mpv_controller.cpp:100,273,525-527`
- 问题：alive 原子位只在 shutdownProcess 清零；mpv 崩溃/被杀后 processAlive() 恒 true，错误只在下一条 IPC 写失败时才 recordError——`--loop-file=inf` 下可能数分钟无任何命令（rotation 默认 300s）。
- 影响：现场"壁纸不动了"无法自愈也无提示，日志反推成本高；服务层永不重试（F-4 复合）。
- 建议：读线程排空循环顺带 `WaitForSingleObject(process, 0)` 清 alive；或 pollPlayerErrorLocked 发现进程已退出时主动补一条中文错误（含退出码）。

### F-2 [Low] service::stop() 在 mu_ 外写 hostStarted_（数据竞争）
- file: `src/module_wallpaper/wallpaper_service.cpp:136`（对照 :74、:541、:562 的 mu_ 内读写）
- 问题：`hostStarted_ = false;` 位于 host_->stop() 之后、mu_ 块之外；在飞 tick/scan 可持 mu_ 同时读该位。形式 UB（TSan 必报），破坏本项目严格锁纪律的可维护性基线。
- 影响：低成本修复的隐患；后续加断言/工具链（TSan CI）会被它绊住。
- 建议：写回挪进首块 mu_ 内，或改 `std::atomic<bool>`。

### F-3 [Low] readLoop 跨迭代复用手动复位事件、依赖内核隐式复位
- file: `src/module_wallpaper/platform/mpv_controller.cpp:217-225`
- 问题：事件每轮 ReadFile 复用且不 ResetEvent，正确性依赖内核在 NtReadFile/NtWriteFile 排队 IRP 时复位调用方事件（NT 实测行为，但 MSDN OVERLAPPED 页未明示）；一旦语义理解有偏差，GetOverlappedResult(bWait=TRUE) 会带陈旧信号立即返回、读到垃圾长度。
- 影响：正确性不变量悬在文档外，后人移植/改写时易踩。
- 建议：每轮 ReadFile 前显式 `ResetEvent(event)` 一行，把不变量写进代码。

### F-4 [Low] spawn 异步失败后 playerRunning_ 恒 true、无恢复路径
- file: `src/module_wallpaper/wallpaper_service.cpp:571-580`（对照 mpv_controller.cpp:402 `if (!running()) break;`）
- 问题：ensurePlayerStartedLocked 在 start 命令入队后立即置 playerRunning_=true；spawn 失败发生在控制线程，此后所有 load 被控制器 running() 静默丢弃，直到整模块重开。
- 影响：错误文案虽会展示一次，但"恢复=重开模块"未成文，用户视角是永久黑屏。
- 建议：pollPlayerErrorLocked 检测 processAlive()==false && playerRunning_ 时，把 playbackError 升级为可操作指引（或自动重试一次 spawn）。

### F-5 [Low] mu_ 内同步等待宿主/播放器，最坏 UI 卡顿 ~4.6s
- file: `src/module_wallpaper/wallpaper_service.cpp:538-541`（stopPlaybackLocked：player_->stop() 梯子 ≤1.6s + host_->hide() ≤3s）、`:444-451`（applyVideoScanLocked：host_->show() ≤3s）
- 问题：以上全部持 mu_ 执行；期间 snapshot() 全阻塞。有界（无死锁），但模块关闭/换来源时 UI 冻结可感。
- 影响：可维护性代价在于"锁持有时间纪律"被打破，后续往这些路径加逻辑会继续放大卡顿。
- 建议：hide/show 移出 mu_（stop 梯子至少 hide 可后置到锁外）；player_->stop() 难移则文档明示上限。

### F-6 [Low] postAndWait 全程持 lifecycleMutex_ 等待 3s
- file: `src/module_wallpaper/platform/wallpaper_host_win.cpp:433-456`
- 问题：宿主线程无响应时，后续 stop() 串在其后再等 3s，模块停止最坏 ~6s+。
- 影响：与 F-5 叠加拖长模块关闭路径。
- 建议：等待期间释放 lifecycleMutex_（先快照 joinable/stopRequested 再等），或缩短 kControlTimeoutMs。

### F-7 [Low] service 装配测试缺 "stop" 命令断言（变异假绿）
- file: `tests/test_modules/test_wallpaper_core.cpp:683-688`
- 问题：用例断言 stop 有界+幂等，但未断言 NullPlayerBackend 收到 "stop" 调用；删掉 stopPlaybackLocked 的 player_->stop()（wallpaper_service.cpp:538）全单测仍绿。
- 影响：服务→播放器的停止链无变异防线，回归只能靠真机。
- 建议：补一行 `DOCTEST_CHECK(hasPlayerCall(*playerRaw, "stop"));`。

### F-8 [Low] utf8ToWide 失败时错误码陈旧
- file: `src/module_wallpaper/platform/mpv_controller.cpp:312-328`
- 问题：mpv 路径非法导致宽字转换失败时 CreateProcessW 未被调用，createError 取陈旧 GetLastError，错误文案"Windows 错误码 N"误导排障。
- 影响：仅错误文案准确性，维护口径问题。
- 建议：application 为空时直接报"mpv 路径含非法字符"。

### F-9 [Info] 写超时与完成赛跑的误杀窗口（契约内取舍）
- file: `src/module_wallpaper/platform/mpv_controller.cpp:191-198`
- 问题：WaitForSingleObject 超时与 IRP 恰好完成存在竞态，正常但极慢的写会被判通道死亡并 TerminateProcess。
- 影响：保守 fail-safe，符合头注契约（"写超时判定 IPC 通道死亡"）；仅记录。
- 建议：不改；如未来出现误杀反馈，再考虑超时前二次探测。

### F-10 [Info] Job 创建/Assign 失败仅 WARN，极端路径进程残留
- file: `src/module_wallpaper/platform/mpv_controller.cpp:336-347`
- 问题：Job 创建或 AssignProcessToJobObject 失败仅 LOG_WARN；若此后 TerminateProcess 也失败，mpv 带窗残留（无 Job 兜底）。
- 影响：双保险同时失效才触发，概率极低；记录为已核。
- 建议：Assign 失败时可在 error 槽留痕（不阻塞启动）。

## 四、核查覆盖声明

- mpv 生命周期契约：spawn→connect→load→控制→stop 全路径句柄链（process/job/pipe/readHandle/event/线程）逐一核对，正常路径与 spawn 四个失败点（mpv 缺失/启动失败/连管超时/DuplicateHandle 失败/读线程创建失败/初始化写失败）均无泄漏；Job kill-on-close 兜底存在。
- 平台红线：业务代码零 `#ifdef _WIN32`（grep 证）；非 Windows 工厂 nullptr（wallpaper_module.cpp:76-79）+ capability false（posix/macos）；模块零兄弟依赖（CMakeLists 仅 core/platform/ui_kit）。
- service 装配回归：P1 门控、onStart/onStop 接线、stop 顺序（saveResume→player.stop→host.stop→join）与幂等均核。
- 证据缺口：本机无 batch-3a-brief.md / WALLPAPER-MODULE-DEV.md（实现者环境产物），文档口径以 spec §6 + handoff §6 替代核对。

## 五、限定复审计（2026-10-27，针对 ef5bdf79 + dce4a25a 两笔修复提交）

- 审计者：独立子代理，未参与两笔修复（实现者为 verify/wallpaper-module 分支主会话），独立性自检通过；范围仅 `git diff eec88dbb..HEAD`，不做全面复审。
- 六维度逐条：正确性（两笔修复本体自洽，见下）；回归面（无回退——hostStarted_ 前移仍在首块 mu_ 内且早于 host_->stop()，只收紧不放宽，wallpaper_service.cpp:130-137）；平台对称性（测试缝/新用例全部 `_WIN32` 门控，非 Windows checkIpcWriteBoundedRegression 直接 return true，mpv_controller.h:247-257）；静默失败面（净改善：意外退出错误经 tickLoop 250ms 轮询浮出，替代原分钟级暴露）；测试有效性（见抽查）；文档口径（见 R-1 归 P5 文件）。

### 处置对账（11/11 有结论，异常 0）

| 原 finding | 处置 | 核对 |
|---|---|---|
| F-0 [M] 死锁修复无回归网 | 修 | 注入型最小 IPC 测试（test_wallpaper_platform.cpp:232-353，真命名管道对端拒绝读 + 伪进程句柄）+ 探针 §5 stop<3s 显式断言（wallpaper_desktop_probe.cpp:244-247）；②的两条建议路线都做了且超出建议下限。F-0①（progress.md 留"修复前 hang/修复后 PASS"）未见留痕 → 记 R-4 |
| F-1 [M] mpv 意外退出无检测 | 修 | readLoop 断管退出后 0 超时探测进程句柄 + stopRequested 区分收尾（mpv_controller.cpp:247-259），shutdownProcess 先置旗标再动通道（:267-268）、spawn 复位（:301-302）；误报面核查见下 R-2/R-3 |
| F-2 [L] hostStarted_ 锁外写 | 修 | 写回挪入首块 mu_（wallpaper_service.cpp:136），与建议一致 |
| F-3 [L] 事件依赖隐式复位 | 修 | 每轮 ReadFile 前 ResetEvent（mpv_controller.cpp:232），与建议逐字一致 |
| F-4 [L] spawn 失败后 playerRunning_ 假位 | 不修（理由成立） | 需给 PlayerBackend 扩 processAlive 破层界；F-1 修复后错误 250ms 内浮出，检测半边已补，恢复口径显式留 P6 |
| F-5 [L] mu_ 内同步等 ~4.6s | 不修（理由成立） | 牵动收尾时序契约 §9.6，单独批处理；未改码，无背离 |
| F-6 [L] postAndWait 持锁 3s | 不修（理由成立） | 同 F-5 |
| F-7 [L] 缺 stop 命令断言 | 修 | hasPlayerCall(*playerRaw, "stop")（test_wallpaper_core.cpp:686），正是建议的一行 |
| F-8 [L] utf8 失败陈旧错误码 | 修 | application/commandLine 空 ⇔ 转换失败（mpv_controller.cpp:344-356；到 CreateProcessW 点 mpvPath 必非空，判据充分），报"路径含无法转换的字符" |
| F-9 [I] 写超时误杀窗口 | 不修（= 原建议"不改"） | 一致 |
| F-10 [I] Job 失败仅 WARN | 不修（理由成立） | 原建议本为可选项，"error 槽噪音"理由成立 |

### 新引入面（派单四点中 ②③ + 无其他 major）

- **② 0 超时探测误报面**：主动收尾区分正确——shutdownProcess 置旗标先于一切通道动作（:268），意外退出与主动收尾无判别竞态；唯一残余窗口在 writeBounded 超时路径 TerminateProcess（:199）先于 stopRequested.store（:200）约纳秒级，读线程理论上可抢先见断管而误报"意外退出"并覆盖"写入超时"文案（recordError 覆盖语义，:131-134）。可达性趋近零且发生在已判死的通道上，仅错误文案口径 → R-2 [Info]。反向漏报面：断管先于进程对象置信号的终止清理窗口内，一次性探测 WAIT_TIMEOUT 不报，回退旧路径（下一条 IPC 写失败），有安全网不劣化 → R-3 [Info]。
- **③ 假管道+伪进程句柄回归测试非恒真**：三重防呆——前置条件自证（fillBroken → REQUIRE_MESSAGE 显式红，:294-299）；下界 900ms + "写入超时"文案断言防瞬时错误撞绿（:332-334）；错误槽轮询 5.5s 窗口钉住 kIpcWriteTimeoutMs=1000（改大即红）。红路径控制器/线程故意泄漏避免析构 join 吞报告（:339-343），doctest 进程退出由 OS 回收，工程上成立。
- 其余 diff 面（mpv_controller.h 测试缝、wallpaper_desktop_probe §5）未发现正确性/静默失败问题，已核。

### 抽查：写超时回归变异敏感（静态推演）

回退 eec88dbb（同步句柄写）推演：pause(true) 异步入队（mpv_controller.cpp:483-488）主线程不挂，worker 卡死在无界 WriteFile；错误槽 5.5s 内不出现 → `error.empty()` → return false **红**（:322-325）；即便跳过①，stop() 的 cv 等满 5s 上限 → stopElapsed ≥ 3s → **红**（:339-342）。两条独立判据，且 300+800+500ms 梯子（kIpcQuitWriteTimeoutMs/kQuitGraceMs/kTerminateWaitMs，mpv_controller.cpp:31-33）与断言 <3s 数值吻合、sendRaw recordFailure=false 与"兜底写不刷错误槽"断言吻合。**结论：变异敏感，非假绿。**

### 复审计结论

**通过**。原 11 条全部有处置结论（修 6 / 不修 5 理由成立 / 异常 0）；新引入面无 major（R-2/R-3 [Info] + R-4 [Info] 留痕缺口）；写超时回归抽查变异敏感。遗留 R-2/R-3/R-4 不阻断合入。
