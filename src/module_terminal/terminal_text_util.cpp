#include "terminal_text_util.h"

#include <vector>

namespace terminal {

namespace {
// UTF-8 首字节 → 该字符字节数（非法序列按 1 处理）
int utf8LeadLen(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

int cellWidthOfLead(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 1;
    return 2;
}
} // namespace

bool isWordByte(unsigned char c) {
    if (c >= 0x80) return true; // CJK 等在 UTF-8 中为高位字节，按词处理
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '_';
}

int textDisplayColumns(const std::string& text) {
    int col = 0;
    for (size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        col += cellWidthOfLead(lead);
        i += static_cast<size_t>(utf8LeadLen(lead));
    }
    return col;
}

std::pair<int, int> wordBoundsAt(const std::string& text, int col) {
    if (col < 0) return {0, 0};
    // 逐字符扫，记录每个字符的 [起列, 止列) 与是否词内
    struct Ch {
        int begin;
        int end;
        bool word;
    };
    std::vector<Ch> chars;
    for (size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        const int w = cellWidthOfLead(lead);
        const int len = utf8LeadLen(lead);
        const int begin = chars.empty() ? 0 : chars.back().end;
        chars.push_back({begin, begin + w, isWordByte(lead)});
        i += static_cast<size_t>(len);
    }
    if (chars.empty()) return {col, col + 1};
    // 定位列所在字符（宽字第二列也命中同一字符）
    size_t at = 0;
    bool found = false;
    for (size_t k = 0; k < chars.size(); ++k) {
        if (col >= chars[k].begin && col < chars[k].end) {
            at = k;
            found = true;
            break;
        }
    }
    if (!found) return {col, col + 1};
    if (!chars[at].word) return {chars[at].begin, chars[at].end};
    size_t lo = at;
    while (lo > 0 && chars[lo - 1].word) --lo;
    size_t hi = at;
    while (hi + 1 < chars.size() && chars[hi + 1].word) ++hi;
    return {chars[lo].begin, chars[hi].end};
}

} // namespace terminal
