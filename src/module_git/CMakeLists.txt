add_library(devdesk_module_git STATIC
    git_module.cpp
    git_page_controller.cpp
    git_worker.cpp
    git_tree_model.cpp
    git_changes_model.cpp
    git_diff_model.cpp
    git_content_model.cpp
    git_search_model.cpp
    git_colors.cpp
    git_repo_store.cpp
    git_views.cpp
)

devdesk_apply_warnings(devdesk_module_git)

# 与 module_markdown 同款：include 根 = src/（"module_git/xxx.h" 以模块目录引用）。
# 审计 CP-07 教训：不得把仓库根加进 include 路径（APFS 大小写不敏感命中 VERSION 文件）。
target_include_directories(devdesk_module_git PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# 架构铁律：模块间禁互依（不 link 任何 devdesk_module_*）；
# core ← platform ← 本模块 单向；EUI-NEO 头经 core 的桥接头可见（不直接 link eui_neo）。
target_link_libraries(devdesk_module_git PUBLIC
    devdesk_core
    devdesk_platform
    devdesk_ui_kit  # ui_kit/text_highlight 内容高亮（need:10 批次三；ui_kit=presentation 层，非 module）
)
