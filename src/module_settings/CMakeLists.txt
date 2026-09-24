add_library(devdesk_module_settings STATIC
    settings_module.cpp
    settings_config.cpp
)

devdesk_apply_warnings(devdesk_module_settings)

target_include_directories(devdesk_module_settings PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# claude_service.h 所在目录（settings_module.cpp 经 ClaudeService::global()
# 读 main 注册的单例——审计 [5][36]：仅头声明通道，不 link 模块库；符号在
# DevDesk/devdesk_tests 最终链接期由直接链接的 devdesk_module_claude_proxy
# 解析）。pet 侧连头都不经：extern 自由函数桥（见 settings_module.cpp），
# module_* 互不依赖铁律恢复（不再 PUBLIC 链 devdesk_module_pet）。
target_include_directories(devdesk_module_settings PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../module_claude_proxy
)

target_link_libraries(devdesk_module_settings PUBLIC
    devdesk_core
    devdesk_platform
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
