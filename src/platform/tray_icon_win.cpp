// src/platform/tray_icon_win.cpp
// Windows 托盘图标：品牌 .ico（assets/tray-icon-win.ico，16/32/48 32bpp BGRA）。为什么必须真 .ico：
// ExtractIconExW 只认 .ico/.exe 资源位，传 PNG 必然 hIcon==NULL → 托盘栏空图标（修前传空串同样是 NULL）。
// 32bpp 而非 24bpp：24bpp 无 alpha，透明区填成黑底方块；AND 掩膜全 0（32bpp 下 alpha 生效）。
#include "platform/tray_icon.h"

namespace platform::tray {

std::string defaultIconPath() {
    return "assets/tray-icon-win.ico";
}

} // namespace platform::tray
