# need#490 网页终端测试源清单。
# 单独成文件的原因：W1–W7 各波并行改测试清单，主 tests/CMakeLists.txt 反复合并冲突；
# 主文件只 include + 展开变量，不再逐行出现 web 用例。
set(DEVDESK_WEB_TERMINAL_TEST_SOURCES
    test_modules/test_terminal_web_service.cpp
    test_modules/test_terminal_web_grid.cpp
    test_modules/test_terminal_web_bridge.cpp
    test_modules/test_terminal_web_crypto.cpp
    test_modules/test_terminal_web_auth.cpp
    test_modules/test_web_access_card.cpp
    test_modules/test_qr_encode.cpp
    test_platform/test_net_interfaces.cpp
)

# 直编进测试二进制（不在任何库）的支撑源：二维码编码器与加密原语。
set(DEVDESK_WEB_TERMINAL_TEST_SUPPORT_SOURCES
    ${CMAKE_SOURCE_DIR}/src/module_terminal/web/qr/qr_encode.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/web_crypto.cpp
)
