# 系统依赖
find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# 验证 SQLite3 FTS5（运行时编译测试）
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_QUIET TRUE)
set(_devdesk_fts5_check "
#include <sqlite3.h>
int main() {
    sqlite3* db = nullptr;
    if (sqlite3_open(\":memory:\", &db) != SQLITE_OK) return 1;
    char* err = nullptr;
    int rc = sqlite3_exec(db,
        \"CREATE VIRTUAL TABLE t USING fts5(content);\",
        nullptr, nullptr, &err);
    sqlite3_close(db);
    return (rc == SQLITE_OK) ? 0 : 1;
}
")
check_cxx_source_compiles("${_devdesk_fts5_check}" SQLITE3_HAS_FTS5)
unset(CMAKE_REQUIRED_QUIET)

if(SQLITE3_HAS_FTS5)
    message(STATUS "SQLite3 FTS5: enabled")
    add_compile_definitions(SQLITE3_HAS_FTS5=1)
else()
    message(WARNING "SQLite3 FTS5: NOT enabled - Markdown search will fallback to LIKE")
    add_compile_definitions(SQLITE3_HAS_FTS5=0)
endif()

# yyjson / md4c / FreeType 复用 EUI-NEO 自带
add_library(yyjson INTERFACE)
target_include_directories(yyjson INTERFACE
    "${EUI_NEO_DIR}/3rd/yyjson-0.12.0/src")

add_library(md4c INTERFACE)
target_include_directories(md4c INTERFACE
    "${EUI_NEO_DIR}/3rd/md4c/src")