add_library(devdesk_module_markdown STATIC
    markdown_module.cpp
    markdown_service.cpp
    md_line_scanner.cpp
    md_tree.cpp
)

devdesk_apply_warnings(devdesk_module_markdown)

target_include_directories(devdesk_module_markdown PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(devdesk_module_markdown PUBLIC
    devdesk_core
    devdesk_platform
)