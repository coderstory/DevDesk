add_library(devdesk_module_schedule STATIC
    schedule_store.cpp
    channels/wecom.cpp
    # 波 S7b：编辑弹窗 + 运行历史抽屉（纯逻辑 + 渲染；弹窗由宿主波经 EditDialogHost/HistoryDrawerHost 挂载）
    schedule_ui_edit.cpp
    schedule_ui_history.cpp
    schedule_engine.cpp
    action_runner.cpp
)

devdesk_apply_warnings(devdesk_module_schedule)

# 与 module_todo 同款：不要加仓库根 include 路径（APFS 大小写不敏感下 #include <version> 撞 VERSION 文件）
target_include_directories(devdesk_module_schedule PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(devdesk_module_schedule PUBLIC
    devdesk_core
    devdesk_platform
    # 波 S7b UI 层依赖：ui_kit/组件绘制（platform 为 ui_kit 传递依赖，与 module_todo 同款）
    devdesk_platform
    devdesk_ui_kit
)
