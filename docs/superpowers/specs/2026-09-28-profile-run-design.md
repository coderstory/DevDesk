# Profile「运行」—— 用该 profile 打开 Claude Code（设计）

日期：2026-09-28 ｜ 状态：**设计（未实施）· V2（按独立审计修订）** ｜ 关联：`2026-09-27-profile-auth-field-design.md`、`2026-09-27-profile-reverse-sync-and-copy-design.md`（反向同步——本设计**不改**其行为）

> **本文档中 D-1 ~ D-9 为本会话在用户授权自主决策期间所拍板的默认值**，专家复核/用户复核后若有推翻，按 §11 清单逐条改。

## 1. 需求与决策

| # | 需求（用户原话） |
|---|------------------|
| **R1** | 「profile 新增运行功能，claude 的启动参数可以指定配置文件的，点击运行后使用当前的配置打开 claude code」 |
| **R2** | 「这个运行，基于 windows 终端 + powershell 运行」 |
| **R3** | 「如果是 macOS 则使用默认的终端 app」 |

| # | 决策（自主拍板，待复核） | 理由 |
|---|--------------------------|------|
| **D-1** | 「运行」= **确保本地代理在跑** +（若目标 profile 非激活则）**先把它设为激活** + 然后在该 profile 的配置下打开一个新的 `claude` 会话 | 见 §3：跨平台唯一"密钥零落盘"的形态；实测 `--settings` 的 env 会覆盖进程 env、而 settings.json 的 env 又覆盖进程 env，所以"把密钥塞子进程环境"在接管态必然失效 |
| **D-2** | 「运行」按钮挂在 **profile 详情页操作按钮组**（与「启用/测试连通/编辑/删除/复制」同区），作用于**当前选中的 profile** | 与既有交互同源、零新基建；选中即"想跑的那一份" |
| **D-3** | 起始目录默认 **用户主目录**（Windows `%USERPROFILE%`／macOS `$HOME`）；不做 per-profile 记忆（YAGNI） | 实测：cwd 决定会话落进 `~/.claude/projects/<cwd 的 slug>`，且**首次进入新目录会弹 workspace trust**（交互式会话），属预期行为，需在 UI 说明一句 |
| **D-4** | Windows：`wt.exe -w new -d <dir> --title "<profile 名> · Claude Code" powershell.exe -NoExit -NoProfile -EncodedCommand <BASE64_UTF16LE>`；**用 `-EncodedCommand`** 绕开 wt 分号分隔与 PowerShell 引号剥壳两层坑 | 实测（本机 wt 1.24.11911.0 + powershell 5.1 + pwsh 7.6.6）：整链无引号故障，含空格路径亦通 |
| **D-5** | `wt.exe` 缺失 → **退化到 `CreateProcessW(powershell.exe …, CREATE_NEW_CONSOLE)`**（同命令、同环境），**并一次性提示**（不静默、不阻断） | 用户要的是"能跑"，不是"必须有 wt"；静默降级不可接受，硬失败也不友好 |
| **D-6** | `-NoExit` **默认开**（claude 退出后保留标签页/控制台，能看到回滚内容） | 交互式 TUI 退出后立刻关窗会丢信息；实测 wt 不带 `-NoExit` 时标签页自动消失 |
| **D-7** | macOS：写 `.command` 脚本（`cd <dir> && claude`）到 `appDataDir()/run-scripts/`，`chmod 0755`，再 `open <脚本>` → **跟随用户默认终端 app**（LaunchServices 决定） | `open -a Terminal` 强绑 Terminal.app、`osascript` 强绑 Terminal + 首次 TCC 弹窗风险；`.command` + `open` 才是"默认终端"。脚本**不含任何密钥**（见 D-8），落盘风险归零 |
| **D-8** | **密钥零落盘、零进命令行/进程环境**：不写任何含密钥的临时 settings 文件，不传 `--settings`，不用 `CLAUDE_CONFIG_DIR`（实测它是"整个配置家目录搬家"，会丢用户的 plugins/技能） | 与既有安全口径一致（`claude_service.h` 头注释：密钥只在 Keychain、明文不入库）；会话读 settings.json 的 `BASE_URL=127.0.0.1:<port>` + `AUTH_TOKEN=PROXY_MANAGED`，真实密钥由代理转发时注入 |
| **D-9** | 停代理 / 退出应用时若存在运行中会话：**只提示、不阻止**（文案说明"正在运行的 Claude Code 会话可能因托管键被撤而失败"） | `wt`/`open` 是分离式启动，DevDesk 拿不到会话句柄，无法精确判定；阻止会伤可用性 |

## 2. 现状事实（实测 + 源码核实）

