// tests/test_platform/test_terminal_launch.cpp
// platform::terminal 覆盖：
//   ① 跨平台纯函数（terminal_launch_common.cpp）：wt/powershell 参数向量、
//      PowerShell 正文、UTF-16LE→base64 载荷（**解码后**与期望正文逐字比对）、
//      macOS .command 正文与脚本路径、Windows 参数引号（空格/中文/反斜杠）。
//   ② Windows 侧额外做 CommandLineToArgvW 回读：拼出的命令行必须能被系统还原
//      成原参数向量——引号规则的黄金判据（UTF-16 转换用 Win32 作参照实现）。
//   ③ 平台失败路径：posix 返回 NotImplemented（仅非 Windows/非 Apple 平台编译，
//      本机不验证）。
#include "doctest.h"

#include "platform/terminal_launch.h"
#include "platform/terminal_launch_common.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
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

// base64 解码（**仅测试用**：生产侧只有编码器，解码器放测试里避免为测试增生产代码）
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

    // 回归闸：派发给 CreateProcessW 的 exe 必须是**绝对路径**。裸名
    // "powershell.exe" 走 lpApplicationName 时不查 PATH（只按当前驱动器 + 当前目录
    // 补全），退化分支窗口根本起不来（GetLastError=2）。
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

// wt 的命令行把 `;` 当命令分隔符：`wt -w new --title 'A;B' …` 的窗口标题只剩
// "B"，必须写成 `A\;B` 才是 "A;B"。

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

    // macOS .command 正文：cd 单引号包裹；路径含单引号 → '\''。
    // 正文里**不能有 exec**：自删靠 EXIT 陷阱，exec 替换掉 shell 就再没有 EXIT、
    // run-settings 的临时文件永久残留。别把 exec「优化」回来。
    DOCTEST_CHECK_EQ(platform::terminal::buildMacCommandBody(makeSpec("t", "/Users/demo")),
                     std::string("cd '/Users/demo' && claude\n"));
    DOCTEST_CHECK_EQ(platform::terminal::buildMacCommandBody(makeSpec("t", "/Users/my dir")),
                     std::string("cd '/Users/my dir' && claude\n"));
    DOCTEST_CHECK_EQ(platform::terminal::buildMacCommandBody(makeSpec("t", "/Users/o'brien")),
                     std::string("cd '/Users/o'\\''brien' && claude\n"));
    return true;
})

REGISTER_TEST(terminal_launch_bodies_with_settings_args,
              "terminal_launch: 正文带 --settings 参数 + 退出后自删段（PS Remove-Item / macOS trap）", {
    // Windows：claude --settings '<路径>' + Remove-Item 自删行
    const LaunchSpec ps = makeSpec("t", "C:/Users/demo");
    platform::terminal::LaunchSpec psArgs = ps;
    psArgs.args = {"--settings", "C:/Users/demo/AppData/Roaming/DevDesk/run-settings/run-3-1730000000.json"};
    DOCTEST_CHECK_EQ(
        platform::terminal::buildPowerShellBody(psArgs),
        std::string("Set-Location -LiteralPath 'C:/Users/demo'\n"
                    "claude '--settings' 'C:/Users/demo/AppData/Roaming/DevDesk/run-settings/run-3-1730000000.json'\n"
                    "Remove-Item -LiteralPath 'C:/Users/demo/AppData/Roaming/DevDesk/run-settings/run-3-1730000000.json' -Force -ErrorAction SilentlyContinue"));
    // 载荷 = 正文的 UTF-16LE + base64（解码后逐字相等，与 wt 分支同一编码链）
    const std::string body = platform::terminal::buildPowerShellBody(psArgs);
    DOCTEST_CHECK_EQ(utf16LEToUtf8ForTest(base64DecodeForTest(
                         platform::terminal::encodePowerShellCommand(body))),
                     body);

    // macOS：cd + claude --settings + trap 自删（trap 行在 claude 退出后执行）
    const LaunchSpec mac = makeSpec("t", "/Users/demo");
    platform::terminal::LaunchSpec macArgs = mac;
    macArgs.args = {"--settings", "/tmp/run-settings/run-3-1730000000.json"};
    DOCTEST_CHECK_EQ(
        platform::terminal::buildMacCommandBody(macArgs),
        std::string("cd '/Users/demo' && claude '--settings' '/tmp/run-settings/run-3-1730000000.json'\n"
                    "trap 'rm -f \"/tmp/run-settings/run-3-1730000000.json\"' EXIT\n"));

    // 无 --settings → 无自删段（既不误删普通参数文件，也不画蛇添足）；
    // noSettings 继承 ps 的 Windows 风格工作目录（C:/Users/demo）
    platform::terminal::LaunchSpec noSettings = ps;
    noSettings.args = {"--foo", "bar"};
    DOCTEST_CHECK_EQ(platform::terminal::buildPowerShellBody(noSettings),
                     std::string("Set-Location -LiteralPath 'C:/Users/demo'\nclaude '--foo' 'bar'"));
    DOCTEST_CHECK_EQ(platform::terminal::buildMacCommandBody(noSettings),
                     std::string("cd 'C:/Users/demo' && claude '--foo' 'bar'\n"));
    return true;
})

