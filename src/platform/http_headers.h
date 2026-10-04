// src/platform/http_headers.h
// HTTP 头名大小写处理：httplib::Headers 大小写不敏感，降级成 std::map（敏感）会漏命中；
// 统一收口为「键一律小写」+ 大小写不敏感查找。
#pragma once

#include <cctype>
#include <map>
#include <string>

#include <httplib.h>

namespace platform::http {

// ASCII 大小写归一（头名是 ASCII token，不做 locale 折叠）。
inline std::string toLowerAscii(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// httplib::Headers → std::map：键统一小写，归到同一键的多条只保留首个 value，值不改写。
inline std::map<std::string, std::string> toLowerKeyHeaderMap(
    const httplib::Headers& src) {
    std::map<std::string, std::string> dst;
    for (const auto& kv : src) {
        dst.emplace(toLowerAscii(kv.first), kv.second);  // emplace：首个 wins
    }
    return dst;
}

// 大小写不敏感查找：lowerKey 传小写键，返回 value 指针，未命中 nullptr。不能退化成只查小写键——
// 响应头由 setHeader 以任意拼写写入，只查小写键会全部失配，把 application/json body 标成 text/plain。
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
