add_library(devdesk_module_claude_proxy STATIC
    call_log_store.cpp
    claude_module.cpp
    claude_ui_list.cpp
    claude_ui_detail.cpp
    claude_ui_form.cpp
    claude_ui_logs.cpp
    claude_ui_logic.cpp
    claude_ui_state.cpp
    claude_service.cpp
    provider_config.cpp
    proxy_router.cpp
    settings_sync.cpp
    streaming_proxy.cpp
    upstream_dialect.cpp
)

devdesk_apply_warnings(devdesk_module_claude_proxy)

target_include_directories(devdesk_module_claude_proxy PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(devdesk_module_claude_proxy PUBLIC
    devdesk_core
    devdesk_platform
    devdesk_ui_kit
)