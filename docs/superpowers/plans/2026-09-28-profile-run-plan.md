# Profile「运行」实施计划（用该 Profile 打开 Claude Code）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Claude Profile 详情页加第 6 键「运行」——点一下即：确保本地代理在跑（端口用 `configuredProxyPort()`）→（若该 profile 非激活）先把它设为激活 → 在新终端窗口里用该 profile 的托管配置打开一个 `claude` 会话（Windows: Windows Terminal 优先、缺失退化传统控制台；macOS: 写 `.command` 脚本走 LaunchServices 默认终端）。

**Architecture:** 新增平台层 `platform::terminal`（头文件平台无关 + `*_win/_macos/_posix.cpp` 实现 + `_common.cpp` 跨平台纯函数，照 `platform::file_open` 既有四文件模式）；命令文本拼装**全部**落在 `_common.cpp` 的纯函数里（可单测），平台实现只做「探测 + 系统调用 + 句柄清理」；`ClaudeService::runProfile(id, hint)` 复用既有 `isProxyRunning` / `startProxy` / `setActiveProfile`（不新开写路径），终端启动经 `setTerminalLauncher` 值注入（默认指向真实现，测试注入假实现）；UI 只加按钮 + `doRun()` + 一行副提示。**密钥零落盘、零进命令行/进程环境**（D-8：会话走本地代理，真实密钥留 Keychain 由代理转发时注入）。

**Tech Stack:** C++20 / CMake + Ninja + MSYS2 MinGW64 + ccache（`scripts/build-test-run.sh`）/ Win32 `CreateProcessW` / 自制迷你 doctest（`REGISTER_TEST` + `DOCTEST_CHECK*`，**不是**真 doctest）。

**Spec:** `docs/superpowers/specs/2026-09-28-profile-run-design.md`（**V2**，commit `98041a1`，177 行）—— plan 与 spec 一起读；两者冲突时**以 spec 为准**并回报（本计划对 spec 的两处刻意补充见末节「与 spec 的偏离」）。

## Global Constraints

- **执行环境**：本项目规定「涉及编译/测试的子任务必须 worktree 隔离开发，主工作树禁跑构建」；worktree 内原子 commit（**显式 pathspec，禁 `git add -A`**）并报 sha；合并由主会话串行执行、合并后立即 `git worktree remove` + `prune`。
- **命令**：构建 + 定向测试一律 `bash scripts/build-test-run.sh --no-run --filter <子串>`（脚本自带工具链 PATH 与 ccache；worktree 首建自动 configure）。**禁全量重建、禁全量套件**。本计划用到的过滤串：`terminal_launch` / `claude_service` / `claude_ui`。
- 测试二进制支持子串过滤：`./build/tests/devdesk_tests.exe <子串>`。`--filter` 后**不跟中文**（Git Bash 传参编码问题），故过滤串一律 ASCII。
- **测试框架是自制迷你 doctest**：`REGISTER_TEST(name, "描述", { ... return true; })` 静态自注册 + `DOCTEST_CHECK` / `DOCTEST_CHECK_EQ` / `DOCTEST_REQUIRE*`（**fail-fast**）。`REGISTER_TEST` 宏实参**不能有裸逗号**（模板实参、`{a, b}` 初始化列表都算）→ 用类型别名或逐项 `push_back` 构造。grep `TEST_CASE` 找不到用例是正常的。
- **不抛 C++ 异常**：错误一律 `core::Result<T>`（`DevDesk/src/core/result.h`）；错误码取 `ValidationError` / `Internal` / `NotImplemented`。`-Wall -Wextra -Werror` 全开：未用形参写 `(void)x` 或不具名。
- **模块隔离**：`module_*` 之间不可互依赖；`core` 不依赖 `platform`；`platform` ✅ 依赖 `core`（macOS 侧用 `core::paths::appDataDir()`）。
- **平台纪律**：平台无关头零 `#ifdef`；实现按 `*_win.cpp` / `*_macos.cpp` / `*_posix.cpp` 拆，CMake 按平台选源；**业务代码零平台宏**（`src/` 下 `#ifdef _WIN32` 只允许出现在 `src/platform/*_win.cpp` 与测试文件的守卫里）。
- 代码注释与 UI 文案**一律中文**；行尾 LF。
- 范围纪律：只做 **spec §6 清单** + 本计划明示的两处接口补充；不做顺手重构 / 顺手修 bug / 清理死代码（spec §7 的「不做」逐条守）。
- commit 粒度：每个 Task 一次原子 commit，消息用本计划的给定文案。

## 文件结构（改动落点一览）

| 文件 | 职责 | 动作 |
|------|------|------|
| `src/platform/terminal_launch.h` | 平台无关接口（`LaunchSpec` + `launch()` + 两个平台查询） | **新增** |
| `src/platform/terminal_launch_common.h` | 跨平台纯函数声明（命令拼装 + 编码）——**见「与 spec 的偏离」①** | **新增** |
| `src/platform/terminal_launch_common.cpp` | 纯函数实现（argv 引号 / UTF-8→UTF-16LE / base64 自实现 / 正文与路径拼装） | **新增** |
| `src/platform/terminal_launch_win.cpp` | Windows：wt 探测 + `CreateProcessW`（wt 分支 / 传统控制台退化分支） | **新增** |
| `src/platform/terminal_launch_macos.cpp` | macOS：`.command` 脚本 + `chmod 0755` + `open` + 陈旧清扫 | **新增** |
| `src/platform/terminal_launch_posix.cpp` | 其他平台：`NotImplemented` | **新增** |
| `src/platform/CMakeLists.txt` | 三支各加一行平台源 + `_common.cpp`（三支之外无条件编译） | 修改 |
| `tests/CMakeLists.txt` | **必须手工登记** `test_platform/test_terminal_launch.cpp`（该文件是显式列表、无 GLOB） | 修改 |
| `src/module_claude_proxy/claude_service.h` / `.cpp` | `runProfile` + `setTerminalLauncher` 值注入 | 修改 |
| `src/module_claude_proxy/claude_module.cpp` | 第 6 键「运行」+ `doRun()` + 副提示（`actW` 由 5 改 6） | 修改 |
| `tests/test_platform/test_terminal_launch.cpp` | 命令拼装纯函数 + 引号回读 + 失败路径用例 | **新增（须登记）** |
| `tests/test_modules/test_claude_service.cpp` | `runProfile` 四条（未运行/非激活/已激活/参数）+ 两条中止 | 修改（已在列） |
| `tests/test_modules/test_claude_ui.cpp` | `claude.act.run` 存在 / 无选中 toast / 假 launch 调用一次 | 修改（已在列） |
| `CHANGELOG.md` | 条目（代码 commit 后回填 sha） | 修改 |

## 执行方式（本计划的批处理口径）

三个 Task **同一条实施线、一个 worktree 内串行**：M2 依赖 M1 的符号（`platform::terminal::*`），M3 依赖 M2 的 `runProfile` / `setTerminalLauncher`，且三者的编译与用例彼此咬合（引用点已全部查清、无需设计判断）→ 按 `DevDesk/CLAUDE.md`「机械任务合并…粒度看复杂度不看文件数」，**每个 Task 一次原子 commit，末尾派一次独立复核**（对照 spec §6 清单逐行核 diff + 复跑三条 `--filter` + 核「未验证项」清单是否照实登记）。

---

## Task 1（M1）: 平台层 `platform::terminal`

**Files:**
- Add: `src/platform/terminal_launch.h`
- Add: `src/platform/terminal_launch_common.h`
- Add: `src/platform/terminal_launch_common.cpp`
- Add: `src/platform/terminal_launch_win.cpp` / `terminal_launch_macos.cpp` / `terminal_launch_posix.cpp`
- Modify: `src/platform/CMakeLists.txt`（`:1-6` 公共源列表 + `:10-34` 三支）
- Modify: `tests/CMakeLists.txt`（`:20` 之后登记新用例文件）
- Test: `tests/test_platform/test_terminal_launch.cpp`

**Interfaces:**
- Produces（Task 2/3 依赖）：
  - `platform::terminal::LaunchSpec`：`{ std::string title; std::string workingDir; std::string program; std::vector<std::string> args; }`
  - `core::Result<void> platform::terminal::launch(const LaunchSpec& spec);`
  - `std::string platform::terminal::homeDir();`
  - `bool platform::terminal::windowsTerminalMissing();`
  - `terminal_launch_common.h` 的全部纯函数（签名见 Step 1）
- Consumes：`core::Result` / `core::ErrorCode`（`core/result.h`）、`core::paths::appDataDir()`（`src/core/paths.h:12`，仅 macOS 侧）

- [ ] **Step 1: 两个头文件（逐字，先落声明）**

`src/platform/terminal_launch.h`（spec §4 逐字 + 两处平台查询，说明见末节「与 spec 的偏离」②）：

```cpp
// src/platform/terminal_launch.h
// 平台无关接口（2026-09-28 profile-run 设计 §4）：在系统终端里打开一个程序。
//   Windows: Windows Terminal（wt.exe）优先，缺失则退化传统控制台（D-5）
//   macOS:   写 .command 脚本 → /usr/bin/open（跟随用户默认终端 app，D-7）
//   其他:    NotImplemented（非目标平台）
// 启动是**分离式**的：不等待被启动的进程退出（D-9：DevDesk 不持有会话句柄，
// 故停代理/退出时只能提示，不能阻止）。
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

// 用户主目录（D-3 起始目录默认值）：Windows %USERPROFILE% / macOS·POSIX $HOME；
// 环境变量缺失时退化为进程当前工作目录（保证非空——起始目录必须有值）。
// 各平台实现读各自的环境变量，业务层零 #ifdef。
std::string homeDir();

// Windows Terminal 是否缺失（D-5 提示用，调用方可无条件查询）：
//   Windows：%LOCALAPPDATA%\Microsoft\WindowsApps\wt.exe 不存在 → true
//   macOS / 其他：恒 false（不用 wt，无退化路径）
// 调用方在 launch 成功后若得 true，附一句「已用传统控制台打开」——不静默、不阻断。
bool windowsTerminalMissing();

} // namespace platform::terminal
```

`src/platform/terminal_launch_common.h`：

