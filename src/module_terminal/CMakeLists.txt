add_library(devdesk_module_terminal STATIC
    module_terminal.cpp
    terminal_page.cpp
    terminal_grid_widget.cpp
    terminal_session_controller.cpp
    terminal_keyencoder.cpp
    terminal_metrics.cpp
    terminal_search.cpp
    terminal_settings.cpp
    # need#490 W4：网页网格帧编解码 + 会话桥（纯逻辑，无 HTTP 依赖）
    web/web_grid.cpp
    web/web_session_bridge.cpp
)

# 终端设置选项表按平台拆（need250：字体/shell 选项各写死一套，AGENTS §三）
if(WIN32)
    target_sources(devdesk_module_terminal PRIVATE terminal_settings_platform_win.cpp)
else()
    target_sources(devdesk_module_terminal PRIVATE terminal_settings_platform_posix.cpp)
endif()

devdesk_apply_warnings(devdesk_module_terminal)

# 同 todo 模块口径：只加 src/，不加仓库根（APFS 大小写陷阱见 module_todo/CMakeLists.txt）
target_include_directories(devdesk_module_terminal PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(devdesk_module_terminal PUBLIC
    devdesk_core
    devdesk_platform
    devdesk_ui_kit
)
