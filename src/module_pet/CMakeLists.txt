add_library(devdesk_module_pet STATIC
    pet_config.cpp
    pet_skin.cpp
    pet_window.cpp
)

devdesk_apply_warnings(devdesk_module_pet)

target_include_directories(devdesk_module_pet PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(devdesk_module_pet PUBLIC
    devdesk_core
    devdesk_platform
)
