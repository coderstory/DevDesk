// 跨模块共用的纯字符串工具（core 层，不依赖 platform / EUI-NEO）
// 只收逐字等价的实现；带 locale 语义的（std::tolower 等）不收，用确定性手写版
#pragma once

#include <cstddef>
#include <string>

namespace core::str {

// ASCII 大小写折叠（仅 A-Z，locale 无关；非 ASCII 字节原样保留）
inline std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// SQL LIKE 模式转义（\ % _ → 字面量）。调用方拼 "%" + x + "%" 并配 ESCAPE '\'。
inline std::string escapeLikePattern(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() + 8);
    for (char ch : raw) {
        if (ch == '\\' || ch == '%' || ch == '_') out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

// 去首尾 ASCII 空白（空格 / 制表 / CR / LF）。
inline std::string trimAscii(const std::string& s) {
    const auto isBlank = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && isBlank(s[b])) ++b;
    while (e > b && isBlank(s[e - 1])) --e;
    return s.substr(b, e - b);
}

}  // namespace core::str