```cpp
// src/platform/terminal_launch_common.h
// terminal_launch 的跨平台纯函数声明（2026-09-28 profile-run 设计 §4）。
//
// 为什么单独一个头（而不是塞进平台无关的 terminal_launch.h，或本平台 win-only 头）：
//   ① 这些函数是**命令文本拼装**的实现细节：业务层只看 terminal_launch.h，
//      平台实现与本用例文件才包含本头；
//   ② 实现放 terminal_launch_common.cpp，该文件**无条件编译**（CMake 三支之外），
//      故这些符号在所有平台都存在——不重演审计 [17] 的「声明在公共头、定义只在
//      某平台」契约破裂（file_open_win.h 的教训）；
//   ③ 单测因此在当前平台（Windows）就能覆盖 wt/PowerShell 与 macOS .command
//      两套正文拼装（纯字符串运算，与宿主机无关）。
#pragma once

#include <string>
#include <vector>

#include "terminal_launch.h"

namespace platform::terminal {

// ---------- 引号 / 转义 ----------

// POSIX shell 单引号转义（' 拆成 '\''）：路径含空格/引号不破坏命令边界。
// 与 file_open_macos.cpp / file_open_posix.cpp 内的 file-static 实现同规则
//（那两份是 file-static，不能跨文件复用）。
std::string shellSingleQuote(const std::string& s);

// PowerShell 单引号转义（' → ''）：PS 单引号串内唯一的转义形式。
std::string powershellSingleQuote(const std::string& s);

// Windows 命令行参数加引号（CreateProcessW 的 lpCommandLine 解析规则，
// 即 CommandLineToArgvW 的逆运算）：不含 空格/Tab/双引号 → 原样返回；
// 否则整体加双引号，且连续反斜杠仅在「引号前」与「串尾」翻倍。
std::string quoteWindowsArg(const std::string& arg);

// argv（不含引号）→ lpCommandLine 单串（各段经 quoteWindowsArg，空格连接）
std::string joinWindowsCommandLine(const std::vector<std::string>& argv);

// ---------- 编码（自实现：不用 3rd/cpp-httplib 的 detail::base64_encode）----------

// UTF-8 文本 → UTF-16LE 字节流（不含 BOM；非法序列按 U+FFFD 兜底，不抛错）
std::string utf8ToUtf16LEBytes(const std::string& utf8);

// 原始字节 → 标准 base64（含 '=' 填充）
std::string base64Encode(const std::string& raw);

// PowerShell -EncodedCommand 载荷 = base64(UTF-16LE(正文))
std::string encodePowerShellCommand(const std::string& body);

// ---------- 命令文本（写死，供实现与单测逐字对照，spec §4.1）----------

// PowerShell 正文：Set-Location 到起始目录 + 换行 + 一行程序名（**不含任何密钥**）
std::string buildPowerShellBody(const LaunchSpec& spec);

// Windows Terminal 分支的参数向量（不带引号；引号由 joinWindowsCommandLine 加）：
//   wt.exe -w new -d <工作目录> --title <标题> powershell.exe
//          -NoExit -NoProfile -EncodedCommand <BASE64_UTF16LE>
std::vector<std::string> buildWindowsLaunchArgs(const LaunchSpec& spec);

// wt 缺失时的退化分支参数向量（传统控制台）：
//   powershell.exe -NoExit -NoProfile -EncodedCommand <BASE64_UTF16LE>
std::vector<std::string> buildWindowsFallbackArgs(const LaunchSpec& spec);

// macOS .command 正文：cd '<工作目录>' && exec <程序名> + 换行（单引号包裹）
std::string buildMacCommandBody(const LaunchSpec& spec);

// 稳定短标识（8 位 hex，FNV-1a 32）：macOS 脚本文件名里的 <id> 槽位。
// LaunchSpec（spec §4 逐字：title/workingDir/program/args）不含 profile id，
// 故用 title 的哈希：同一 profile 稳定、不同 profile 不撞（见末节「与 spec 的偏离」③）
std::string stableToken(const std::string& text);

// macOS 脚本路径：<dir>/run-<稳定标识>-<时间戳>.command（spec §4/§6 的文件名形状）。
// 手工用 '/' 拼接（纯函数，与宿主机路径分隔符无关——macOS 侧就是 '/'）
std::string runScriptPath(const std::string& dir, const LaunchSpec& spec,
                          const std::string& timestamp);

} // namespace platform::terminal
```

- [ ] **Step 2: 写纯函数用例 + 登记进 `tests/CMakeLists.txt`**

`tests/CMakeLists.txt`：在 `test_platform/test_file_open.cpp`（`:20`）之后插入一行（**漏登记 = 新用例静默不参与编译**，这是本步唯一的构建系统动作）：

```cmake
    test_platform/test_terminal_launch.cpp
```

`tests/test_platform/test_terminal_launch.cpp` 新建（全文；引号回读与 UTF-16 参照实现用 `_WIN32` 守卫，posix 失败路径用非 Windows/非 Apple 守卫——两条在本机**都不编译**，见「未验证项」）：

