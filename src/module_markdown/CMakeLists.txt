add_library(devdesk_module_markdown STATIC
    markdown_module.cpp
    markdown_service.cpp
    md_highlight.cpp
    md_keyword_highlight.cpp
    md_keyboard.cpp
    md_line_scanner.cpp
    md_file_open.cpp
    md_tree.cpp
    md_view_state.cpp
    md_block_model.cpp
    md_block_interact.cpp
    md_preview_highlight.cpp
    md_sync_map.cpp
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