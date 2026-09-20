add_library(devdesk_platform STATIC
    http_client.cpp
    http_server.cpp
    keychain.cpp
    json.cpp
)

target_include_directories(devdesk_platform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(devdesk_platform
    PUBLIC
        devdesk_core
        CURL::libcurl
        Threads::Threads
)