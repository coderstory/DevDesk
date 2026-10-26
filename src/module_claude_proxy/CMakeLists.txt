add_library(devdesk_module_claude_proxy STATIC
    ccswitch_import.cpp
    ccswitch_source.cpp
    claude_module.cpp
    claude_ui_ccswitch.cpp
    claude_ui_list.cpp
    claude_ui_detail.cpp
    claude_ui_form.cpp
    claude_ui_logic.cpp
    claude_ui_state.cpp
    claude_service.cpp
    claude_tools.cpp
    profile_repository.cpp
    provider_config.cpp
    proxy_router.cpp
    proxy_runtime.cpp
    settings_sync.cpp
    settings_takeover.cpp
    streaming_proxy.cpp
    upstream_dialect.cpp
)

devdesk_apply_warnings(devdesk_module_claude_proxy)

# 审计 CP-07 同类（macOS 复发）：**不要**把仓库根（`${CMAKE_CURRENT_SOURCE_DIR}/../..`）
# 加进 include 路径——macOS 的 APFS 大小写不敏感，libc++ 内部 `#include <version>`
# 会命中仓库根的 VERSION 版本号文件（内容 `0.1.5`）→ `expected unqualified-id`
# 编译爆炸。仓库根零个被 git 追踪的头文件，这一项服务不了任何东西。
target_include_directories(devdesk_module_claude_proxy PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(devdesk_module_claude_proxy PUBLIC
    devdesk_core
    devdesk_platform
    devdesk_ui_kit
)