add_library(devdesk_module_claude_proxy STATIC
    claude_module.cpp
    claude_service.cpp
    proxy_router.cpp
    streaming_proxy.cpp
)

devdesk_apply_warnings(devdesk_module_claude_proxy)

target_include_directories(devdesk_module_claude_proxy PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(devdesk_module_claude_proxy PUBLIC
    devdesk_core
    devdesk_platform
)