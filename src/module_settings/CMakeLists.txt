add_library(devdesk_module_settings STATIC
    settings_module.cpp
    settings_config.cpp
    settings_ui_logic.cpp
    settings_ui_cards.cpp
    app_tools.cpp
)

# 平台适配：磨砂降级文案按平台拆文件（业务代码零平台宏，见 window_effect_hint.h）
if(WIN32)
    target_sources(devdesk_module_settings PRIVATE window_effect_hint_win.cpp)
else()
    target_sources(devdesk_module_settings PRIVATE window_effect_hint_macos.cpp)
endif()

devdesk_apply_warnings(devdesk_module_settings)

# 审计 CP-07 同类（macOS 复发）：**不要**把仓库根（`${CMAKE_CURRENT_SOURCE_DIR}/../..`）
# 加进 include 路径——macOS 的 APFS 大小写不敏感，libc++ 内部 `#include <version>`
# 会命中仓库根的 VERSION 版本号文件（内容 `0.1.5`）→ `expected unqualified-id`
# 编译爆炸。仓库根零个被 git 追踪的头文件，这一项服务不了任何东西。
target_include_directories(devdesk_module_settings PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# R0 / 审计 D6：本模块**零兄弟模块 include 目录、零兄弟模块库边**。
#   claude 侧：改经 core 只读服务定位器（core/claude_proxy_service.h 的窄接口，
#   装配段 main.cpp 注册适配器转发到它持有的 ClaudeService 单例）——不再经
#   claude_service.h，故删掉原先 PRIVATE 指向 ../module_claude_proxy 的 include；
#   pet 侧：中立桥头 module_pet_bridge.h 的自由函数（见 settings_module.cpp），
#   连头都不经模块目录。
# 两侧都在最终可执行文件链接期解析（DevDesk / devdesk_tests 直接链接全部
# module_*），module_* 互不依赖铁律保持。
# R3：settings 是四个页面模块里唯一此前没链接 devdesk_ui_kit 的（无 toast /
# 无对话框）；表头几何改用 ui_kit/metrics.h 的 kHeaderH 后补上这条边。ui_kit
# 是 presentation 层（非兄弟 module_*），守卫脚本 B 项只禁兄弟模块库，不受影响。

target_link_libraries(devdesk_module_settings PUBLIC
    devdesk_core
    devdesk_platform
    devdesk_ui_kit
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
