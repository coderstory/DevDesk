add_library(devdesk_module_settings STATIC
    settings_module.cpp
    settings_config.cpp
)

devdesk_apply_warnings(devdesk_module_settings)

target_include_directories(devdesk_module_settings PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# claude_service.h 所在目录（仅 .cpp 内部 include，PRIVATE 即可）
target_include_directories(devdesk_module_settings PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../module_claude_proxy
)

# 依赖说明：module_* 之间原则上不互相依赖（防循环）。这里是单向
# settings → claude_proxy，仅为读取 main.cpp 注册的 ClaudeService::global()
# 单例（既有跨模块桥接模式，claude_module.cpp 同款用法）；claude_proxy
# 不反向依赖 settings，无环。
target_link_libraries(devdesk_module_settings PUBLIC
    devdesk_core
    devdesk_platform
    devdesk_module_claude_proxy
)

# M2 桌宠卡（设计 2026-09-22 §4.7）：settings → pet 单向（discoverSkins/
# applyPetSkin/resetPetPosition）；pet 不反向依赖 settings，无环
target_link_libraries(devdesk_module_settings PUBLIC
    devdesk_module_pet
)

# 关于卡（spec §8.5，spec 缩水 C）：版本/构建信息注入。PROJECT_VERSION 来自
# 顶层 project(VERSION)；EUI_NEO_GIT_REV / DEVDESK_EUI_NEO_MIN_REV 是顶层
# 「EUI-NEO 版本诊断」段的变量（add_subdirectory(src) 前已 set，作用域可达）。
# 消费方 settings_config.cpp（aboutVersionLine/aboutEuiNeoLine），宏缺失有
# #ifndef 回退，测试二进制经本库对象文件链接同样拿到定义
target_compile_definitions(devdesk_module_settings PRIVATE
    DEVDESK_VERSION="${PROJECT_VERSION}"
    DEVDESK_BUILD_TYPE="$<CONFIG>"
    DEVDESK_EUI_NEO_REV="${EUI_NEO_GIT_REV}"
    DEVDESK_EUI_NEO_MIN_REV="${DEVDESK_EUI_NEO_MIN_REV}"
)
