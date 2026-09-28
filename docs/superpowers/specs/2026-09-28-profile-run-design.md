# Profile「运行」—— 用该 profile 打开 Claude Code（设计）

日期：2026-09-28 ｜ 状态：**设计（未实施）· V3（按用户拍板：临时 settings + `--settings` 直连，零全局改动）** ｜ 关联：`2026-09-27-profile-auth-field-design.md`、`2026-09-27-profile-reverse-sync-and-copy-design.md`（反向同步——本设计**不改**其行为）

> **本文档中 D-1 ~ D-9 为本会话在用户授权自主决策期间所拍板的默认值**，专家复核/用户复核后若有推翻，按 §11 清单逐条改。

## 1. 需求与决策

| # | 需求（用户原话） |
|---|------------------|
| **R1** | 「profile 新增运行功能，claude 的启动参数可以指定配置文件的，点击运行后使用当前的配置打开 claude code」 |
| **R2** | 「这个运行，基于 windows 终端 + powershell 运行」 |
| **R3** | 「如果是 macOS 则使用默认的终端 app」 |

| # | 决策（自主拍板，待复核） | 理由 |
|---|--------------------------|------|
| **D-1** | 「运行」= **生成一份临时 settings JSON**（含该 profile 的完整 env：BASE_URL / 密钥 / 模型槽位 / 额外 env，规格见 §4.2）→ 以 **`claude --settings <该文件>`** 启动会话；**不启代理、不切激活 profile、不读也不改 `~/.claude/settings.json`**，**零全局副作用** | 调研实测的优先级：`--settings` 的 env **>** 用户 settings.json 的 env **>** 进程 env，故临时文件能稳定压过全局配置；且这是唯一"跑任意 profile 且不动全局"的形态（详见 §3 / §4.2） |
| **D-2** | 「运行」按钮挂在 **profile 详情页操作按钮组**（与「启用/测试连通/编辑/删除/复制」同区），作用于**当前选中的 profile** | 与既有交互同源、零新基建；选中即"想跑的那一份" |
| **D-3** | 起始目录默认 **用户主目录**（Windows `%USERPROFILE%`／macOS `$HOME`）；不做 per-profile 记忆（YAGNI） | 实测：cwd 决定会话落进 `~/.claude/projects/<cwd 的 slug>`，且**首次进入新目录会弹 workspace trust**（交互式会话），属预期行为，需在 UI 说明一句 |
| **D-4** | Windows：`wt.exe -w new -d <dir> --title "<profile 名> · Claude Code" powershell.exe -NoExit -NoProfile -EncodedCommand <BASE64_UTF16LE>`；**用 `-EncodedCommand`** 绕开 wt 分号分隔与 PowerShell 引号剥壳两层坑 | 实测（本机 wt 1.24.11911.0 + powershell 5.1 + pwsh 7.6.6）：整链无引号故障，含空格路径亦通 |
| **D-5** | `wt.exe` 缺失 → **退化到 `CreateProcessW(powershell.exe …, CREATE_NEW_CONSOLE)`**（同命令、同环境），**并一次性提示**（不静默、不阻断） | 用户要的是"能跑"，不是"必须有 wt"；静默降级不可接受，硬失败也不友好 |
| **D-6** | `-NoExit` **默认开**（claude 退出后保留标签页/控制台，能看到回滚内容） | 交互式 TUI 退出后立刻关窗会丢信息；实测 wt 不带 `-NoExit` 时标签页自动消失 |
| **D-7** | macOS：写 `.command` 脚本（`cd <dir> && claude`）到 `appDataDir()/run-scripts/`，`chmod 0755`，再 `open <脚本>` → **跟随用户默认终端 app**（LaunchServices 决定） | `open -a Terminal` 强绑 Terminal.app、`osascript` 强绑 Terminal + 首次 TCC 弹窗风险；`.command` + `open` 才是"默认终端"。脚本**不含任何密钥**（见 D-8），落盘风险归零 |
| **D-8** | **密钥零落盘、零进命令行/进程环境**：不写任何含密钥的临时 settings 文件，不传 `--settings`，不用 `CLAUDE_CONFIG_DIR`（实测它是"整个配置家目录搬家"，会丢用户的 plugins/技能） | 与既有安全口径一致（`claude_service.h` 头注释：密钥只在 Keychain、明文不入库）；会话读 settings.json 的 `BASE_URL=127.0.0.1:<port>` + `AUTH_TOKEN=PROXY_MANAGED`，真实密钥由代理转发时注入 |
| **D-9** | 停代理 / 退出应用时若存在运行中会话：**只提示、不阻止**（文案说明"正在运行的 Claude Code 会话可能因托管键被撤而失败"） | `wt`/`open` 是分离式启动，DevDesk 拿不到会话句柄，无法精确判定；阻止会伤可用性 |

