add_executable(devdesk_tests
    test_main.cpp
    ${CMAKE_SOURCE_DIR}/src/app_sidebar.cpp
    ${CMAKE_SOURCE_DIR}/src/app_assembly.cpp
    # app::toggle 桥实现缝：settings_module.cpp 引用其符号；probe 同样需要
    #（probe 只链 module_* 库，app_assembly.cpp 不在 probe 链接集）
    ${CMAKE_SOURCE_DIR}/src/module_toggle_bridge.cpp
    # app::tray 壁纸托盘桥实现缝：test_wallpaper_tray.cpp 引用其符号
    ${CMAKE_SOURCE_DIR}/src/wallpaper_tray_bridge.cpp
    test_core/test_result.cpp
    test_core/test_paths.cpp
    test_core/test_logging.cpp
    test_core/test_sqlite.cpp
    test_core/test_sqlite_concurrency.cpp
    test_core/test_sql_dump.cpp
    test_core/test_theme.cpp
    test_core/test_theme_fontscale.cpp
    test_core/test_theme_accents.cpp
    test_core/test_router.cpp
    test_core/test_schema.cpp
    test_core/test_app_config.cpp
    test_core/test_proxy_stats.cpp
    test_core/test_time.cpp
    test_core/test_window_layout.cpp
    test_core/test_window_geometry.cpp
    test_core/test_tool_registry.cpp
    test_core/test_mcp_tool_registry.cpp
    test_core/test_file_perms.cpp
    test_core/test_atomic_write.cpp
    test_core/test_db_backup.cpp
    test_core/test_event_bus.cpp
    test_platform/test_http_client.cpp
    test_platform/test_http_server.cpp
    test_platform/test_keychain.cpp
    test_platform/test_credential_protection.cpp
    test_platform/test_json.cpp
    test_platform/test_github_api.cpp
    test_platform/test_utf8.cpp
    test_platform/test_file_open.cpp
    test_platform/test_terminal_launch.cpp
    test_platform/test_file_watcher.cpp
    test_platform/test_tray_icon.cpp
    test_platform/test_single_instance.cpp
    test_platform/test_notification.cpp
    # 开机自启动平台层（UAT6）：注入缝假 writer 断言注册内容/注销/回读，headless 不写系统
    test_platform/test_autostart.cpp
    test_modules/test_markdown.cpp
    test_modules/test_markdown_service.cpp
    test_modules/test_md_folder_tree.cpp
    test_modules/test_markdown_tools.cpp
    test_modules/test_md_note_git.cpp
    test_modules/test_md_highlight.cpp
    test_modules/test_md_keyword_highlight.cpp
    test_modules/test_md_file_open.cpp
    test_modules/test_md_keyboard.cpp
    test_modules/test_md_block_model.cpp
    test_modules/test_md_block_interact.cpp
    test_modules/test_md_live_edit.cpp
    test_modules/test_md_live_spike.cpp
    test_modules/test_md_preview_highlight.cpp
    test_modules/test_md_sync_map.cpp
    test_modules/test_todo_service.cpp
    test_modules/test_todo_tools.cpp
    # UAT6.2 H31：todo 卡片关联 md 文档（置值/解除/失联灰态/迁移幂等/正面图标/
    # 详情选文档→跳转负载全链）
    test_modules/test_todo_doc_link.cpp
    test_modules/test_agent_store.cpp
    test_modules/test_agent_cron.cpp
    test_modules/test_agent_time_text.cpp
    test_modules/test_agent_trigger_config.cpp
    test_modules/test_agent_scheduler.cpp
    test_modules/test_agent_loop.cpp
    test_modules/test_agent_draft.cpp
    test_modules/test_agent_tools.cpp
    test_modules/test_hot_parse.cpp
    test_modules/test_hot_tool.cpp
    test_modules/test_notify_tool.cpp
    test_modules/test_app_now_tool.cpp
    test_modules/test_net_fetch_tool.cpp
    test_modules/test_message_blocks.cpp
    test_modules/test_agent_ui.cpp
    test_modules/test_agent_detail_layout.cpp
    test_modules/test_agent_system_prompt.cpp
    test_modules/test_direct_llm_client.cpp
    test_modules/test_todo_ui.cpp
    test_modules/test_ui_modern.cpp
    # W2b（§4.1/§4.15）：四级按钮 + FormRow helper 无头断言（色值/几何/尺寸统一）
    test_modules/test_ui_kit_button.cpp
    # W2c（§4.3/§4.2/§4.4）：选中态 / Tab / 开关 helper 无头断言
    #（绿竖线/绿下划线/绿轨道三件套 + 灰态防回归）
    test_modules/test_ui_kit_selection.cpp
    # W2d（§4.10/§4.12/§4.9）：微信式轻提示 + EmptyState + 问号字形验证无头断言
    #（黑半透明 bg alpha / 白字 onPrimary / 1.5s 自动消失 / 空状态就近按钮 /
    # 0xF059 问号字形绑定 fontFamily=Icon）。
    test_modules/test_ui_kit_feedback.cpp
    # UAT6.2 字号无级调节批：slider helper 无头断言（四节点/取值绑定/token 落点/
    # 行程映射与越界钳制）+ 归一化值→百分比映射钉值。
    test_modules/test_ui_kit_slider.cpp
    # 图标批（2026-10-20 C 方案终端窗 >_）：品牌几何 logo helper 契约
    #（结构 id / 归一化几何与位图资产同源 / 档位缩放线性 / opacity 透传）
    test_modules/test_logo_helper.cpp
    test_modules/test_view_state.cpp
    test_modules/test_claude.cpp
    test_modules/test_claude_service.cpp
    test_modules/test_credential_storage.cpp
    test_modules/test_claude_tools.cpp
    test_modules/test_provider_config.cpp
    test_modules/test_settings_sync.cpp
    test_modules/test_claude_ui.cpp
    test_modules/test_proxy_router.cpp
    test_modules/test_streaming_proxy.cpp
    test_modules/test_streaming_stress.cpp
    test_modules/test_upstream_dialect.cpp
    test_modules/test_home.cpp
    test_modules/test_github_sync_card.cpp
    test_modules/test_github_sync_card_spacing.cpp
    test_modules/test_github_sync_engine.cpp
    test_modules/test_github_sync_logic.cpp
    test_modules/test_local_git_probe.cpp
    test_modules/test_settings.cpp
    test_modules/test_module_icons.cpp
    test_modules/test_font_em_layout.cpp
    test_modules/test_module_gating.cpp
    test_modules/test_menu_beautify.cpp
    test_modules/test_mcp_session_store.cpp
    test_modules/test_mcp_protocol.cpp
    test_modules/test_mcp_tools_builtin.cpp
    test_modules/test_mcp_html_to_markdown.cpp
    test_modules/test_mcp_fetch_tool.cpp
    # UI 批：MCP 菜单页（卡片网格/详情弹窗/后台试运行）
    test_modules/test_mcp_page.cpp
    test_modules/test_tray_dynamic.cpp
    test_modules/test_theme_hierarchy.cpp
    test_modules/test_editor_font.cpp
    test_modules/test_app_sidebar.cpp
    test_modules/test_app_assembly.cpp
    test_modules/test_module_toggle_bridge.cpp
    # 全局 ticker（core 层）：注册/注销、firstAt/interval/expireAt 时间维度、
    # 回调内自注销、空表空转、user 透传
    test_modules/test_ticker.cpp
    test_modules/test_wallpaper_module.cpp
    # P2 纯逻辑 + P4 服务骨架：设置/轮换/让路/片库/文案/服务接线
    test_modules/test_wallpaper_core.cpp
    # P3a 平台层纯逻辑：mpv 路径/参数/JSON/管道名/stub 行为
    test_modules/test_wallpaper_platform.cpp
    # P5 播放/片库页面 UI：来源/Tab/状态卡/折叠/空态/片库卡/1000×640 无滚动条
    test_modules/test_wallpaper_ui.cpp
    # P5 尾托盘壁纸组：spec 形状/打开页映射/托盘桥/让路态暂停语义
    test_modules/test_wallpaper_tray.cpp
    test_modules/test_splash_contract.cpp
    # B3：MCP 设置面（配置/桥/卡片）+ 服务生命周期用例
    test_modules/test_mcp_settings.cpp
    test_modules/test_mcp_service.cpp
    # app::mcp 桥实现缝：app_assembly.cpp 与 settings_module.cpp 引用其符号
    #（module_toggle_bridge.cpp 同款理由）
    ${CMAKE_SOURCE_DIR}/src/mcp_service_bridge.cpp
)

