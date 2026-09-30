// src/platform/tray_icon_win.cpp
// Windows 托盘图标：品牌 .ico（assets/tray-icon-win.ico，16/32/48 三档
// 32bpp BGRA DIB 条目，由 macOS bundle 的母图 DevDesk.icns 派生）。
//
// 为什么必须真 .ico：EUI-NEO 3rd/tray/tray.h 的
//   ExtractIconExW(icon_path)  → LoadImageW(..., IMAGE_ICON)
// 只认 .ico / .exe 资源位；传 PNG（哪怕 EUI-NEO 内部按扩展名以外的方式兜底）
// 必然 hIcon == NULL → 托盘栏空图标。这是本次修复的全部理由。
// 修前 DevDesk 传的是空串（同样是 NULL），不是回归，是修复漏了 Windows。
//
// 32bpp 32 位 DIB 而非 24bpp：24bpp 无 alpha 通道，透明区会填成黑底方块，
// 托盘图标看起来是个黑方块而不是圆角图标。AND 掩膜全 0（32bpp 下 alpha 生效）。
#include "platform/tray_icon.h"

namespace platform::tray {

std::string defaultIconPath() {
    return "assets/tray-icon-win.ico";
}

} // namespace platform::tray
