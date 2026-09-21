# ---------------------------------------------------------------------------
# Windows/MSYS2 工具链查找提示（烤进 CMake，全部带 NOT DEFINED 守卫，-D 可覆盖）
#
# 之前这些是每次 configure 手敲的 -D workaround（OPENSSL_ROOT_DIR / ZLIB_*），
# 现固化为默认值，裸 cmake configure 零 -D 参数即可用。
# ---------------------------------------------------------------------------
if(WIN32 AND EXISTS "C:/msys64/mingw64")
    # OpenSSL：MSYS2 MinGW64 安装根（FindOpenSSL 认 OPENSSL_ROOT_DIR）
    if(NOT DEFINED OPENSSL_ROOT_DIR AND NOT DEFINED OpenSSL_ROOT)
        set(OPENSSL_ROOT_DIR "C:/msys64/mingw64")
    endif()

    # ZLIB 兜底：若走到系统 FindZLIB（EUI-NEO 内置 zlib 未被某消费者采用时），
    # 库用 MSYS2 的，头必须用 EUI-NEO 自带 1.3.1 ——
    # libpng 1.6.43 与 zlib 头版本严格配对，系统 1.3.2 头会
    # #error ZLIB_VERNUM != PNG_ZLIB_VERNUM
    if(NOT DEFINED ZLIB_LIBRARY AND EXISTS "C:/msys64/mingw64/lib/libz.dll.a")
        set(ZLIB_LIBRARY "C:/msys64/mingw64/lib/libz.dll.a"
            CACHE FILEPATH "zlib library (MSYS2 fallback)")
    endif()
    if(NOT DEFINED ZLIB_INCLUDE_DIR AND EXISTS "${EUI_NEO_DIR}/3rd/zlib-1.3.1/zlib.h")
        set(ZLIB_INCLUDE_DIR "${EUI_NEO_DIR}/3rd/zlib-1.3.1"
            CACHE PATH "zlib headers (EUI-NEO bundled 1.3.1, pairs with libpng)")
    endif()
endif()

# 系统依赖
find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# 验证 SQLite3 FTS5（编译+链接测试）
# 注意：必须把 SQLite3 库放进 CMAKE_REQUIRED_LIBRARIES —— 否则 sqlite3_open 等
# 符号链接失败 → 假阴性（MSYS2 sqlite 实际带 FTS5，之前被误报 NOT enabled）
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_QUIET TRUE)
set(CMAKE_REQUIRED_LIBRARIES SQLite3::SQLite3)
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
unset(CMAKE_REQUIRED_LIBRARIES)

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
