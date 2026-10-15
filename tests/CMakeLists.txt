add_executable(devdesk_tests
    test_main.cpp
    ${CMAKE_SOURCE_DIR}/src/app_sidebar.cpp
    ${CMAKE_SOURCE_DIR}/src/app_assembly.cpp
    # app::toggle 桥实现缝：settings_module.cpp 引用其符号；probe 同样需要
    #（probe 只链 module_* 库，app_assembly.cpp 不在 probe 链接集）
    ${CMAKE_SOURCE_DIR}/src/module_toggle_bridge.cpp
    test_core/test_result.cpp
    test_core/test_paths.cpp
    test_core/test_logging.cpp
    test_core/test_sqlite.cpp
    test_core/test_sqlite_concurrency.cpp
    test_core/test_theme.cpp
    test_core/test_theme_fontscale.cpp
    test_core/test_router.cpp
    test_core/test_schema.cpp
    test_core/test_app_config.cpp
    test_core/test_proxy_stats.cpp
    test_core/test_time.cpp
    test_core/test_window_layout.cpp
    test_core/test_tool_registry.cpp
    test_core/test_file_perms.cpp
    test_core/test_atomic_write.cpp
    test_core/test_db_backup.cpp
    test_core/test_event_bus.cpp
    test_platform/test_http_client.cpp
    test_platform/test_http_server.cpp
    test_platform/test_keychain.cpp
    test_platform/test_credential_protection.cpp
    test_platform/test_json.cpp
    test_platform/test_utf8.cpp
    test_platform/test_file_open.cpp
    test_platform/test_terminal_launch.cpp
    test_platform/test_file_watcher.cpp
    test_platform/test_tray_icon.cpp
    test_modules/test_markdown.cpp
    test_modules/test_markdown_service.cpp
    test_modules/test_markdown_tools.cpp
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
    test_modules/test_agent_store.cpp
    test_modules/test_agent_cron.cpp
    test_modules/test_agent_trigger_config.cpp
    test_modules/test_agent_scheduler.cpp
    test_modules/test_agent_loop.cpp
    test_modules/test_agent_tools.cpp
    test_modules/test_message_blocks.cpp
    test_modules/test_agent_ui.cpp
    test_modules/test_llm_client.cpp
    test_modules/test_todo_ui.cpp
    test_modules/test_ui_modern.cpp
    test_modules/test_view_state.cpp
    test_modules/test_claude.cpp
    test_modules/test_claude_service.cpp
    test_modules/test_credential_storage.cpp
    test_modules/test_claude_tools.cpp
    test_modules/test_claude_quota.cpp
    test_modules/test_claude_quota_ccswitch.cpp
    test_modules/test_ccswitch_import.cpp
    test_modules/test_claude_quota_dialog.cpp
    test_modules/test_claude_quota_minimax.cpp
    test_modules/test_claude_quota_store.cpp
    test_modules/test_claude_quota_volcano.cpp
    test_modules/test_claude_quota_live_fixtures.cpp
    test_modules/test_claude_quota_lifecycle.cpp
    # 「真机探针绝不进默认门禁」的**默认门禁里跑的**代码断言。
    # 此前 tests/CMakeLists.txt 与 scripts/run-quota-live-probe.sh 都声称
    # 「由本文件守门」，而文件不存在（静态审计 B2）。补上，注释才变成真的。
    test_modules/test_probe_gate_labels.cpp
    test_modules/test_provider_config.cpp
    test_modules/test_settings_sync.cpp
    test_modules/test_claude_ui.cpp
    test_modules/test_claude_calls_latency.cpp
    test_modules/test_proxy_router.cpp
    test_modules/test_streaming_proxy.cpp
    test_modules/test_streaming_stress.cpp
    test_modules/test_upstream_dialect.cpp
    test_modules/test_home.cpp
    test_modules/test_home_quota_card.cpp
    test_modules/test_settings.cpp
    test_modules/test_module_icons.cpp
    test_modules/test_font_em_layout.cpp
    test_modules/test_module_gating.cpp
    test_modules/test_pet.cpp
    test_modules/test_menu_beautify.cpp
    test_modules/test_tray_dynamic.cpp
    test_modules/test_theme_hierarchy.cpp
    test_modules/test_editor_font.cpp
    test_modules/test_app_sidebar.cpp
    test_modules/test_app_assembly.cpp
    test_modules/test_module_toggle_bridge.cpp
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
    devdesk_module_pet
)

# 确保 doctest.h 可被 #include "doctest.h" 找到

