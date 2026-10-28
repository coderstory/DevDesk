# src/module_wallpaper/CMakeLists.txt
# 壁纸页独立模块。**模块隔离**：零兄弟模块 include 目录、零兄弟模块库边；
# 依赖只走 core / platform / ui_kit。
add_library(devdesk_module_wallpaper STATIC
    wallpaper_module.cpp
    wallpaper_module.h
    # 页面组装（播放/片库）：吃快照、吐元素树，交互经 UiActions 回到服务。
    wallpaper_ui.cpp
    wallpaper_ui.h
    # 服务骨架（扫描/轮换/让路/快照）；UI 批次只消费 wallpaper_service.h 的快照与命令。
    wallpaper_service.cpp
    wallpaper_service.h
    # 纯逻辑层：零 Win32 / EUI-NEO 依赖，跨平台单测。
    core/model.h
    core/settings.cpp
    core/settings.h
    core/rotation.cpp
    core/rotation.h
    core/arbiter.cpp
    core/arbiter.h
    core/availability.cpp
    core/availability.h
    core/video_library.cpp
    core/video_library.h
    core/image_library.cpp
    core/image_library.h
    core/presentation.cpp
    core/presentation.h
    core/folder_policy.cpp
    core/folder_policy.h
    # 播放后端接口；Null 仅测试注入，生产走 platform/ 的 mpv 实现。
    platform/player_backend.h
    platform/player_null.cpp
    # 让路事件源接口（锁屏/电池/全屏/换屏 → arbiter）：平台无关缝，测试注入假实现。
    platform/yield_watchers.h
    # 图片尺寸头解码接口（像素档过滤）：core 定义返回结构，平台层实现探针。
    platform/image_probe.h
)

# 平台层（WallpaperHost / MpvController）：Win32 实现与 stub 按平台选择。
# 契约：源文件必须登记在本模块级 CMakeLists——Windows 交叉编译守卫
# （scripts/check-windows-cross-compile.sh）只解析 src/*/CMakeLists.txt 一层，
# 嵌套 CMakeLists 里的 *_win.cpp 会被判「存在于磁盘却没进受编集合」。
if(WIN32)
    target_sources(devdesk_module_wallpaper PRIVATE
        platform/mpv_player.cpp
        platform/mpv_player.h
        platform/wallpaper_host_win.cpp
        platform/mpv_controller.cpp
        platform/yield_watchers_win.cpp
        platform/image_probe_win.cpp
    )
    # yield_watchers：wtsapi32（WTS 会话通知）；image_probe：ole32 + windowscodecs（COM + WIC）。
    target_link_libraries(devdesk_module_wallpaper PRIVATE wtsapi32 ole32 windowscodecs)
else()
    target_sources(devdesk_module_wallpaper PRIVATE
        platform/mpv_player.cpp
        platform/mpv_player.h
        platform/wallpaper_host_stub.cpp
        platform/mpv_controller_stub.cpp
        platform/yield_watchers_stub.cpp
        platform/image_probe_stub.cpp
    )
endif()

devdesk_apply_warnings(devdesk_module_wallpaper)

# 同 module_about：**不要**把仓库根加进 include 路径——macOS APFS 大小写不敏感，
# libc++ 内部 `#include <version>` 会命中仓库根的 VERSION 文件 → 编译爆炸。
target_include_directories(devdesk_module_wallpaper PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(devdesk_module_wallpaper PUBLIC
    devdesk_core
    devdesk_platform
    devdesk_ui_kit
)