# ---------------------------------------------------------------------------
# Windows/MinGW 工具链查找提示（烤进 CMake，全部带 NOT DEFINED 守卫，-D 可覆盖）
#
# 之前这些是每次 configure 手敲的 -D workaround（OPENSSL_ROOT_DIR / ZLIB_*），
# 现固化为默认值，裸 cmake configure 零 -D 参数即可用。
# MinGW 安装根从编译器位置推导（如 C:/msys64/mingw64），不硬编码路径。
# ---------------------------------------------------------------------------
if(MINGW)
    get_filename_component(DEVDESK_MINGW_BINDIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(DEVDESK_MINGW_ROOT "${DEVDESK_MINGW_BINDIR}" DIRECTORY)
    if(NOT EXISTS "${DEVDESK_MINGW_ROOT}/lib")
        message(WARNING "DEVDESK: MinGW lib dir not found under compiler root: ${DEVDESK_MINGW_ROOT}/lib")
    endif()
endif()

if(MINGW AND EXISTS "${DEVDESK_MINGW_ROOT}/lib")
    # OpenSSL：MinGW 安装根（FindOpenSSL 认 OPENSSL_ROOT_DIR）
    if(NOT DEFINED OPENSSL_ROOT_DIR AND NOT DEFINED OpenSSL_ROOT)
        set(OPENSSL_ROOT_DIR "${DEVDESK_MINGW_ROOT}")
    endif()

    # ZLIB 兜底：若走到系统 FindZLIB（EUI-NEO 内置 zlib 未被某消费者采用时），
    # 库用 MinGW 的，头必须用 EUI-NEO 自带 1.3.1 ——
    # libpng 1.6.43 与 zlib 头版本严格配对，系统 1.3.2 头会
    # #error ZLIB_VERNUM != PNG_ZLIB_VERNUM
    if(DEVDESK_STATIC)
        set(_devdesk_zlib_lib "${DEVDESK_MINGW_ROOT}/lib/libz.a")
    else()
        set(_devdesk_zlib_lib "${DEVDESK_MINGW_ROOT}/lib/libz.dll.a")
    endif()
    if(NOT DEFINED ZLIB_LIBRARY AND EXISTS "${_devdesk_zlib_lib}")
        set(ZLIB_LIBRARY "${_devdesk_zlib_lib}"
            CACHE FILEPATH "zlib library (MinGW fallback)")
    endif()
    if(NOT DEFINED ZLIB_INCLUDE_DIR AND EXISTS "${EUI_NEO_DIR}/3rd/zlib-1.3.1/zlib.h")
        set(ZLIB_INCLUDE_DIR "${EUI_NEO_DIR}/3rd/zlib-1.3.1"
            CACHE PATH "zlib headers (EUI-NEO bundled 1.3.1, pairs with libpng)")
    endif()

    # SQLite3 / CURL 头文件同源直指：find_path 的默认搜索覆盖不了所有 MinGW 安装根
    # （编译器隐式 include 目录不参与搜索；独立安装的 MSYS2 与 CI 的 setup-msys2
    # 都可能落空），与下面两个 *_LIBRARY 钉死配对。按值判断 + FORCE：上次失败跑
    # 会把 NOTFOUND 焙进缓存，NOT DEFINED 挡不住、无 FORCE 的 CACHE set 也不覆盖
    if(NOT SQLite3_INCLUDE_DIR AND EXISTS "${DEVDESK_MINGW_ROOT}/include/sqlite3.h")
        set(SQLite3_INCLUDE_DIR "${DEVDESK_MINGW_ROOT}/include"
            CACHE PATH "SQLite3 headers (MinGW)" FORCE)
    endif()
    if(NOT CURL_INCLUDE_DIR AND EXISTS "${DEVDESK_MINGW_ROOT}/include/curl/curl.h")
        set(CURL_INCLUDE_DIR "${DEVDESK_MINGW_ROOT}/include/curl"
            CACHE PATH "CURL headers (MinGW)" FORCE)
    endif()

    # -----------------------------------------------------------------------
    # T3：MinGW 默认全静态（DEVDESK_STATIC，默认 ON，-DDEVDESK_STATIC=OFF 可关）
    # -----------------------------------------------------------------------
    if(DEVDESK_STATIC)
        # SQLite3：直接指 libsqlite3.a（find_library 默认优先 .dll.a 导入库）
        if(NOT DEFINED SQLite3_LIBRARY AND EXISTS "${DEVDESK_MINGW_ROOT}/lib/libsqlite3.a")
            set(SQLite3_LIBRARY "${DEVDESK_MINGW_ROOT}/lib/libsqlite3.a"
                CACHE FILEPATH "SQLite3 static library")
        endif()

        # CURL：MSYS2 的 CURLConfig.cmake 只导出 shared target（libcurl-4.dll），
        # 关掉 config 模式走 FindCURL 模块模式，并直接指 libcurl.a
        if(EXISTS "${DEVDESK_MINGW_ROOT}/lib/libcurl.a")
            if(NOT DEFINED CURL_NO_CURL_CMAKE)
                set(CURL_NO_CURL_CMAKE TRUE)
            endif()
            if(NOT DEFINED CURL_LIBRARY)
                set(CURL_LIBRARY "${DEVDESK_MINGW_ROOT}/lib/libcurl.a"
                    CACHE FILEPATH "libcurl static library")
            endif()
        endif()

        # ws2_32/crypt32 追加进标准库集（标准库块在链接行最末、位于 libcurl.a 之后）：
        #   GNU ld 左→右解析，curl 静态链的 Winsock 符号要求 -lws2_32 在其后；
        #   CMake 链接项去重保首位，走 target_link_libraries 无论放哪都会被早位出现抢走。
        #   append 不覆盖默认集（gdi32/user32 等保留）——这是当年 -D 覆盖事故（T2）的正解形态。
        # -lssl -lcrypto 已替换为显式静态 .a 路径（2026-10-19）：OPENSSL_USE_STATIC_LIBS
        #   （下方 L77-L79）让 FindOpenSSL 产出 libssl.a/libcrypto.a 进链接行，但它们位于
        #   libcurl.a 之前（CMake target_link_libraries 序），GNU ld 左→右单遍扫描无法用
        #   它们解析 libcurl.a 的 OpenSSL 引用。此处必须补一份 OpenSSL 在链接行末尾——
        #   但不能用 -lssl（MinGW 优先解析成 DLL 导入库 libssl.dll.a，与前方显式 libssl.a
        #   形成同一进程内静动混合链接 → OpenSSL 全局状态分裂 → CURLSSLOPT_NATIVE_CA 失效
        #   → libcurl 77）。故此用 ${DEVDESK_MINGW_ROOT}/lib/libssl.a 显式路径：末尾位置
        #   保证解析 libcurl.a 的引用，显式 .a 保证纯静态链接。-lws2_32 -lcrypt32 均已移
        #   至 .a 之后（libcrypto.a 引 gai_strerrorA 和 CertFindCertificateInStore，DLL
        #   导入库在 .a 之前时 GNU ld 已扫过不会为后续 .a 补存根；必须排在后面才生效）。
        #   实证见 2026-10-19 诊断：改前 agent_e2e_probe 稳定 77，改后全流程 77 消失。
        set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES} ${DEVDESK_MINGW_ROOT}/lib/libssl.a ${DEVDESK_MINGW_ROOT}/lib/libcrypto.a -lws2_32 -lcrypt32")

        # OpenSSL：静态模式（FindOpenSSL 把查找后缀收窄到 .a，
        # 并给 OpenSSL::SSL/Crypto 自动补 ws2_32/crypt32 接口依赖）
        if(NOT DEFINED OPENSSL_USE_STATIC_LIBS)
            set(OPENSSL_USE_STATIC_LIBS TRUE)
        endif()
    endif()
