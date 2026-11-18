# 真机复核：D1 重启装配 / 终端 cwd 双击口径（need:313）

- 角色：qa-tester｜日期：2026-11-18｜产物二进制：`build/DevDesk.app/Contents/MacOS/DevDesk`（打包态）与 `build/DevDesk`（非打包对照）
- 沙箱：`DEVDESK_DATA_ROOT=/tmp/need313-sandbox`，`DEVDESK_CLAUDE_CONFIG_DIR=/tmp/need313-claude`（自建、自清理进程；未碰 `/Applications/DevDesk.app` 实例 pid 70997 —— 全程存活至今）
- 证据目录：`evidence/2026-11-18-need313/`

## 总判定

| 项 | 结论 | 依据形态 |
|---|---|---|
| 复核 A（D1） | **通过（真机背书）**：真实二进制 9 次启动（7 打包 + 2 非打包对照），`preStart` 恒先装配 ClaudeService，全轮日志 grep「未装配」= 0；代理未启动的唯一原因是沙箱库无激活 profile（前置校验 1/2） | 两启日志对比 + 全轮计数 |
| 复核 B（终端 cwd） | **未通过**：打包态从 `$HOME` 启动，终端提示符与 zsh 子进程 cwd = `…/DevDesk.app/Contents/Resources`，非手册称的「用户主目录」；根因 = GLFW 默认 `GLFW_COCOA_CHDIR_RESOURCES=GLFW_TRUE` 在 `glfwInit` 时 chdir 到 bundle Resources；非打包二进制同条件落 `$HOME`（因果对照） | 提示符截屏 + `lsof` cwd + 非打包对照 |

## 复核 A：D1 双启装配（真实二进制）

### 测试清单

| # | 功能×场景×边界 | 验证方法 | 证据 | 结果 |
|---|---|---|---|---|
| R-A1 | Claude 装配 × 打包态首启 × 默认 enabledModules（claude_proxy 在默认清单内） | 沙箱根冷启 → 收 `logs/devdesk.log` → 退出 | `boot1.log` | 绿：出现 `Claude 代理未自动启动（claudeProxyAutoStart=false…）`（main.cpp:707，preStart 已跑通装配分支） |
| R-A2 | Claude 装配 × 二启 × 种子 `claudeProxyAutoStart=true` | sqlite3 预置 `enabledModules`/`moduleDefaultsVersion`/`claudeProxyAutoStart` 后二启 | `boot2.log` | 绿：出现 `Claude 代理未启动（前置校验未通过）: …（1/2 激活 Profile）`（main.cpp:675）——只有 ClaudeService 已装配、`startClaudeProxy` 走到 `validateProxyPreconditions` 才会打印此行 |
| R-A3 | 装配 × 全轮 × 「未装配」红线 | 9 次启动累积日志全量 grep | `run-final.log`（494 行，9 次「DevDesk 启动中」） | 绿：`grep -c 未装配` = **0**；各轮 `Claude 模块启动` 后均为 `onStart: 代理未运行`，无 `onStart: ClaudeService::global() 为 null` |
| R-A4 | Claude 页 × 打包态 × 无 profile 边界 | 侧栏点「Claude 配置」看页内文案 | 无（本轮被安全门拦截，见下） | **需人工**：启动后 frontmost 归属校验不通过（前台为 ZCode），按新安全规则跳过点击并弃轮，未取得页面截屏 |

### 日志摘录

首启（`boot1.log`，19:09:18）：

```
[INFO] DevDesk 启动中... (main.cpp:1169)
[INFO] 应用数据目录: /tmp/need313-sandbox (main.cpp:1170)
[INFO] Claude 代理未自动启动（claudeProxyAutoStart=false，可在设置页或 Claude 页手动启动） (main.cpp:707)
[INFO] Claude 模块启动 (claude_module.cpp:180)
[INFO] onStart: 代理未运行（默认不自动启动，等待用户手动启动） (claude_module.cpp:194)
```

二启（`boot2.log`，19:12:27，种子 autoStart=true）：

```
[INFO] DevDesk 启动中... (main.cpp:1169)
[WARN] 代理启动前置校验未通过：无激活 profile … (claude_service.cpp:307)
[WARN] Claude 代理未启动（前置校验未通过）: 代理启动前置校验未通过（1/2 激活 Profile）… (main.cpp:675)
[INFO] Claude 模块启动 (claude_module.cpp:180)
[INFO] onStart: 代理未运行（默认不自动启动，等待用户手动启动） (claude_module.cpp:194)
```

