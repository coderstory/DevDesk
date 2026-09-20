add_library(devdesk_core STATIC
    result.cpp
    paths.cpp
    sqlite_db.cpp
    logging.cpp
    theme.cpp
    router.cpp
    drag_controller.cpp
)

target_include_directories(devdesk_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(devdesk_core
    PUBLIC
        SQLite::SQLite3
        yyjson
        md4c
        Threads::Threads
)

target_compile_definitions(devdesk_core PUBLIC
    SQLITE_HAS_FTS5=${SQLITE3_HAS_FTS5}
)

# 添加 EUI-NEO include
if(TARGET eui_neo)
    target_link_libraries(devdesk_core PUBLIC eui_neo)
else()
    target_include_directories(devdesk_core PUBLIC
        ${CMAKE_SOURCE_DIR}/../EUI-NEO/include
        ${CMAKE_SOURCE_DIR}/../EUI-NEO/3rd/yyjson-0.12.0/src
        ${CMAKE_SOURCE_DIR}/../EUI-NEO/3rd/md4c/src
    )
endif()