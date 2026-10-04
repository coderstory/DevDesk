// src/platform/http_headers.h
// HTTP 头名大小写处理：cpp-httplib 的 httplib::Headers 是大小写不敏感的 multimap，降级成
// std::map（大小写敏感）会漏命中（请求侧同名不同拼写并存；响应侧写入 "Content-Type" 而按
// "content-type" 查）。故统一收口为「键一律小写」+ 大小写不敏感查找。
#pragma once

#include <cctype>
#include <map>
#include <string>

#include <httplib.h>

namespace platform::http {

// ASCII 大小写归一（头名是 ASCII token，不做 locale 折叠；与 upstream_dialect.cpp 的同名工具
// 刻意各自保留——那侧操作请求头副本，本侧是 httplib 降级层，语义边界不同）。
inline std::string toLowerAscii(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// httplib::Headers（大小写不敏感 multimap）→ std::map：键统一小写，归到同一键的多条只保留
// 首个 value（http_server 原有语义），值不改写。
inline std::map<std::string, std::string> toLowerKeyHeaderMap(
    const httplib::Headers& src) {
    std::map<std::string, std::string> dst;
    for (const auto& kv : src) {
        dst.emplace(toLowerAscii(kv.first), kv.second);  // emplace：首个 wins
    }
    return dst;
}

// 大小写不敏感查找：lowerKey 传小写键（如 "content-type"），返回 value 指针，未命中 nullptr。
// **不能退化成「只把小写查询键丢给 std::map::find」**——响应头由 setHeader 以任意拼写写入
//（如 "Content-Type"），只查小写键会全部失配，把 application/json 的错误响应 body 标成 text/plain。
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
