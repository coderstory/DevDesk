add_executable(devdesk_tests
    test_main.cpp
    test_core/test_result.cpp
    test_core/test_paths.cpp
    test_core/test_logging.cpp
    test_core/test_sqlite.cpp
    test_core/test_theme.cpp
    test_core/test_migrations.cpp
    test_platform/test_http_client.cpp
    test_platform/test_http_server.cpp
    test_platform/test_keychain.cpp
    test_platform/test_json.cpp
    test_modules/test_markdown.cpp
    test_modules/test_markdown_service.cpp
    test_modules/test_todo.cpp
    test_modules/test_todo_service.cpp
    test_modules/test_claude.cpp
    test_modules/test_claude_service.cpp
    test_modules/test_proxy_router.cpp
    test_modules/test_streaming_proxy.cpp
    test_modules/test_streaming_stress.cpp
    test_modules/test_home.cpp
    test_modules/test_settings.cpp
)

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
)

# 确保 doctest.h 可被 #include "doctest.h" 找到

add_test(NAME devdesk_unit_tests COMMAND devdesk_tests)