> **用户原话（决定性，D-1 依此拍板，V3 起生效）**：「我不是说了 claude 的启动命令支持设置 settings 吗？你生成一个临时的 setting 然后作为启动参数啊 不修改当前的任何东西」

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

## 3. 形态选择（为什么是"临时 settings + `--settings` 直连"）

| 形态 | 密钥位置 | 结论 |
|------|----------|------|
| ① **临时 settings 文件含密钥 + `--settings`** | 会话期间明文落盘于 `appDataDir()/run-settings/`（用户级 ACL 守护 + 会话退出自删 + 启动时清扫，见 §5） | ✅ **采用**（D-1，V3）。实测优先级 `--settings` 的 env **>** 用户 settings.json 的 env **>** 进程 env，能稳定压过全局配置；且是唯一"跑任意 profile 且**不动全局**"的形态 |
| ② 临时文件只放非机密键 + 密钥走子进程 env | — | ❌ **不可行**：密钥键会被 settings.json 的 `ANTHROPIC_AUTH_TOKEN=PROXY_MANAGED` 覆盖（实测 E/A） |
| ③ 全走子进程 env、不写文件 | 不落盘但**进进程环境**（`Win32_Process` 可读） | ❌ 接管态不可行（同上） |
| ④ **代理托管**：会话指向本地代理，密钥留 Keychain 由代理注入 | Keychain（零新增落盘） | ❌ **已否决**（用户否决：该形态必然"切激活 / 改写全局 settings.json"，用户要求"不修改当前的任何东西"）；登记为后续可选（§11） |

**代价必须如实登记（V3——用户已知并接受的三条取舍）**：

1. **密钥短暂明文落盘**：会话期间该临时文件内含真实密钥，目录 `appDataDir()/run-settings/`——靠**用户级 ACL + 会话内自删 + 启动清扫**兜底；Windows 无 0600 等价物，同用户进程可读该文件。
2. **该会话不走代理**：→ **无用量统计**（`claude_call_logs`）、**无方言处理**（`upstream_dialect`）、**无模型名归一**（`[1M]` 剥除等），直连上游原样行为。
3. **密钥自备**：不再由代理在转发时注入——本功能把 profile 的 Keychain 密钥读入临时文件（见 §4.2 读取依赖）。

（D-8 原"密钥零落盘、不传 `--settings`"的口径由此被 D-1 **覆盖**；`CLAUDE_CONFIG_DIR` 仍不可用——实测它是整个配置家目录搬家，会丢用户的 plugins/技能。）

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
    std::vector<std::string> args;            // 追加参数（本需求固定：--settings + 临时文件路径，§4.2）
};