endif()

# 系统依赖
find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# L1（2026-09-27）：发布物可追溯 —— 单 exe 静态链入 OpenSSL / libcurl（安全相关），
# 出 CVE 时要能回答"这个 exe 里的 OpenSSL 是哪个版本"。版本来自在 include 目录里
# 解析出的 opensslv.h / curlver.h（由 FindOpenSSL / FindCURL 完成，不额外拉依赖）。
# 探测不到只打印"未检测到"，不 fail——版本探测失败不该阻断构建。
# 与顶层 CMakeLists 打印 EUI-NEO rev 同款做法；当前版本组合记在 CHANGELOG 顶部。
# ---------------------------------------------------------------------------
if(OPENSSL_VERSION)
    message(STATUS "OpenSSL version: ${OPENSSL_VERSION}")
else()
    message(STATUS "OpenSSL version: 未检测到（未解析到 opensslv.h）")
endif()
if(CURL_VERSION)
    message(STATUS "libcurl version: ${CURL_VERSION}")
elseif(CURL_VERSION_STRING)
    message(STATUS "libcurl version: ${CURL_VERSION_STRING}")
else()
    message(STATUS "libcurl version: 未检测到（未解析到 curlver.h）")
endif()

# ---------------------------------------------------------------------------
# T3：libcurl.a 静态传递依赖闭包
#
# MSYS2 的 libcurl.a 是全功能构建（SSH / HTTP2 / HTTP3(QUIC) / brotli / zstd /
# IDN / psl），静态链接时其符号引用必须全部显式补齐，缺一个就 undefined
# reference。闭包与 `pkg-config --static --libs libcurl` 对齐。
# 顺序规则：依赖方在前、被依赖方在后（GNU ld 对 .a 单遍扫描）；
# ssl/crypto 不在此列——由 OpenSSL::SSL/Crypto target 提供（链接序在本闭包之后）。
# ---------------------------------------------------------------------------
if(DEVDESK_STATIC AND MINGW AND TARGET CURL::libcurl)
    set(_devdesk_curl_static_closure
        ssh2               # libcurl -> SSH
        nghttp2            # libcurl -> HTTP/2
        ngtcp2_crypto_ossl # libcurl -> HTTP/3 QUIC (OpenSSL crypto 后端)
        ngtcp2             #   ^ 依赖
        nghttp3            # libcurl -> HTTP/3
        psl                # libcurl -> 公共后缀列表
        idn2               # psl -> libidn2
        brotlidec          # libcurl -> brotli 解压
        brotlicommon       #   ^ 依赖
        zstd               # libcurl -> zstd 解压
        unistring          # psl/idn2 -> libunistring
        iconv              # unistring/idn2 -> libiconv
        z                  # libcurl/ssh2 -> zlib
    )
    set(_devdesk_curl_static_libs "")
    foreach(_dep IN LISTS _devdesk_curl_static_closure)
        find_library(_devdesk_curl_lib_${_dep}
            NAMES "lib${_dep}.a"
            HINTS "${DEVDESK_MINGW_ROOT}/lib"
            NO_DEFAULT_PATH)
        if(_devdesk_curl_lib_${_dep})
            list(APPEND _devdesk_curl_static_libs "${_devdesk_curl_lib_${_dep}}")
        else()
            message(STATUS "DEVDESK_STATIC: curl 依赖 ${_dep} 的静态库未找到（缺了再按链接报错补）")
        endif()
    endforeach()
    # Windows 系统导入库（只有 DLL 版，系统自带 DLL 属于单 exe 的豁免范围；
    # 不在 GCC 默认链接集里，静态 curl/openssl 引用它们，需显式补）
    list(APPEND _devdesk_curl_static_libs bcrypt secur32 wldap32 iphlpapi)

    # FindCURL 在 pkg-config 命中时会把 PC_CURL_LINK_LIBRARIES（解析出来的
    # libcurl.dll.a 导入库）设成 CURL::libcurl 的 INTERFACE_LINK_LIBRARIES——
    # 静态模式下必须整体替换掉，否则 exe 照样导入 libcurl-4.dll。
    set_property(TARGET CURL::libcurl PROPERTY
        INTERFACE_LINK_LIBRARIES ${_devdesk_curl_static_libs})

    # curl.h 在 _WIN32 下默认 __declspec(dllimport)，
    # 静态链接必须定义 CURL_STATICLIB（与 pkg-config --static --cflags 一致，
    # 另三个 NGHTTP*/NGTCP2 宏只影响我们不含的头文件，无需引入）
    set_property(TARGET CURL::libcurl APPEND PROPERTY
        INTERFACE_COMPILE_DEFINITIONS CURL_STATICLIB)

    message(STATUS "DEVDESK_STATIC: MinGW 全静态链接已启用（单 exe 零外部 DLL）")
endif()

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
else()
    message(WARNING "SQLite3 FTS5: NOT enabled - Markdown search will fallback to LIKE")
endif()
# 注：SQLITE3_HAS_FTS5 是 check_cxx_source_compiles 写入的 CMake 缓存变量，仅用于
# 上面的状态提示。历史上此处还 add_compile_definitions 定义了全局宏 SQLITE3_HAS_FTS5
# 与 core PUBLIC 的 SQLITE_HAS_FTS5 两个「编译期宏」，但全源码零消费者（FTS5 降级
# 实际在 markdown_service.cpp 运行时判定 + LIKE 兜底），属死配置且会泄漏进 EUI-NEO
# 子目录作用域，已删除。FTS5 检测本身（check_cxx_source_compiles）保留。

# yyjson / md4c / FreeType 复用 EUI-NEO 自带
add_library(yyjson INTERFACE)
target_include_directories(yyjson INTERFACE
    "${EUI_NEO_DIR}/3rd/yyjson-0.12.0/src")

add_library(md4c INTERFACE)
target_include_directories(md4c INTERFACE
    "${EUI_NEO_DIR}/3rd/md4c/src")
