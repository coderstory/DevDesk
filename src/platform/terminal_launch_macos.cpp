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
#include <ios>
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
