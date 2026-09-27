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
