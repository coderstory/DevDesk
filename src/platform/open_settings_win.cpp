// src/platform/open_settings_win.cpp
// Windows：ShellExecuteW 拉 ms-settings:defaultapps（系统设置「默认应用」页，
// Win10+ 公开跳转入口）。返回值 > 32 = 成功（SE_ERR 经典约定，≤32 为错误码）。
#include "open_settings.h"

#include <windows.h>
#include <shellapi.h>

namespace platform::open_settings {

bool oneClickAvailable() { return true; }

bool openDefaultAppsSettings() {
    const HINSTANCE r = ::ShellExecuteW(nullptr, L"open",
                                        L"ms-settings:defaultapps", nullptr,
                                        nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

} // namespace platform::open_settings
