// src/platform/terminal_launch_common.h
// terminal_launch 的跨平台纯函数声明。单独一个头（不塞进平台无关的 terminal_launch.h 或 win-only 头）：
// 这些是命令文本拼装的实现细节，业务层只看 terminal_launch.h；实现放 terminal_launch_common.cpp，
// 该文件**无条件编译**，故符号在所有平台都存在（不重演「声明在公共头、定义只在某平台」的契约破裂）。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "terminal_launch.h"
#include "file_open_common.h"

namespace platform::terminal {

// 引号 / 转义

// POSIX shell 单引号转义（' 拆成 '\''）：路径含空格/引号不破坏命令边界。已下沉为
// platform::file_open::shellSingleQuote（定义在 file_open_common.cpp 全平台常编），此处经 using 复用。
using platform::file_open::shellSingleQuote;

// PowerShell 单引号转义（' → ''）：PS 单引号串内唯一的转义形式。
std::string powershellSingleQuote(const std::string& s);

// Windows 命令行参数加引号（CreateProcessW 的 lpCommandLine 解析规则，CommandLineToArgvW 的逆运算）：
// 不含 空格/Tab/双引号 → 原样；否则整体加双引号，连续反斜杠仅在「引号前」与「串尾」翻倍。
std::string quoteWindowsArg(const std::string& arg);

// argv（不含引号）→ lpCommandLine 单串（各段经 quoteWindowsArg，空格连接）
std::string joinWindowsCommandLine(const std::vector<std::string>& argv);

// 编码（自实现，不用 cpp-httplib 的 base64）

// UTF-8 文本 → UTF-16LE 字节流（不含 BOM；非法序列按 U+FFFD 兜底，不抛错）
std::string utf8ToUtf16LEBytes(const std::string& utf8);

// 原始字节 → 标准 base64（含 '=' 填充）
std::string base64Encode(const std::string& raw);

// PowerShell -EncodedCommand 载荷 = base64(UTF-16LE(正文))
std::string encodePowerShellCommand(const std::string& body);

// 命令文本（写死，供实现与单测逐字对照）

// PowerShell 正文：Set-Location 到起始目录 + 换行 + 一行「程序 + 各参数」（逐个 powershellSingleQuote）；
// args 含 --settings 时追加一行 Remove-Item 自删临时 settings 文件。正文**不含任何密钥**（密钥在
// --settings 指向的临时文件里）。
std::string buildPowerShellBody(const LaunchSpec& spec);

// Windows Terminal 分支参数向量（引号由 joinWindowsCommandLine 加）：wt.exe -w new -d <工作目录>
// --title <标题> powershell.exe -NoExit -NoProfile -EncodedCommand <BASE64_UTF16LE>
std::vector<std::string> buildWindowsLaunchArgs(const LaunchSpec& spec);

// wt 缺失时的退化分支：powershell.exe -NoExit -NoProfile -EncodedCommand <BASE64_UTF16LE>
std::vector<std::string> buildWindowsFallbackArgs(const LaunchSpec& spec);

// 退化分支派发给 CreateProcessW 的 PowerShell **绝对路径**：<systemRoot>\System32\WindowsPowerShell\
// v1.0\powershell.exe。为什么必须绝对路径：lpApplicationName 传部分名时只按「当前驱动器+当前目录」补全、
// **不走 PATH**（MSDN），实测裸名落空 GetLastError=2 窗口起不来。systemRoot 空 → 回落 C:\Windows。
std::string fallbackPowerShellExe(const std::string& systemRoot);

// wt 的 `;` 是命令分隔符：标题原样交给 wt 会被截断（实测 `--title 'A;B'` 标题只剩 "B"），故拼参数前
// 对 `;` 做 `\;` 转义（实测 `--title 'A\;B'` → "A;B"）。只在 wt 参数向量调用，别过度转义。
std::string escapeWtTitle(const std::string& title);

// macOS .command 正文：cd '<工作目录>' && 「claude + 各参数」+ 换行；args 含 --settings 时追加
// `trap 'rm -f "<settings>"' EXIT` 自删。**不能 exec**：exec 替换 shell 进程，EXIT 陷阱随之消失、自删就没了。
std::string buildMacCommandBody(const LaunchSpec& spec);

// 启动清扫：删 run-settings/ 里 24h 前的陈旧 run-*.json（会话正常退出由正文自删，这里兜底崩溃/强杀
// 留下的孤儿）。镜像 macOS 侧 run-scripts/ 的 24h sweep；best-effort，单条失败忽略不阻断启动。
void sweepStaleRunSettingsFiles(const std::filesystem::path& dir);

// 稳定短标识（8 位 hex，FNV-1a 32）：macOS 脚本文件名的 <id> 槽位。LaunchSpec 不含 profile id，故用
// title 哈希：同一 profile 稳定、不同 profile 不撞。
std::string stableToken(const std::string& text);

// macOS 脚本路径：<dir>/run-<稳定标识>-<时间戳>.command（手工用 '/' 拼接，纯函数）
std::string runScriptPath(const std::string& dir, const LaunchSpec& spec,
                          const std::string& timestamp);

} // namespace platform::terminal
