add_library(devdesk_module_schedule STATIC
    schedule_store.cpp
)

devdesk_apply_warnings(devdesk_module_schedule)

# 与 module_todo 同款：不要加仓库根 include 路径（APFS 大小写不敏感下 #include <version> 撞 VERSION 文件）
target_include_directories(devdesk_module_schedule PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(devdesk_module_schedule PUBLIC
    devdesk_core
)
