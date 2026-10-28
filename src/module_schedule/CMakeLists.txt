add_library(devdesk_module_schedule STATIC
    schedule_store.cpp
    # 波 S7b：编辑弹窗 + 运行历史抽屉（纯逻辑 + 渲染；弹窗由宿主波经 EditDialogHost/HistoryDrawerHost 挂载）
    schedule_ui_edit.cpp
    schedule_ui_history.cpp
    schedule_engine.cpp
    action_runner.cpp
    channels/wecom.cpp
    # 波 S5：渠道层（tests/CMakeLists.txt 仍直编一份供渠道单测；宿主波把
    # 渠道源登记进模块库 = DevDesk 链接侧的唯一来源，archive 惰性抽取不会
    # 与 tests 的直编对象重复定义）
    channel_registry.cpp
    channels/fullscreen_channel.cpp
    channels/toast_channel.cpp
    channels/os_notification.cpp
    channels/silent_channel.cpp
    # 波 S7a：内部任务注册表缝 + 系统定时只读来源接口 + 列表页 + 模块宿主
    internal_task_registry.cpp
    schedule_provider.cpp
    schedule_ui_list.cpp
    schedule_module.cpp
)

devdesk_apply_warnings(devdesk_module_schedule)

# 与 module_todo 同款：不要加仓库根 include 路径（APFS 大小写不敏感下 #include <version> 撞 VERSION 文件）
target_include_directories(devdesk_module_schedule PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# devdesk_platform：列表页规则摘要解析走 platform::json（yyjson 薄封装）；
# eui 头经 devdesk_core 的 PUBLIC 链接传播（components/scrollview、switch 等）
target_link_libraries(devdesk_module_schedule PUBLIC
    devdesk_core
    devdesk_platform
    # 波 S7b UI 层依赖：ui_kit/组件绘制（platform 为 ui_kit 传递依赖，与 module_todo 同款）
    devdesk_ui_kit
)
