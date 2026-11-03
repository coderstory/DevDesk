# Windows CI 测试层环境分叉·系统扫描报告（池 118 后续扫描线）

- 日期：2026-11-03；执行：qa-tester（只读 + 实验，未改任何 src/tests 代码）
- worktree：`/Users/coderstory/dev/DevDesk-wt-scan`（分支 `wt/win-ci-scan`，HEAD `b32c3170`）
- 背景：2026-11-03 已实证修复 7 类分叉（R2-R7，见池 118）；本扫描找未爆的同类雷。

## 总判定：**未发现新的高概率会红分叉；1 项中概率建议加守卫，其余记录在案**

- 本机全量基线：`bash scripts/build-test-run.sh --no-run` → **2095 例，Passed 2089 / Failed 0**（6 skip），守卫链全绿，exit=0。
- Windows 交叉编译守卫：397 源文件全过（exit=0）；**边界：3 个测试文件因本机缺 curl/curl.h 未被覆盖**（见 §GCC16）。
- 动态抽查 6 项全绿（git PATH 剥离降级 skip、junction mac skip、watcher/process_runner/凭据缝绿、CI=1 零耦合）。

## 一、静态扫描清单（按 7 类，CI 面 = test_core/test_platform/test_modules 的 doctest 全量；probes 不进 CI（ci.yml:20 `-DDEVDESK_BUILD_PROBES=OFF`）、tests/live 由 env 门控不进 CI）

### C1 POSIX 路径/工具调用
| 位置 | 内容 | 概率 |
|---|---|---|
| tests/test_modules/test_agent_tools.cpp:1130 | `/bin/sh`-c + dd（128KB 载荷）——**#else 分支**，Windows 走 cmd type（:1117-1128），双向齐 | 低（已修 R5/R6，护栏在） |
| tests/test_modules/test_markdown_service.cpp:96-102 | `std::system("mklink /J …")` 悬空 junction 注入；POSIX 无 mklink → :217-220 `DOCTEST_SKIP`（D2 动态实证 mac SKIP） | 低（红不可能；**覆盖不对称**：断言仅 Windows CI 实跑，mac 全绿的「绿」对本用例是 skip 态——记录在案） |
| tests/live/github_sync_live.cpp:103 | `::popen` | 低（live env 门控，不进 CI） |
| tests/test_modules/test_md_note_git.cpp:67 | runGit 在 gitSubprocessWorks 探针内（0xC0000022 受控文件夹访问防护） | 低（探针+守卫齐） |

### C2 凭据/钥匙串缝
| 位置 | 内容 | 概率 |
|---|---|---|
| tests/test_platform/test_keychain.cpp:29-33 | `SKIP_ON_MACOS_KEYCHAIN`（mac 真实 login keychain 不可隔离 → skip；Windows DPAPI+%APPDATA% 文件照常执行） | 低（设计护栏） |
| tests/test_platform/test_keychain.cpp:380-390 | Windows AV/Defender sharing violation → **已有 5×20ms 有界重试** | 低（已修） |
| tests/test_platform/test_credential_protection.cpp | 6 用例；Windows skip 缝 R2 已修；D5 动态 mac 6/6 绿 | 低 |
| tests/test_modules/test_streaming_proxy.cpp:260 | 独立测试命名空间 `DevDeskTestStreamingProxy`，不触真实 Keychain | 低 |

### C3 字体度量断言
| 位置 | 内容 | 概率 |
|---|---|---|
| tests/test_modules/test_font_em_layout.cpp、test_editor_font.cpp、test_theme_fontscale.cpp | font-em CJK 分叉已留痕 skip（池 117）；本机全量绿 | 低（CI 面已护栏；真发现=池 117 交真机 UAT，非本扫描判定范围） |

