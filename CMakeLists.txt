cmake_minimum_required(VERSION 3.14)
project(DevDesk VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)

# 大气档编译选项
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
else()
    add_compile_options(-Wall -Wextra)
endif()

include(cmake/Dependencies.cmake)
include(cmake/CompilerWarnings.cmake)

# EUI-NEO 作为子项目（dev 时使用绝对路径，生产用 submodule）
set(EUI_NEO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../EUI-NEO" CACHE PATH "EUI-NEO source dir")
if(NOT EXISTS "${EUI_NEO_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "EUI-NEO not found at ${EUI_NEO_DIR}")
endif()
add_subdirectory(${EUI_NEO_DIR} ${CMAKE_BINARY_DIR}/_eui_neo_build EXCLUDE_FROM_ALL)

# Core + Platform 是 static lib
add_subdirectory(src/core)
add_subdirectory(src/platform)

# 四个功能模块
add_subdirectory(src/module_home)
add_subdirectory(src/module_markdown)
add_subdirectory(src/module_todo)
add_subdirectory(src/module_claude_proxy)

# 测试
enable_testing()
add_subdirectory(tests)

# 主程序
add_executable(DevDesk src/main.cpp)
eui_neo_configure_app(DevDesk)
target_link_libraries(DevDesk PRIVATE
    devdesk_core
    devdesk_platform
    devdesk_module_home
    devdesk_module_markdown
    devdesk_module_todo
    devdesk_module_claude_proxy
    eui_neo
)
target_include_directories(DevDesk PRIVATE src)

# 安装 / 拷贝资源
add_custom_command(TARGET DevDesk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/fonts" "$<TARGET_FILE_DIR:DevDesk>/fonts"
    COMMENT "Copying fonts to build directory"
)