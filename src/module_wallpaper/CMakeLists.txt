# src/module_wallpaper/CMakeLists.txt
# 壁纸页独立模块。**模块隔离**：零兄弟模块 include 目录、零兄弟模块库边；
# 依赖只走 core / platform / ui_kit。
add_library(devdesk_module_wallpaper STATIC
    wallpaper_module.cpp
    wallpaper_module.h
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
)

# 平台层（WallpaperHost / MpvController）：Win32 实现与 stub 由子目录按平台选择。
add_subdirectory(platform)

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