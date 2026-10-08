add_library(devdesk_platform STATIC
    http_client.cpp
    http_server.cpp
    json.cpp
    file_open_common.cpp
    terminal_launch_common.cpp          # 跨平台纯函数（三支之外，无条件编译）
)

# 平台适配实现按平台选择编译（接口隔离：头文件平台无关，实现 *_win/_macos/_posix，
# 业务代码零平台宏 —— 与 EUI-NEO 后端选择同一模式）
if(WIN32)
    target_sources(devdesk_platform PRIVATE
        keychain_win.cpp
        file_open_win.cpp
        system_theme_win.cpp
        file_watcher_win.cpp
        http_client_ca_win.cpp
        terminal_launch_win.cpp
    )
elseif(APPLE)
    target_sources(devdesk_platform PRIVATE
        keychain_macos.cpp
        file_open_macos.cpp
        system_theme_macos.cpp
        file_watcher_macos.cpp
        http_client_ca_posix.cpp
        terminal_launch_macos.cpp
    )
else()
    target_sources(devdesk_platform PRIVATE
        keychain_posix.cpp
        file_open_posix.cpp
        system_theme_posix.cpp
        file_watcher_stub.cpp
        http_client_ca_posix.cpp
        terminal_launch_posix.cpp
    )
endif()

target_include_directories(devdesk_platform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}
    # cpp-httplib header-only (P0a PoC)
    ${CMAKE_SOURCE_DIR}/3rd/cpp-httplib
)

target_link_libraries(devdesk_platform
    PUBLIC
        devdesk_core
        CURL::libcurl
        Threads::Threads
        # cpp-httplib OpenSSL 支持（可选，定义 CPPHTTPLIB_OPENSSL_SUPPORT 时需要）
        OpenSSL::SSL
        OpenSSL::Crypto
)

# 启用 cpp-httplib 的 OpenSSL 支持
target_compile_definitions(devdesk_platform PUBLIC
    CPPHTTPLIB_OPENSSL_SUPPORT
)

# macOS Security.framework (Keychain) + CoreFoundation（APPLE 分支系统深浅检测
# 实现读 NSGlobalDomain AppleInterfaceStyle 用；两者都只在 APPLE 分支链接）
# CoreServices：file_watcher_macos.cpp 的 FSEvents（FSEventStreamCreate 等）
if(APPLE)
    target_link_libraries(devdesk_platform PUBLIC
        "-framework Security"
        "-framework CoreFoundation"
        "-framework CoreServices"
    )
endif()

# Windows：Winsock2（http_client/http_server 网络）+ Crypt32（TLS 证书链）
# + Shell32（file_open_win.cpp 的 CommandLineToArgvW）
# 之前靠 -DCMAKE_CXX_STANDARD_LIBRARIES="-lws2_32 -lcrypt32" 绕过，现烤进构建系统
# advapi32：system_theme_win.cpp 的 RegGetValueW（跟随系统主题，spec 缩水 A）
if(WIN32)
    target_link_libraries(devdesk_platform PUBLIC ws2_32 crypt32 shell32 advapi32)
endif()

# DevDesk 自己的警告等级（-Werror 只作用于 DevDesk target，不泄漏给第三方）
devdesk_apply_warnings(devdesk_platform)