```cpp
// tests/test_platform/test_terminal_launch.cpp
// platform::terminal 覆盖（2026-09-28 profile-run 设计 §8）：
//   ① 跨平台纯函数（terminal_launch_common.cpp）：wt/powershell 参数向量、
//      PowerShell 正文、UTF-16LE→base64 载荷（**解码后**与期望正文逐字比对）、
//      macOS .command 正文与脚本路径、Windows 参数引号（空格/中文/反斜杠）。
//   ② Windows 侧额外做 CommandLineToArgvW 回读：拼出的命令行必须能被系统还原
//      成原参数向量——引号规则的黄金判据（UTF-16 转换用 Win32 作参照实现）。
//   ③ 平台失败路径：posix 返回 NotImplemented（非 Windows/非 Apple 才编译，
//      本机不验证，见计划「未验证项」）。
#include "doctest.h"

#include "platform/terminal_launch.h"
#include "platform/terminal_launch_common.h"

#include <string>
#include <vector>

namespace {

using platform::terminal::LaunchSpec;

LaunchSpec makeSpec(const std::string& title, const std::string& dir) {
    LaunchSpec s;
    s.title = title;
    s.workingDir = dir;
    s.program = "claude";
    return s;
}

// base64 解码（**仅测试用**：生产侧只有编码器；解码器放测试里避免为测试增生产代码）
// 返回解码后的原始字节（本用例里 = UTF-16LE 字节流）
std::string base64DecodeForTest(const std::string& text) {
    const auto val = [](const char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    int buf = 0;
    int bits = 0;
    for (const char c : text) {
        const int v = val(c);
        if (v < 0) continue;                       // '=' 与空白跳过
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xFF);
        }
    }
    return out;
}

// UTF-16LE 字节流 → UTF-8（仅测试用；本用例数据全在 BMP 内，不做代理对）
std::string utf16LEToUtf8ForTest(const std::string& le) {
    std::string out;
    for (size_t i = 0; i + 1 < le.size(); i += 2) {
        const unsigned int u =
            static_cast<unsigned char>(le[i]) |
            (static_cast<unsigned int>(static_cast<unsigned char>(le[i + 1])) << 8);
        if (u < 0x80) {
            out += static_cast<char>(u);
        } else if (u < 0x800) {
            out += static_cast<char>(0xC0 | (u >> 6));
            out += static_cast<char>(0x80 | (u & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (u >> 12));
            out += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (u & 0x3F));
        }
    }
    return out;
}

} // namespace

// ---- ① Windows：wt 分支参数向量 + -EncodedCommand 载荷 ----

REGISTER_TEST(terminal_launch_windows_wt_argv,
              "terminal_launch: wt 分支参数向量 + EncodedCommand 正文解码后逐字相等", {
    const LaunchSpec spec = makeSpec("官方 · Claude Code", "C:/Users/demo");
    const std::vector<std::string> argv =
        platform::terminal::buildWindowsLaunchArgs(spec);

    DOCTEST_REQUIRE_EQ(argv.size(), static_cast<size_t>(12));
    DOCTEST_CHECK_EQ(argv[0], std::string("wt.exe"));
    DOCTEST_CHECK_EQ(argv[1], std::string("-w"));
    DOCTEST_CHECK_EQ(argv[2], std::string("new"));
    DOCTEST_CHECK_EQ(argv[3], std::string("-d"));
    DOCTEST_CHECK_EQ(argv[4], std::string("C:/Users/demo"));
    DOCTEST_CHECK_EQ(argv[5], std::string("--title"));
    DOCTEST_CHECK_EQ(argv[6], std::string("官方 · Claude Code"));
    DOCTEST_CHECK_EQ(argv[7], std::string("powershell.exe"));
    DOCTEST_CHECK_EQ(argv[8], std::string("-NoExit"));
    DOCTEST_CHECK_EQ(argv[9], std::string("-NoProfile"));
    DOCTEST_CHECK_EQ(argv[10], std::string("-EncodedCommand"));

    // 正文 = Set-Location -LiteralPath '<dir>' + 换行 + claude（不含任何密钥）
    const std::string body = platform::terminal::buildPowerShellBody(spec);
    DOCTEST_CHECK_EQ(body,
                     std::string("Set-Location -LiteralPath 'C:/Users/demo'\nclaude"));
    // 载荷 = 正文的 UTF-16LE + base64：编码函数与正文两条路径的值必须一致，
    // 且**解码后**（而不是直接比 base64 串）与期望正文逐字相等
    DOCTEST_CHECK_EQ(argv[11], platform::terminal::encodePowerShellCommand(body));
    DOCTEST_CHECK_EQ(utf16LEToUtf8ForTest(base64DecodeForTest(argv[11])), body);
    return true;
})

REGISTER_TEST(terminal_launch_windows_fallback_argv,
              "terminal_launch: wt 缺失退化分支参数向量（无 wt -d，正文同源）", {
    const LaunchSpec spec = makeSpec("t", "C:/Users/demo");
    const std::vector<std::string> argv =
        platform::terminal::buildWindowsFallbackArgs(spec);

    DOCTEST_REQUIRE_EQ(argv.size(), static_cast<size_t>(5));
    DOCTEST_CHECK_EQ(argv[0], std::string("powershell.exe"));
    DOCTEST_CHECK_EQ(argv[1], std::string("-NoExit"));
    DOCTEST_CHECK_EQ(argv[2], std::string("-NoProfile"));
    DOCTEST_CHECK_EQ(argv[3], std::string("-EncodedCommand"));
    // 与 wt 分支的载荷同源（正文只由 buildPowerShellBody 生成一处）
    DOCTEST_CHECK_EQ(argv[4],
                     platform::terminal::encodePowerShellCommand(
                         platform::terminal::buildPowerShellBody(spec)));
    return true;
})

// ---- ① 正文与引号：含空格 / 单引号 / 中文 ----

REGISTER_TEST(terminal_launch_bodies_with_tricky_paths,
              "terminal_launch: 正文拼装（路径含空格/单引号/中文不破坏）", {
    // PowerShell：单引号转义 ' → ''
    const LaunchSpec ps = makeSpec("t", "C:/Program Files/O'Brien");
    DOCTEST_CHECK_EQ(platform::terminal::buildPowerShellBody(ps),
                     std::string("Set-Location -LiteralPath 'C:/Program Files/O''Brien'\nclaude"));
    // 中文目录不经代码页转换（原样进 UTF-8 正文，再整段转 UTF-16LE）
    const LaunchSpec cn = makeSpec("t", "D:/笔记 目录");
    DOCTEST_CHECK_EQ(platform::terminal::buildPowerShellBody(cn),
                     std::string("Set-Location -LiteralPath 'D:/笔记 目录'\nclaude"));

    // macOS .command 正文：cd 单引号包裹；路径含单引号 → '\''
    DOCTEST_CHECK_EQ(platform::terminal::buildMacCommandBody(makeSpec("t", "/Users/demo")),
                     std::string("cd '/Users/demo' && exec claude\n"));
    DOCTEST_CHECK_EQ(platform::terminal::buildMacCommandBody(makeSpec("t", "/Users/my dir")),
                     std::string("cd '/Users/my dir' && exec claude\n"));
    DOCTEST_CHECK_EQ(platform::terminal::buildMacCommandBody(makeSpec("t", "/Users/o'brien")),
                     std::string("cd '/Users/o'\\''brien' && exec claude\n"));
    return true;
})

REGISTER_TEST(terminal_launch_windows_arg_quoting,
              "terminal_launch: 参数引号（空格加引号/尾随反斜杠翻倍/内嵌引号/中文免引号）", {
    using platform::terminal::quoteWindowsArg;
    DOCTEST_CHECK_EQ(quoteWindowsArg("plain"), std::string("plain"));
    DOCTEST_CHECK_EQ(quoteWindowsArg(""), std::string("\"\""));
    DOCTEST_CHECK_EQ(quoteWindowsArg("C:/Program Files/x"),
                     std::string("\"C:/Program Files/x\""));
    // 尾随反斜杠在带引号时翻倍（否则闭合引号被吃掉）
    DOCTEST_CHECK_EQ(quoteWindowsArg("C:\\Program Files\\"),
                     std::string("\"C:\\Program Files\\\\\""));
    // 内嵌双引号 → \"
    DOCTEST_CHECK_EQ(quoteWindowsArg("say \"hi\""),
                     std::string("\"say \\\"hi\\\"\""));
    // 中文（无空格）不需要引号
    DOCTEST_CHECK_EQ(quoteWindowsArg("D:/笔记/目录"), std::string("D:/笔记/目录"));
    return true;
})

// ---- ① macOS 脚本路径形状 ----

REGISTER_TEST(terminal_launch_mac_script_path,
              "terminal_launch: .command 路径 = run-<8位hex>-<ts>.command（稳定且不撞）", {
    const std::string p = platform::terminal::runScriptPath(
        "/tmp/run-scripts", makeSpec("官方 · Claude Code", "/x"), "1730000000");
    DOCTEST_CHECK_EQ(p.substr(0, std::string("/tmp/run-scripts/run-").size()),
                     std::string("/tmp/run-scripts/run-"));
    DOCTEST_CHECK(p.find("1730000000") != std::string::npos);
    DOCTEST_CHECK_EQ(p.substr(p.size() - 8), std::string(".command"));
    // 目录末尾带 '/' 不产生双斜杠
    DOCTEST_CHECK(platform::terminal::runScriptPath(
                      "/tmp/run-scripts/", makeSpec("t", "/x"), "1").find("//") ==
                  std::string::npos);

    // 稳定短标识：同 title 稳定（同一 profile 重复运行 = 同一 token）、
    // 不同 title 不撞
    DOCTEST_CHECK_EQ(platform::terminal::stableToken("官方 · Claude Code").size(),
                     static_cast<size_t>(8));
    DOCTEST_CHECK_EQ(platform::terminal::stableToken("甲"),
                     platform::terminal::stableToken("甲"));
    DOCTEST_CHECK(platform::terminal::stableToken("甲") !=
                  platform::terminal::stableToken("乙"));
    return true;
})

// ---- ① 平台查询（homeDir 非空；wt 探测可调用不崩）----

REGISTER_TEST(terminal_launch_home_dir_and_wt_probe,
              "terminal_launch: homeDir 非空 + windowsTerminalMissing 可调用", {
    DOCTEST_CHECK(!platform::terminal::homeDir().empty());
    (void)platform::terminal::windowsTerminalMissing();   // 不崩即过（值随机器而异）
    return true;
})

// ---- ② Windows：命令行回读（引号规则黄金判据）----

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>

REGISTER_TEST(terminal_launch_windows_commandline_roundtrip,
              "terminal_launch: 命令行经 CommandLineToArgvW 回读 = 原参数向量", {
    const LaunchSpec spec = makeSpec("官方 · Claude Code", "C:/Program Files/中文 目录");
    const std::vector<std::string> argv =
        platform::terminal::buildWindowsLaunchArgs(spec);
    const std::string line = platform::terminal::joinWindowsCommandLine(argv);

    // UTF-8 → UTF-16（生产侧 spawn 用 MultiByteToWideChar 同款；此处直接用 Win32 做参照）
    const auto toWide = [](const std::string& s) -> std::wstring {
        if (s.empty()) return std::wstring();
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                          static_cast<int>(s.size()), nullptr, 0);
        if (n <= 0) return std::wstring();
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                            w.data(), n);
        return w;
    };
    const std::wstring wide = toWide(line);
    DOCTEST_REQUIRE(!wide.empty());

    int argc = 0;
    LPWSTR* parsed = CommandLineToArgvW(wide.c_str(), &argc);
    DOCTEST_REQUIRE(parsed != nullptr);
    DOCTEST_REQUIRE_EQ(argc, static_cast<int>(argv.size()));
    for (int i = 0; i < argc; ++i) {
        DOCTEST_CHECK(std::wstring(parsed[i]) == toWide(argv[static_cast<size_t>(i)]));
    }
    LocalFree(parsed);
    return true;
})

#endif  // _WIN32

// ---- ③ 平台失败路径（非 Windows / 非 Apple 才编译；本机不验证）----

#if !defined(_WIN32) && !defined(__APPLE__)

REGISTER_TEST(terminal_launch_posix_not_implemented,
              "terminal_launch: posix launch → NotImplemented（非目标平台）", {
    auto r = platform::terminal::launch(makeSpec("t", "/tmp"));
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK(r.errorCode() == core::ErrorCode::NotImplemented);
    return true;
})

#endif  // !_WIN32 && !__APPLE__
```

> **不在用例里调 macOS/Windows 的真实 `launch()`**：Windows 会真开 wt/控制台、macOS 会真开终端——那是「人工点验」项（见末节），用例只覆盖纯函数与失败路径。

- [ ] **Step 3: 跑一次（预期失败）**

```bash
bash scripts/build-test-run.sh --no-run --filter terminal_launch
```

Expected: **失败**，`terminal_launch_common.h` 里的符号全是未定义引用（`undefined reference to platform::terminal::quoteWindowsArg...`）或直接编译不过——这正是 Step 4 要实现的。若报的是「找不到测试文件 / 无匹配用例名」，说明 Step 2 的 `tests/CMakeLists.txt` 登记没生效。

- [ ] **Step 4: 实现 `src/platform/terminal_launch_common.cpp`（逐字）**

