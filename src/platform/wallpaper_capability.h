// src/platform/wallpaper_capability.h
// 壁纸播放能力探测：平台层唯一判据，业务代码（模块工厂）据此决定是否注册页面。
#pragma once

namespace platform::wallpaper {

// 返回 true = 本平台具备壁纸播放所需的宿主能力，壁纸页模块可注册。
// Windows：true（WorkerW 宿主 + mpv 后端可用）。
// macOS/POSIX：false（第二阶段 ObjC++ 平台层，本阶段不注册；工厂返回 nullptr）。
// 调用方不得据此假设引擎已启动——本阶段只探测能力，不创建任何播放资源。
bool supported();

} // namespace platform::wallpaper