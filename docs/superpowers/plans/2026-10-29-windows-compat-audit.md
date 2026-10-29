# Windows 兼容性只读审计（2026-10-29）

审计者：devdesk-auditor 子任务（只读，唯一写动作 = 本文件）。
范围：全仓 `*_win.cpp` / `*_windows.cpp`、src/ 平台泄漏、近期调度线平台对称性、平台专属命令、时区/DST。EUI-NEO submodule 不在审计范围。

## 总判定

**需处理（轻量）**：无 P0。1 条 P1（Windows 进程执行器管道死锁风险）+ 若干 P2。平台文件清单完整、CMake 平台选择齐全，业务代码平台宏泄漏极少（3 处），整体 Windows 侧编译风险低。P1 不阻塞编译，属运行期行为错。

## 维度 1：Windows 平台文件清单与逐文件核查

已核 30 个 `*_win.cpp`（git ls-files）：src/core 5 个、src/platform 18 个、module_memory 3 个、module_settings 1 个、module_wallpaper 3 个。全部按「头文件平台无关接口 + 平台实现拆文件」模式；`src/platform/CMakeLists.txt:20-39`、`src/core/CMakeLists.txt`、三个模块级 CMakeLists 的 WIN32/APPLE 分支源清单逐一对照，无「磁盘存在却未进受编集合」遗漏（wallpaper CMake 注释明确交叉编译守卫只解析一层，已把源登记在模块级 CMakeLists）。

逐文件要点：`single_instance_win.cpp`（命名 mutex + 事件监听线程，防重入 magic static 有注释契约）、`keychain_win.cpp`（DPAPI + 文件名可逆编码 + 旧命名兼容 + sweep 编码后比对，逻辑自洽）、`credential_protection_win.cpp`（DWORD 窄化显式判上限，无截断口）、`file_watcher_win.cpp`（ReadDirectoryChangesW + OVERLAPPED + 去抖，路径统一转 POSIX 分隔）、`autostart_win.cpp`、`terminal_launch_win.cpp`（app execution alias 用 GetFileAttributesW 探测，正确）、`window_visibility_win.cpp`（fail-open 降级）、`notification_win.cpp`（EUI 托盘 NIF_INFO，BackendUnavailable 不假装成功）、`http_client_ca_win.cpp`（CURLSSLOPT_NATIVE_CA，注释已更正 CURL_CA_BUNDLE 误传）——均未发现 P0/P1 问题。

### findings

（本维度无）

## 维度 2：业务代码平台泄漏核查

已核：`#ifdef _WIN32 / __APPLE__` 全 src/ 扫描（平台实现文件除外），命中 7 处（含 1 处纯注释引用）；硬编码路径分隔符与拼接；include 大小写对照（git ls-files + EUI-NEO 目录实测比对）；CRLF 假设；sqlite3 / 时间戳 / 文件大小的整型窄化。

### findings

- [P2] src/module_agent/agent_system_prompt.h:82-88 — 业务头文件内 `#if defined(_WIN32)/#elif defined(__APPLE__)` 拼系统名 — 违反 CLAUDE.md「业务代码禁止散布平台宏」铁律（虽两平台都能编译、行为等价）— 建议下沉到 core 层平台拆文件（如 `os_name_win.cpp` / `_macos.cpp`）取 `platformOsName()`。
- [P2] src/module_memory/engine/telemem_config.cpp:85-90 — 业务编译单元内 `#ifdef _WIN32` 走 `_wfopen_s`（宽字符 fopen，语义正确但属平台宏泄漏）— 同上铁律；宽字符打开是对的（Windows 路径非 ASCII 必需），问题只在宏位置 — 建议把「按 fs::path 打开 FILE*」收进 core 平台拆文件。
- [P2] src/module_wallpaper/platform/mpv_controller.h:243,250 + mpv_controller.cpp:572-580 — 平台无关头文件内 `#if defined(_WIN32)` 暴露 `MpvControllerTestAccess` 测试注入缝（.cpp 侧同为 WIN32 分支）— 该 .cpp 只在 WIN32 编译（模块 CMake 分支选择），但头文件两平台都参与编译，测试缝在非 Windows 侧类型缺失；当前无 mac 侧引用故不炸 — 建议注入缝挪进 win-only 的私有头或改由 stub TU 提供空实现，消除头内宏。

