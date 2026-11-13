// src/module_git/git_search_model.cpp
#include "git_search_model.h"

#include <algorithm>

namespace module::git {

namespace {

constexpr size_t kMaxHitsPerFile = 5;          // 方案 §5：单文件命中上限
constexpr size_t kPreviewRadiusCodepoints = 40; // 方案 §5：match 前后码点窗

// UTF-8 序列起点字节（continuation = 0x80~0xBF）
bool isCodepointStart(unsigned char c) { return (c & 0xC0) != 0x80; }

constexpr const char* kEllipsis = "\xE2\x80\xA6";  // … U+2026

} // namespace

std::string asciiFold(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::vector<SearchHit> matchLines(const std::vector<std::string>& lines,
                                  const std::string& needle) {
    std::vector<SearchHit> hits;
    if (needle.empty()) return hits;
    const std::string foldedNeedle = asciiFold(needle);
    // 每行取首个命中（多命中行只出一条）；5 条上限收口后不再计数
    for (size_t i = 0; i < lines.size() && hits.size() < kMaxHitsPerFile; ++i) {
        const std::string& line = lines[i];
        const size_t pos = asciiFold(line).find(foldedNeedle);
        if (pos == std::string::npos) continue;
        SearchHit hit;
        hit.line = static_cast<int>(i) + 1;
        hit.preview = truncateAtCodepoint(line, pos, needle.size(),
                                          kPreviewRadiusCodepoints,
                                          &hit.matchStart, &hit.matchLen);
        hits.push_back(std::move(hit));
    }
    if (hits.size() == kMaxHitsPerFile) {
        // 收口标记：第 5 条之后是否还有（多扫一行，不计数入 hits）
        for (size_t i = static_cast<size_t>(hits.back().line);
             i < lines.size(); ++i) {
            if (asciiFold(lines[i]).find(foldedNeedle) != std::string::npos) {
                hits.back().moreInFile = true;
                break;
            }
        }
    }
    return hits;
}

std::string truncateAtCodepoint(const std::string& s, size_t matchStart,
                                size_t matchLen, size_t radius,
                                size_t* outMatchStart, size_t* outMatchLen) {
    // 窗左界：从 matchStart 向前走 radius 个码点起点（落在序列起点 = 边界安全）
    size_t left = matchStart;
    size_t counted = 0;
    while (left > 0 && counted < radius) {
        --left;
        if (isCodepointStart(static_cast<unsigned char>(s[left]))) ++counted;
    }
    // 窗右界：match 尾（字节边界）起向后走 radius 个码点（码点整走，不停续字节）
    size_t right = matchStart + matchLen;
    counted = 0;
    while (right < s.size() && counted < radius) {
        ++counted;
        do {
            ++right;
        } while (right < s.size() &&
                 !isCodepointStart(static_cast<unsigned char>(s[right])));
    }
    const bool cutLeft = left > 0;
    const bool cutRight = right < s.size();
    const size_t ellipsisLen = cutLeft ? std::char_traits<char>::length(kEllipsis) : 0;
    std::string out;
    out.reserve(ellipsisLen + (matchStart - left) + matchLen);
    if (cutLeft) out += kEllipsis;
    out.append(s, left, matchStart - left);
    out.append(s, matchStart, matchLen);
    out.append(s, matchStart + matchLen, right - (matchStart + matchLen));
    if (cutRight) out += kEllipsis;
    if (outMatchStart != nullptr) *outMatchStart = ellipsisLen + (matchStart - left);
    if (outMatchLen != nullptr) *outMatchLen = matchLen;
    return out;
}

} // namespace module::git