### C4 固定时长 sleep（>500ms 墙钟假设；CI 面内全部 >500ms 睡点已逐一定位判向）
| 位置 | 内容 | 概率 |
|---|---|---|
| tests/test_platform/test_file_watcher.cpp:343 | 5 文件突发后固定睡 1500ms 再断言「全部送达」——正向等待但**无轮询**，慢机若去抖 300ms+交付 >1500ms 则假红 | **中**（建议改条件轮询，见 §四） |
| tests/test_platform/test_http_client.cpp:118 | 1500ms=本地 /slow handler 延迟；断言客户端 300ms 提前返回——客户端侧确定性 | 低 |
| tests/test_modules/test_claude_ui.cpp:536 | 1500ms=假上游故意延迟，造「在飞宽窗」——方向安全（慢机窗口更宽） | 低 |
| tests/test_modules/test_net_fetch_tool.cpp:165、test_claude_service.cpp:2348 | 同上型假上游/超时断言 | 低 |
| tests/test_modules/test_markdown.cpp:4339 等 18 处 520ms | 「睡过 400ms 防抖阈值→走一帧 flush→读盘逐字节比对」——等足再断言，方向安全 | 低（仅套件时长成本） |
| tests/test_modules/test_markdown.cpp:1594 | 5200ms poll 窗正向等待 | 低 |
| tests/test_modules/test_project_cards.cpp:210/233 | 1100ms=跨 epoch 秒边界（睡 ≥1s 恒跨 ≥1 边界），方向安全 | 低 |

### C5 路径硬编码
| 位置 | 内容 | 概率 |
|---|---|---|
| tests/test_platform/test_terminal_launch.cpp:88-199 | `C:/Users/demo`、`/Users/demo` 字面量——纯字符串装配断言；`buildMacCommandBody` 实现在 src/platform/terminal_launch_common.cpp:247（**跨平台**，非 macos 专有）；平台分支 `#if`（:323/:361）齐全 | 低 |
| tests/test_core/test_paths.cpp:31-42,240-264 | USERPROFILE/HOME/APPDATA 与实现同源映射 + ScopedEnv 注入临时目录 | 低 |
| tests/test_main.cpp:136-145 | 数据根统一树内 DEVDESK_DATA_ROOT，无用户绝对路径 | 低 |

### C6 外部工具 spawn
| 位置 | 内容 | 概率 |
|---|---|---|
| tests/test_modules/test_md_note_git.cpp:130 | `executableOnPath("git")` 守卫 → skip；**D1 动态实证**：PATH=/nonexistent 下 10 例 8 过 2 SKIP 0 红 | 低（R3 守卫实证有效） |
| 同上（残余） | runner git config 镜像默认值（autocrlf 等）依赖 GitHub Actions windows-runner 镜像不变；当前绿 | 低（记录在案） |
| tests/test_platform/test_http_server.cpp | 直接用 libcurl API（静态链），非外部进程 | 低 |
| mklink | 见 C1 | 低 |

### C7 日期/时区窗
| 位置 | 内容 | 概率 |
|---|---|---|
| tests/test_modules/test_home.cpp | **另一棒在修，只登记不判**（简报口径） | — |
| tests/test_core/test_schema.cpp:1275 | H30 归档用 now−2×86400（恒早于今日零点，设计已防跨午夜） | 低 |
| tests/test_core/test_schema.cpp:1361 | 零点边界用例：竞窗=「now 捕获与 archive 调用之间恰跨本地午夜」≈亚毫秒级（1/86400000 量级） | 低（记录在案，不建议修） |
| tests/test_modules/test_agent_time_text.cpp:22-40 | 固定日期 + dayAt(delta) 构造，now 只捕获一次 | 低 |
| tests/test_modules/test_agent_scheduler.cpp:256-262、test_agent_cron.cpp:79/229 | 时钟注入 `[&nowSec]{return nowSec;}` + `tm_isdst=-1` mktime 归一 | 低 |
| tests/test_core/test_time.cpp:79-101 | midnight/DST 不变式自洽断言 | 低 |
| tests/test_platform/test_terminal_launch.cpp:229-260 | 24h sweep 用文件 mtime 25h/1h 构造 | 低 |

## 二、动态抽查记录（命令与输出全文：/tmp/d1-d6.log、/tmp/scan-build.log；二进制 build/tests/devdesk_tests）

