add_executable(devdesk_tests
    test_main.cpp
    ${CMAKE_SOURCE_DIR}/src/app_sidebar.cpp
    ${CMAKE_SOURCE_DIR}/src/app_assembly.cpp
    test_core/test_result.cpp
    test_core/test_paths.cpp
    test_core/test_logging.cpp
    test_core/test_sqlite.cpp
    test_core/test_theme.cpp
    test_core/test_theme_fontscale.cpp
    test_core/test_router.cpp
    test_core/test_migrations.cpp
    test_core/test_proxy_stats.cpp
    test_core/test_time.cpp
    test_core/test_window_layout.cpp
    test_platform/test_http_client.cpp
    test_platform/test_http_server.cpp
    test_platform/test_keychain.cpp
    test_platform/test_json.cpp
    test_platform/test_file_open.cpp
    test_platform/test_file_watcher.cpp
    test_modules/test_markdown.cpp
    test_modules/test_markdown_service.cpp
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
    test_modules/test_todo_ui.cpp
    test_modules/test_view_state.cpp
    test_modules/test_claude.cpp
    test_modules/test_claude_service.cpp
    test_modules/test_provider_config.cpp
    test_modules/test_settings_sync.cpp
    test_modules/test_claude_ui.cpp
    test_modules/test_proxy_router.cpp
    test_modules/test_streaming_proxy.cpp
    test_modules/test_streaming_stress.cpp
    test_modules/test_upstream_dialect.cpp
    test_modules/test_home.cpp
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
)

# ==================== 用例登记自检（TIN-10） ====================
# 上面是**手工源表**：漏登记一个用例 .cpp = 该文件永不编译、用例永不注册，
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
    devdesk_module_claude_proxy
    devdesk_module_settings
    devdesk_module_pet
)

# 确保 doctest.h 可被 #include "doctest.h" 找到

add_test(NAME devdesk_unit_tests COMMAND devdesk_tests)