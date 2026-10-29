# macOS 兼容性代码审查报告（2026-10-29）

- 审查人：傲娇毒舌魔女（前端开发，交叉审查口径同 Windows 报告 docs/superpowers/plans/2026-10-29-windows-compat-audit.md）
- 背景：老板点名「Windows 时期写的 macOS 代码，过时 API 问题」；只做代码审查，不上真机
- 范围：src/ 下 17 个 mac 平台文件（16 个 *_macos.cpp + notification_macos.mm）+ EUI-NEO 4 个 .mm 文件抽读
- 证据基线：全量重建（rm -rf build）构建绿 exit 0，**deprecation/过时 API 警告 0 条**（唯一警告 = ld 重复链接 libpng16d.a，见 P2-1）；逐文件通读全部 17 个文件

## 结论

无 P0。1 条 P1（process_runner 同类死锁，Windows 侧同病已修、mac 侧未修）。其余为记录级。

## Findings

### [P1] src/platform/process_runner_macos.cpp:211-267 — 先等退出后读输出 + 先写 stdin 后读输出，管道缓冲即死锁 — 为什么：与 Windows 线 P1（process_runner_win.cpp，后端已修 f7e88f89）完全同类 — 建议：交后端修（两处独立死锁）

死锁机理（macOS 管道缓冲上限 64KB）：
1. **输出侧**：`waitpid`（阻塞或 WNOHANG 轮询到超时）全部走完才 `readFd` 读 stdout/stderr（:255-260）。子进程输出 >64KB 时写端写满阻塞 → 子进程永不退出 → 有超时设了也必然 timedOut=true + SIGKILL + 输出截断；无超时则无限挂。agent 工具跑 `git log`/构建类命令（输出轻松超 64KB）即触发。
2. **输入侧**：stdin 写入（:211-216）在等待退出之前、且单次同步 `write`。stdinText >64KB 且子进程不读 stdin（或在读它之前先被输出阻塞）→ 父进程 `write` 永久阻塞，任何超时机制都管不到这一段。

对照：Windows 版后端已按「读线程 drain + 先读后等」修过同类 P1（f7e88f89 修 ReadFile 请求量，P1 本体先读后等也在其修复范围），mac 版结构没跟上。修复方向：为 stdout/stderr 各起 drain 线程（或 poll+非阻塞读），stdin 写入同样移入带截止的写线程；`local_git_probe_macos.cpp` 的「deadline 优先于读数据」是仓内已验证的同款正确姿势。

### [P2] 构建日志 — `ld: warning: ignoring duplicate libraries: libpng16d.a`（EUI-NEO bundled 依赖被链两次）— 为什么：无害但每次全量构建都刷一条，干扰「警告=0」的判断口径 — 建议：EUI-NEO 侧 target 链接清单去重，长期记录不紧急

### [P2] src/module_settings/terminal_launch_macos.cpp — `std::system` 与 message_box 的 posix_spawn 口径不一致 — 为什么：当前拼的是 shellSingleQuote 包装的固定路径，非用户数据、无注入面，纯口径问题 — 建议：记录在案，下次触碰该文件时统一为 posix_spawn，不单独派工

### 记录级（不算缺陷，核实过原因）

1. **keychain_macos.cpp SecKeychain\* 弃用 API**：macOS 27 SDK 标弃用，但已逐语句 `#pragma clang diagnostic ignored "-Wdeprecated-declarations"` 窄抑制 + 枚举段已用现代 SecItem API。属已知已处置，不重复立案。长期方向：全面迁 SecItem。
2. **file_watcher_macos.cpp:293 FSEventStreamScheduleWithRunLoop 弃用**（macOS 13+，推荐 SetDispatchQueue）：已窄抑制，文件头写明选 CFRunLoop 的理由（跨线程 CFRunLoopStop 悬垂窗口）。设计自洽（有界 tick 停止时延 ≤0.25s），不动。
3. **file_open_macos.cpp kAEOpenDocuments 桥未实现**：头部注释明确「无 mac 实机时风险>收益，刻意不做」。维持待办状态。
4. **notification_macos.mm / 4 个 EUI-NEO .mm 抽读**：hasUsableAppBundle 守卫、delegate 生命周期、-fobjc-arc 均正确，无发现。

## 其余文件核过干净的（逐条已读，零发现）

single_instance（恒 true 空实现）/ tray_icon / wallpaper_capability / window_layout / system_theme（CFPreferences 正确释放）/ paths（_NSGetExecutablePath 两段式）/ window_visibility / folder_picker（osascript 转义）/ autostart（plist 0644）/ message_box（posix_spawn+转义+取消/失败区分）/ local_git_probe（非阻塞读+deadline 优先，正确姿势）/ file_watcher（FSEvents 全套：APFS 判据、双前缀 /tmp 符号链接、溢出折叠 Unknown、回调线程内 stop 不 join 防 UAF——质量最高的一个）。

## 判定

macOS 平台层整体纪律良好（薄适配、弃用窄抑制、防线齐全），无需返工。P1 交后端修 process_runner_macos.cpp，修完我交叉审。
