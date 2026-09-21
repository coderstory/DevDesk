add_library(devdesk_platform STATIC
    http_client.cpp
    http_server.cpp
    keychain.cpp
    json.cpp
)

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

# macOS Security.framework (Keychain)
if(APPLE)
    target_link_libraries(devdesk_platform PUBLIC "-framework Security")
endif()

# Windows：Winsock2（http_client/http_server 网络）+ Crypt32（TLS 证书链）
# 之前靠 -DCMAKE_CXX_STANDARD_LIBRARIES="-lws2_32 -lcrypt32" 绕过，现烤进构建系统
if(WIN32)
    target_link_libraries(devdesk_platform PUBLIC ws2_32 crypt32)
endif()

# DevDesk 自己的警告等级（-Werror 只作用于 DevDesk target，不泄漏给第三方）
devdesk_apply_warnings(devdesk_platform)