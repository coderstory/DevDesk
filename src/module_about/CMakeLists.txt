# src/module_about/CMakeLists.txt
# 关于页独立模块（2026-09-28 about 页设计）。**模块隔离**：零兄弟模块
# include 目录、零兄弟模块库边（devdesk_module_* 一个都不 link；grep
# 零 include "module_"，与 settings/pet 侧同款守卫口径）。依赖只走
# core / platform / ui_kit（presentation 层，非兄弟 module_*）。
#
# 编译期信息全部挂本 target（D-02 自给）：DEVDESK_VERSION /
# DEVDESK_BUILD_TYPE 来自顶层 project()，DEVDESK_EUI_NEO_REV /
# DEVDESK_EUI_NEO_MIN_REV / DEVDESK_GIT_REV 是顶层「构建期信息注入」段的
# 变量（add_subdirectory 继承普通变量）；平台/架构走 CMake 注入
#（DEVDESK_PLATFORM / DEVDESK_ARCH），业务代码零平台宏（核心约束 #5）。
add_library(devdesk_module_about STATIC
    about_module.cpp
)

devdesk_apply_warnings(devdesk_module_about)

target_include_directories(devdesk_module_about PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(devdesk_module_about PUBLIC
    devdesk_core
    devdesk_platform
    devdesk_ui_kit
)

target_compile_definitions(devdesk_module_about PRIVATE
    DEVDESK_VERSION="${PROJECT_VERSION}"
    DEVDESK_BUILD_TYPE="$<CONFIG>"
    DEVDESK_EUI_NEO_REV="${DEVDESK_EUI_NEO_REV}"
    DEVDESK_EUI_NEO_MIN_REV="${DEVDESK_EUI_NEO_MIN_REV}"
    DEVDESK_GIT_REV="${DEVDESK_GIT_REV}"
    DEVDESK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    DEVDESK_PLATFORM="${CMAKE_SYSTEM_NAME}"
    DEVDESK_ARCH="${CMAKE_SYSTEM_PROCESSOR}"
)