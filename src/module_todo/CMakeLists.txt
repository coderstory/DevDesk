add_library(devdesk_module_todo STATIC
    todo_module.cpp
)

target_include_directories(devdesk_module_todo PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(devdesk_module_todo PUBLIC
    devdesk_core
    devdesk_platform
)