// 跨模块共用的纯字符串工具（core 层，不依赖 platform / EUI-NEO）
// 只收逐字等价的实现：写法有语义差异的不合并（如 toLowerAscii 的 ASCII-only
// 版与 std::tolower 版在非 ASCII 字节上行为不同，合并会静默改变行为）
#pragma once

#include <cstddef>
#include <string>

namespace core::str {

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