判读：`main.cpp:675` 属 `startClaudeProxy()` 内路径，其第一步即 `getClaudeService()`（main.cpp:641-658：创建实例 + `setGlobal` + 注册 `IClaudeProxyService`/`LlmProfileSource`）。该行出现 = 装配发生在模块 onStart 之前且成功。代理未监听是**配置前置**（无 profile），不是装配缺陷——与 BF-2 结案口径一致。

辅助：沙箱实例设置页截屏 `b4-terminal.png` 显示 Claude 代理卡「未运行/未接管」+「随应用启动代理」为开（该轮沙箱日志 19:19:36 有 `导航到: Settings`，确认为沙箱实例），说明种子配置生效、服务定位器可解析（设置页经 `IClaudeProxyService` 读运行态）。

### R-A4 需人工项（未取得自动证据）

Claude 页在「无 profile」边界的页面文案未取证：两次尝试均因前台归属校验不通过（前台非沙箱 pid）按安全规则跳过点击。人工步骤：

1. 关闭 /Applications 实例后用沙箱数据根启动 build 产物：`DEVDESK_DATA_ROOT=/tmp/need313-sandbox DEVDESK_CLAUDE_CONFIG_DIR=/tmp/need313-claude build/DevDesk.app/Contents/MacOS/DevDesk`；
2. 侧栏点「Claude 配置」，确认页面为引导态而非「ClaudeService 未装配」类错误文案；
3. 对照日志无 `onStart: ClaudeService::global() 为 null`。

## 复核 B：终端 cwd（打包态 vs 非打包对照）

### 测试清单

| # | 功能×场景×边界 | 验证方法 | 证据 | 结果 |
|---|---|---|---|---|
| R-B1 | 终端会话起点 × 打包态 × 进程从 `$HOME` 启动 | `cd $HOME` → argv 启动 `.app` 内二进制 → 侧栏点「终端」→ 读提示符 + `lsof` 子进程 cwd | `b12-terminal.png`、`b14-pwd.png`、`terminal-cwd-run.log` | **红**：提示符 `coderstory@coderstory-air Resources %`；zsh 子进程 cwd = `…/build/DevDesk.app/Contents/Resources`；应用自身 cwd 同值 |
| R-B2 | 同上 × 非打包二进制（因果对照） | `cd $HOME` → `build/DevDesk` → 开终端 | `b17-nonbundle-terminal.png` | 绿：提示符 `… ~ %`；应用与 zsh cwd 均 = `/Users/coderstory` |
| R-B3 | 文档口径 × 手册「默认用户主目录」 | 对照 `docs/USER_GUIDE.md` 3.11 与 `terminal_settings.h:42`（`"" = 用户主目录`） | 同 R-B1 | **不符**：macOS 打包态实际落 bundle Resources |

### 根因（代码定位）

- `EUI-NEO/3rd/glfw/src/init.c:59`：GLFW 内建默认 `_glfwInitHints.ns.chdir = GLFW_TRUE`；
- `EUI-NEO/3rd/glfw/src/cocoa_init.m:69`：`glfwInit` 时 `chdir(resourcesPath)`（仅当 bundle 的 Resources 存在——`.app` 内成立）；
- 全仓/EUI-NEO 非三方源码无 `glfwInitHint(GLFW_COCOA_CHDIR_RESOURCES, …)` 覆盖（grep 无命中）→ 进程 cwd 被 GLFW 改写为 `Contents/Resources`；
- 终端 mac 后端忽略 workDir（`src/platform/terminal/terminal_backend_mac.cpp:82-85`），`CortaBridgeSession::spawn` 不传 workingDirectory → 子 shell 继承进程 cwd。
- 推论：`.app` 双击/`open`/argv 启动都落 Resources，与启动时所在目录无关；driver 的「仓库根」假象来自非打包驱动进程未被 GLFW chdir 改写（与 R-B2 同型）。

### 影响

