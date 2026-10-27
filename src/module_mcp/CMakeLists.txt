# src/module_mcp/CMakeLists.txt
# MCP 服务模块（Streamable HTTP + 可插拔能力接缝）。**模块隔离**：只链接
# core / platform；不依赖 ui_kit、不依赖任何兄弟 module_*（settings 侧只经
# app 层桥读状态/发指令，不 include 本模块）。
#
# 编译期信息挂本 target（业务代码零平台宏）：DEVDESK_VERSION / DEVDESK_BUILD_TYPE
# 来自顶层 project()，平台名走 CMake 注入。
add_library(devdesk_module_mcp STATIC
    mcp_service.cpp
    mcp_session_store.cpp
    mcp_protocol.cpp
    mcp_http_handler.cpp
    mcp_tools_builtin.cpp
    html_to_markdown.cpp
)

devdesk_apply_warnings(devdesk_module_mcp)

target_include_directories(devdesk_module_mcp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(devdesk_module_mcp PUBLIC
    devdesk_core
    devdesk_platform
)

target_compile_definitions(devdesk_module_mcp PRIVATE
    DEVDESK_VERSION="${PROJECT_VERSION}"
    DEVDESK_BUILD_TYPE="$<CONFIG>"
    DEVDESK_PLATFORM="${CMAKE_SYSTEM_NAME}"
)