# src/module_memory/CMakeLists.txt
# 记忆页独立模块。**模块隔离**：零兄弟模块 include 目录、零兄弟模块库边；
# 依赖只走 core / platform / ui_kit。
add_library(devdesk_module_memory STATIC
    memory_module.cpp
    memory_module.h
    sanitize/sanitize_gate.cpp
    sanitize/sanitize_gate.h
    collector/session_reader.cpp
    collector/session_reader.h
    collector/collector.cpp
    collector/collector.h
    gateway/telemem_client.cpp
    gateway/telemem_client.h
    hook/inject.cpp
    hook/inject.h
    hook/hook_command.cpp
    hook/hook_command.h
    hook/hooks_settings.cpp
    hook/hooks_settings.h
    proposals/usage_ledger.cpp
    proposals/usage_ledger.h
    proposals/proposal_store.cpp
    proposals/proposal_store.h
    proposals/curator.cpp
    proposals/curator.h
    panel/settings_store.cpp
    panel/settings_store.h
    panel/recall_eval.cpp
    panel/recall_eval.h
    panel/recall_eval_data.h
    panel/panel_state.cpp
    panel/panel_state.h
    engine/telemem_config.cpp
    engine/telemem_config.h
    engine/python_locator.cpp
    engine/python_locator.h
    engine/telemem_process.cpp
    engine/telemem_process.h
    engine/llama_host.cpp
    engine/llama_host.h
    engine/telemem_engine.cpp
    engine/telemem_engine.h
)

# 平台适配实现按平台选择编译（接口隔离：头文件平台无关，业务代码零平台宏）
if(WIN32)
    target_sources(devdesk_module_memory PRIVATE
        engine/python_locator_win.cpp
        engine/telemem_process_win.cpp
        engine/llama_host_win.cpp
    )
else()
    target_sources(devdesk_module_memory PRIVATE
        engine/python_locator_posix.cpp
        engine/telemem_process_posix.cpp
        engine/llama_host_posix.cpp
    )
endif()

devdesk_apply_warnings(devdesk_module_memory)

# 同 module_wallpaper：**不要**把仓库根加进 include 路径——macOS APFS 大小写不敏感，
# libc++ 内部 `#include <version>` 会命中仓库根的 VERSION 文件 → 编译爆炸。
target_include_directories(devdesk_module_memory PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(devdesk_module_memory PUBLIC
    devdesk_core
    devdesk_platform
    devdesk_ui_kit
)
