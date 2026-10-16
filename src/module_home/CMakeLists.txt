add_library(devdesk_module_home STATIC
    home_module.cpp
    home_quota_card.cpp
    home_service.cpp
)

devdesk_apply_warnings(devdesk_module_home)

# 审计 CP-07 同类（macOS 复发）：**不要**把仓库根（`${CMAKE_CURRENT_SOURCE_DIR}/../..`）
# 加进 include 路径——macOS 的 APFS 大小写不敏感，libc++ 内部 `#include <version>`
# 会命中仓库根的 VERSION 版本号文件（内容 `0.1.5`）→ `expected unqualified-id`
# 编译爆炸。仓库根零个被 git 追踪的头文件，这一项服务不了任何东西。
target_include_directories(devdesk_module_home PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# 🔴 本库**不是自洽的**：`libdevdesk_module_home.a` 里有两个未定义符号
#   module::claude::quota::listQuotaHomeRows(core::data::SqliteDb&)
#   module::claude::quota::requestQuotaImmediateRefresh()
# 实现全部在 `devdesk_module_claude_proxy.a`（quota_home_view.cpp /
# quota_refresh.cpp），而本库的 target_link_libraries **故意不**链
# devdesk_module_claude_proxy。两侧都在**最终可执行文件链接期**解析
#（DevDesk / devdesk_tests / 每个 probe 都同时链接全部 module_* 库），所以
# 现在能链接上。姿态与 devdesk_module_settings 完全同款：
# **include 层面**零跨模块（经 `src/quota_home_bridge.h` 这个中立头），**链接
# 层面**依赖最终目标兜住，module_* 互不依赖的铁律保持。
#
# ⚠ **静态库链接是顺序敏感的**：顶层 CMakeLists.txt 的链接表里
# devdesk_module_home 必须排在 devdesk_module_claude_proxy **之前**，否则
# 这两个符号 undefined。「有人把链接表整理成字母序」是一次真实的炸法。
# 这条顺序由 `scripts/check-module-isolation.sh` 的 home 侧断言 D6 用**代码**
# 钉住（不是靠本注释）；本注释只负责说清**为什么**是这个顺序。
#
# 编译期不可表达、不变式靠约定的不变式（改 head 子元素时**必须**同步）：
#   `home_quota_card.cpp` 的 `headChildCount = 3 + (stale?1:0) + (cfg?1:0)`
#   必须等于 head 实际挂上的常驻子元素数。headChildCount 由 head 子元素
#   构建派生（不是手数字面量），probe 组 I(b) 用真几何断言钉住 gap。
target_link_libraries(devdesk_module_home PUBLIC
    devdesk_core
    devdesk_platform
)