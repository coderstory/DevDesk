// core::detail::replaceFile 的 Windows 实现（CMake 按平台选源）
#include "atomic_write.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace core::detail {

bool prepareRestrictedFile(const std::filesystem::path& tmp, std::error_code& ec) {
    // 用 CreateFileW 建占位文件：避过 MinGW 静态链接 libstdc++ 下 ofstream
    // 构造失败（症状：路径可写、权限足够、但句柄拿不到）
    ec.clear();
    const std::wstring wsp = tmp.wstring();
    HANDLE h = ::CreateFileW(wsp.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        ::CloseHandle(h);
        return true;
    }
    auto le = ::GetLastError();
    if (le == ERROR_FILE_EXISTS) {
        ec.clear();
        return true;  // 崩溃残留：复用
    }
    ec = std::error_code(static_cast<int>(le), std::system_category());
    return false;
}

void replaceFile(const std::filesystem::path& tmp,
                 const std::filesystem::path& target,
                 std::error_code& ec) {
    // 一步原子替换：MoveFileExW + MOVEFILE_REPLACE_EXISTING
    if (::MoveFileExW(tmp.wstring().c_str(), target.wstring().c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ec.clear();
        return;
    }
    ec = std::error_code(static_cast<int>(::GetLastError()),
                         std::system_category());
}

bool writeBinaryFile(const std::filesystem::path& path, const std::string& content,
                     std::error_code& ec) {
    // CreateFileW + WriteFile + CloseHandle：避过 MinGW 静态链接 libstdc++ 的
    // ofstream 问题，直写 Win32
    ec.clear();
    const std::wstring wsp = path.wstring();
    HANDLE h = ::CreateFileW(wsp.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        ec = std::error_code(static_cast<int>(::GetLastError()),
                             std::system_category());
        return false;
    }
    DWORD written = 0;
    BOOL ok = ::WriteFile(h, content.data(), static_cast<DWORD>(content.size()),
                          &written, nullptr);
    ::CloseHandle(h);
    if (!ok || written != content.size()) {
        if (ok) ec = std::make_error_code(std::errc::no_space_on_device);
        else ec = std::error_code(static_cast<int>(::GetLastError()),
                                  std::system_category());
        return false;
    }
    ec.clear();
    return true;
}

} // namespace core::detail
