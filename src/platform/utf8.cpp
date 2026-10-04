// src/platform/utf8.cpp
// UTF-8 良构校验与清洗实现（口径与用途见 utf8.h 头注）。
#include "utf8.h"

#include <cstdint>

namespace platform::utf8 {
namespace {

constexpr char kReplacement[] = "\xEF\xBF\xBD";   // U+FFFD 的 UTF-8 编码

// 位置 i 处一个良构 UTF-8 序列的字节长度（1..4）；非法/截断 → 0。逐条判据（RFC 3629）：前导字节
// ASCII / C2-DF / E0-EF / F0-F4，其余（孤立续字节、C0/C1 overlong 前导、F5-FF）非法；续字节必须
// 10xxxxxx；码点 ≥ 该长度最小编码值、≤ U+10FFFF、不在代理区。
size_t seqLenAt(std::string_view s, size_t i) {
    const size_t n = s.size();
    const unsigned char b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) return 1;

    size_t need = 0;      // 续字节个数
    uint32_t cp = 0;
    uint32_t minCp = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        need = 1; cp = b0 & 0x1F; minCp = 0x80;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        need = 2; cp = b0 & 0x0F; minCp = 0x800;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        need = 3; cp = b0 & 0x07; minCp = 0x10000;
    } else {
        return 0;
    }
    if (n - i <= need) return 0;                       // 截断（含末尾缺续字节）
    for (size_t k = 1; k <= need; ++k) {
        const unsigned char bk = static_cast<unsigned char>(s[i + k]);
        if ((bk & 0xC0) != 0x80) return 0;             // 非续字节
        cp = (cp << 6) | (bk & 0x3F);
    }
    if (cp < minCp) return 0;                          // overlong（E0 9F 之类）
    if (cp > 0x10FFFF) return 0;                       // 越界（F4 90 起）
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;        // UTF-8 编码的代理区
    return need + 1;
}

} // namespace

bool isValid(std::string_view text) {
    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        const size_t len = seqLenAt(text, i);
        if (len == 0) return false;
        i += len;
    }
    return true;
}

std::string sanitize(std::string_view text) {
    if (isValid(text)) return std::string(text);       // 快速路径：良构零改写
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        const size_t len = seqLenAt(text, i);
        if (len == 0) {
            // 非法序列逐字节换成 U+FFFD（不做「最大子部分」合并——替换字符的个数不承载语义，
            // 重要的是输出恒良构且可读部分逐字保真）
            out += kReplacement;
            ++i;
        } else {
            out.append(text.data() + i, len);
            i += len;
        }
    }
    return out;
}

} // namespace platform::utf8
