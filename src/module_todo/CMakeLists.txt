add_library(devdesk_module_todo STATIC
    todo_module.cpp
    todo_service.cpp
    todo_ui_board.cpp
    todo_ui_dialogs.cpp
    todo_view_state.cpp
)

devdesk_apply_warnings(devdesk_module_todo)

# 审计 CP-07 同类（macOS 复发）：**不要**把仓库根（`${CMAKE_CURRENT_SOURCE_DIR}/../..`）
# 加进 include 路径——macOS 的 APFS 大小写不敏感，libc++ 内部 `#include <version>`
# 会命中仓库根的 VERSION 版本号文件（内容 `0.1.5`）→ `expected unqualified-id`
# 编译爆炸。仓库根零个被 git 追踪的头文件，这一项服务不了任何东西。
target_include_directories(devdesk_module_todo PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(devdesk_module_todo PUBLIC
    devdesk_core
    devdesk_platform
    devdesk_ui_kit
)