| 事实 | 证据 |
|------|------|
| `claude --settings <file\|json>` 是**合并**（additional settings），不是替换 | 本机 `claude --help`（2.1.283）原文：`Path to a settings JSON file or a JSON string to load additional settings from` |
| ⚠️ **调研期实测、本次独立审计未能复现（需起真实会话）**：优先级 `--settings` 的 `env` **>** `~/.claude/settings.json` 的 `env` **>** 继承的进程环境变量 | 调研期本机 mock 探测：settings.json 覆盖进程 env（探测 E/A）、`--settings` 覆盖进程 env 与 settings.json（C/D/PS 端到端）；独立审计未能复现 |
| ⚠️ **调研期实测、本次独立审计未能复现（需起真实会话）**：`CLAUDE_CONFIG_DIR` = 配置家目录整体搬家（含 `~/.claude.json` 登录态、`projects/`、`plugins/`） | 官方文档原文 + 调研期实测（临时目录内新生成了 `.claude.json`/`projects/`/`sessions/`/`plugins/`/`backups/`）；独立审计未能复现 |
| ⚠️ **调研期实测、本次独立审计未能复现（需起真实会话）**：本机 wt 能把父进程环境块带给新标签页 | 调研期实测：`export KEY=… ; wt -d … powershell -Command "…Out-File…"` 回读 = 该值（**但 D-8 选择不利用它**）；独立审计未能复现 |
| `wt.exe` 参数语义：`wt [options] [command ;]`，不给命令默认 `new-tab`；`-w new` 强制新窗口；`;` 是命令分隔符 | MS Learn command-line-arguments（调研已取证）；bash 手敲需 `\;`，**DevDesk 走 `CreateProcessW` 不经 MSYS，不受此影响** |
| ⚠️ **调研期实测、本次独立审计未能复现（本机无 Mac）**：macOS `open <文件>` 用 LaunchServices 默认处理器打开；`open -a Terminal` 才是强绑 | `man open` 原文（调研期取证）；本机无 Mac，独立审计未能复现 |
| 代理转发**每请求取激活 profile** | `proxy_router.cpp` 的 `getActiveProfile()` + `loadApiKey()`（此前认证字段设计已核实为每请求） |
| ⚠️ **调研期实测、本次独立审计未能复现（需起真实会话）**：会话历史仍写 `~/.claude/projects/<cwd slug>/`，`--settings` 不搬历史 | 官方文档 + 调研结论；故 D-3 的 cwd 决定会话桶；独立审计未能复现 |
| 本仓**无任何进程/终端启动封装** | 全仓仅 3 处外呼：`file_open_win.cpp`（`ShellExecuteExW` 开目录）、`file_open_macos.cpp`（`std::system("/usr/bin/open …")`）、`file_open_posix.cpp`（`xdg-open`）→ 需新增平台文件，**不得塞进 file_open** |
| 平台适配纪律 | `DevDesk/CLAUDE.md` 核心约束 5：头文件平台无关接口 + `*_win.cpp`/`*_macos.cpp`/`*_posix.cpp` + CMake 选源；业务代码零 `#ifdef` |

> **证据等级说明**：上表标 ⚠️ 的 5 条 = 调研期实测、本次独立审计**未能复现**（需起真实会话 / 本机无 Mac），证据等级**低于**其余 CONFIRMED 项——**实施时不得当作既定事实引用**；§3 形态表 ②/③ 与 D-1 的理由引用了其中部分，同此口径。其余条目（`--settings` 合并语义、代理每请求取激活 profile、仓内 3 处外呼、平台纪律、CMake 三支、`appDataDir`、按钮组 5 键与 `actW`、`setActiveProfile`/`startProxy`/`isProxyRunning` 位置、`runProfile` 未占用、无选中 toast 同款、`syncCaptureActiveProfile`+`syncTakeOver` 触发）本次独立审计均判 **CONFIRMED**。

## 3. 形态选择（为什么是"代理托管式"）

| 形态 | 密钥位置 | 结论 |
|------|----------|------|
| ① 临时 settings 文件含密钥 + `--settings` | **明文落盘** | ❌ 破既有安全口径；且实测"接管态下它确实能覆盖 settings.json"——但代价不值 |
| ② 临时文件只放非机密键 + 密钥走子进程 env | — | ❌ **不可行**：密钥键会被 settings.json 的 `ANTHROPIC_AUTH_TOKEN=PROXY_MANAGED` 覆盖（实测 E/A） |
| ③ 全走子进程 env、不写文件 | 不落盘但**进进程环境**（`Win32_Process` 可读） | ❌ 接管态不可行（同上） |
| ④ **代理托管**：会话指向本地代理，密钥留 Keychain 由代理注入 | Keychain（零新增落盘） | ✅ **采用**（D-8）；跨平台唯一自洽 |

