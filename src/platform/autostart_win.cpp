// src/platform/autostart_win.cpp
// Windows 开机自启动：HKCU Software\Microsoft\Windows\CurrentVersion\Run 的
// "DevDesk" 值写/删/查（REG_SZ，值 = '"<exe>" --minimized'，内容组装见
// autostart.h buildCommandValue）。exe 路径 GetModuleFileNameW 运行时解析；
// 命令行 GetCommandLineW + CommandLineToArgvW（与 file_open_win 同一编码链，
// 但取全量 argv——那边只要 argv[0..1]）。
#include "autostart.h"
#include "file_open_common.h"  // platform::detail::wideToUtf8 / utf8ToWide 共用声明

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace platform::autostart {

namespace {

constexpr wchar_t kRunSubkey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"DevDesk";

struct WinRunOps final : public AutostartOps {
    bool isRegistered() override {
        wchar_t buf[MAX_PATH] = {};
        DWORD size = sizeof(buf);
        const LSTATUS st =
            RegGetValueW(HKEY_CURRENT_USER, kRunSubkey, kRunValueName,
                         RRF_RT_REG_SZ, nullptr, buf, &size);
        return st == ERROR_SUCCESS;  // 有值 = 开（真值回读，防漂移）
    }

    core::Result<void> writeEntry(const std::string& content) override {
        const std::wstring wide = platform::detail::utf8ToWide(content);
        if (wide.empty()) {
            return core::Result<void>::fail(
                core::ErrorCode::Internal, "注册内容 UTF-16 转换失败");
        }
        const LSTATUS st = RegSetKeyValueW(
            HKEY_CURRENT_USER, kRunSubkey, kRunValueName, REG_SZ, wide.c_str(),
            static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t)));
        if (st != ERROR_SUCCESS) {
            return core::Result<void>::fail(
                core::ErrorCode::Internal,
                "写 HKCU Run 注册表值失败 (RegSetKeyValueW=" +
                    std::to_string(static_cast<long>(st)) + ")");
        }
        return core::Result<void>::ok();
    }

    core::Result<void> removeEntry() override {
        const LSTATUS st =
            RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunSubkey, kRunValueName);
        if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
            return core::Result<void>::fail(
                core::ErrorCode::Internal,
                "删 HKCU Run 注册表值失败 (RegDeleteKeyValueW=" +
                    std::to_string(static_cast<long>(st)) + ")");
        }
        return core::Result<void>::ok();  // 值不存在 = 已是注销态，幂等成功
    }
};

} // namespace

bool supported() { return true; }

std::unique_ptr<AutostartOps> systemOps() { return std::make_unique<WinRunOps>(); }

std::string executablePath() {
    std::wstring buf(32768, L'\0');
    const DWORD len = GetModuleFileNameW(
        nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0 || len >= buf.size()) return {};
    buf.resize(len);
    return platform::detail::wideToUtf8(buf.c_str(), static_cast<int>(buf.size()));
}

std::vector<std::string> processArgsUtf8() {
    std::vector<std::string> out;
    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argvW == nullptr) return out;
    for (int i = 0; i < argc; ++i) {
        out.push_back(argvW[i] != nullptr
                          ? platform::detail::wideToUtf8(argvW[i],
                                                        lstrlenW(argvW[i]))
                          : std::string{});
    }
    LocalFree(argvW);
    return out;
}

std::string buildEntryContent(const std::string& exePath) {
    return buildCommandValue(exePath);
}

} // namespace platform::autostart
