# src/module_wallpaper/CMakeLists.txt
# 壁纸页独立模块。**模块隔离**：零兄弟模块 include 目录、零兄弟模块库边；
# 依赖只走 core / platform / ui_kit。
add_library(devdesk_module_wallpaper STATIC
    wallpaper_module.cpp
    wallpaper_module.h
)

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