// src/platform/http_headers.h
// HTTP 头名大小写处理工具（CP-06）
//
// 背景：cpp-httplib 的 httplib::Headers 是**大小写不敏感**的 multimap
// （3rd/cpp-httplib/httplib.h:506-508，case_ignore hash/equal_to）。把它降级成
// std::map<std::string, std::string>（大小写敏感）会引入两类缺陷：
//   1. 请求侧：同名不同拼写的头（Authorization + authorization）在 map 里并存，
//      消费方按某一种拼写查找就会漏命中；
//   2. 响应侧：写头方用 "Content-Type"、读头方按 "content-type" 查会漏命中。
// 本文件把「降级」这一步统一收口为「键一律小写」，并提供大小写不敏感的查找。
#pragma once

#include <cctype>
#include <map>
#include <string>

#include <httplib.h>

namespace platform::http {

// ASCII 大小写归一。HTTP 头名是 ASCII token，不做 locale 折叠
//（与 upstream_dialect.cpp 的同名工具刻意各自保留：那侧操作的是 std::map
//  请求头副本，本侧是 httplib 降级层，语义边界不同）。
inline std::string toLowerAscii(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// httplib::Headers（大小写不敏感 multimap）→ std::map：键统一小写。
// 归一到同一个键的多条只保留首个 value（http_server 原有语义），值不改写。
inline std::map<std::string, std::string> toLowerKeyHeaderMap(
    const httplib::Headers& src) {
    std::map<std::string, std::string> dst;
    for (const auto& kv : src) {
        dst.emplace(toLowerAscii(kv.first), kv.second);  // emplace：首个 wins
    }
    return dst;
}

// 大小写不敏感查找：lowerKey 传小写键（如 "content-type"），返回 value 指针，
// 未命中返回 nullptr。**不能退化成「只把小写查询键丢给 std::map::find」**——
// 响应头由 setHeader 以任意拼写写入（如 "Content-Type"），只查小写键会全部
// 失配，把本该 application/json 的错误响应 body 标成 text/plain。
inline const char* findHeaderCI(const std::map<std::string, std::string>& headers,
                                const char* lowerKey) {
    const std::string want = toLowerAscii(std::string(lowerKey));
    auto exact = headers.find(want);  // 常见情况：键已归一为小写，一次命中
    if (exact != headers.end()) return exact->second.c_str();
    for (const auto& kv : headers) {
        if (toLowerAscii(kv.first) == want) return kv.second.c_str();
    }
    return nullptr;
}

} // namespace platform::http