**D-1 的副作用必须明示**：把非激活 profile 设为激活会**改写全局 `~/.claude/settings.json` 的托管键**（所有 Claude Code 客户端跟随），并触发既有的反向同步（切 profile 前收编）。→ UI 上给一句说明；实现上直接复用 `ClaudeService::setActiveProfile`（唯一咽喉，勿新开写路径）。

**选中 == 激活时，D-1 的两种解读完全等价、零副作用**（不触发切换、不重写 settings.json）；只有"选中 ≠ 激活"时才有全局副作用。UI 文案应按此措辞，避免让用户以为每次点「运行」都会改全局。

## 4. 平台层接口（新增）

```cpp
// src/platform/terminal_launch.h（平台无关，零 #ifdef）
#pragma once
#include <map>
#include <string>
#include <vector>
#include "core/result.h"

namespace platform::terminal {

struct LaunchSpec {
    std::string title;                        // 窗口/标签标题（如 "官方 · Claude Code"）
    std::string workingDir;                   // 起始目录（D-3）
    std::string program;                      // 可执行名（本需求固定 "claude"）
    std::vector<std::string> args;            // 追加参数（本需求为空）
};

// 在终端里以 LaunchSpec 打开一个程序；成功 = 已成功派发（分离式，不等待退出）
core::Result<void> launch(const LaunchSpec& spec);

} // namespace platform::terminal
```

| 文件 | 职责 |
|------|------|
| `terminal_launch_common.cpp` | 跨平台共享的**纯函数**：argv 引号/转义（按 `CommandLineToArgvW` 规则）+ UTF-16LE→base64 编码。照 `file_open_common.cpp` 的先例。**base64 自实现**（几行，不用 `3rd/cpp-httplib` 的内部 `detail::base64_encode`），编码器放这里**以便单测** |
| `terminal_launch_win.cpp` | 拼 `wt.exe … powershell.exe -NoExit -NoProfile -EncodedCommand <BASE64_UTF16LE>`；wt 缺失（探测 `%LOCALAPPDATA%\Microsoft\WindowsApps\wt.exe`）→ 退化 `CreateProcessW(..., CREATE_NEW_CONSOLE)` 并由调用方提示（D-5）。**命令文本只在本文件内拼**，业务层只传语义参数 |
| `terminal_launch_macos.cpp` | 写 `.command`（正文 `cd <dir> && exec claude`）到 `appDataDir()/run-scripts/run-<profileId>-<时间戳>.command`，`chmod 0755`，`std::system("/usr/bin/open '<路径>'")`（单引号转义照 `file_open_macos.cpp` 既有做法）；启动时清扫该目录陈旧文件 |
| `terminal_launch_posix.cpp` | 明确返回 `NotImplemented`（Linux 非目标平台） |

CMake：追加进 `src/platform/CMakeLists.txt` 既有的 `if(WIN32)/elseif(APPLE)/else()` 三支（与该文件既有模式同款）；`terminal_launch_common.cpp` 跨平台，加在三支之外无条件编译。

### 4.1 Windows 命令构造（写死，供实现与单测逐字对照）

- **PowerShell 正文**（示意；正文里**不含任何密钥**）：`Set-Location -LiteralPath '<工作目录>'` + 换行 + 一行 `claude`（如将来要传参再加）。
- **编码链**：正文 → **UTF-16LE** → base64 → `powershell.exe -NoExit -NoProfile -EncodedCommand <BASE64>`。
- **`wt` 分支**：`wt.exe -w new -d "<dir>" --title "<标题>" …`；`.exe`/标题/dir 含空格或中文时按 `CommandLineToArgvW` 规则加引号——**整条参数向量由 `terminal_launch_common.cpp` 的共享纯函数生成**（便于单测）。
- **`wt` 缺失的退化分支**：`CreateProcessW(powershell.exe, …, lpCurrentDirectory=<工作目录>)`——退化路径没有 `wt -d`，**必须显式设 `lpCurrentDirectory`**（正文里的 `Set-Location` 是双保险）。

## 5. 交互与失败路径