其余已核无问题：
- `src/platform/http_server.cpp:213` 的 `#ifdef _WIN32`（SO_EXCLUSIVEADDRUSE vs SO_REUSEADDR）位于 platform 层且两侧语义注释完整，属平台层内部合理分支，不算业务泄漏。
- include 大小写：全仓提取的 5 个「表面疑似」经目录实测全部真实存在（`EUI-NEO/core/platform/window_effect.h`、`core/window/window_backend.h`、`core/render/text.h`、`core/platform/platform.h`、`3rd/yyjson-0.12.0/src/yyjson.h` 且 include 路径已挂）——零大小写错配。
- 路径拼接：`src/module_markdown/markdown_service.cpp:1428`、`src/module_markdown/md_module_tree.cpp:720,749,852` 用 `parent + "/" + name` 拼相对路径，但该仓口径是 POSIX 归一化字符串入 SQLite（`toPosix`），Windows 的 std::filesystem 亦接受 `/`，一致；`src/platform/terminal_launch_common.cpp:300` 的 `dir + "/"` 仅被 `terminal_launch_macos.cpp:65`（mac-only TU）调用。合规。
- CRLF：仅 HTTP 协议字面量与编辑器行终结符处理，无「假设文件是 \n」读盘逻辑问题。
- 整型窄化：`src/core/sqlite_db.cpp:162,173` 的 `static_cast<int>(value.size())` 是 sqlite3 bind 的 int 形参要求（SQLite 语法层面如此，超 2GB 文本不属现实场景）；时间戳全部显式 `static_cast<int64_t>`；`credential_protection_win.cpp` DWORD 窄化有显式上限判断。未见实际截断错。

## 维度 3：平台对称性（近期调度线 / channel_registry / schedule_engine / schedule_ui_*）

已核：`git log --oneline -30` 近期改动（调度线 S4/S7a/S7b/S6b、channel_registry、schedule_engine、schedule_ui_*、memcpp 集成）触及文件清单与平台相关面（托盘 / 窗口 / 定时器 / 路径 / keychain）逐项对照。

- `src/module_schedule/channels/os_notification.cpp` 走 `platform::notification.h` facade，`_win`（托盘 NIF_INFO）与 `_macos`（UNUserNotificationCenter）双侧实现齐备且都在 platform/CMakeLists 登记。
- `wecom.cpp` 凭据走 `platform/keychain.h` facade（DPAPI / Keychain 双侧齐备）。
- `fullscreen_channel.cpp` / `toast_channel.cpp` 只依赖 EUI-NEO 与 core，无平台面。
- schedule_engine / schedule_rule / internal_task_registry 的时间推进统一走 `core::time`（`time_win.cpp` 用 `localtime_s`），无平台分支。
- memcpp 线新增 `process_host_win.cpp` / `process_host_posix.cpp` 双侧（见 `src/platform/CMakeLists.txt:37,58,79`），对称。

### findings

（本维度无）

## 维度 4：平台专属命令出现在 src/

已核：grep `screencapture|otool|pkill -f|/opt/homebrew|/usr/bin/|lsof|open -a|osascript` 于 src/（非 scripts/docs）。

命中全部在 mac-only 平台实现文件或注释里：`src/platform/folder_picker_macos.cpp`（osascript）、`src/platform/message_box_macos.cpp:79-80`（`/usr/bin/osascript` posix_spawn）、`src/platform/message_box.h`（注释）、`src/main.cpp:459`（注释提及 open -a，实走 Apple Event 抽象）。无业务代码直拼平台命令。

### findings

（本维度无）

## 维度 5：时区 / DST

已核：grep `localtime|mktime|std::localtime|daylight|tzset` 全 src/。

主流口径统一：`src/core/time.h` 提供 `localTime`（线程安全，Windows=`localtime_s`）与 `localMidnightUnix`（`tm_isdst=-1` 交 mktime 决断，注释写明春进/秋退语义）；`src/core/schedule_rule.cpp:179,184,220`、`src/module_agent/task_draft_parse.h:98`、`agent_time_text.cpp`、`usage_ledger.cpp` 均按此口径，DST 切换日的「不存在时刻 / 重复时刻」处理有显式注释与测试口径。Windows CRT 与 macOS mktime 在 `tm_isdst=-1` 时都按本地规则决断，行为一致面已覆盖。

