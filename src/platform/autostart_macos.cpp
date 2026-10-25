// src/platform/autostart_macos.cpp
// macOS 开机自启动：~/Library/LaunchAgents/com.coderstory.devdesk.plist 写/删/查。
// 内容 = Label / ProgramArguments[exe, --minimized] / RunAtLoad 最小字段集（组装见
// autostart.h buildPlistContent）；exe 走 _NSGetExecutablePath（.app bundle 内真实
// 可执行，运行时解析）；命令行 _NSGetArgc/_NSGetArgv（与 file_open_macos 同源 API，
// 这里取全量）。
#include "autostart.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <vector>

#include <filesystem>

extern "C" int* _NSGetArgc(void);
extern "C" char*** _NSGetArgv(void);
extern "C" int _NSGetExecutablePath(char* buf, std::uint32_t* bufsize);

namespace platform::autostart {

namespace {

std::filesystem::path plistPath() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) return {};
    return std::filesystem::path(home) / "Library" / "LaunchAgents" /
           (std::string(kMacosLabel) + ".plist");
}

struct MacPlistOps final : public AutostartOps {
    bool isRegistered() override {
        std::error_code ec;
        return std::filesystem::exists(plistPath(), ec);  // 文件在 = 开（真值回读）
    }

    core::Result<void> writeEntry(const std::string& content) override {
        const std::filesystem::path p = plistPath();
        if (p.empty()) {
            return core::Result<void>::fail(core::ErrorCode::Internal,
                                            "HOME 未设置，LaunchAgents 路径未知");
        }
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        if (!out) {
            return core::Result<void>::fail(
                core::ErrorCode::FileWriteFailed,
                "写 LaunchAgents plist 失败: " + p.string());
        }
        out << content;
        out.close();
        if (out.fail()) {
            return core::Result<void>::fail(
                core::ErrorCode::FileWriteFailed,
                "写 LaunchAgents plist 落盘失败: " + p.string());
        }
        // launchd 惯例 0644（属主可写、其他只读）
        std::filesystem::permissions(
            p, std::filesystem::perms::owner_read |
                   std::filesystem::perms::owner_write |
                   std::filesystem::perms::group_read |
                   std::filesystem::perms::others_read,
            ec);
        return core::Result<void>::ok();
    }

    core::Result<void> removeEntry() override {
        const std::filesystem::path p = plistPath();
        std::error_code ec;
        const bool removed = std::filesystem::remove(p, ec);
        if (ec) {
            return core::Result<void>::fail(
                core::ErrorCode::Internal,
                "删 LaunchAgents plist 失败: " + ec.message());
        }
        (void)removed;
        return core::Result<void>::ok();  // 文件不存在 = 已是注销态，幂等成功
    }
};

} // namespace

bool supported() { return true; }

std::unique_ptr<AutostartOps> systemOps() {
    return std::make_unique<MacPlistOps>();
}

std::string executablePath() {
    std::vector<char> buf(4096, '\0');
    std::uint32_t size = static_cast<std::uint32_t>(buf.size());
    if (_NSGetExecutablePath(buf.data(), &size) != 0) {
        // 缓冲不够：size 已回填所需字节数，扩一次重取
        buf.assign(size, '\0');
        size = static_cast<std::uint32_t>(buf.size());
        if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    }
    return std::string(buf.data());  // NUL 终止，构造即截断
}

std::vector<std::string> processArgsUtf8() {
    std::vector<std::string> out;
    const int argc = *_NSGetArgc();
    char** argv = *_NSGetArgv();
    if (argc <= 0 || argv == nullptr) return out;
    for (int i = 0; i < argc; ++i) {
        out.push_back(argv[i] != nullptr ? std::string(argv[i]) : std::string{});
    }
    return out;
}

std::string buildEntryContent(const std::string& exePath) {
    return buildPlistContent(exePath);
}

} // namespace platform::autostart