```cpp
// src/platform/terminal_launch_common.cpp
// terminal_launch 的跨平台纯函数（2026-09-28 profile-run 设计 §4）：
// 命令文本拼装的**全部**逻辑集中在这里，平台实现只负责系统调用——这样两套正文
// 拼装（Windows wt/PowerShell、macOS .command）在当前平台就能被单测覆盖。
// 本文件**无条件编译**（CMake 三支之外），故这些符号在所有平台都存在。
#include "terminal_launch_common.h"

#include <cstdint>

namespace platform::terminal {

// ---------- 引号 / 转义 ----------

std::string shellSingleQuote(const std::string& s) {
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string powershellSingleQuote(const std::string& s) {
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') out += "''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string quoteWindowsArg(const std::string& arg) {
    // 不含空格/Tab/双引号 → 原样（含中文等非 ASCII 字节：它们都 >= 0x80，
    // 不会被误当分隔符或引号）
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) {
        return arg;
    }
    std::string out = "\"";
    size_t backslashes = 0;
    for (const char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            // 反斜杠翻倍 + 转义这个引号（CommandLineToArgvW 的规则）
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
            backslashes = 0;
            continue;
        }
        out.append(backslashes, '\\');
        backslashes = 0;
        out += c;
    }
    // 串尾反斜杠翻倍：否则最后一个反斜杠会把闭合引号转义掉
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

std::string joinWindowsCommandLine(const std::vector<std::string>& argv) {
    std::string out;
    for (const auto& arg : argv) {
        if (!out.empty()) out += ' ';
        out += quoteWindowsArg(arg);
    }
    return out;
}

// ---------- 编码 ----------

std::string utf8ToUtf16LEBytes(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size() * 2);
    size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char b0 = static_cast<unsigned char>(utf8[i]);
        uint32_t cp = 0xFFFD;   // 非法首字节 → U+FFFD（尽力而为，不抛错）
        size_t len = 1;
        if (b0 < 0x80) {
            cp = b0;
        } else if ((b0 & 0xE0) == 0xC0) {
            cp = b0 & 0x1Fu;
            len = 2;
        } else if ((b0 & 0xF0) == 0xE0) {
            cp = b0 & 0x0Fu;
            len = 3;
        } else if ((b0 & 0xF8) == 0xF0) {
            cp = b0 & 0x07u;
            len = 4;
        }
        if (len > 1) {
            for (size_t k = 1; k < len; ++k) {
                if (i + k >= utf8.size()) {   // 截断 → 只消费已有的字节
                    cp = 0xFFFD;
                    len = k;
                    break;
                }
                const unsigned char bk = static_cast<unsigned char>(utf8[i + k]);
                if ((bk & 0xC0) != 0x80) {    // 续字节非法 → 只消费到 k
                    cp = 0xFFFD;
                    len = k;
                    break;
                }
                cp = (cp << 6) | (bk & 0x3Fu);
            }
        }
        i += len;
        if (cp >= 0x10000 && cp <= 0x10FFFF) {          // 代理对（U+10000 以上）
            const uint32_t v = cp - 0x10000;
            const uint16_t hi = static_cast<uint16_t>(0xD800 + (v >> 10));
            const uint16_t lo = static_cast<uint16_t>(0xDC00 + (v & 0x3FFu));
            out += static_cast<char>(hi & 0xFF);
            out += static_cast<char>(hi >> 8);
            out += static_cast<char>(lo & 0xFF);
            out += static_cast<char>(lo >> 8);
        } else {
            const uint16_t u = static_cast<uint16_t>(cp <= 0xFFFF ? cp : 0xFFFD);
            out += static_cast<char>(u & 0xFF);
            out += static_cast<char>(u >> 8);
        }
    }
    return out;
}

std::string base64Encode(const std::string& raw) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((raw.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= raw.size()) {
        const uint32_t v =
            (static_cast<uint32_t>(static_cast<unsigned char>(raw[i])) << 16) |
            (static_cast<uint32_t>(static_cast<unsigned char>(raw[i + 1])) << 8) |
            static_cast<uint32_t>(static_cast<unsigned char>(raw[i + 2]));
        out += kTable[(v >> 18) & 0x3Fu];
        out += kTable[(v >> 12) & 0x3Fu];
        out += kTable[(v >> 6) & 0x3Fu];
        out += kTable[v & 0x3Fu];
        i += 3;
    }
    const size_t rest = raw.size() - i;
    if (rest == 1) {
        const uint32_t v =
            static_cast<uint32_t>(static_cast<unsigned char>(raw[i])) << 16;
        out += kTable[(v >> 18) & 0x3Fu];
        out += kTable[(v >> 12) & 0x3Fu];
        out += "==";
    } else if (rest == 2) {
        const uint32_t v =
            (static_cast<uint32_t>(static_cast<unsigned char>(raw[i])) << 16) |
            (static_cast<uint32_t>(static_cast<unsigned char>(raw[i + 1])) << 8);
        out += kTable[(v >> 18) & 0x3Fu];
        out += kTable[(v >> 12) & 0x3Fu];
        out += kTable[(v >> 6) & 0x3Fu];
        out += '=';
    }
    return out;
}

std::string encodePowerShellCommand(const std::string& body) {
    return base64Encode(utf8ToUtf16LEBytes(body));
}

// ---------- 命令文本 ----------

std::string buildPowerShellBody(const LaunchSpec& spec) {
    // ⚠️ 正文里**不含任何密钥**（D-8）：会话的密钥由本地代理转发时注入
    return "Set-Location -LiteralPath " + powershellSingleQuote(spec.workingDir) +
           "\n" + spec.program;
}

std::vector<std::string> buildWindowsLaunchArgs(const LaunchSpec& spec) {
    std::vector<std::string> argv;
    argv.push_back("wt.exe");
    argv.push_back("-w");
    argv.push_back("new");
    argv.push_back("-d");
    argv.push_back(spec.workingDir);
    argv.push_back("--title");
    argv.push_back(spec.title);
    argv.push_back("powershell.exe");
    argv.push_back("-NoExit");       // D-6：claude 退出后保留窗口，回滚内容可见
    argv.push_back("-NoProfile");
    argv.push_back("-EncodedCommand");
    argv.push_back(encodePowerShellCommand(buildPowerShellBody(spec)));
    return argv;
}

std::vector<std::string> buildWindowsFallbackArgs(const LaunchSpec& spec) {
    std::vector<std::string> argv;
    argv.push_back("powershell.exe");
    argv.push_back("-NoExit");
    argv.push_back("-NoProfile");
    argv.push_back("-EncodedCommand");
    argv.push_back(encodePowerShellCommand(buildPowerShellBody(spec)));
    return argv;
}

std::string buildMacCommandBody(const LaunchSpec& spec) {
    // D-7：cd + exec；脚本不含密钥
    return "cd " + shellSingleQuote(spec.workingDir) + " && exec " + spec.program + "\n";
}

std::string stableToken(const std::string& text) {
    uint32_t h = 2166136261u;                      // FNV-1a 32
    for (const char c : text) {
        h ^= static_cast<uint32_t>(static_cast<unsigned char>(c));
        h *= 16777619u;
    }
    static const char kHex[] = "0123456789abcdef";
    std::string out(8, '0');
    for (int i = 0; i < 8; ++i) {
        out[static_cast<size_t>(7 - i)] =
            kHex[static_cast<size_t>((h >> (i * 4)) & 0xFu)];
    }
    return out;
}

std::string runScriptPath(const std::string& dir, const LaunchSpec& spec,
                          const std::string& timestamp) {
    const std::string name = "run-" + stableToken(spec.title) + "-" + timestamp +
                             ".command";
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

} // namespace platform::terminal
```

- [ ] **Step 5: 跑一次（预期通过：纯函数全绿）**

```bash
bash scripts/build-test-run.sh --no-run --filter terminal_launch
```

Expected: `✅ 测试全绿`，命中 7 个用例（`terminal_launch_windows_wt_argv` / `_windows_fallback_argv` / `_bodies_with_tricky_paths` / `_windows_arg_quoting` / `_mac_script_path` / `_home_dir_and_wt_probe` / `_windows_commandline_roundtrip`）。若 `roundtrip` 红：这是引号规则的真缺陷，**改 `quoteWindowsArg` 而不是放宽断言**。

- [ ] **Step 6: 三平台实现 + 平台 CMake（逐字）**

`src/platform/terminal_launch_win.cpp`：

```cpp
// src/platform/terminal_launch_win.cpp
// Windows 实现（2026-09-28 profile-run 设计 §4/§4.1、D-4/D-5）：
//   wt.exe 在（探测 %LOCALAPPDATA%\Microsoft\WindowsApps\wt.exe）→
//     CreateProcessW(wt.exe 全路径, "wt.exe -w new -d <dir> --title <t> powershell…")
//   否则 → CreateProcessW(powershell.exe, "powershell.exe -NoExit -NoProfile
//                        -EncodedCommand <b64>", CREATE_NEW_CONSOLE,
//                        lpCurrentDirectory = <起始目录>)
// 命令文本全部来自 terminal_launch_common.cpp 的纯函数（单测覆盖）；本文件只做
// 「探测 + UTF-8→UTF-16 转换 + CreateProcessW + 句柄清理」。
#include "terminal_launch.h"
#include "terminal_launch_common.h"

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace platform::terminal {
namespace {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                        static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), need);
    return out;
}

// wt.exe 全路径：WindowsApps 下是 app execution alias，且 PATH 顺序不保证 →
// 探测与派发共用同一绝对路径
std::wstring windowsTerminalPath() {
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    if (local == nullptr || *local == L'\0') return std::wstring();
    std::wstring p(local);
    p += L"\\Microsoft\\WindowsApps\\wt.exe";
    return p;
}

// 分离式派发：不等待退出、不继承句柄；进程/线程句柄用完即关（D-9：不持有会话）
core::Result<void> spawn(const std::wstring& exe,
                         const std::vector<std::string>& argv,
                         const std::wstring& currentDir) {
    std::wstring cmdLine = utf8ToWide(joinWindowsCommandLine(argv));
    // CreateProcessW 可能就地改写 lpCommandLine → 必须给可写缓冲
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const wchar_t* dir = currentDir.empty() ? nullptr : currentDir.c_str();
    const BOOL ok = CreateProcessW(exe.empty() ? nullptr : exe.c_str(),
                                   buf.data(), nullptr, nullptr, FALSE,
                                   CREATE_NEW_CONSOLE, nullptr, dir, &si, &pi);
    if (ok == FALSE) {
        const DWORD err = GetLastError();
        return core::Result<void>::fail(core::ErrorCode::Internal,
            "启动终端进程失败（GetLastError=" + std::to_string(err) + "）");
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return core::Result<void>::ok();
}

} // namespace

bool windowsTerminalMissing() {
    const std::wstring wt = windowsTerminalPath();
    if (wt.empty()) return true;   // 取不到 LOCALAPPDATA = 视为缺失
    // app execution alias 是重解析点：直接问 Win32 文件属性比 filesystem::exists
    // 更可靠（后者对别名不保证语义）
    return GetFileAttributesW(wt.c_str()) == INVALID_FILE_ATTRIBUTES;
}

std::string homeDir() {
    const char* p = std::getenv("USERPROFILE");
    if (p != nullptr && *p != '\0') return p;
    std::error_code ec;   // 环境变量缺失 → 进程当前目录兜底（保证非空）
    const auto cwd = std::filesystem::current_path(ec);
    return ec ? std::string() : cwd.string();
}

core::Result<void> launch(const LaunchSpec& spec) {
    const std::wstring wt = windowsTerminalPath();
    const bool hasWt = !wt.empty() &&
        GetFileAttributesW(wt.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (hasWt) {
        // wt -d 已带起始目录 → 不需要 lpCurrentDirectory（spec §4.1）
        return spawn(wt, buildWindowsLaunchArgs(spec), std::wstring());
    }
    // 退化分支（D-5）：没有 wt -d，**必须**显式设 lpCurrentDirectory；
    // 正文里的 Set-Location 是双保险（spec §4.1）
    return spawn(L"powershell.exe", buildWindowsFallbackArgs(spec),
                 utf8ToWide(spec.workingDir));
}

} // namespace platform::terminal
```

`src/platform/terminal_launch_macos.cpp`：

