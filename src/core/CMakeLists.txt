add_library(devdesk_core STATIC
    result.cpp
    paths.cpp
    sqlite_db.cpp
    logging.cpp
    theme.cpp
    router.cpp
    proxy_stats.cpp
    database.cpp
)

# 平台适配实现按平台选择编译（接口隔离：头文件平台无关，实现 *_win/_macos/_posix，
# 业务代码零平台宏 —— 与 EUI-NEO 后端选择同一模式）
if(WIN32)
    target_sources(devdesk_core PRIVATE
        paths_win.cpp
        time_win.cpp
    )
elseif(APPLE)
    target_sources(devdesk_core PRIVATE
        paths_macos.cpp
        time_posix.cpp
    )
else()
    target_sources(devdesk_core PRIVATE
        paths_posix.cpp
        time_posix.cpp
    )
endif()

devdesk_apply_warnings(devdesk_core)

target_include_directories(devdesk_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    # 注意：不要把 src/core 自身加进 include 路径——src/core/time.h 会经 -I 优先级
    # 劫持系统 <time.h>/<ctime>（MinGW 下 <mutex>→<ctime> 链条实测编译爆炸）。
    # core 内部一律同目录引号 include（"time.h"），外部一律 <core/xxx.h>（走 -I src）。
)

# EUI-NEO include 路径（直接添加，因为 add_subdirectory EXCLUDE_FROM_ALL 时 PUBLIC 链路可能不传递）
target_include_directories(devdesk_core PUBLIC
    ${EUI_NEO_DIR}/include
    ${EUI_NEO_DIR}
    ${EUI_NEO_DIR}/3rd/yyjson-0.12.0/src
    ${EUI_NEO_DIR}/3rd/md4c/src
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