add_library(devdesk_module_home STATIC
    home_module.cpp
)

target_include_directories(devdesk_module_home PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(devdesk_module_home PUBLIC
    devdesk_core
    devdesk_platform
)