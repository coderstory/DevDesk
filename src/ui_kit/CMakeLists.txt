# src/ui_kit —— presentation 能力层（任务 D / R1 基座，R2 续，R6 续）
#
# 收编跨模块重复的 UI 接线（toast = R1；dialog_form = R2；card = R6；chip 在
# 后续里程碑迁入）。依赖 devdesk_core（Result/Tokens/ThemeColorTokens 桥接；
# core PUBLIC 链接 eui_neo，本层所需的 EUI-NEO 组件头与 include 路径经此
# 传播），被 module_* 与 app 层（app_sidebar，编在 DevDesk / devdesk_tests
# 两处）链接。core / platform 不得反向依赖本层
#（spec 2026-09-27-refactor-incremental §4.1）。
add_library(devdesk_ui_kit STATIC
    toast.cpp
    dialog_form.cpp
    card.cpp
    splash.cpp
    # W2b 新增（§4.1/§4.15）：四级按钮 helper + 表单栅格 helper。新组件并存，
    # 既有 button 调用不动，W5-W10 逐页切换调新组件。
    button.cpp
    form_row.cpp
)

devdesk_apply_warnings(devdesk_ui_kit)

target_include_directories(devdesk_ui_kit PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    # 只加到 src 为止：消费方一律 <ui_kit/xxx.h>，不把本目录自身加进 -I
    # （避免与其它同名头（如 toast.h）撞名）
)

target_link_libraries(devdesk_ui_kit PUBLIC
    devdesk_core
)