// 在终端里以 LaunchSpec 打开一个程序；成功 = 已成功派发（分离式，不等待退出）
core::Result<void> launch(const LaunchSpec& spec);

} // namespace platform::terminal
```

| 文件 | 职责 |
|------|------|
| `terminal_launch_common.cpp` | 跨平台共享的**纯函数**：argv 引号/转义（按 `CommandLineToArgvW` 规则）+ UTF-16LE→base64 编码。照 `file_open_common.cpp` 的先例。**base64 自实现**（几行，不用 `3rd/cpp-httplib` 的内部 `detail::base64_encode`），编码器放这里**以便单测** |
| `terminal_launch_win.cpp` | 拼 `wt.exe … powershell.exe -NoExit -NoProfile -EncodedCommand <BASE64_UTF16LE>`；wt 缺失（探测 `%LOCALAPPDATA%\Microsoft\WindowsApps\wt.exe`）→ 退化 `CreateProcessW(..., CREATE_NEW_CONSOLE)` 并由调用方提示（D-5）。**命令文本只在本文件内拼**，业务层只传语义参数；正文 = `Set-Location` + `claude --settings '<临时文件>'` + 退出后 `Remove-Item`（会话结束自删，§5） |
| `terminal_launch_macos.cpp` | 写 `.command`（正文 `cd <dir> && exec claude --settings '<临时文件>'`，脚本退出后 `rm -f '<临时文件>'` 自删，§5）到 `appDataDir()/run-scripts/run-<profileId>-<时间戳>.command`，`chmod 0755`，`std::system("/usr/bin/open '<路径>'")`（单引号转义照 `file_open_macos.cpp` 既有做法）；启动时清扫该目录陈旧文件 |
| `terminal_launch_posix.cpp` | 明确返回 `NotImplemented`（Linux 非目标平台） |

CMake：追加进 `src/platform/CMakeLists.txt` 既有的 `if(WIN32)/elseif(APPLE)/else()` 三支（与该文件既有模式同款）；`terminal_launch_common.cpp` 跨平台，加在三支之外无条件编译。

### 4.1 Windows 命令构造（写死，供实现与单测逐字对照）

- **PowerShell 正文**（示意；正文里**不含任何密钥**——密钥在临时 settings 文件里，§4.2）：`Set-Location -LiteralPath '<工作目录>'` + 换行 + 一行 `claude --settings '<临时文件路径>'` + 一行 `Remove-Item -LiteralPath '<临时文件路径>'`（`claude` 退出后自删，§5）。
- **编码链**：正文 → **UTF-16LE** → base64 → `powershell.exe -NoExit -NoProfile -EncodedCommand <BASE64>`。
- **`wt` 分支**：`wt.exe -w new -d "<dir>" --title "<标题>" …`；`.exe`/标题/dir 含空格或中文时按 `CommandLineToArgvW` 规则加引号——**整条参数向量由 `terminal_launch_common.cpp` 的共享纯函数生成**（便于单测）。
- **`wt` 缺失的退化分支**：`CreateProcessW(powershell.exe, …, lpCurrentDirectory=<工作目录>)`——退化路径没有 `wt -d`，**必须显式设 `lpCurrentDirectory`**（正文里的 `Set-Location` 是双保险）。

### 4.2 临时 settings 的内容与生成（V3 新增）

- **路径**：`core::paths::appDataDir() / "run-settings" / ("run-" + profileId + "-" + 时间戳 + ".json")`（时间戳 = 秒级 unix，照 `nowUnix` 既有用法）。目录不存在先建；用 **`core::atomicWriteFile`** 落盘（既有先例：keychain_win / settings 写路径）。
- **内容**：一个合法 settings JSON，只含 `env` 子对象：`{"env": { ... }}`。键按 profile 数据装配，**复用既有纯函数、不新写推导逻辑**：
  - `ANTHROPIC_BASE_URL` = profile.baseUrl **去尾斜杠**（既有 `stripTrailingSlash` 语义，照 `updateProfile` 写侧口径）。
  - **密钥键名按 profile 的「认证字段」三态决定**（复用 `AuthField` / `authFieldFromText` / `authFieldToText`；密钥值从 Keychain 读出，既有 `loadApiKey` 同类读取）：
    - `bearer` → 只写 `ANTHROPIC_AUTH_TOKEN`；
    - `api_key` → 只写 `ANTHROPIC_API_KEY`；
    - `both`（**默认**）→ **只写 `ANTHROPIC_AUTH_TOKEN`**。
    - 说明：**直连路径只能选一个键**——直连请求只携带其一生效；"双发"是代理路径的特性（代理把密钥经两处转发）。若上游网关要求 `x-api-key`（如某些自建网关），把该 profile 的认证字段设为 `api_key` 即可。
  - **模型槽位与 `*_NAME`**：遍历 `modelSlotTable()`，照 profile 的 `models_config`（`effectiveModelsConfig` 语义）——**空槽位跳过**；非空槽位写 `m.envKey` = model；`m.nameKey`（非 nullptr）写 name（name 空则照 `buildProxyEnv` 同款规则派生显示名）。`subagent` 槽位对应 `CLAUDE_CODE_SUBAGENT_MODEL`（表行无 `*_NAME` 变体，照表派生即可）。
  - **extra_env 逐键并入**：`parseExtraEnv(profile.extraEnvJson)` + `applyEnvAliases`（照 `buildProxyEnv` 规则 5 同款：extra_env 先入表，**托管键——BASE_URL 与密钥键——由本函数最后写入，压过 extra_env**）。
- **落点建议**：新增**纯函数**（如 `provider_config.h/.cpp` 的 `buildDirectEnv(...)` 或同层等价物；输入 = baseUrl / models_config JSON / extra_env JSON / 认证字段 / Keychain 密钥值，输出 = `std::map<std::string, std::string>` env）——**具体命名与落点由实施线定**，本 spec 只写职责与输入输出；纯函数便于直接单测（§8）。
- **读取依赖**：本功能需读到 profile 的 Keychain 密钥（直连形态下密钥必须由本功能带进临时文件）；**读 Keychain 失败 → toast 中止（§5），不落盘、不开窗**。

## 5. 交互与失败路径

| 场景 | 行为 |
|------|------|
| 读 profile 失败（id 无效 / 不存在） | toast 报错并中止（不生成文件、不开会话） |
| 读 Keychain 密钥失败 / 条目不存在 | toast 报错并中止（**不落盘、不开窗**） |
| 写临时 settings 文件失败（目录不可建 / 磁盘错误） | toast 报错并中止（不启动会话） |
| 无选中 profile | 与同区其它按钮同款：toast「请先选择 Profile」 |
| `wt` 缺失 | 退化传统控制台（`CreateProcessW`，全路径 `powershell.exe`，D-5）+ 一次性提示（不静默、不阻断） |
| macOS 写 `.command` 失败 / `open` 失败 | toast 报错（含路径），不静默 |
| **会话启动成功后** | 临时文件由会话正文**自删**（PowerShell 正文与 macOS `.command` 都在 `claude` 退出后删自己的文件）：另有**启动时清扫** `run-settings/` 里的陈旧文件（阈值：**例如 24 小时**——兜底上次崩溃 / 强制结束留下的孤儿） |
| 已存在运行中会话时停代理/退出 | 提示文案（D-9），不阻止；直连会话不依赖代理，互不影响 |
| 会话运行期间用户又切了激活 profile | **无影响**：会话启动时已取得完整 env（临时文件已固化），后续不再读全局、不触发反向同步——与代理托管形态（老会话按新 profile 走）完全分离 |
| 用户自行设置了 `CLAUDE_CONFIG_DIR` | 本功能不经 `~/.claude/settings.json`：env 全走 `--settings` 临时文件，**不受 `CLAUDE_CONFIG_DIR` 影响**（既有已知限制只约束接管写，见 `docs/KNOWN_LIMITATIONS.md` §10） |

> **顺序固定：读 profile → 读 Keychain 密钥 → 生成临时文件并落盘 → 启动会话**。任何一步失败即 toast 中止，不把失败留到"开了窗之后"。

## 6. 文件改动清单

| 文件 | 动作 |
|------|------|
| `src/platform/terminal_launch.h` | 新增（§4 接口） |
| `src/platform/terminal_launch_win.cpp` / `_macos.cpp` / `_posix.cpp` / `_common.cpp` | 新增三平台实现 + 跨平台纯函数（`_common.cpp`：argv 引号/转义 + UTF-16LE→base64，编码器自实现） |
| `src/platform/CMakeLists.txt` | 三支各加一行平台源文件 + `terminal_launch_common.cpp` 一行（跨平台，加在三支之外） |
| `tests/CMakeLists.txt` | **必须手工登记新测试文件**——该文件是**显式文件列表**（无 GLOB），漏登记则新测试不参与编译 |
| `src/module_claude_proxy/claude_service.h/.cpp` | 改造既有 `core::Result<void> runProfile(int64_t id, std::string* errHint)`：**删除对 `isProxyRunning` / `startProxy` / `setActiveProfile` 的全部依赖**（这几个不再调用，也不触发反向同步）——改为「读 profile（`getProfile`）→ 读 Keychain 密钥（既有 `loadApiKey` 同类）→ 经新纯函数生成临时 settings 文本（§4.2）→ 目录建好后 `core::atomicWriteFile` 落盘 → 拼 `LaunchSpec`（`args` 含 `--settings <路径>`）→ `platform::terminal::launch`」。值注入点 `void setTerminalLauncher(...)`（镜像既有 `setSettingsSyncPaths` / `setProfileProvider` 的值注入先例，默认指向真实现）**保留**。另新增：临时 settings 的**生成纯函数**（§4.2；落点可由实施线定在 `provider_config` 层）与**启动清扫** `run-settings/` 陈旧文件的助手（挂应用启动序列，阈值 24 小时，§5）；具体命名/落点由实施线定，职责见 §4.2/§5 |
| `src/module_claude_proxy/claude_module.cpp` | 详情页操作按钮组加「运行」（`actW` 由 5 键改 6 键）+ `doRun()` + 副提示（D-1 副作用说明）+ `dataDirty_` 处理 |
| `tests/test_platform/test_terminal_launch.cpp` | 新增（**须登记进 `tests/CMakeLists.txt`**）：`terminal_launch_common.cpp` 纯函数——如 `buildWindowsLaunchArgs(const LaunchSpec&)` 返回参数向量，断言参数向量与 base64 正文（解码后）+ 三平台的失败路径（posix 返回 NotImplemented） |
| `tests/test_modules/test_claude_service.cpp` / `test_claude_ui.cpp` | service：`runProfile` 的**零全局回归闸**（§8：settings.json 未写、`is_active` 未变、未调 `startProxy`/`setActiveProfile`）+ 临时文件生成与路径断言——**用 `setTerminalLauncher` 注入假实现**（断言调用参数含 `--settings <路径>`，不真起进程）；UI：按钮存在、无选中时 toast。**`test_claude_ui.cpp` 用的是真实 `ClaudeService`（`setGlobal`），UI 用例必须在点击「运行」前注入假实现**，否则测试会真的启动 wt + claude |
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
| 纯函数 | Windows 命令拼装（`terminal_launch_common.cpp`）：`wt` 参数向量、`-EncodedCommand` 正文 **base64 解码后与期望 PowerShell 文本逐字相等（比对对象 = 解码后的正文）**；含空格/中文的 profile 名与目录不破坏拼接；正文含 `claude --settings '<路径>'` 与退出自删段。macOS `.command` 正文：`cd` 用单引号包裹、路径含空格/单引号时的转义 |
| 纯函数（V3 新增） | `buildDirectEnv`（或等价生成纯函数，§4.2）：① **三态密钥键名正确**——`bearer` → 只有 `ANTHROPIC_AUTH_TOKEN`；`api_key` → 只有 `ANTHROPIC_API_KEY`；`both`（默认）→ 只有 `ANTHROPIC_AUTH_TOKEN`；② `ANTHROPIC_BASE_URL` **去尾斜杠**；③ 槽位（空槽跳过、`*_NAME` 派生显示名）与 extra_env（别名归一）**并入**；④ **托管键压过 extra_env**（同名冲突时 BASE_URL / 密钥键最后写入） |
| 平台失败路径 | `terminal_launch_posix` 返回 `NotImplemented`；macOS 写文件失败（目录不可写）→ 返回 fail 带路径 |
| service（V3 零全局回归闸） | 用 `setTerminalLauncher` 注入假 launch 跑 `runProfile`，断言：① **调用前后 `~/.claude/settings.json` 未被写**（内容/mtime 不变）、`is_active` 未变、`isProxyRunning()` 未变（未调 `startProxy` / `setActiveProfile`）；② 临时文件**路径在 `appDataDir()/run-settings/` 下**、内容（`{"env":…}`）与 `--settings` 参数所指向的**同一路径**；③ 模拟「读 profile 失败 / 读密钥失败 / 写文件失败」→ toast 中止、不开窗 |
| 无头 UI | `claude.act.run` 存在；无选中 → toast「请先选择 Profile」；点击 → 假 launch 被调用一次且 **argv 含 `--settings <路径>`**（断言参数，不真起进程） |
| 人工点验（需真机） | Windows：点「运行」→ wt 新窗口出现 PowerShell 标签、标题正确、`claude` 起来且能用（**直连上游**，密钥来自临时文件）；会话内 `~/.claude/settings.json` **未被修改**；会话结束后临时文件消失（自删）；wt 缺失场景（临时改名）→ 传统控制台 + 提示；macOS：点「运行」→ 默认终端打开 `.command` 并起 claude |

## 9. 拆任务（每项 ≤0.5 天；编译/测试子任务一律 worktree 隔离）

| 里程碑 | 内容 | 依赖 | 验收 |
|--------|------|------|------|
| **M1** 平台层 | `terminal_launch.h` + 三实现 + CMake + 纯函数用例 | — | 命令拼装用例全绿；posix 返回 NotImplemented |
| **M2** service | `runProfile` + 可注入 launch + service 用例 | M1 | 四条 service 用例全绿 |
| **M3** UI + 收尾 | 按钮 + `doRun` + 副提示 + 无头用例 + CHANGELOG | M2 | 无头用例全绿；人工点验步骤写入文档 |

## 10. 已接受的边界

1. 起始目录默认主目录；新目录首次会弹 workspace trust（Claude Code 行为，非本功能 bug）。
2. 运行中会话不受 DevDesk 管理：停代理 / 退出后它可能失败（D-9，只提示）。
3. macOS 侧实现**本机无法编译/运行验证**（无 Mac）：按保守写法 + 静态评审交付，实机验证登记为待办。
4. Linux/其它平台：`NotImplemented`（非目标平台）。
5. 会话运行期间用户又切了激活 profile：对本次**直连会话无影响**（env 已在启动时固化，不触发反向同步）；旧"代理托管"形态下老会话会跟随新 profile，本功能已不用该形态。
6. 用户自行设置 `CLAUDE_CONFIG_DIR`：接管只写默认 `~/.claude/settings.json`（既有已知限制），新会话拿不到托管 env → 无可用密钥；本功能不做兼容（见 `docs/KNOWN_LIMITATIONS.md` §10）。
7. 本功能的三条代价已如实登记并**为用户已知且接受**（§3）：① 密钥以明文存在于临时文件（会话期间，目录 `appDataDir()/run-settings/`；靠用户级 ACL + 会话内自删 + 启动清扫兜底，Windows 无 0600 等价物）；② 直连会话**不走代理** → 无用量统计（`claude_call_logs`）、无方言处理（`upstream_dialect`）、无模型名归一；③ 密钥自备（本功能把 Keychain 密钥读入临时文件，不再由代理注入）。
8. 停在**旧版形态**（代理托管）或其它途径启动的会话**不受本功能管理**：自删与启动清扫只作用于 `appDataDir()/run-settings/` 下本功能创建的文件。

## 11. 待用户复核（本会话自主拍板的默认值）

| # | 决策 | 备选 |
|---|------|------|
| D-1 | 运行 = 临时 settings + `--settings` 直连、零全局改动 —— **已由用户拍板（V3，原话见 §1）**，不再待复核 | 已否决：代理托管（用户否决：会切激活 / 改全局 settings.json） |
| D-2 | 按钮位置 = 详情页按钮组第 6 键 | 列表卡片右键菜单 |
| D-3 | 起始目录 = 用户主目录 | per-profile 记忆 / 目录选择器 |
| D-4/D-5 | wt 优先、缺失退化传统控制台 | 硬失败提示装 Windows Terminal |
| D-6 | `-NoExit` 默认开 | 默认关（退出即关窗） |
| D-7 | macOS = `.command` + `open`（跟随默认终端） | `osascript`（不落盘但强绑 Terminal + TCC 风险） |
| D-9 | 停代理时只提示 | 阻止 / 不处理 |
| 新增 | 运行期间切激活的行为（老会话按新 profile 走） | 接受 / 需要提示 / 需要阻止 |
| 新增 | 选中 ≠ 激活 时是否要**二次确认**再切（因为会重写全局 settings.json） | 二次确认 / 直接切（现状） |
| 后续可选 | 是否**另开一条线**做「代理按会话选 profile」（保留代理路径的形态：密钥零落盘 / 用量统计 / 方言处理；代价是代理需改造，且仍不动激活态） | **本次不做**（D-1 已由用户拍板直连）；若直连的三条代价（§3）在真机使用中不可接受，再立项 |