REGISTER_TEST(terminal_launch_sweep_run_settings_24h,
              "terminal_launch: sweepStaleRunSettingsFiles 只删 24h 前的 run-* 文件", {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("devdesk_test_run_settings_sweep_" +
         std::to_string(static_cast<long long>(std::time(nullptr))));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    DOCTEST_REQUIRE(std::filesystem::is_directory(dir));

    const auto touch = [&](const std::string& name, std::chrono::hours age) {
        const std::filesystem::path f = dir / name;
        {
            std::ofstream out(f, std::ios::binary | std::ios::trunc);
            out << "{}";
        }
        // 写过去的 mtime 只能靠 file_time_type（MinGW 下映射到 SetFileTime），
        // 直接设置系统时间会污染整台机器。
        std::filesystem::last_write_time(
            f, std::filesystem::file_time_type::clock::now() - age, ec);
        return f;
    };
    const auto oldFile = touch("run-3-1730000000.json", std::chrono::hours(25));
    const auto oldTmp = touch("run-3-1730000000.json.devdesk-tmp", std::chrono::hours(25));
    const auto freshFile = touch("run-9-1730000001.json", std::chrono::hours(1));
    const auto foreign = touch("user-file.json", std::chrono::hours(25));

    platform::terminal::sweepStaleRunSettingsFiles(dir);

    DOCTEST_CHECK(!std::filesystem::exists(oldFile, ec));
    // 原子写的 .devdesk-tmp 残留也带 run- 前缀，同属 24h 清扫对象
    DOCTEST_CHECK(!std::filesystem::exists(oldTmp, ec));
    DOCTEST_CHECK(std::filesystem::exists(freshFile, ec));   // 24h 内保留
    DOCTEST_CHECK(std::filesystem::exists(foreign, ec));     // 非 run- 前缀不碰

    std::filesystem::remove_all(dir, ec);
    return true;
})

REGISTER_TEST(terminal_launch_windows_arg_quoting,
              "terminal_launch: 参数引号（空格加引号/尾随反斜杠翻倍/内嵌引号/中文免引号）", {
    using platform::terminal::quoteWindowsArg;
    DOCTEST_CHECK_EQ(quoteWindowsArg("plain"), std::string("plain"));
    DOCTEST_CHECK_EQ(quoteWindowsArg(""), std::string("\"\""));
    DOCTEST_CHECK_EQ(quoteWindowsArg("C:/Program Files/x"),
                     std::string("\"C:/Program Files/x\""));
    // 尾随反斜杠在带引号时翻倍——不翻倍的话它会转义掉闭合引号，参数边界整个丢失
    DOCTEST_CHECK_EQ(quoteWindowsArg("C:\\Program Files\\"),
                     std::string("\"C:\\Program Files\\\\\""));
    // 内嵌双引号 → \"
    DOCTEST_CHECK_EQ(quoteWindowsArg("say \"hi\""),
                     std::string("\"say \\\"hi\\\"\""));
    // 中文（无空格）不需要引号
    DOCTEST_CHECK_EQ(quoteWindowsArg("D:/笔记/目录"), std::string("D:/笔记/目录"));
    return true;
})

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

REGISTER_TEST(terminal_launch_home_dir_and_wt_probe,
              "terminal_launch: homeDir 非空 + windowsTerminalMissing 可调用", {
    DOCTEST_CHECK(!platform::terminal::homeDir().empty());
    (void)platform::terminal::windowsTerminalMissing();   // 不崩即过（值随机器而异）
    return true;
})

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

#if !defined(_WIN32) && !defined(__APPLE__)

REGISTER_TEST(terminal_launch_posix_not_implemented,
              "terminal_launch: posix launch → NotImplemented（非目标平台）", {
    auto r = platform::terminal::launch(makeSpec("t", "/tmp"));
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK(r.errorCode() == core::ErrorCode::NotImplemented);
    return true;
})

#endif  // !_WIN32 && !__APPLE__