```cpp
// src/platform/terminal_launch_macos.cpp
// macOS 实现（2026-09-28 profile-run 设计 §4、D-7）：
//   写 .command 脚本 → chmod 0755 → /usr/bin/open（LaunchServices 决定**用户
//   默认终端 app**，不强绑 Terminal.app）。脚本只含 cd + 程序名，**不含密钥**（D-8）。
// ⚠️ 本机（Windows）无法编译/运行验证：按静态评审交付，实机验证见计划「未验证项」。
#include "terminal_launch.h"
#include "terminal_launch_common.h"

#include "core/paths.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <system_error>

namespace platform::terminal {
namespace {

namespace fs = std::filesystem;

// 陈旧清扫（spec §4：启动时清扫该目录陈旧文件）：删 run-*.command 里
// last_write_time 早于 24h 的；单条失败忽略（best-effort，不阻断本次启动）
void sweepStaleScripts(const fs::path& dir) {
    const auto now = fs::file_time_type::clock::now();
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) return;
        if (entry.path().extension() != ".command") continue;
        const std::string name = entry.path().filename().string();
        if (name.rfind("run-", 0) != 0) continue;
        std::error_code inner;
        const auto mt = fs::last_write_time(entry.path(), inner);
        if (inner) continue;
        if (now - mt > std::chrono::hours(24)) {
            fs::remove(entry.path(), inner);
        }
    }
}

} // namespace

bool windowsTerminalMissing() { return false; }   // macOS 不用 wt，无退化路径

std::string homeDir() {
    const char* p = std::getenv("HOME");
    if (p != nullptr && *p != '\0') return p;
    std::error_code ec;   // 环境变量缺失 → 进程当前目录兜底（保证非空）
    const auto cwd = fs::current_path(ec);
    return ec ? std::string() : cwd.string();
}

core::Result<void> launch(const LaunchSpec& spec) {
    const fs::path dir = core::paths::appDataDir() / "run-scripts";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
            "创建 run-scripts 目录失败: " + dir.string() + "（" + ec.message() + "）");
    }
    sweepStaleScripts(dir);

    const std::string ts =
        std::to_string(static_cast<long long>(std::time(nullptr)));
    const fs::path script(runScriptPath(dir.string(), spec, ts));

    {
        std::ofstream out(script, std::ios::binary | std::ios::trunc);
        if (!out) {
            return core::Result<void>::fail(core::ErrorCode::Internal,
                "写 run 脚本失败: " + script.string());
        }
        out << buildMacCommandBody(spec);
        if (!out) {
            return core::Result<void>::fail(core::ErrorCode::Internal,
                "写 run 脚本失败（写入中断）: " + script.string());
        }
    }
    // 0755：.command 必须可执行，否则 LaunchServices 拒绝打开
    if (::chmod(script.c_str(), 0755) != 0) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
            "chmod 0755 失败: " + script.string());
    }
    // open <脚本> = 交给 LaunchServices 用**用户默认终端 app** 打开（D-7）；
    // 单引号转义照 file_open_macos.cpp 既有做法（shellSingleQuote 在 common 里）
    const int ret = std::system(
        ("/usr/bin/open " + shellSingleQuote(script.string())).c_str());
    if (ret != 0) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
            "/usr/bin/open 打开 run 脚本失败（exit=" + std::to_string(ret) +
            "）: " + script.string());
    }
    return core::Result<void>::ok();
}

} // namespace platform::terminal
```

`src/platform/terminal_launch_posix.cpp`：

```cpp
// src/platform/terminal_launch_posix.cpp
// 其他平台（Linux 等）：非目标平台，明确不支持（spec §10.5）
#include "terminal_launch.h"

#include <cstdlib>
#include <filesystem>

namespace platform::terminal {

bool windowsTerminalMissing() { return false; }

std::string homeDir() {
    const char* p = std::getenv("HOME");
    if (p != nullptr && *p != '\0') return p;
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    return ec ? std::string() : cwd.string();
}

core::Result<void> launch(const LaunchSpec&) {
    return core::Result<void>::fail(core::ErrorCode::NotImplemented,
        "终端启动在 Linux/其他平台未实现（非目标平台）");
}

} // namespace platform::terminal
```

`src/platform/CMakeLists.txt` 两处（与该文件既有模式同款：公共源进 `add_library` 列表，平台源进对应分支）：

```cmake
add_library(devdesk_platform STATIC
    http_client.cpp
    http_server.cpp
    json.cpp
    file_open_common.cpp
    terminal_launch_common.cpp          # 跨平台纯函数（三支之外，无条件编译）
)
```

```cmake
if(WIN32)
    target_sources(devdesk_platform PRIVATE
        keychain_win.cpp
        file_open_win.cpp
        system_theme_win.cpp
        file_watcher_win.cpp
        http_client_ca_win.cpp
        terminal_launch_win.cpp
    )
elseif(APPLE)
    target_sources(devdesk_platform PRIVATE
        keychain_macos.cpp
        file_open_macos.cpp
        system_theme_posix.cpp
        file_watcher_stub.cpp
        http_client_ca_posix.cpp
        terminal_launch_macos.cpp
    )
else()
    target_sources(devdesk_platform PRIVATE
        keychain_posix.cpp
        file_open_posix.cpp
        system_theme_posix.cpp
        file_watcher_stub.cpp
        http_client_ca_posix.cpp
        terminal_launch_posix.cpp
    )
endif()
```

- [ ] **Step 7: 跑一次（预期通过）**

```bash
bash scripts/build-test-run.sh --no-run --filter terminal_launch
```

Expected: `✅ 测试全绿`（同一批 7 个用例）。三平台实现里 macOS/posix 两个文件在本机**只编译不执行**——`_win.cpp` 进构建、`_macos.cpp`/`_posix.cpp` 在本机被 CMake 排除（`elif` 分支互斥），故它们**只要不参与编译就无法被本机验证**（见「未验证项」）。构建若报 `terminal_launch_macos.cpp` 找不到符号，说明它被误加进了 UNIX 分支之外的支（交叉检查 CMake 改动）。

- [ ] **Step 8: Commit**

```bash
git add src/platform/terminal_launch.h src/platform/terminal_launch_common.h \
        src/platform/terminal_launch_common.cpp src/platform/terminal_launch_win.cpp \
        src/platform/terminal_launch_macos.cpp src/platform/terminal_launch_posix.cpp \
        src/platform/CMakeLists.txt tests/CMakeLists.txt \
        tests/test_platform/test_terminal_launch.cpp
git commit -m "feat(platform): terminal_launch 平台层 —— wt/PowerShell 命令拼装纯函数 + 三平台实现 + CMake 登记"
```

---

## Task 2（M2）: `ClaudeService::runProfile`

**Files:**
- Modify: `src/module_claude_proxy/claude_service.h`（`:238-264` 代理生命周期区旁加注入点与 `runProfile`；私有区加成员）
- Modify: `src/module_claude_proxy/claude_service.cpp`（`doSetActive` 类操作区附近加实现；文件尾不必动）
- Test: `tests/test_modules/test_claude_service.cpp`

**Interfaces:**
- Consumes（Task 1 产出）：`platform::terminal::LaunchSpec` / `launch` / `homeDir` / `windowsTerminalMissing`
- Consumes（既有，**不改语义**）：`ClaudeService::isProxyRunning()`（`claude_service.cpp:1421`）、`startProxy(bindAddress, port)`（`:1365`）、`setActiveProfile(int64_t)`（`:657`）、`getProfile(int64_t)`、`configuredProxyPort()`（`claude_service.h:263`）
- Produces（Task 3 依赖）：
  - `core::Result<void> ClaudeService::runProfile(int64_t id, std::string* hint = nullptr);`
  - `void ClaudeService::setTerminalLauncher(std::function<core::Result<void>(const platform::terminal::LaunchSpec&)> launcher);`

- [ ] **Step 1: 写 service 用例（先红）**

在 `tests/test_modules/test_claude_service.cpp` 的匿名命名空间内追加两个测试基建（放在 `ScopedProxyEnv` 之后即可）：

```cpp
// 动态空闲端口（借 Server bind 0 向系统要一个随即释放）：多 worktree / 残留进程
// 并发跑测试时不再撞写死端口；失败返回 0（调用方 REQUIRE 拦）
int pickFreePort() {
    platform::http::Server probe("127.0.0.1", 0);
    if (!probe.start()) return 0;
    const int port = probe.actualPort();
    probe.stop();
    return port;
}

// 接管通道路径用的临时文件（settings.json + sidecar；绝不碰真实 ~/.claude）。
// runProfile 的「非激活 → 先切激活」分支要靠「坏 settings.json 让反向同步失败」
// 来观察，故必须注入通道路径。
struct TempSyncPaths {
    std::filesystem::path settings;
    std::filesystem::path sidecar;

    explicit TempSyncPaths(const std::string& tag) {
        const std::string base = tempDbPath(tag).string();   // 复用同一 tempPath 工厂
        settings = base + ".settings.json";
        sidecar = base + ".sidecar.json";
        std::error_code ec;
        std::filesystem::remove(settings, ec);
        std::filesystem::remove(sidecar, ec);
    }
    ~TempSyncPaths() {
        std::error_code ec;
        std::filesystem::remove(settings, ec);
        std::filesystem::remove(sidecar, ec);
    }

    void writeSettings(const std::string& content) const {
        std::ofstream(settings, std::ios::binary | std::ios::trunc) << content;
    }
    module::claude::settings_sync::Paths paths() const {
        module::claude::settings_sync::Paths p;
        p.settingsFile = settings;
        p.sidecarFile = sidecar;
        return p;
    }
};
```

该文件顶部 include 补两行（`settings_sync.h` 目前只经 `claude_service.h` 间接可见，显式写上）：

```cpp
#include "settings_sync.h"
#include "platform/terminal_launch.h"
```

追加四条用例（spec §8「service」层的四条 + 一条 id 校验；**全部注入假 launch，不真起终端**）：

