# src/module_wallpaper/platform/CMakeLists.txt
# 平台适配层：Win32 宿主/mpv 只在 Windows 构建；其它平台编 stub，
# 保证 createWallpaperHost / MpvController 符号跨平台不缺失（macOS 构建不得缺符号）。
target_sources(devdesk_module_wallpaper PRIVATE
    mpv_player.cpp
    mpv_player.h
)

if(WIN32)
    target_sources(devdesk_module_wallpaper PRIVATE
        wallpaper_host_win.cpp
        mpv_controller.cpp
    )
else()
    target_sources(devdesk_module_wallpaper PRIVATE
        wallpaper_host_stub.cpp
        mpv_controller_stub.cpp
    )
endif()