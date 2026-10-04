// src/platform/terminal_launch_common.cpp
// terminal_launch 的跨平台纯函数：命令文本拼装的全部逻辑集中在这里，平台实现只负责系统调用——两套
// 正文拼装（Windows wt/PowerShell、macOS .command）在当前平台就能被单测覆盖。本文件**无条件编译**。
#include "terminal_launch_common.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <system_error>

namespace platform::terminal {

namespace {

// 从启动参数找 --settings 的下一项（临时 settings 文件路径）。自删段只跟这个文件走；找不到 → 空串
//（正文退化为「程序 + 参数」、无自删段）
std::string settingsPathArg(const std::vector<std::string>& args) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--settings") return args[i + 1];
    }
    return {};
}

// 「程序 + 各参数」拼成一行命令文本；arg 逐个经 quoteOne 包裹（调用方传各自平台的单引号转义）
std::string commandLine(const LaunchSpec& spec,
                        std::string (*quoteOne)(const std::string&)) {
    std::string out = spec.program;
    for (const auto& arg : spec.args) {
        out += ' ';
        out += quoteOne(arg);
    }
    return out;
}

} // namespace

// 引号 / 转义

// shellSingleQuote 已下沉为 platform::file_open::shellSingleQuote，经头文件 using 复用，不再自持。

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
    // 不含空格/Tab/双引号 → 原样（中文等非 ASCII 字节都 >= 0x80，不会被误当分隔符或引号）
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

// 编码

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

// 命令文本

std::string buildPowerShellBody(const LaunchSpec& spec) {
    // 正文本身**不含任何密钥**（密钥在 --settings 指向的临时文件里，正文只引用它的路径）
    std::string body = "Set-Location -LiteralPath " +
                       powershellSingleQuote(spec.workingDir) + "\n";
    body += commandLine(spec, powershellSingleQuote);
    // 退出后自删：claude 退出 → 删临时 settings；强杀/崩溃留下的孤儿由 run-settings/ 24h 启动清扫兜底
    const std::string settingsPath = settingsPathArg(spec.args);
    if (!settingsPath.empty()) {
        body += "\nRemove-Item -LiteralPath " +
                powershellSingleQuote(settingsPath) +
                " -Force -ErrorAction SilentlyContinue";
    }
    return body;
}

std::string escapeWtTitle(const std::string& title) {
    // wt 命令行用 `;` 分隔多条命令 → 标题里的 `;` 必须写成 `\;`（实测 `--title 'A;B'` 标题只剩 "B"，
    // `--title 'A\;B'` 才得 "A;B"）。只处理 `;`，其余字符（含 `\`）原样交给 wt——不引入未实测的转义规则。
    std::string out;
    out.reserve(title.size() + 4);
    for (const char c : title) {
        if (c == ';') out += "\\;";
        else out += c;
    }
    return out;
}

std::string fallbackPowerShellExe(const std::string& systemRoot) {
    std::string root = systemRoot;
    while (!root.empty() && (root.back() == '\\' || root.back() == '/')) {
        root.pop_back();   // 去尾分隔符，避免拼出 `C:\Windows\\System32\…`
    }
    if (root.empty()) root = "C:\\Windows";   // Windows 默认安装位置（环境变量缺失兜底）
    return root + "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
}

std::vector<std::string> buildWindowsLaunchArgs(const LaunchSpec& spec) {
    std::vector<std::string> argv;
    argv.push_back("wt.exe");
    argv.push_back("-w");
    argv.push_back("new");
    argv.push_back("-d");
    argv.push_back(spec.workingDir);
    argv.push_back("--title");
    argv.push_back(escapeWtTitle(spec.title));   // `;` 是 wt 的命令分隔符，不转义会截断标题
    argv.push_back("powershell.exe");
    argv.push_back("-NoExit");       // claude 退出后保留窗口，回滚内容可见
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
    // cd + （无 exec 的）claude --settings '<临时文件>'；脚本本身不含密钥。为什么去掉 exec：自删靠 EXIT
    // 陷阱——exec 会替换 shell 进程、trap 随之消失，自删语义就没了。
    std::string body = "cd " + shellSingleQuote(spec.workingDir) + " && " +
                       commandLine(spec, shellSingleQuote) + "\n";
    const std::string settingsPath = settingsPathArg(spec.args);
    if (!settingsPath.empty()) {
        // 退出后自删：trap 行在 claude 退出后删临时 settings；强杀/崩溃留下的孤儿由 24h 启动清扫兜底
        body += "trap 'rm -f \"" + settingsPath + "\"' EXIT\n";
    }
    return body;
}

void sweepStaleRunSettingsFiles(const std::filesystem::path& dir) {
    // 镜像 terminal_launch_macos.cpp 的 sweepStaleScripts：删 24h 前修改的 run-*.json（含原子写残留的
    // .devdesk-tmp，名字也是 run- 前缀）。best-effort，单条失败忽略；目录不存在 → 构造失败 → 无操作。
    const auto now = std::filesystem::file_time_type::clock::now();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) return;
        // 只碰本功能创建的文件（run- 前缀）
        const std::string name = entry.path().filename().string();
        if (name.rfind("run-", 0) != 0) continue;
        std::error_code inner;
        const auto mt = std::filesystem::last_write_time(entry.path(), inner);
        if (inner) continue;
        if (now - mt > std::chrono::hours(24)) {
            std::filesystem::remove(entry.path(), inner);
        }
    }
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