| # | 命令 | 结果 |
|---|---|---|
| 基线 | `bash scripts/build-test-run.sh --no-run` | exit=0；Total 2095 / Passed 2089 / Failed 0（6 skip），守卫链全绿 |
| D1 | `env PATH=/nonexistent devdesk_tests --filter=notes_git` | rc=0；10 例 8 过 2 SKIP 0 红（git PATH 守卫降级 skip，非红） |
| D2 | `devdesk_tests --filter=traversal` | rc=0；1 过 1 SKIP（mac 无 mklink → junction 注入 SKIP） |
| D3 | `devdesk_tests --filter=file:test_file_watcher` | rc=0；6/6 绿（mac 轮询兜底路径健康） |
| D4 | `devdesk_tests --filter=file:test_process_runner` | rc=0；3/3 绿（POSIX dd 分支） |
| D5 | `devdesk_tests --filter=file:test_credential_protection` | rc=0；6/6 绿（凭据缝 mac 面） |
| D6 | `env CI=1 devdesk_tests --filter=file:test_process_runner` | rc=0；3/3 绿（CI 环境变量零耦合实证） |

交叉编译：`bash scripts/check-windows-cross-compile.sh` → exit=0，397 文件全过（llvm-mingw clang 23.1.2）。

## 三、GCC16 编译面

- HEAD 交叉编译面**过**（397 文件）。脚本自声明边界：本机 clang+libc++ ≠ CI GCC+libstdc++；另 **3 个测试文件（test_proxy_router / test_http_client / test_http_server）本机缺 curl/curl.h 未验证**——本机预检盲区，CI 侧 MSYS2 实编已覆盖（当前绿）。
- `std::function` 返回 `core::Result` 模式全仓清单（25 处，maybe-uninitialized 同族风险点）：
  src/core/sqlite_db.h:110；src/module_memory/proposals/proposal_store.h:62,64,77；src/module_memory/proposals/curator.h:37；src/module_schedule/channels/wecom.h:138；src/module_schedule/action_runner.h:77；src/platform/github_api.h:32；src/platform/process_runner.h:29；src/module_mcp/mcp_tools_builtin.h:15；src/module_claude_proxy/claude_service.h:155,249；src/module_claude_proxy/proxy_router.h:47；src/module_claude_proxy/profile_repository.h:47（.cpp:157）；src/module_agent/net_fetch_tool.h:28；src/module_agent/direct_llm_client.h:21；src/module_release/release_config_ui.h:88-91（.cpp:584,621）。
- 防护现状：**全仓仅 proposal_store.h:79-82 一处定点 pragma**（+R4 空闭包守卫）；其余 24 处当前编译干净（交叉守卫过 + CI 绿）。测试文件内唯一同类落点：test_proxy_router.cpp:450（恰在 curl 盲区 TU 内，CI 实编已覆盖）。

## 四、防护建议（哪些修 / 哪些加守卫 / 哪些记录在案）

**该修（新高危）**：无。

**建议加守卫**：
1. tests/test_platform/test_file_watcher.cpp:343 —— 1500ms 固定交付窗改条件轮询（同文件 ：106-113 `waitWatcherArmed` 已有 deadline+seen() 轮询范式，可直接复用；本项是唯一中概率点）。
2. GCC16 模式入检查单：新代码出现「std::function 槽返回 core::Result」时按 proposal_store.h 范式定点 pragma 或空闭包守卫（沉淀进 .zcode/lessons；24 处存量不动）。

**记录在案（不动）**：
1. mklink junction 用例（test_markdown_service.cpp:204）仅 Windows 实跑断言，POSIX 恒 SKIP——覆盖不对称是设计使然，审计「mac 全绿」时须知此成分。
2. runner git config 镜像依赖（可选加固：runGit 统一 `-c core.autocrlf=false`，非必须）。
3. test_schema.cpp:1361 零点竞窗亚毫秒级，不值得修。
4. test_markdown 520ms×18 / 5200ms、test_project_cards 1100ms×2 只耗套件时长（≈15s）不致红；如做 CI 提速可再议。
5. 交叉检查脚本 curl 盲区 3 文件——CI 全量实编已兜底。

## 五、边界与自报
- 未改任何 src/tests 代码；worktree 内仅新增本报告 + build/（git-ignored）；未 commit/push；未碰主树与 wt-need3。
- test_home.cpp 日期窗按简报口径只登记不判（另一棒修复中）。
- 实际工具调用数：27（简报预算 ≤40）。
