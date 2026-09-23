// src/platform/system_theme_win.cpp
// 系统深浅检测（Windows）：注册表 AppsUseLightTheme（spec 缩水任务 A）。
#include "system_theme.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace platform {

bool prefersDarkTheme() {
    // Win10 1809+ 才有 Personalize 键；键/值缺失（旧系统、Server、手动删除）
    // 一律按浅色兜底——与接口文档承诺一致（调用方 auto 模式解析出 light）
    DWORD value = 1;  // AppsUseLightTheme 默认 1 = 浅色
    DWORD type = 0;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_DWORD, &type, &value, &size);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    return value == 0;
}

} // namespace platform