### findings

- [P2] src/module_release/release_reminder.cpp:135 — `std::localtime(&now)` 直用（非 core::time::localTime）— 依赖「引擎只在主线程」的局部约定绕开静态缓冲线程安全问题，且绕开了统一的 DST/时区口径层；Windows MSVCRT 的 std::localtime 同样非线程安全，未来被挪线程即踩雷 — 建议改走 `core::time::localTime`（一处机械替换，零行为变化）。

## 汇总：全部 findings

| # | severity | 位置 | 问题 | 建议 |
|---|----------|------|------|------|
| 1 | P1 | src/platform/process_runner_win.cpp:59-67 + 247-262 | 先 `waitProcess` 等进程退出、**之后**才 `readPipe` 读 stdout/stderr：子进程输出超过管道缓冲（Windows 匿名管道默认约 4-64KB）且未退出时会阻塞在写管道，父进程阻塞在 WaitForSingleObject → 双向死锁，直到 `timeoutMs` 超时才被 TerminateProcess 解开；`timeoutMs<=0`（INFINITE）时永久挂死。macOS 侧 `process_runner_macos.cpp` 不一定同构（若同构则两平台同病，仍按本条修） | 读循环与 wait 并行（双线程或 PeekNamedPipe 轮询 + 限时 wait，文件里已有 PeekNamedPipe 基建），先排空管道再收尸 |
| 2 | P2 | src/platform/process_runner_win.cpp:57-60 | 引号规则用「内部 `"` → `\"`」，与同仓 `exec_capture_win.cpp` / `process_host_win.cpp` 的 CommandLineToArgvW 逆规则（`""` 翻倍）不一致；含尾随反斜杠的引号参数会被错误解析 | 三处统一为 CommandLineToArgvW 逆规则，抽成共用函数 |
| 3 | P2 | src/module_agent/agent_system_prompt.h:82-88 | 业务头内平台宏拼 OS 名，违反接口隔离铁律 | 下沉 core 平台拆文件 |
| 4 | P2 | src/module_memory/engine/telemem_config.cpp:85-90 | 业务 TU 内 `_WIN32` 宏走 `_wfopen_s` | 「按 fs::path 开 FILE*」收进 core 平台层 |
| 5 | P2 | src/module_wallpaper/platform/mpv_controller.h:243,250 + mpv_controller.cpp:572 | 平台无关头内 `#if defined(_WIN32)` 暴露测试注入缝 | 注入缝挪 win-only 私有头或 stub 提供空实现 |
| 6 | P2 | src/module_release/release_reminder.cpp:135 | 直用 `std::localtime`，绕开 core::time 统一口径与线程安全层 | 改 `core::time::localTime` |

## 已核无问题的维度（一句话口径）

- 维度 1（平台文件清单与 CMake 受编集合）：30 个 `*_win.cpp` 全部登记进对应 CMake 平台分支，无遗漏、无 P0/P1。
- 维度 2 补充项：include 大小写零错配；POSIX 路径拼接口径统一且 Windows 兼容；无 CRLF 假设错误；sqlite/时间戳整型窄化均有显式处理。
- 维度 3（调度线平台对称性）：通知 / keychain / 进程托管全部走 facade 且双侧实现齐备。
- 维度 4（平台专属命令）：命中全部局限在 mac-only 平台实现文件与注释。

## 补录（前端补审并入 + 未上行项确认）

- [P2] src/module_schedule/schedule_ui_list.cpp 行尾操作区 — `scaled()` 与裸逻辑 px 混用（`contentW - 124.0f` 编辑/删除偏移、44×24 开关）— uiScale>1（Windows 125%）时裸 px 常量不随缩放，右侧操作区间距可能不均 — S7a 存量模式延续，R3 不顺手改，记入真机 UAT 一并核（前端补审口径，2026-10-29）。
- 未上行项确认：壁纸退出 terminate 修复不在本仓 dev 树（TODO 提交 1121f05f 标注「待验证/未上行」，代码在 Windows 机），本次只确认现状不修。
- DST 确认：时区主口径统一 `core::time`（`src/core/time_win.cpp` 用 `localtime_s`，接口隔离合规）；除 finding#6（release_reminder.cpp:135 直用 std::localtime）外无 Windows 行为差异点新增。
