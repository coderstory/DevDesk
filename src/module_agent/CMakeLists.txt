add_library(devdesk_agent STATIC
    agent_store.cpp
    agent_time_text.cpp
    cron.cpp
    trigger_config.cpp
    hot_parse.cpp
    hot_tool.cpp
    message_blocks.cpp
    tool_info.cpp
    agent_loop.cpp
    scheduler.cpp
    app_stats_tool.cpp
    app_notify_tool.cpp
    app_now_tool.cpp
    net_fetch_tool.cpp
    direct_llm_client.cpp
    task_draft.cpp
    task_draft_job.cpp
    agent_builtin_tools.cpp
    agent_detail_dialog.cpp
    agent_module.cpp
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

# 依赖 core（SqliteDb/Result/llm_endpoint）+ platform（AgentLoop 的 blocks JSON
# 解析拼装、DirectLlmClient 的 HTTP 客户端与 JSON 组装）+ ui_kit（2026-10-02 批次 A：
# 任务 / 详情弹窗走共享的 DialogFrame/drawDialogFrame 接线，与 todo / claude /
# markdown 四处站点同源——不自造第二套弹窗外框）。
# 注：ui_kit 是 presentation 能力层，不是兄弟模块，不涉模块隔离红线
target_link_libraries(devdesk_agent PUBLIC
    devdesk_core
    devdesk_platform
    devdesk_ui_kit
)
