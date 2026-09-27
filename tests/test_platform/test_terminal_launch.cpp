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

    // 回归闸：派发给 CreateProcessW 的 exe 必须是**绝对路径**。
    // 裸名 "powershell.exe" 走 lpApplicationName 时不查 PATH（只按当前驱动器 +
    // 当前目录补全）→ 实测 GetLastError=2，退化分支窗口根本起不来。
    const std::string exe = platform::terminal::fallbackPowerShellExe("");
    DOCTEST_CHECK(exe.find(':') != std::string::npos);             // 含盘符
    DOCTEST_CHECK(exe.find("System32") != std::string::npos);      // 在系统目录下
    DOCTEST_CHECK_EQ(exe,
                     std::string("C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe"));
    // <systemRoot> 原样铺开（含尾分隔符也归一，不产生 `\\` 双斜杠）
    DOCTEST_CHECK_EQ(platform::terminal::fallbackPowerShellExe("D:\\Win"),
                     std::string("D:\\Win\\System32\\WindowsPowerShell\\v1.0\\powershell.exe"));
    DOCTEST_CHECK_EQ(platform::terminal::fallbackPowerShellExe("D:\\Win\\"),
                     std::string("D:\\Win\\System32\\WindowsPowerShell\\v1.0\\powershell.exe"));
    return true;
})

// ---- ① wt 标题里的 `;` 必须转义（否则被当命令分隔符，标题后半被吞）----
// 本机实测：`wt -w new --title 'A;B' …` 窗口标题只剩 "B"；`--title 'A\;B'` 才是 "A;B"。

REGISTER_TEST(terminal_launch_wt_title_semicolon_escape,
              "terminal_launch: wt --title 里的 `;` 转义为 `\\;`（其余字符原样）", {
    using platform::terminal::escapeWtTitle;
    DOCTEST_CHECK_EQ(escapeWtTitle("官方 · Claude Code"), std::string("官方 · Claude Code"));
    DOCTEST_CHECK_EQ(escapeWtTitle("A;B"), std::string("A\\;B"));
    DOCTEST_CHECK_EQ(escapeWtTitle("A;B;C"), std::string("A\\;B\\;C"));
    DOCTEST_CHECK_EQ(escapeWtTitle(";"), std::string("\\;"));
    DOCTEST_CHECK_EQ(escapeWtTitle(""), std::string(""));
    // 反斜杠本身不二次转义（只处理 `;`，不引入未实测的规则）
    DOCTEST_CHECK_EQ(escapeWtTitle("C:\\dir"), std::string("C:\\dir"));

    // 拼进 wt 参数向量：--title 位（第 7 项）必须是已转义的值
    const std::vector<std::string> argv = platform::terminal::buildWindowsLaunchArgs(
        makeSpec("甲;乙 · Claude Code", "C:/Users/demo"));
    DOCTEST_CHECK_EQ(argv[6], std::string("甲\\;乙 · Claude Code"));
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