| 场景 | 行为 |
|------|------|
| 点「运行」（代理未运行） | 先判 `isProxyRunning()`；未运行才启动，且**必须传参** `startProxy("127.0.0.1", configuredProxyPort())`——**不可用无参默认（默认端口 8888）**，否则设置页改过端口时会起错端口；代理已在跑时 `startProxy` 会因 server 已存在而硬失败。失败 → toast 报错并中止（不打开会话） |
| 目标 profile 非激活 | 先 `setActiveProfile(id)`；失败 → toast 并中止 |
| 目标 profile = 激活 | 跳过切换，直接开会话 |
| 无选中 profile | 与同区其它按钮同款：toast「请先选择 Profile」 |
| `wt` 缺失 | 退化传统控制台 + 一次性提示（D-5） |
| macOS 写 `.command` 失败 / `open` 失败 | toast 报错（含路径），不静默 |
| 已存在运行中会话时停代理/退出 | 提示文案（D-9），不阻止 |
| 会话运行期间用户又切了激活 profile | 该会话的**后续请求按新 profile 的密钥/上游走**（代理每请求取激活）：可能表现为模型不存在或跨供应商串账。处置待拍板（§11） |
| 用户自行设置了 `CLAUDE_CONFIG_DIR` | 接管写入只认默认 `~/.claude/settings.json`（既有已知限制），新会话读不到托管 env → 拿不到可用密钥；本功能**不为其做兼容**（见 `docs/KNOWN_LIMITATIONS.md` §10） |

> **顺序固定：代理 → `setActiveProfile` → 启动会话**。理由：`startProxy` 内部会 `syncTakeOver()`；随后 `syncCaptureActiveProfile` 读回的是它自己刚写入的受保护托管值（G2/G3 守卫跳过），顺序反了会把 live 值误收编进 profile。

## 6. 文件改动清单

| 文件 | 动作 |
|------|------|
| `src/platform/terminal_launch.h` | 新增（§4 接口） |
| `src/platform/terminal_launch_win.cpp` / `_macos.cpp` / `_posix.cpp` / `_common.cpp` | 新增三平台实现 + 跨平台纯函数（`_common.cpp`：argv 引号/转义 + UTF-16LE→base64，编码器自实现） |
| `src/platform/CMakeLists.txt` | 三支各加一行平台源文件 + `terminal_launch_common.cpp` 一行（跨平台，加在三支之外） |
| `tests/CMakeLists.txt` | **必须手工登记新测试文件**——该文件是**显式文件列表**（无 GLOB），漏登记则新测试不参与编译 |
| `src/module_claude_proxy/claude_service.h/.cpp` | 新增 `core::Result<void> runProfile(int64_t id, std::string* errHint)`（或等价）：确保代理运行 + 必要时 `setActiveProfile` + 拼 `LaunchSpec` + 调 `platform::terminal::launch`。**复用既有函数，不新开写路径**。并加值注入点 `void setTerminalLauncher(std::function<core::Result<void>(const platform::terminal::LaunchSpec&)>)`（**镜像既有 `setSettingsSyncPaths` / `setProfileProvider` 的值注入先例**，默认指向真实现） |
| `src/module_claude_proxy/claude_module.cpp` | 详情页操作按钮组加「运行」（`actW` 由 5 键改 6 键）+ `doRun()` + 副提示（D-1 副作用说明）+ `dataDirty_` 处理 |
| `tests/test_platform/test_terminal_launch.cpp` | 新增（**须登记进 `tests/CMakeLists.txt`**）：`terminal_launch_common.cpp` 纯函数——如 `buildWindowsLaunchArgs(const LaunchSpec&)` 返回参数向量，断言参数向量与 base64 正文（解码后）+ 三平台的失败路径（posix 返回 NotImplemented） |
| `tests/test_modules/test_claude_service.cpp` / `test_claude_ui.cpp` | service：`runProfile` 在"代理未运行/非激活/无选中"下的行为——**用 `setTerminalLauncher` 注入假实现**（断言调用参数，不真起进程）；UI：按钮存在、无选中时 toast。**`test_claude_ui.cpp` 用的是真实 `ClaudeService`（`setGlobal`），UI 用例必须在点击「运行」前注入假实现**，否则测试会真的启动 wt + claude |
| `CHANGELOG.md` | 条目（合并后回填 sha） |

## 7. 不做（YAGNI）

1. 不传密钥、不用 `--settings`、不用 `CLAUDE_CONFIG_DIR`（D-8）。
2. 不做"直连上游"形态（要另立项 + 需用户接受密钥短暂落盘）。
3. 不做 per-profile 记忆起始目录、不做目录选择器（D-3）。
4. 不改代理的 profile 路由（"用任意 profile 不切激活地跑"需改造代理，登记为后续项）。
5. 不做会话句柄跟踪（分离式启动，D-9 只提示）。
6. 不改反向同步 / 接管写入协议。

## 8. 测试策略