```cpp
// ---- 运行（profile-run 设计 §5/§8；D-1 顺序：代理 → 激活 → 启动会话）----

REGISTER_TEST(claude_service_run_profile_starts_proxy_and_passes_spec,
              "claude_service: runProfile 拉起代理（configuredProxyPort）+ 假 launch 收到正确参数", {
    TestDb tdb("runprofile1");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    const int port = pickFreePort();
    DOCTEST_REQUIRE(port != 0);
    svc.setConfiguredProxyPort(port);
    DOCTEST_CHECK_EQ(svc.configuredProxyPort(), port);

    auto a = svc.createProfile("官方", "https://api.anthropic.com",
                               "sk-ant-run-0001", "");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());

    int calls = 0;
    platform::terminal::LaunchSpec got;
    svc.setTerminalLauncher([&](const platform::terminal::LaunchSpec& s) {
        ++calls;
        got = s;
        return core::Result<void>::ok();
    });

    // ① 代理未运行 → 用 configuredProxyPort 拉起（不是默认 8888）
    DOCTEST_CHECK(!svc.isProxyRunning());
    std::string hint;
    auto r = svc.runProfile(a.value().id, &hint);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(svc.isProxyRunning());
    DOCTEST_CHECK_EQ(svc.proxyPort(), port);

    // ④ 调用参数（title / cwd / program / args）
    DOCTEST_CHECK_EQ(calls, 1);
    DOCTEST_CHECK_EQ(got.title, std::string("官方 · Claude Code"));
    DOCTEST_CHECK_EQ(got.workingDir, platform::terminal::homeDir());
    DOCTEST_CHECK(!got.workingDir.empty());
    DOCTEST_CHECK_EQ(got.program, std::string("claude"));
    DOCTEST_CHECK(got.args.empty());

    svc.stopProxy();
    cleanupKey(a.value().id);
    return true;
})

REGISTER_TEST(claude_service_run_profile_proxy_start_failure_aborts,
              "claude_service: runProfile 代理起不来 → 返回 fail 且不开会话", {
    TestDb tdb("runprofile2");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    const int port = pickFreePort();
    DOCTEST_REQUIRE(port != 0);
    // 占住该端口 → startProxy 必失败
    platform::http::Server blocker("127.0.0.1", port);
    DOCTEST_REQUIRE(blocker.start());
    svc.setConfiguredProxyPort(port);

    auto a = svc.createProfile("官方", "https://api.anthropic.com",
                               "sk-ant-run-0002", "");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());

    int calls = 0;
    svc.setTerminalLauncher([&](const platform::terminal::LaunchSpec&) {
        ++calls;
        return core::Result<void>::ok();
    });
    auto r = svc.runProfile(a.value().id, nullptr);
    DOCTEST_CHECK(r.isFail());     // 中止
    DOCTEST_CHECK_EQ(calls, 0);    // 不打开会话

    blocker.stop();
    cleanupKey(a.value().id);
    return true;
})

REGISTER_TEST(claude_service_run_profile_switches_only_when_inactive,
              "claude_service: runProfile 非激活 → 先切激活（失败即中止）；已激活 → 不切", {
    TestDb tdb("runprofile3");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 注入接管通道路径（不注入 → 反向同步整段跳过，②③ 就无从区分）
    TempSyncPaths paths("runprofile3");
    svc.setSettingsSyncPaths(paths.paths());

    auto a = svc.createProfile("甲", "https://a.example.com", "sk-ant-run-a", "");
    auto b = svc.createProfile("乙", "https://b.example.com", "sk-ant-run-b", "");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());

    const int port = pickFreePort();
    DOCTEST_REQUIRE(port != 0);
    svc.setConfiguredProxyPort(port);
    // 先起代理：避免 runProfile 内的 takeOver 覆盖掉下面写入的坏 settings.json
    DOCTEST_REQUIRE(svc.startProxy("127.0.0.1", port).isOk());

    int calls = 0;
    svc.setTerminalLauncher([&](const platform::terminal::LaunchSpec&) {
        ++calls;
        return core::Result<void>::ok();
    });

    // ③ 目标 = 激活 profile：跳过 setActiveProfile → 坏 settings.json 不参与 → 成功
    paths.writeSettings("{ 这不是合法 JSON");
    std::string hint;
    auto r3 = svc.runProfile(a.value().id, &hint);
    DOCTEST_CHECK(r3.isOk());
    DOCTEST_CHECK_EQ(calls, 1);

    // ② 目标 ≠ 激活 profile：会调 setActiveProfile → 反向同步读坏文件失败 → 中止且不开会话
    paths.writeSettings("{ 这不是合法 JSON");
    auto r2 = svc.runProfile(b.value().id, nullptr);
    DOCTEST_CHECK(r2.isFail());
    DOCTEST_CHECK_EQ(calls, 1);                       // 未新增调用
    auto active = svc.getActiveProfile();
    DOCTEST_REQUIRE(active.isOk());
    DOCTEST_CHECK_EQ(active.value().id, a.value().id);  // 激活态未被改（中止在事务前）

    svc.stopProxy();
    cleanupKey(a.value().id);
    cleanupKey(b.value().id);
    return true;
})

REGISTER_TEST(claude_service_run_profile_invalid_id,
              "claude_service: runProfile id=0 / 不存在 → fail（不开会话）", {
    TestDb tdb("runprofile4");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    int calls = 0;
    svc.setTerminalLauncher([&](const platform::terminal::LaunchSpec&) {
        ++calls;
        return core::Result<void>::ok();
    });
    auto zero = svc.runProfile(0, nullptr);
    DOCTEST_CHECK(zero.isFail());
    DOCTEST_CHECK(zero.errorCode() == core::ErrorCode::ValidationError);
    auto missing = svc.runProfile(9999, nullptr);
    DOCTEST_CHECK(missing.isFail());
    DOCTEST_CHECK_EQ(calls, 0);
    return true;
})
```

- [ ] **Step 2: 跑一次（预期失败）**

```bash
bash scripts/build-test-run.sh --no-run --filter claude_service
```

Expected: **编译失败** —— `ClaudeService` 没有成员 `runProfile` / `setTerminalLauncher`（`claude_service.cpp` 里 `r3/r2` 相关断言自然也不会执行）。

- [ ] **Step 3: 实现（逐字）**

`src/module_claude_proxy/claude_service.h`：

1. 顶部 include 区补两个头（`<functional>` 该文件目前没有；`platform/terminal_launch.h` 提供 `LaunchSpec`）：

```cpp
#include <functional>
```

```cpp
#include "platform/terminal_launch.h"
```

2. 在「代理 server 生命周期」区（`bool isProxyRunning() const;` / `int proxyPort() const;` 之后、`configuredProxyPort` 之前，即 `:254-256` 那段之后）插入：

```cpp
    // ---- 运行：用该 Profile 打开一个新的 Claude Code 会话（2026-09-28
    //      profile-run 设计 §5/§6；D-1：代理托管式，密钥零落盘）----
    // 顺序**固定**：确保代理运行 → 必要时 setActiveProfile → 启动会话。
    // 理由：startProxy 内部会 syncTakeOver()；随后 setActiveProfile 的
    // syncCaptureActiveProfile 读回的正是它自己刚写的受保护托管值（G2/G3 守卫
    // 跳过）——顺序反了会把 live 值误收编进 profile。
    //   - id：目标 profile；0 / 不存在 / 代理起不来 / 切换失败 → fail（不开会话）
    //   - hint：可选**非致命提示**出参（如 Windows 无 wt 已退化传统控制台，D-5）；
    //     成功时写入，无提示时清空。失败原因仍走 Result（与既有接口一致）
    core::Result<void> runProfile(int64_t id, std::string* hint = nullptr);

    // 终端启动注入点（默认指向真实现 platform::terminal::launch）。
    // 镜像既有 setSettingsSyncPaths / setProfileProvider 的值注入先例：
    // 测试注入假实现以断言调用参数，而不真起终端进程。
    void setTerminalLauncher(
        std::function<core::Result<void>(const platform::terminal::LaunchSpec&)>
            launcher);
```

3. 私有成员区（`int configuredProxyPort_ = kDefaultProxyPort;` 附近）加：

```cpp
    // 终端启动实现（profile-run 设计 §6）：默认 = platform::terminal::launch；
    // 测试经 setTerminalLauncher 注入假实现（断言参数、不真起进程）
    std::function<core::Result<void>(const platform::terminal::LaunchSpec&)>
        terminalLauncher_ = &platform::terminal::launch;
```

`src/module_claude_proxy/claude_service.cpp`（插在该文件 `setConfiguredProxyPort` 函数体收尾的 `}`（`:1438`）之后，与本文件其它「设置/操作」段同区）：

```cpp
void ClaudeService::setTerminalLauncher(
    std::function<core::Result<void>(const platform::terminal::LaunchSpec&)>
        launcher) {
    if (launcher) terminalLauncher_ = std::move(launcher);
}

core::Result<void> ClaudeService::runProfile(int64_t id, std::string* hint) {
    if (hint != nullptr) hint->clear();
    if (id == 0) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "profile.id 不能为 0");
    }

    // 目标 profile 必须存在（标题、激活态判定都取自它）
    auto prof = getProfile(id);
    if (prof.isFail()) {
        return core::Result<void>::fail(prof.errorCode(), prof.errorMessage());
    }

    // 顺序 1：代理必须先跑起来（会话只指向本地代理，密钥由代理转发时注入，D-8）。
    // 已在跑时**不重复 startProxy**（server 已存在会硬失败）；端口必须显式传
    // configuredProxyPort()——设置页改过端口后，无参默认 8888 会起错端口。
    if (!isProxyRunning()) {
        auto started = startProxy("127.0.0.1", configuredProxyPort());
        if (started.isFail()) {
            return core::Result<void>::fail(started.errorCode(),
                "启动本地代理失败，未打开会话: " + started.errorMessage());
        }
    }

    // 顺序 2：目标非激活 → 先切激活（全局 settings.json 托管键随之改写，
    // UI 侧已用副提示明示）。失败（含反向同步读不到/解析不了 settings.json）
    // → 中止本次运行，不打开会话。
    if (!prof.value().isActive) {
        auto switched = setActiveProfile(id);
        if (switched.isFail()) {
            return core::Result<void>::fail(switched.errorCode(),
                "切换激活 Profile 失败，未打开会话: " + switched.errorMessage());
        }
    }

    // 顺序 3：派发终端（分离式；命令文本由平台层内部拼装，业务层只传语义参数）。
    // D-3：起始目录默认用户主目录；D-8：不带任何密钥/临时配置。
    platform::terminal::LaunchSpec spec;
    spec.title = prof.value().name + " · Claude Code";
    spec.workingDir = platform::terminal::homeDir();
    spec.program = "claude";

    auto launched = terminalLauncher_(spec);
    if (launched.isFail()) {
        return launched;
    }

    // D-5：wt 缺失 → 平台层已退化传统控制台，这里补一句非致命提示（不静默、不阻断）
    if (hint != nullptr && platform::terminal::windowsTerminalMissing()) {
        *hint = "未检测到 Windows Terminal，已用传统控制台打开会话";
    }
    LOG_INFO("已在终端里用 profile id=" + std::to_string(id) +
             " 打开 Claude Code 会话（" + spec.workingDir + "）");
    return core::Result<void>::ok();
}
```

