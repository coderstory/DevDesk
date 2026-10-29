# 上线模块（骨架）：两个 Router 页面 + 后续任务各自的实现文件。
# 模块隔离：只依赖 core / platform / ui_kit，不 link 任何兄弟 module_*。
add_library(devdesk_module_release STATIC
    release_store.cpp
    release_logic.cpp
    release_schedule_source.cpp
    release_reminder_logic.cpp
    release_config_ui.cpp
    release_list_ui.cpp
    release_detail_ui.cpp
    release_dialogs.cpp
    release_reminder.cpp
    release_view_state.cpp
    release_module.cpp
)

devdesk_apply_warnings(devdesk_module_release)

# 审计 CP-07 同类（macOS）：不要把仓库根加进 include 路径（APFS 大小写不敏感会命中 VERSION）。
target_include_directories(devdesk_module_release PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(devdesk_module_release PUBLIC
    devdesk_core
    devdesk_platform
    devdesk_ui_kit
)