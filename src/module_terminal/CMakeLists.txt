add_library(devdesk_module_terminal STATIC
    module_terminal.cpp
    terminal_page.cpp
    terminal_grid_widget.cpp
    terminal_session_controller.cpp
    terminal_keyencoder.cpp
    terminal_input_router.cpp
    terminal_metrics.cpp
    terminal_search.cpp
    terminal_text_util.cpp
    terminal_settings.cpp
    terminal_dir_menu.cpp
)

# need#490 网页终端源清单独立成文件：该清单在 W1–W7 多波并行改动中反复制造
# CMakeLists 合并冲突，集中到 web_sources.cmake 后主文件只留 include。
include(${CMAKE_CURRENT_SOURCE_DIR}/web_sources.cmake)
target_sources(devdesk_module_terminal PRIVATE ${DEVDESK_WEB_TERMINAL_SOURCES})


# 终端设置选项表按平台拆（need250：字体/shell 选项各写死一套，AGENTS §三）
if(WIN32)
    target_sources(devdesk_module_terminal PRIVATE terminal_settings_platform_win.cpp)
else()
    target_sources(devdesk_module_terminal PRIVATE terminal_settings_platform_posix.cpp)
endif()

# need#490 W7：web/www/* 编译期嵌入。生成源放构建目录，依赖前端源文件；
# 改 JS/CSS 只触发一次生成 + 一个 TU 重编，不触发全量 configure。
set(_web_www_dir ${CMAKE_CURRENT_SOURCE_DIR}/web/www)
set(_web_assets_cpp ${CMAKE_CURRENT_BINARY_DIR}/web_assets_generated.cpp)
add_custom_command(
    OUTPUT ${_web_assets_cpp}
    COMMAND ${CMAKE_COMMAND}
            -DWWW_DIR=${_web_www_dir}
            -DOUT_FILE=${_web_assets_cpp}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/web/embed_assets.cmake
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/web/embed_assets.cmake
        ${_web_www_dir}/index.html
        ${_web_www_dir}/app.css
        ${_web_www_dir}/app.js
        ${_web_www_dir}/protocol.js
        ${_web_www_dir}/render.js
        ${_web_www_dir}/crypto.js
    COMMENT "嵌入网页终端静态资源 (need#490 W7)"
    VERBATIM)
target_sources(devdesk_module_terminal PRIVATE ${_web_assets_cpp})

devdesk_apply_warnings(devdesk_module_terminal)

# 同 todo 模块口径：只加 src/，不加仓库根（APFS 大小写陷阱见 module_todo/CMakeLists.txt）
target_include_directories(devdesk_module_terminal PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(devdesk_module_terminal PUBLIC
    devdesk_core
    devdesk_platform
    devdesk_ui_kit
)