> `terminalLauncher_(_spec_)` 的调用点：注入为空时 `setTerminalLauncher` 不覆盖默认值，故生产路径永远是真实现——**不要**在 `runProfile` 里加 `if (!terminalLauncher_)` 分支（`setTerminalLauncher` 已挡住空 `std::function` 调用会抛 `bad_function_call` 的唯一入口）。

- [ ] **Step 4: 跑一次（预期通过）**

```bash
bash scripts/build-test-run.sh --no-run --filter claude_service
```

Expected: `✅ 测试全绿`；新 4 条 + 既有全部 `claude_service_*` 用例绿。若 `_switches_only_when_inactive` 的 ③ 分支红（即已激活也被要求切激活），说明 `runProfile` 少了 `isActive` 判定——**不要改断言**（spec §10.1 的中止语义就靠它锁）。

- [ ] **Step 5: Commit**

```bash
git add src/module_claude_proxy/claude_service.h src/module_claude_proxy/claude_service.cpp \
        tests/test_modules/test_claude_service.cpp
git commit -m "feat(claude): ClaudeService::runProfile —— 代理/激活前置 + 可注入终端启动 + service 用例"
```

---

## Task 3（M3）: UI 按钮 + 副提示 + CHANGELOG

**Files:**
- Modify: `src/module_claude_proxy/claude_module.cpp`（`:754-869` 操作按钮组 + `:2470` 起操作区加 `doRun`）
- Modify: `CHANGELOG.md`
- Test: `tests/test_modules/test_claude_ui.cpp`

**Interfaces:**
- Consumes（Task 2 产出）：`ClaudeService::runProfile(int64_t, std::string*)`、`setTerminalLauncher`
- Produces：UI 节点 `claude.act.run` / `claude.act.run.bg` / `claude.act.run.hint`；成员函数 `ClaudeModule::doRun(int64_t)`

- [ ] **Step 1: 写无头 UI 用例（先红）**

在 `tests/test_modules/test_claude_ui.cpp` 末尾追加（模式照该文件既有 `TestEnv` + `compose` + `click` 写法；**点击前必须注入假 launch**——该文件用的是真实 `ClaudeService`，不注入会真启 wt + claude）：

```cpp
// ---- 运行第 6 键（2026-09-28 profile-run 设计 §6/§8）----
// ⚠️ 本文件用真实 ClaudeService（setGlobal）：点「运行」前**必须**注入假 launch，
//    否则测试会真的启动 wt/powershell + claude（spec §6 第 4 项）
REGISTER_TEST(claude_module_run_button_headless,
              "claude_module: 「运行」按钮存在 + 无选中 toast + 假 launch 被调用一次", {
    TestEnv env;
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_run");
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };

    int launchCalls = 0;
    platform::terminal::LaunchSpec got;
    env.svc->setTerminalLauncher([&](const platform::terminal::LaunchSpec& s) {
        ++launchCalls;
        got = s;
        return core::Result<void>::ok();
    });
    // runProfile 会真起代理：端口用动态空闲口（写死端口在多 worktree 下会撞）
    const int port = pickFreePort();
    DOCTEST_REQUIRE(port != 0);
    env.svc->setConfiguredProxyPort(port);

    // 第 6 键存在且可点（无 profile 时：无选中 → toast「请先选择 Profile」，不启动）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.act.run") != nullptr);
        DOCTEST_CHECK(ui.find("claude.act.run.bg") != nullptr);
        DOCTEST_CHECK(ui.find("claude.act.run.hint") != nullptr);   // 副提示（D-1）
        DOCTEST_CHECK(click(ui, "claude.act.run.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* msg = ui.find("claude.toast.message");
        DOCTEST_REQUIRE(msg != nullptr);
        DOCTEST_CHECK(msg->text.find("请先选择 Profile") != std::string::npos);
        DOCTEST_CHECK_EQ(launchCalls, 0);          // 无选中不开会话
        DOCTEST_CHECK(!env.svc->isProxyRunning()); // 也没起代理
    }

    // 有 profile 且选中 → 点击后假 launch 被调用一次、参数正确、代理已起
    auto a = env.svc->createProfile("运行目标", "https://run.example.com",
                                    "sk-ant-run-ui-0001", "");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(env.svc->setActiveProfile(a.value().id).isOk());
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.profile.0.bg"));   // 选中
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.act.run.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(launchCalls, 1);
        DOCTEST_CHECK_EQ(got.title, std::string("运行目标 · Claude Code"));
        DOCTEST_CHECK_EQ(got.program, std::string("claude"));
        DOCTEST_CHECK(env.svc->isProxyRunning());          // 顺带把代理拉起来了
        const auto* msg = ui.find("claude.toast.message");
        DOCTEST_REQUIRE(msg != nullptr);
        // D-9 的提示文案（会话不受 DevDesk 管理）
        DOCTEST_CHECK(msg->text.find("不受 DevDesk 管理") != std::string::npos);
    }
    cleanupKey(a.value().id);
    env.svc->stopProxy();
    return true;
})
```

`tests/test_modules/test_claude_ui.cpp` 顶部 include 补一行：

```cpp
#include "platform/terminal_launch.h"
```

- [ ] **Step 2: 跑一次（预期失败）**

```bash
bash scripts/build-test-run.sh --no-run --filter claude_ui
```

Expected: **编译通过但用例红** —— `missing clickable: claude.act.run.bg`（按钮还没加）。**不要**在看到红之前就点运行按钮去试真机。

- [ ] **Step 3: 实现 UI（逐字）**

`src/module_claude_proxy/claude_module.cpp` 三处：

(a) `:754-756`（5 键均分 → 6 键）：

```cpp
                                const float actGap = 12.0f;
                                // 6 键均分（设计 §4.1，M5 + profile-run：启用/测试连通/
                                // 编辑/删除/复制/运行）——按钮数改一处，gap 数也要跟着改
                                const float actW = (w0 - actGap * 6.0f) / 6.0f;
```

(b) 在「复制」按钮 `.build();`（`:867`）之后、`.content([&] { ... })` 收尾之前，追加第 6 键（样式与文案逐字对齐同区的 `claude.act.dup`）：

```cpp
                                        // 运行（第 6 键，2026-09-28 profile-run 设计
                                        // D-1/D-2/D-4）：在新终端里用该 Profile 打开
                                        // 一个 claude 会话
                                        components::button(ui, "claude.act.run")
                                            .size(actW, 36.0f)
                                            .text("运行")
                                            .fontSize(t.sizes.fontSizeSmall)
                                            .colors(t.colors.surfaceElevated,
                                                    t.colors.hover, t.colors.active)
                                            .textColor(t.colors.textPrimary)
                                            .radius(t.sizes.radius)
                                            .border(1.0f, t.colors.borderDefault)
                                            .onClick([this, shownId] {
                                                if (shownId == 0) {
                                                    showToast("运行失败",
                                                              "请先选择 Profile",
                                                              true);
                                                    return;
                                                }
                                                doRun(shownId);
                                            })
                                            .build();
```

(c) 在按钮组 `.build();`（`:869`）之后、分隔线（`:872` 的 `claude.divider`，`actY + 60.0f`）之前，插入副提示（**占按钮行 36px 底到分隔线 60px 之间的既有空隙，不挤动下方"最近调用"表**——该表末行与「查看全部」已贴面板底，不能再下移）：

```cpp
                                // 「运行」副提示（设计 §6：D-1 副作用必须明示；§3 措辞
                                // 按「选中 ≠ 激活才切」表述，避免用户以为每次都改全局）
                                ui.text("claude.act.run.hint")
                                    .position(x0, actY + 40.0f)
                                    .size(w0, 16.0f)
                                    .text("以该 Profile 打开新的 Claude Code 会话"
                                          "（起始目录 = 用户主目录）；未激活会先切为激活")
                                    .fontSize(11.0f)
                                    .lineHeight(16.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
```

(d) 操作区（`void doSetActive(int64_t id)`（`:2494`）之后，与 `doDuplicate`（`:2508`）/`doTestConnection`（`:2545`）同区）加 `doRun`：

```cpp
    // 运行：确保代理 + 必要时切激活 + 在终端里开一个新的 claude 会话（D-1）
    void doRun(int64_t id) {
        ClaudeService* svc = ClaudeService::global();
        if (svc == nullptr) {
            showToast("运行失败", "服务未就绪（ClaudeService 未注册）", true);
            return;
        }
        if (id == 0) {
            showToast("运行失败", "请先选择 Profile", true);
            return;
        }
        std::string hint;
        auto r = svc->runProfile(id, &hint);
        if (r.isFail()) {
            showToast("运行失败", r.errorMessage(), true);
            return;
        }
        // 成功：D-5 的退化提示（hint 非空时）+ D-9 的「会话不受 DevDesk 管理」
        const std::string head = hint.empty()
            ? std::string("已在新终端里打开 Claude Code 会话（起始目录 = 用户主目录）。")
            : (hint + std::string("。"));
        showToast("已启动",
                  head + "运行中的会话不受 DevDesk 管理，停代理/退出后可能失败。",
                  false);
        dataDirty_ = true;   // 可能刚起代理 / 刚切激活 → 下帧重查（状态点、ACTIVE 徽标）
    }
```

- [ ] **Step 4: 跑一次（预期通过）**

```bash
bash scripts/build-test-run.sh --no-run --filter claude_ui
```

Expected: `✅ 测试全绿`（含既有的重复键/表单/调用记录用例——**特别是** `claude.act.dup`（`:1740`）与详情页「最近调用」相关断言：若它们红，说明副提示插错了位置（把表挤下去了），回到 (c) 只占 40..56 那段空隙）。

- [ ] **Step 5: CHANGELOG 条目**

在 `CHANGELOG.md` 顶部批次节内（`### [AuthField]`（`:13`）之前）插入，sha 用 Step 6 的代码 commit 短 sha（故**先 commit 代码再补本条**）：

