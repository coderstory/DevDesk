add_library(devdesk_agent STATIC
    agent_store.cpp
)

devdesk_apply_warnings(devdesk_agent)

# 审计 CP-07 同类（macOS 复发）：**不要**把仓库根（`${CMAKE_CURRENT_SOURCE_DIR}/../..`）
# 加进 include 路径——macOS 的 APFS 大小写不敏感，libc++ 内部 `#include <version>`
# 会命中仓库根的 VERSION 版本号文件（内容 `0.1.5`）→ `expected unqualified-id`
# 编译爆炸。与 module_todo 同款：PUBLIC 暴露 src/，消费者写
# `#include "module_agent/agent_store.h"`。
target_include_directories(devdesk_agent PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# 落库层只依赖 core（SqliteDb/Result）；platform 留给后续 Task 接 AgentLoop 的
# HTTP/JSON 侧（plan task-3-brief：链接同款 core/platform 依赖）
target_link_libraries(devdesk_agent PUBLIC
    devdesk_core
    devdesk_platform
)
