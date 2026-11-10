// src/platform/open_settings_macos.cpp
// macOS：无按文件类型默认页的程序化直达（PRD D-5 定稿口径：系统没有可一键
// 直达的按类型默认页，官方唯一批量入口 = Finder「显示简介→打开方式→全部更改」）。
// 恒不支持一键，设置页 UI 走文字指引，不假装成功。
#include "open_settings.h"

namespace platform::open_settings {

bool oneClickAvailable() { return false; }

bool openDefaultAppsSettings() { return false; }

} // namespace platform::open_settings
