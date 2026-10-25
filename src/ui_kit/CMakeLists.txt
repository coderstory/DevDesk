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
    # W2c 新增（§4.3/§4.2/§4.4）：选中态 / Tab / 开关 helper。新组件并存，
    # 既有选中态/Tab/开关调用不动，W3 nav / W4 settings 逐页切换调新组件。
    selection_bar.cpp
    wtab.cpp
    wswitch.cpp
    # W2d 新增（§4.10/§4.12）：微信式轻提示 + 空状态 helper。新组件并存：
    # 既有通知式 toast 保留不动（§4.10「两种 toast 并存」）；新反馈点调
    # light_toast，空列表/无搜索结果调 empty_state。
    light_toast.cpp
    empty_state.cpp
    # 图标批（2026-10-20 C 方案终端窗 >_）：品牌几何 logo 矢量 helper，三消费方
    # （侧边栏 brand 行 / 自绘标题栏 / 启动 splash）共用；几何契约与
    # scripts/build_icon_assets.py（= assets/icon.ico 位图版）同源。
    logo.cpp
    # UAT3 多列分栏批：双列几何单一来源（半宽配对列 / 卡内双列共用，
    # 防各消费方公式漂移；同步卡 colGap=8 另一套口径，刻意不迁移）。
    columns.cpp
    # UAT6.2 字号无级调节批：横向滑杆 helper（EUI 运行时托管取值，点击跳值 /
    # 拖动连续取值）；首个消费方 = settings 外观卡字号行。
    slider.cpp
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
