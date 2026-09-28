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

#include <filesystem>
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

// PowerShell 正文：Set-Location 到起始目录 + 换行 + 一行「程序 + 各参数」
//（参数逐个 powershellSingleQuote 包裹）+ 〔args 含 --settings 时追加一行
// Remove-Item -LiteralPath '<settings 路径>' -Force -ErrorAction SilentlyContinue
// ——claude 退出后自删临时 settings 文件（profile-run §5）〕。
// 正文里**不含任何密钥**（D-1：密钥在 --settings 指向的临时文件里）
std::string buildPowerShellBody(const LaunchSpec& spec);

// Windows Terminal 分支的参数向量（不带引号；引号由 joinWindowsCommandLine 加）：
//   wt.exe -w new -d <工作目录> --title <标题> powershell.exe
//          -NoExit -NoProfile -EncodedCommand <BASE64_UTF16LE>
std::vector<std::string> buildWindowsLaunchArgs(const LaunchSpec& spec);

// wt 缺失时的退化分支参数向量（传统控制台）：
//   powershell.exe -NoExit -NoProfile -EncodedCommand <BASE64_UTF16LE>
std::vector<std::string> buildWindowsFallbackArgs(const LaunchSpec& spec);

// 退化分支（wt 缺失）派发给 CreateProcessW 的 PowerShell **可执行文件绝对路径**：
//   <systemRoot>\System32\WindowsPowerShell\v1.0\powershell.exe
// 为什么必须是绝对路径：CreateProcessW 的 lpApplicationName 传**部分名**（不含目录）
// 时只按「当前驱动器 + 当前目录」补全，**不走 PATH**（MSDN 原文 "The function will
// not use the search path"）——实测裸名 "powershell.exe" 落空 GetLastError=2
// (ERROR_FILE_NOT_FOUND)，窗口起不来。与 wt 分支同口径：探测与派发共用同一绝对路径。
// systemRoot 为空（环境变量读不到）→ 回落 Windows 默认安装位置 C:\Windows。
std::string fallbackPowerShellExe(const std::string& systemRoot);

// Windows Terminal 的 `;` 是**命令分隔符**：标题原样交给 wt 会被截断（实测
// `--title 'A;B'` → 窗口标题只剩 "B"），故拼参数前须对 `;` 做 wt 的 `\;` 转义
//（同机实测 `--title 'A\;B'` → 标题为 "A;B"，含空格的双引号包裹形态亦通过
// 探针验证）。只在 wt 参数向量里调用——传统控制台分支无此语义，不要过度转义。
std::string escapeWtTitle(const std::string& title);

// macOS .command 正文：cd '<工作目录>' && 「claude + 各参数」+ 换行；args 含
// --settings 时再追加一行 `trap 'rm -f "<settings 路径>"' EXIT`（claude 退出后
// 自删临时 settings 文件，profile-run §5）。**不能 exec**：exec 会替换 shell
// 进程，EXIT 陷阱随 shell 消失——自删语义就没了
std::string buildMacCommandBody(const LaunchSpec& spec);

// 启动清扫（profile-run 设计 §5）：删 run-settings/ 目录里 24h 前的陈旧
// run-*.json 临时 settings 文件——会话正常退出由终端正文自删，这里兜底上次
// 崩溃 / 强制结束留下的孤儿。镜像 macOS 侧 run-scripts/ 的 24h sweep 模式
//（terminal_launch_macos.cpp sweepStaleScripts）。best-effort：目录不存在 /
// 单条失败都忽略，不阻断本次启动
void sweepStaleRunSettingsFiles(const std::filesystem::path& dir);

// 稳定短标识（8 位 hex，FNV-1a 32）：macOS 脚本文件名里的 <id> 槽位。
// LaunchSpec（spec §4 逐字：title/workingDir/program/args）不含 profile id，
// 故用 title 的哈希：同一 profile 稳定、不同 profile 不撞（见末节「与 spec 的偏离」③）
std::string stableToken(const std::string& text);

// macOS 脚本路径：<dir>/run-<稳定标识>-<时间戳>.command（spec §4/§6 的文件名形状）。
// 手工用 '/' 拼接（纯函数，与宿主机路径分隔符无关——macOS 侧就是 '/'）
std::string runScriptPath(const std::string& dir, const LaunchSpec& spec,
                          const std::string& timestamp);

} // namespace platform::terminal