add_test(NAME devdesk_unit_tests COMMAND devdesk_tests)

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
# 外），整段包进开关不影响单测；quota_live_probe 在本段之外（它是 bash 脚本
# 测试、LABELS "live"，没有编译成本，也不该跟着消失）。
option(DEVDESK_BUILD_PROBES "构建 UI probe 可执行（无 GUI 环境如 CI 设 OFF，省 12 个可执行的编译链接时长）" ON)
if(DEVDESK_BUILD_PROBES)
file(GLOB _devdesk_probe_sources CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/probes/*.cpp)

foreach(_probe_source ${_devdesk_probe_sources})
    get_filename_component(_probe_name "${_probe_source}" NAME_WE)
    # module_toggle_bridge.cpp：settings 模块引用 app::toggle 桥符号，probe
    # 链接集（module_* 库）里没有 app_assembly.cpp，缝必须单独编进来
    # module_toggle_sidebar_probe：活门控真窗口 probe 还要真 app::compose +
    # 真 dispatchModuleToggle → 额外编译 app_sidebar.cpp + app_assembly.cpp
    #（其余 probe 维持最小链接面，不拖 app 层）
    if(_probe_name STREQUAL "module_toggle_sidebar_probe")
        add_executable(${_probe_name} "${_probe_source}"
            "${CMAKE_SOURCE_DIR}/src/module_toggle_bridge.cpp"
            "${CMAKE_SOURCE_DIR}/src/app_sidebar.cpp"
            "${CMAKE_SOURCE_DIR}/src/app_assembly.cpp")
    else()
        add_executable(${_probe_name} "${_probe_source}"
            "${CMAKE_SOURCE_DIR}/src/module_toggle_bridge.cpp")
    endif()
    target_include_directories(${_probe_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests
        ${CMAKE_SOURCE_DIR}/src/module_claude_proxy
        ${CMAKE_SOURCE_DIR}/src/module_settings
    )
    target_link_libraries(${_probe_name} PRIVATE
        devdesk_module_home
        devdesk_module_markdown
        devdesk_module_todo
        devdesk_agent
        devdesk_module_claude_proxy
        devdesk_module_settings
        devdesk_module_about
        devdesk_module_pet
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
    # home 的短窗尺寸档（2026-10-03 final 批；同日口径核定：真实设计最小窗是
    # 1330×845 逻辑（main.cpp:529-530 kMinWindowLogical*），1200×720 是 spec
    # 早期过期值——本档刻意**低于**一切真实下限，作守护性深档）。默认那条按
    # 1200×1000 跑，而「矮窗下余量卡画不画」在 1000 高的旧实现也画 → 只跑
    # 默认档恒真。组 K 的多档无头 compose 让每条注册档都覆盖 845/805/700 三档
    # 真实/守护高度；本条额外保留「真实窗口 + 真实滚动管线」在 720 下的整跑。
    if(_probe_name STREQUAL "home_probes")
        add_test(NAME ${_probe_name}_min_window
            COMMAND ${_probe_name} --size 1200x720)
        set_tests_properties(${_probe_name}_min_window
            PROPERTIES LABELS "probe")
        set_tests_properties(${_probe_name}_min_window
            PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endforeach()
endif()

# ==================== 真机出网探针（**默认门禁之外**，2026-10-03）============
# 走真网络 + 需要用户本机 ~/.cc-switch/cc-switch.db + 每次实打实打四家供应商
# 接口 → 绝不能进任何默认门禁（CI / 离线环境必红，且别的机器上没有凭据）。
#
# 但「跑它要打一长串编译命令」不可持续（G8 遗留 4：入口只有 .cpp 文件头那段
# 手敲命令，而那段命令在 macOS 上照抄会直接 link 失败——漏了 yyjson 目标与
# openssl 路径）。于是给一条**显式入口**：
#     bash scripts/run-quota-live-probe.sh [--dump <path>] [provider_id ...]
#     (cd build && ctest -L live)
#
# LABELS 只有 **live**、**没有 probe** —— 门禁跑的是 `ctest -L probe`，看不到它。
# 「绝不进默认门禁」由**两个独立的代码实现**守住（改坏任一个另一个仍报警）：
#   ① scripts/check-probe-gate-labels.sh（awk 状态机，build-test-run.sh 串跑）
#   ② tests/test_modules/test_probe_gate_labels.cpp（本文件下一段的单测，进全量）
# 两者解析路径刻意不同（awk vs C++ 逐行扫）：一个的解析逻辑被改坏时，另一个
# 仍是有效信号。**不是靠注释保证** —— 那句注释曾经声称有个不存在的文件在守。
if(UNIX)
    add_test(NAME quota_live_probe
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/run-quota-live-probe.sh"
            "${CMAKE_BINARY_DIR}")
    set_tests_properties(quota_live_probe PROPERTIES LABELS "live")
endif()