```markdown
### [Profile-Run] `<sha>` —— Profile「运行」：用该 profile 在新终端打开 Claude Code
- 详情页第 6 键「运行」：确保本地代理在跑（用 `configuredProxyPort()`）→ 目标 profile 非激活则先切激活 → 在新终端里用该 profile 的托管配置打开 `claude` 会话。密钥零落盘、零进命令行/进程环境（会话走本地代理，真实密钥留 Keychain 由代理转发注入）。
- 新增平台层 `platform::terminal`（`src/platform/terminal_launch*.{h,cpp}`）：Windows = Windows Terminal 优先（`%LOCALAPPDATA%\Microsoft\WindowsApps\wt.exe` 探测），缺失退化传统控制台并提示；macOS = 写 `appDataDir()/run-scripts/run-<id>-<ts>.command` → `open`（跟随默认终端）；其他平台 `NotImplemented`。命令文本拼装为跨平台纯函数，单测逐字比对（含 `-EncodedCommand` 的 UTF-16LE+base64 载荷解码回读）。
- 已知边界（spec §10）：起始目录默认用户主目录（首次进入新目录会弹 workspace trust）；运行中的会话不受 DevDesk 管理（停代理/退出只提示）；macOS 侧本机无 Mac，未实机验证。
```

- [ ] **Step 6: Commit**

```bash
git add src/module_claude_proxy/claude_module.cpp tests/test_modules/test_claude_ui.cpp
git commit -m "feat(claude): 「运行」第 6 键 + doRun + 副提示 + 无头用例"
# 取短 sha 回填 CHANGELOG 后再提交文档
git add CHANGELOG.md
git commit -m "docs(changelog): 回填 [Profile-Run] 条目"
```

---

## 验收（实施线与复核线都跑）

1. **三条定向用例全绿**（每条都要给出命令与输出统计）：

```bash
bash scripts/build-test-run.sh --no-run --filter terminal_launch   # 期望：✅ 测试全绿（7 例）
bash scripts/build-test-run.sh --no-run --filter claude_service    # 期望：✅ 测试全绿（新增 4 例 + 既有）
bash scripts/build-test-run.sh --no-run --filter claude_ui         # 期望：✅ 测试全绿（新增 1 例 + 既有）
```

2. **三处测试文件登记校验**（`tests/CMakeLists.txt` 是显式列表、无 GLOB）：

```bash
grep -n "test_terminal_launch\|test_claude_service\|test_claude_ui" tests/CMakeLists.txt
```

Expected: 三条都必须在列 —— ① `test_platform/test_terminal_launch.cpp` **新增一行**（本计划唯一需要动的登记）；② `test_modules/test_claude_service.cpp`、③ `test_modules/test_claude_ui.cpp` **本就在列**（`tests/CMakeLists.txt:39`、`:42`），改前改后各 grep 一次确认行仍在、未被误删。三者缺一 = 对应用例静默不参与编译。

3. **spec §6 清单逐条对账**：`terminal_launch.h` + 三个平台 + `_common.cpp` + platform CMake + tests CMake + `claude_service.h/.cpp` + `claude_module.cpp` + 三个测试文件 + CHANGELOG —— 逐行核 diff，**无清单外文件**（`git status --porcelain` 只应出现上述路径）。

4. **平台纪律复核**：`grep -rn "ifdef _WIN32\|ifdef __APPLE__" src/ --include=*.cpp --include=*.h` 只应命中 `src/platform/*_win.cpp` / `*_macos.cpp` / `*_posix.cpp`（`terminal_launch_common.cpp` 与 `claude_service.cpp`、`claude_module.cpp` **零命中**）。

5. **密钥零落盘复核**：`grep -rn "apiKeyPlain\|loadApiKey\|--settings\|CLAUDE_CONFIG_DIR" src/platform/terminal_launch*` **零命中**；生成的 PowerShell 正文与 `.command` 正文里只出现 `Set-Location` / `cd` / `claude`（纯函数用例的逐字断言已锁）。

6. `git status` 干净、commit 全部显式 pathspec（无 `git add -A`）、每个 Task 一个原子 commit。

7. **CHANGELOG 回填**：`[Profile-Run]` 条目的 sha 与代码 commit 的短 sha 一致（`git log --oneline -1 <代码 commit>`）。

## 未验证项（不得声称已验证；按下列人工点验）

| # | 未验证项 | 为什么 | 人工点验步骤 |
|---|----------|--------|--------------|
| 1 | **Windows wt 那一跳**（wt 真开窗、标题正确、`claude` 起来且能用） | 单元测试只覆盖命令文本；真实 `launch()` 会真开终端窗口，不能进自动化用例 | ① `bash scripts/build-test-run.sh`（构建 + 启 app）② Claude 页选中一个 profile → 点「运行」③ 目视：新窗口出现、标签标题 = `<profile 名> · Claude Code`、PowerShell 停在用户主目录、`claude` 交互正常 ④ 在会话里发一条消息 → DevDesk 代理日志出现 `/v1/messages` 记录（证明托管生效、密钥未落盘） |
| 2 | **wt 缺失退化分支**（传统控制台 + 提示） | 同上；且本机装了 wt | 临时把 `%LOCALAPPDATA%\Microsoft\WindowsApps\wt.exe` 改名（或设 `LOCALAPPDATA` 指向空目录启动 app）→ 点「运行」→ 目视：弹出传统控制台窗口 + toast 明示「未检测到 Windows Terminal，已用传统控制台打开会话」；验完**改回** |
| 3 | **macOS 侧全部**（`.command` 写入 / `chmod 0755` / `open` 跟随默认终端 / 陈旧清扫 / 失败路径 toast） | 本机无 Mac，`terminal_launch_macos.cpp` 与对应分支**根本没进构建**（CMake `elseif(APPLE)` 在本机为假），只有静态评审 | 在 Mac 上：① 点「运行」→ 默认终端打开新窗口、执行 `cd` 到主目录并起 `claude` ② `ls -l ~/Library/Application\ Support/DevDesk/run-scripts/` 确认 `.command` 存在且 `-rwxr-xr-x` ③ 把 `appDataDir()/run-scripts` 改成不可写（`chmod 500`）→ 点「运行」→ toast 报错且**带路径** ④ 放一个 25 小时前的假 `run-*.command` → 点「运行」→ 确认它被清掉 |
| 4 | `terminal_launch_posix.cpp`（`NotImplemented`） | 非目标平台，本机不编译 | 交叉编译/在 Linux 上跑 `--filter terminal_launch`，期望 `terminal_launch_posix_not_implemented` 绿 |
| 5 | 副提示文案在最小窗口宽度（1200×720）下不溢出 | 无头用例只断言节点存在，不断言排版 | 把窗口拖到最小宽度目视副提示与「最近调用」表均完整、无重叠 |

> 本计划**已经在自动化用例里锁住的**（不要重复声称未验证）：wt/传统控制台两套 argv 与正文、`-EncodedCommand` 载荷（解码回读逐字相等）、Windows 参数引号经 `CommandLineToArgvW` 回读一致、macOS `.command` 正文与路径形状、`homeDir` 非空、`runProfile` 的代理/激活前置与四条参数、无选中 toast 与假 launch 调用次数。

## 与 spec 的偏离（刻意项，逐条给理由）

1. **新增 `src/platform/terminal_launch_common.h`**（spec §6 的文件表只列了 `terminal_launch.h` 与四个 `.cpp`）。理由：纯函数是命令拼装的实现细节，不宜进业务层可见的 `terminal_launch.h`；又不该进 win-only 头（会重演审计 [17]「声明在公共头、定义只在某平台」的契约破裂）。`_common.cpp` 无条件编译 → 这些符号在所有平台都存在（即 spec「编码器放这里以便单测」的直接落地）。
2. **`terminal_launch.h` 在 spec §4 逐字内容之外多了两个平台查询**：`homeDir()` 与 `windowsTerminalMissing()`。理由：spec D-3 要求起始目录 = 用户主目录（业务层零 `#ifdef` → 只能由平台层提供），spec D-5/§5 要求「wt 缺失由**调用方**提示」（`launch()` 签名已被 spec 固定为单参、无出参），两者都需要一个平台层查询入口。`launch()` 的签名与 `LaunchSpec` 的字段**逐字未动**。
3. **macOS 脚本文件名的 `<id>` 段用 `stableToken(title)` 的 8 位 hex**：spec §4 表/§6 写的是 `run-<profileId>-<时间戳>.command`，但 spec §4 逐字的 `LaunchSpec`（title/workingDir/program/args）**不含 profile id**，服务层无法把它传进来。按「接口以 §4 逐字为准」，用 title 的稳定短哈希填充该槽位：同一 profile 稳定、不同 profile 不撞（同名校验在 DB 层），文件名形状与 spec 一致。
4. **D-9 的提示落在「运行」成功 toast + 副提示**（spec §5 只写「停代理/退出时提示文案」，§6 未点名落点）：改停代理路径属清单外改动，故把「运行中的会话不受 DevDesk 管理」并入启动成功 toast（用户此刻正在建立会话，提示最可操作）。若复核认为必须落在停代理路径，按 spec 另行开 Task。
5. **`runProfile` 的出参命名为 `hint`**（spec §6 写 `std::string* errHint`，并明示「或等价」）：它承载的是**非致命提示**（wt 退化）而非错误，错误走 `Result::errorMessage`。

## 自检记录（写计划时已核对）

- 计划中引用的 file:line 锚点均为本次实际读到的位置（`claude_module.cpp:756` 的 `actW` 5 键算式、`:849` 的 `claude.act.dup`、`:873` 的 `actY + 60.0f` 分隔线、`:495` 的 `w0 = rightW - 56.0f`、`:2494/2508/2545` 的 `doSetActive/doDuplicate/doTestConnection`；`claude_service.h:263` 的 `configuredProxyPort`、`:271` 的 `setSettingsSyncPaths`、`:254-256` 的 `isProxyRunning/proxyPort`；`claude_service.cpp:657/1365/1421/1438` 的 `setActiveProfile/startProxy/isProxyRunning/setConfiguredProxyPort` 函数锚点（1438 为该函数收尾 `}`）、`:1677` 的 `syncTakeOver`；`tests/CMakeLists.txt:20` 的 `test_platform/test_file_open.cpp`；`src/platform/CMakeLists.txt:1-34` 的三支结构；`src/core/paths.h:12` 的 `appDataDir`）。
- `claude.act.run` 与既有 5 键 id（`claude.act.enable` / `.test` / `.edit` / `.del` / `.dup`）**不冲突**；`.bg` 后缀是 `components::button` 的点击节点约定（同区其他键同款）。
