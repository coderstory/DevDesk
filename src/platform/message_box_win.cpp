// src/platform/message_box_win.cpp
// Windows：MessageBoxW（user32，MinGW 默认链接，无需补 target_link_libraries）。
// MB_SETFOREGROUND|MB_TOPMOST：启动期应用窗口尚不存在，强制对话框前置，否则会被其它窗口埋掉。
#include "message_box.h"

#include <windows.h>

#include <string>

namespace platform::msgbox {
namespace {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                         static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), need);
    return out;
}

} // namespace

Choice show(const std::string& title, const std::string& message, bool withCancel) {
    const UINT type = MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST |
                      (withCancel ? MB_OKCANCEL : MB_OK);
    const int rc = MessageBoxW(nullptr, utf8ToWide(message).c_str(),
                                utf8ToWide(title).c_str(), type);
    return rc == IDOK || rc == IDYES ? Choice::Ok : Choice::Cancel;
}

} // namespace platform::msgbox
