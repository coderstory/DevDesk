# 网页终端静态资源嵌入（need#490 W7）。
# 由 module_terminal/CMakeLists.txt 的 add_custom_command 调用（-DWWW_DIR=... -DOUT_FILE=... -P）。
# 为什么不用文本版资源：前端是浏览器直接执行的 JS/CSS，任何转义/换行改写都会改变语义；
# 这里只做字节级 HEX 转 C 字符串字面量（\xNN 一一对应），运行时零解析。
cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED WWW_DIR OR NOT DEFINED OUT_FILE)
    message(FATAL_ERROR "embed_assets.cmake 需要 -DWWW_DIR=<www 目录> -DOUT_FILE=<生成源路径>")
endif()

set(_assets index.html app.css app.js protocol.js render.js crypto.js)

function(_mime_for name out)
    if(name MATCHES "\\.html$")
        set(mime "text/html; charset=utf-8")
    elseif(name MATCHES "\\.css$")
        set(mime "text/css; charset=utf-8")
    elseif(name MATCHES "\\.js$")
        set(mime "text/javascript; charset=utf-8")
    elseif(name MATCHES "\\.svg$")
        set(mime "image/svg+xml")
    elseif(name MATCHES "\\.png$")
        set(mime "image/png")
    elseif(name MATCHES "\\.woff2$")
        set(mime "font/woff2")
    else()
        set(mime "application/octet-stream")
    endif()
    set(${out} "${mime}" PARENT_SCOPE)
endfunction()

set(_bodies "")
set(_table "")
set(_index 0)
foreach(_name IN LISTS _assets)
    set(_path "${WWW_DIR}/${_name}")
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR "缺少前端资源: ${_path}")
    endif()
    file(READ "${_path}" _hex HEX)
    file(MD5 "${_path}" _etag)
    _mime_for("${_name}" _mime)

    string(LENGTH "${_hex}" _hexLen)
    set(_literal "")
    set(_offset 0)
    # 4096 字节/行：MSVC 字符串字面量上限 65535 字节，\xNN 形式 4 字符/字节留足余量
    while(_offset LESS _hexLen)
        math(EXPR _remaining "${_hexLen} - ${_offset}")
        if(_remaining GREATER 8192)
            set(_take 8192)
        else()
            set(_take ${_remaining})
        endif()
        string(SUBSTRING "${_hex}" ${_offset} ${_take} _chunk)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "\\\\x\\1" _escaped "${_chunk}")
        string(APPEND _literal "\"${_escaped}\"\n")
        math(EXPR _offset "${_offset} + ${_take}")
    endwhile()
    math(EXPR _size "${_hexLen} / 2")

    string(APPEND _bodies "static const char kAssetBody${_index}[] =\n${_literal};\n\n")
    string(APPEND _table
        "    {\"${_name}\", \"${_mime}\", \"${_etag}\", kAssetBody${_index}, ${_size}},\n")
    math(EXPR _index "${_index} + 1")
endforeach()

file(WRITE "${OUT_FILE}"
"// 由 embed_assets.cmake 生成，勿手改；改前端请改 web/www/ 下的源文件。\n"
"#include \"module_terminal/web/web_assets.h\"\n\n"
"#include <cstring>\n\n"
"namespace module::terminal::web {\n\n"
"${_bodies}"
"namespace {\n\n"
"const EmbeddedAsset kAssets[] = {\n"
"${_table}"
"};\n\n"
"} // namespace\n\n"
"const EmbeddedAsset* findEmbeddedAsset(std::string_view name) {\n"
"    if (name == \"/\" || name == \"/index.html\") name = \"index.html\";\n"
"    for (const auto& asset : kAssets) {\n"
"        if (name == asset.name) return &asset;\n"
"    }\n"
"    return nullptr;\n"
"}\n\n"
"} // namespace module::terminal::web\n")