| 层 | 用例 |
|----|------|
| 纯函数 | Windows 命令拼装（`terminal_launch_common.cpp`）：`wt` 参数向量、`-EncodedCommand` 正文 **base64 解码后与期望 PowerShell 文本逐字相等（比对对象 = 解码后的正文）**；含空格/中文的 profile 名与目录不破坏拼接。macOS `.command` 正文：`cd` 用单引号包裹、路径含空格/单引号时的转义 |
| 平台失败路径 | `terminal_launch_posix` 返回 `NotImplemented`；macOS 写文件失败（目录不可写）→ 返回 fail 带路径 |
| service | 用 `setTerminalLauncher` 注入假 launch：① 代理未运行 → 先 `startProxy("127.0.0.1", configuredProxyPort())`，失败即中止 ② 非激活 → 先 `setActiveProfile` ③ 激活 → 不调用 `setActiveProfile` ④ 调用参数（title/cwd/program）正确 |
| 无头 UI | `claude.act.run` 存在；无选中 → toast「请先选择 Profile」；点击 → 假 launch 被调用一次 |
| 人工点验（需真机） | Windows：点「运行」→ wt 新窗口出现 PowerShell 标签、标题正确、`claude` 起来且能用（走代理）；wt 缺失场景（临时改名）→ 传统控制台 + 提示；macOS：点「运行」→ 默认终端打开 `.command` 并起 claude |

## 9. 拆任务（每项 ≤0.5 天；编译/测试子任务一律 worktree 隔离）

| 里程碑 | 内容 | 依赖 | 验收 |
|--------|------|------|------|
| **M1** 平台层 | `terminal_launch.h` + 三实现 + CMake + 纯函数用例 | — | 命令拼装用例全绿；posix 返回 NotImplemented |
| **M2** service | `runProfile` + 可注入 launch + service 用例 | M1 | 四条 service 用例全绿 |
| **M3** UI + 收尾 | 按钮 + `doRun` + 副提示 + 无头用例 + CHANGELOG | M2 | 无头用例全绿；人工点验步骤写入文档 |

## 10. 已接受的边界

1. 「运行」会**顺带把该 profile 设为激活**（D-1），进而改写全局 settings.json 托管键 —— UI 明示。反向同步的持久副作用要说全：**选中 ≠ 激活时**点「运行」会先把 settings.json 的 env 收编进**原激活 profile**——出现键覆盖、缺席的非守卫键清空、**token 缺席时会清空该 profile 的 Keychain 条目与三列 key 元数据**；若 settings.json 读不了/解析失败，整个「运行」会中止（沿用既有反向同步语义）。
2. 起始目录默认主目录；新目录首次会弹 workspace trust（Claude Code 行为，非本功能 bug）。
3. 运行中会话不受 DevDesk 管理：停代理/退出后它可能失败（D-9，只提示）。
4. macOS 侧实现**本机无法编译/运行验证**（无 Mac）：按保守写法 + 静态评审交付，实机验证登记为待办。
5. Linux/其它平台：`NotImplemented`（非目标平台）。
6. 会话运行期间用户又切了激活 profile：该会话**后续请求按新 profile 走**（代理每请求取激活），可能模型不存在或跨供应商串账（处置见 §11）。
7. 用户自行设置 `CLAUDE_CONFIG_DIR`：接管只写默认 `~/.claude/settings.json`（既有已知限制），新会话拿不到托管 env → 无可用密钥；本功能不做兼容（见 `docs/KNOWN_LIMITATIONS.md` §10）。

## 11. 待用户复核（本会话自主拍板的默认值）

| # | 决策 | 备选 |
|---|------|------|
| D-1 | 运行 = 代理托管 + 顺带切激活 | 改造代理支持"按会话选 profile"（立项）；或允许直连（密钥短暂落盘） |
| D-2 | 按钮位置 = 详情页按钮组第 6 键 | 列表卡片右键菜单 |
| D-3 | 起始目录 = 用户主目录 | per-profile 记忆 / 目录选择器 |
| D-4/D-5 | wt 优先、缺失退化传统控制台 | 硬失败提示装 Windows Terminal |
| D-6 | `-NoExit` 默认开 | 默认关（退出即关窗） |
| D-7 | macOS = `.command` + `open`（跟随默认终端） | `osascript`（不落盘但强绑 Terminal + TCC 风险） |
| D-9 | 停代理时只提示 | 阻止 / 不处理 |
| 新增 | 运行期间切激活的行为（老会话按新 profile 走） | 接受 / 需要提示 / 需要阻止 |
| 新增 | 选中 ≠ 激活 时是否要**二次确认**再切（因为会重写全局 settings.json） | 二次确认 / 直接切（现状） |