# ==================== 用例登记自检（TIN-10） =============# 上面是**手工源表**：漏登记一个用例 .cpp = 该文件永不编译、用例永不注册，
# 套件仍然全绿且无任何提示（唯一信号是 Total 变小，文档改成「以运行输出为准」
# 后更难察觉）。这里拿磁盘实况与已登记表求差，非空即 configure 期 FATAL_ERROR。
# 只读 GLOB + 计数，不改任何编译行为。
file(GLOB _devdesk_cases_on_disk CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*/*.cpp)

get_target_property(_devdesk_declared devdesk_tests SOURCES)
# REAL_PATH 把 8.3 短名（C:/Users/XXXXXX~1/…）归一成长名——get_target_property
# 拿到的是相对路径、GLOB 拿到的是绝对路径，同一文件可能两种写法，不归一会误判
file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}" _devdesk_tests_dir)
set(_devdesk_cases_declared "")
foreach(_f ${_devdesk_declared})
    get_filename_component(_abs "${_f}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    file(REAL_PATH "${_abs}" _canon)
    # 只收 tests/ 下的用例文件：test_main.cpp（main 定义）与 src/*.cpp 不在核对范围
    string(FIND "${_canon}" "${_devdesk_tests_dir}/" _pos)
    if(NOT _pos EQUAL 0)
        continue()
    endif()
    if(_canon STREQUAL "${_devdesk_tests_dir}/test_main.cpp")
        continue()
    endif()
    list(APPEND _devdesk_cases_declared "${_canon}")
endforeach()

set(_devdesk_cases_on_disk_canon "")
foreach(_f ${_devdesk_cases_on_disk})
    file(REAL_PATH "${_f}" _canon)
    # skip probes/ dir (independent probe exes, not devdesk_tests cases)
    string(FIND "${_canon}" "${_devdesk_tests_dir}/probes/" _probe_pos)
    if(_probe_pos EQUAL 0)
        continue()
    endif()
    # skip live/ dir（真机验收驱动：独立可执行，真网络 + 真凭据，不进 devdesk_tests）
    string(FIND "${_canon}" "${_devdesk_tests_dir}/live/" _live_pos)
    if(_live_pos EQUAL 0)
        continue()
    endif()
    list(APPEND _devdesk_cases_on_disk_canon "${_canon}")
endforeach()

set(_devdesk_cases_missing "")  # 磁盘有、清单无 = 漏登记（会静默不跑）
foreach(_f ${_devdesk_cases_on_disk_canon})
    if(NOT _f IN_LIST _devdesk_cases_declared)
        list(APPEND _devdesk_cases_missing "${_f}")
    endif()
endforeach()
set(_devdesk_cases_stale "")    # 清单有、磁盘无 = 多余条目（文件已删/改名）
foreach(_f ${_devdesk_cases_declared})
    if(NOT _f IN_LIST _devdesk_cases_on_disk_canon)
        list(APPEND _devdesk_cases_stale "${_f}")
    endif()
endforeach()

if(_devdesk_cases_missing OR _devdesk_cases_stale)
    message(FATAL_ERROR
        "tests/CMakeLists.txt 的用例源表与磁盘不一致（TIN-10）：\n"
        "  漏登记（磁盘上有、add_executable 里没有——该文件不会被编译、用例不会被注册）:\n"
        "    ${_devdesk_cases_missing}\n"
        "  多余（add_executable 里有、磁盘上没有）:\n"
        "    ${_devdesk_cases_stale}\n"
        "  修法：把漏登记的文件补进 add_executable(devdesk_tests …)，或删掉已不存在的条目。")
endif()

devdesk_apply_warnings(devdesk_tests)

# ---- 版本真值注入（2026-09-29）----
# test_settings.cpp 的 aboutVersionLine 断言必须与**被测量**同源：
# devdesk_module_settings 用 PRIVATE 的 DEVDESK_VERSION="${PROJECT_VERSION}"
# 编译 settings_config.cpp，测试 TU 看不到那个宏，历史写法只能把版本号写死
# （v.find("DevDesk 0.1.")，VERSION 涨到 0.7.x 后恒红、且与真值脱钩）。这里把
# **同一个** PROJECT_VERSION 也注进测试 target，断言改成
# 「前缀 == "DevDesk " + DEVDESK_VERSION」——版本一变两边同步变。
# 只挂 devdesk_tests（probes 无版本断言）；不做 PUBLIC 传播，宏不外泄到生产目标。
target_compile_definitions(devdesk_tests PRIVATE
    DEVDESK_VERSION="${PROJECT_VERSION}"
)

target_include_directories(devdesk_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/src/module_claude_proxy
    ${CMAKE_SOURCE_DIR}/src/module_settings
    ${CMAKE_SOURCE_DIR}/src/module_mcp
)

# 确保 doctest.h 可被 #include "doctest.h" 找到

target_link_libraries(devdesk_tests PRIVATE
    devdesk_module_home
    devdesk_module_markdown
    devdesk_module_todo
    devdesk_agent
    devdesk_module_claude_proxy
    devdesk_module_settings
    devdesk_module_about
    devdesk_module_wallpaper
    devdesk_module_mcp
)

# 确保 doctest.h 可被 #include "doctest.h" 找到

add_test(NAME devdesk_unit_tests COMMAND devdesk_tests)

# 托盘图标资产拷贝（与 DevDesk app 目标同款 POST_BUILD，根 CMakeLists.txt :334-347）：
# test_tray_icon.cpp 的 tray_icon_asset_shipped 与 tray_icon_retina_sibling_shipped
# 两条用例通过 findShippedAsset 在 exeDir.parent_path()（即 <build>/assets/）下查找
# defaultIconPath() 返回的 assets 相对路径。app 目标的 POST_BUILD 只在 DevDesk 被
# 编译时运行——干净构建只编 devdesk_tests 不会触发资产拷贝，两条用例必红。
# 本块把托盘图标三件套复制到 devdesk_tests 可执行文件旁的上层 assets/ 目录（与
# DevDesk 目标的拷贝产物同径、同文件，不冗余写不变文件）。
add_custom_command(TARGET devdesk_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:devdesk_tests>/../assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/assets/tray-icon-template.png"
        "$<TARGET_FILE_DIR:devdesk_tests>/../assets/tray-icon-template.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/assets/tray-icon-template@2x.png"
        "$<TARGET_FILE_DIR:devdesk_tests>/../assets/tray-icon-template@2x.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/assets/tray-icon-win.ico"
        "$<TARGET_FILE_DIR:devdesk_tests>/../assets/tray-icon-win.ico"
    COMMENT "Copying tray icon assets for devdesk_tests"
)

# ==================== 独立 probe 可执行（probes/ 目录） =============# probes/ 下每个 .cpp 是独立小程序（如 dropdown z 可视化验证），不属于
# devdesk_tests 用例（已在上方 TIN-10 自检中排除）：单独编译成可执行，
# 以 LABELS "probe" 注册 ctest，便于定向跑单个 probe。
# DEVDESK_BUILD_PROBES=OFF（2026-10-03，CI 瘦身）：不生成任何 probe 目标与
# ctest 注册。CI 无 GUI 会话，6 个 probe 测试全走 SKIP_RETURN_CODE 77 跳过，
# 但 4 个 probe 可执行仍要编译链接——纯浪费。本地默认 ON（真门禁在本地）。
# （2026-10-03 轻量化：probes/ 从 12 个 .cpp 收到 4 个——删掉的 8 个与无头单测
#  逐条重复，且其中 2 个还被刻意设计成「当前必红」当 TODO 清单用，红灯混在门禁
#  里会淹没真信号。留下的 4 个各有独占判据，见 CLAUDE.md「功能开发生命周期」。）
# devdesk_tests 不引用任何 probes/*.cpp（TIN-10 也把 probes/ 排除在用例核对
# 外），整段包进开关不影响单测；live 类测试（LABELS "live"）不靠 GLOB 注册、
# 也不受本开关影响，见下方各自专段。
option(DEVDESK_BUILD_PROBES "构建 UI probe 可执行（无 GUI 环境如 CI 设 OFF，省 12 个可执行的编译链接时长）" ON)
if(DEVDESK_BUILD_PROBES)
file(GLOB _devdesk_probe_sources CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/probes/*.cpp)

foreach(_probe_source ${_devdesk_probe_sources})
    get_filename_component(_probe_name "${_probe_source}" NAME_WE)
    # module_toggle_bridge.cpp：settings 模块引用 app::toggle 桥符号，probe
    # 链接集（module_* 库）里没有 app_assembly.cpp，缝必须单独编进来
    # module_toggle_sidebar_probe / wallpaper_probe：活门控真窗口 probe 还要真
    # app::compose + 真 dispatchModuleToggle → 额外编译 app_sidebar.cpp +
    # app_assembly.cpp（其余 probe 维持最小链接面，不拖 app 层）
    # mcp_page_module.cpp：app_registry.h 的 kModuleRegistry（inline constexpr）
    # 初始化器取全部工厂地址，编 app_assembly.cpp 即 odr-use
    # create_mcp_page_module——其宿主库 devdesk_module_mcp 不在本分支链接集，
    # 该单源须编进来（自洽：只依赖 core/eui/桥缝，不依赖库内其他 TU）
    if(_probe_name STREQUAL "module_toggle_sidebar_probe" OR
       _probe_name STREQUAL "wallpaper_probe" OR
       _probe_name STREQUAL "h13_fluid_background_probe" OR
       _probe_name STREQUAL "uat62_page_shots_probe")
        add_executable(${_probe_name} "${_probe_source}"
            "${CMAKE_SOURCE_DIR}/src/module_toggle_bridge.cpp"
            "${CMAKE_SOURCE_DIR}/src/mcp_service_bridge.cpp"
            "${CMAKE_SOURCE_DIR}/src/app_sidebar.cpp"
            "${CMAKE_SOURCE_DIR}/src/app_assembly.cpp"
            "${CMAKE_SOURCE_DIR}/src/module_mcp/mcp_page_module.cpp")
    else()
        # mcp_service_bridge.cpp：probe 全体链 devdesk_module_settings，settings 引用
        # app::mcp::* 桥符号后缝必须单独编进来（与 module_toggle_bridge.cpp 同款理由）
        add_executable(${_probe_name} "${_probe_source}"
            "${CMAKE_SOURCE_DIR}/src/module_toggle_bridge.cpp"
            "${CMAKE_SOURCE_DIR}/src/mcp_service_bridge.cpp")
    endif()
    target_include_directories(${_probe_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests
        ${CMAKE_SOURCE_DIR}/src/module_claude_proxy
        ${CMAKE_SOURCE_DIR}/src/module_settings
        ${CMAKE_SOURCE_DIR}/src/module_wallpaper
    )
    target_link_libraries(${_probe_name} PRIVATE
        devdesk_module_home
        devdesk_module_markdown
        devdesk_module_todo
        devdesk_agent
        devdesk_module_claude_proxy
        devdesk_module_settings
        devdesk_module_about
        devdesk_module_wallpaper
    )
    # probe_harness.h 的帧捕获段（--screenshot）要 <png.h> 编码 PNG：libpng 由
    # EUI-NEO vendored（1.6.43，简化 API png_image_write_to_file 自带 deflate），
    # 这里只把它的 target 显式链给探针 + 借它传递 include 路径（源目录 + 生成
    # pnglibconf.h 的 build 目录），不引入任何新 vendored 头。EUI-NEO 若哪天
    # 不再带 libpng，这里硬失败——截图能力不能悄悄消失。
    if(TARGET PNG::PNG)
        target_link_libraries(${_probe_name} PRIVATE PNG::PNG)
    elseif(TARGET png_static)
        target_link_libraries(${_probe_name} PRIVATE png_static)
    else()
        message(FATAL_ERROR
            "probe 帧捕获（--screenshot）需要 libpng：EUI-NEO 未导出 "
            "PNG::PNG / png_static target")
    endif()
    devdesk_apply_warnings(${_probe_name})
    if(MSVC)
        target_link_options(${_probe_name} PRIVATE /SUBSYSTEM:CONSOLE)
    elseif(MINGW)
        target_link_options(${_probe_name} PRIVATE -mconsole)
    endif()
    add_test(NAME ${_probe_name} COMMAND ${_probe_name})
    set_tests_properties(${_probe_name} PROPERTIES LABELS "probe")
    # 无显示环境（CI runner 不在 WindowServer / 桌面会话里）→ 探针以
    # probe::kSkipExitCode(77) 退出，这里把它登记成 ctest 的 Skipped 而非
    # Failed。**判据在 tests/probes/probe_harness.h 的金丝雀逻辑里**（裸 GLFW
    # 建一个探针同等要求的隐藏窗，建不出来且错误码属「平台/能力不可用」才跳）。
    # 本行只负责告诉 ctest「77 = 跳过」，不负责判定；少这一行 77 会被当失败。
    set_tests_properties(${_probe_name} PROPERTIES SKIP_RETURN_CODE 77)
    # --screenshot 变体：默认那条不带参数走的是「零开销不写文件」路径，光靠它证
    # 不了开关本身能落图。补一条带参的（落图留在构建目录，build/ 已 gitignore）。
    if(_probe_name STREQUAL "screenshot_probe")
        add_test(NAME ${_probe_name}_screenshot_switch
            COMMAND ${_probe_name}
                --screenshot "${CMAKE_CURRENT_BINARY_DIR}/probe-screenshot.png")
        set_tests_properties(${_probe_name}_screenshot_switch
            PROPERTIES LABELS "probe")
        set_tests_properties(${_probe_name}_screenshot_switch
            PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    # home 的短窗尺寸档（2026-10-03 final 批；设计最小窗 UAT3 D-R3-2 起为
    # 1000×640 逻辑（main.cpp:492-493 kMinWindowLogical*），两平台均不钳——
    # 本档 600 刻意**低于**一切真实下限（= 设计下限 640），作守护性深档；
    # 600 低于首页固定内容高，滚动条吸收溢出，保留「真实窗口 + 真实滚动管线」
    # 整跑。默认那条按 1200×1000 跑；组 K 的多档无头 compose 覆盖 845 合成/778
    # 深矮窗/640 设计/600 守护四档高度。
    if(_probe_name STREQUAL "home_probes")
        add_test(NAME ${_probe_name}_min_window
            COMMAND ${_probe_name} --size 1200x600)
        set_tests_properties(${_probe_name}_min_window
            PROPERTIES LABELS "probe")
        set_tests_properties(${_probe_name}_min_window
            PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endforeach()
endif()

# ==================== GitHub Sync 真机验收（**默认不构建**）====================
# tests/live/github_sync_live.cpp 驱动真同步引擎打真 GitHub API：真网络 +
# 真 PAT + 真远端仓，CI 与离线环境必红，所以**默认门禁之外**、LABELS "live"。
#
# **为什么它走开关门控的 CMake 目标**：本驱动要实例化
# SettingsModule（UI 模块），必须链 EUI-NEO 静态库 + macOS 系统框架 + freetype /
# png / md4c / zlib 一整套 vendored 依赖 —— 手敲链接是纯负债。改为**开关门控的
# CMake 目标**：默认 OFF → 默认构建 / CI / `ctest -L probe` 全都看不到它。
#
# 显式入口（两条等价）：
#     cmake -B build ... -DDEVDESK_BUILD_LIVE_TOOLS=ON && cmake --build build
#     bash scripts/run-ghsync-live.sh [scenario ...]
#     (cd build && ctest -L live)
option(DEVDESK_BUILD_LIVE_TOOLS
    "构建真机验收驱动（tests/live/；真网络 + 真凭据，默认 OFF）" OFF)
if(DEVDESK_BUILD_LIVE_TOOLS AND UNIX)
    # **整体（目标 + ctest 条目）都在 if(UNIX) 里**，不只是 add_test：受编集合
    # 解析器按 if(WIN32)/if(UNIX) 划 Windows 编译面，块里有裸 add_executable 时
    # github_sync_live.cpp 被视作 Windows 必编 → CI Windows job 必红（驱动用
    # popen/sys/wait.h，Windows 无此物）。驱动是 macOS/Linux 真机验收工具，
    # Windows 侧没有「编译了但跑不了」的中间态。
    # module_toggle_bridge.cpp：settings 模块引用 app::toggle 桥符号
    # app_sidebar.cpp / app_assembly.cpp：同 module_toggle_sidebar_probe 的理由
    #（settings_module.cpp 的装配期符号在这些 TU 里）
    # mcp_page_module.cpp：同 probe——kModuleRegistry odr-use 其工厂地址，
    # 链接集里没有 devdesk_module_mcp
    add_executable(github_sync_live
        live/github_sync_live.cpp
        ${CMAKE_SOURCE_DIR}/src/module_toggle_bridge.cpp
        ${CMAKE_SOURCE_DIR}/src/mcp_service_bridge.cpp
        ${CMAKE_SOURCE_DIR}/src/app_sidebar.cpp
        ${CMAKE_SOURCE_DIR}/src/app_assembly.cpp
        ${CMAKE_SOURCE_DIR}/src/module_mcp/mcp_page_module.cpp)
    target_include_directories(github_sync_live PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests
        ${CMAKE_SOURCE_DIR}/src/module_claude_proxy
        ${CMAKE_SOURCE_DIR}/src/module_settings
    )
    target_link_libraries(github_sync_live PRIVATE
        devdesk_module_home
        devdesk_module_markdown
        devdesk_module_todo
        devdesk_agent
        devdesk_module_claude_proxy
        devdesk_module_settings
        devdesk_module_about
        devdesk_module_wallpaper
    )
    devdesk_apply_warnings(github_sync_live)

    add_test(NAME github_sync_live
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/run-ghsync-live.sh"
            "${CMAKE_BINARY_DIR}")
    # LABELS 只有 live、没有 probe：默认门禁跑 `ctest -L probe`，看不到它。
    set_tests_properties(github_sync_live PROPERTIES LABELS "live")
endif()

# ==================== 智能体真机全场景验收（**默认不构建**）====================
# tests/live/agent_live.cpp 驱动进程内 AgentLoop + DirectLlmClient 用**当前激活
# profile 的真 key** 发真 LLM 请求跑场景矩阵 A-I —— 真网络 + 真凭据，CI 与离线
# 环境必红，同 github_sync_live 一类：live 目标默认不构建、不进任何门禁。
#
# **为什么整段包 DEVDESK_BUILD_LIVE_TOOLS && UNIX 双门**：目标链接 todo / markdown /
# claude_proxy / settings / agent 五个模块库，默认 OFF → 默认构建 / CI / Windows
# 受编集合全都看不到它（矩阵票审计教训：live 类目标不受门控会进 Windows 编译炸 CI）。
#
# 显式入口（两条等价）：
#     cmake -B build ... -DDEVDESK_BUILD_LIVE_TOOLS=ON && cmake --build build --target agent_live
#     bash scripts/run-agent-live.sh [scenario ...]
#     (cd build && ctest -L live)
if(DEVDESK_BUILD_LIVE_TOOLS AND UNIX)
    # module_toggle_bridge.cpp：settings 模块引用 app::toggle 桥符号
    #（与 devdesk_tests 同款理由，见该目标注释）
    add_executable(agent_live
        live/agent_live.cpp
        ${CMAKE_SOURCE_DIR}/src/module_toggle_bridge.cpp
        ${CMAKE_SOURCE_DIR}/src/mcp_service_bridge.cpp)
    # module_claude_proxy / module_settings：claude_tools.h / app_tools.h 的同级头
    target_include_directories(agent_live PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/module_claude_proxy
        ${CMAKE_SOURCE_DIR}/src/module_settings)
    target_link_libraries(agent_live PRIVATE
        devdesk_module_todo
        devdesk_module_markdown
        devdesk_module_claude_proxy
        devdesk_module_settings
        devdesk_agent)
    devdesk_apply_warnings(agent_live)

    add_test(NAME agent_live
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/run-agent-live.sh"
            "${CMAKE_BINARY_DIR}")
    # LABELS 只有 live、没有 probe：默认门禁跑 `ctest -L probe`，看不到它。
    set_tests_properties(agent_live PROPERTIES LABELS "live")
endif()

# ==================== B4：MCP live 驱动（实连，默认不构建）====================
# 台账 B4-02 的**独立块，无 AND UNIX**：mcp_live.cpp 零平台宏、include 全
# "module_mcp/…" 前缀形态，Windows 受编集合必须纳入它（交叉编译守卫同验）。
# 显式入口：cmake -B build -DDEVDESK_BUILD_LIVE_TOOLS=ON && cmake --build build
#           --target mcp_live；build/tests/mcp_live --check | --serve
if(DEVDESK_BUILD_LIVE_TOOLS)
    # 只链 devdesk_module_mcp（core / platform 经其 PUBLIC 传递）；mcp_live.cpp
    # 不引用 app 层符号,bridge.cpp 只依赖 core——不拉 settings 等兄弟模块
    add_executable(mcp_live
        live/mcp_live.cpp
        ${CMAKE_SOURCE_DIR}/src/mcp_service_bridge.cpp)
    target_include_directories(mcp_live PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests)
    target_link_libraries(mcp_live PRIVATE devdesk_module_mcp)
    devdesk_apply_warnings(mcp_live)

    add_test(NAME mcp_live_check COMMAND mcp_live --check)
    # LABELS 只有 live、没有 probe：默认门禁跑 `ctest -L probe`，看不到它。
    set_tests_properties(mcp_live_check PROPERTIES LABELS "live")
endif()
