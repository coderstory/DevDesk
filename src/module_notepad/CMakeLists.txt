add_library(devdesk_module_notepad STATIC
    notepad_module.cpp
    notepad_session.cpp
    notepad_window.cpp
    notepad_file_io.cpp
    notepad_encoding.cpp
    notepad_prefs.cpp
    # 扩展名→语言映射单一真源 + per-line 高亮 provider（need263 批 2，走 ui_kit LangSpec 单入口）
    notepad_language.cpp
    # 未保存保护纯决策层（need263 批 4 F-05：三选映射/veto 裁决/quit guard 扫描）
    notepad_protect.cpp
    # SQL 格式化纯函数引擎（need380 最小范围：关键字规范化 + 缩进）
    notepad_sql_format.cpp
)

devdesk_apply_warnings(devdesk_module_notepad)

# 与兄弟模块同款：PUBLIC 暴露 src/（消费方 include "module_notepad/…"）。
# 审计 CP-07 同类：禁把仓库根加进 include 路径（APFS 大小写不敏感会命中
# 仓库根 VERSION 文件，编译爆炸——见 module_markdown/CMakeLists.txt 同注）。
target_include_directories(devdesk_module_notepad PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(devdesk_module_notepad PUBLIC
    devdesk_core
    devdesk_platform
    devdesk_ui_kit
)