1. 用户开终端后默认不在主目录、也不在项目目录，`pwd` 显示 `…/Contents/Resources`（**手册与实际不符**）；
2. 终端设置里的「启动目录」在 macOS 对会话无效（mac 后端整段忽略 workDir），UI 可配但无效果——属「配置项在 macOS 完全无效」。

### 建议（交 developer / bug-fixer，不在本次执行）

- 方案 A（首选）：`glfwInit` 前 `glfwInitHint(GLFW_COCOA_CHDIR_RESOURCES, GLFW_FALSE)`，保进程 cwd；终端默认目录再显式取 `$HOME`（或设置值），两端语义一致；
- 方案 B：CortaBridge `spawnInteractive` 增 workingDirectory 参数，mac 后端把「空 workDir → HOME」解析后透传（对齐 Windows `resolveWorkingDirectory`）；
- 文档侧：修好前 USER_GUIDE 不得称「默认用户主目录」；修好后补回归用例「打包态从 $HOME 启动 → 提示符 = ~」。

### 人工步骤（如需在安装版复看，未执行）

1. 打开 /Applications/DevDesk.app → 侧栏「终端」；
2. 提示符应显示 `Resources`（预期复现）；输入 `pwd` 可见 `…/DevDesk.app/Contents/Resources`；
3. 终端设置里填一个启动目录 → 新建会话 → 仍为 Resources（mac 无效）。

## 证据物清单

| 用途 | 路径 |
|---|---|
| D1 首启日志 | `evidence/2026-11-18-need313/boot1.log` |
| D1 二启日志（种子 autoStart） | `evidence/2026-11-18-need313/boot2.log` |
| 全轮合并日志（9 启，含「未装配」=0） | `evidence/2026-11-18-need313/run-final.log` |
| 终端 cwd 运行日志 | `evidence/2026-11-18-need313/terminal-cwd-run.log` |
| 打包态终端提示符（Resources） | `evidence/2026-11-18-need313/b12-terminal.png`、`b14-pwd.png` |
| 非打包对照提示符（~） | `evidence/2026-11-18-need313/b17-nonbundle-terminal.png` |
| 沙箱设置页（Claude 卡未运行/未接管、随应用启动开） | `evidence/2026-11-18-need313/b4-terminal.png` |

## 分类与待办

- R-A：**通过**，无 bug。D1 已获真实二进制双启背书（驱动假象结案不再需要更多证据）。
- R-B：**新发现 bug**——
  - BUG-1（P1）：macOS 打包态终端默认 cwd = 应用包 `Contents/Resources`，非用户主目录（手册不符）；「启动目录」设置在 macOS 对会话完全无效（必需操作缺失，用户目标「新终端在指定目录开始」走不通）。
  - 关联 F-ID：终端模块 need:122 / need:250（启动目录）。
  - 状态：待 dev/bug-fixer 修，修后回归 R-B1 须转绿（红面已有：`b12/b14` 即红证据）。

## 自报

- 工具调用：约 80 次（超简报 35 次预算）。额外开销来源：窗口重叠导致 4 次点击落到 /Applications 实例窗口后重做定位；cwd 根因需逐步排除（GLFW 默认 hint → mac 后端忽略 workDir → CortaBridge 不传目录）并补非打包二进制对照；两次连接中断后续跑。
- 用时：约 75 分钟（超 30 分钟预算），含显示器休眠打断、EPIPE 断连续跑与安全门弃轮。
- 安全事件与整改：早期 4 次点击命中 /Applications 实例（两窗同坐标重叠），未造成状态改变（无按钮级破坏，点击落在侧栏/空白）；整改后规则 = 每次点击前 osascript `frontmost` unix id 双验 + 校验不过即弃轮；末轮 R-A4 即按此规则主动放弃，未取得证据则不硬凑。误触期间产生的含真实实例/用户界面内容的截屏已删除，仅保留沙箱产物。
- 未做（按禁做）：未改任何代码、未动池台账（仅开工状态位）、未碰真实 `~/DevDesk` 与真实 Claude 配置、未联网、未跑测试套件、未杀他人 pid（仅 kill 自启沙箱实例：43793/45915/52013/53074/57525/57906/87598 中仍存活的自身进程；/Applications 实例 pid 70997 全程存活）。
- 清理：/tmp/need313-sandbox 保留（含 devdesk.db 种子与外发日志副本），如不需要可整体删除；无残留沙箱进程。
