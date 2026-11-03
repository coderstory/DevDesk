add_library(devdesk_module_terminal STATIC
    module_terminal.cpp
    terminal_page.cpp
    terminal_grid_widget.cpp
    terminal_session_controller.cpp
    terminal_keyencoder.cpp
)

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
