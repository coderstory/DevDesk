// 网页终端静态资源（need#490 W7）：web/www/* 编译期嵌入二进制，运行期按名查表。
// 生成源由 embed_assets.cmake 产出（web_assets_generated.cpp，位于构建目录）。
#pragma once

#include <cstddef>
#include <string_view>

namespace module::terminal::web {

struct EmbeddedAsset {
    const char* name;   // 相对 www/ 的文件名，如 "app.js"
    const char* mime;   // 已带 charset 的完整 Content-Type
    const char* etag;   // 构建期 MD5，用于 If-None-Match / 304
    const char* data;   // 原始字节（可能含 NUL，必须按 size 使用）
    std::size_t size;
};

// 未命中返回 nullptr；"/" 与 "/index.html" 归一到 index.html
const EmbeddedAsset* findEmbeddedAsset(std::string_view name);

} // namespace module::terminal::web
