#include "terminal_links.h"

#include <cstring>

#include "terminal_text_util.h"

namespace terminal {

namespace {

// —— 截止与剥尾 ——

bool isAsciiSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

// 链接实体到此截止：空白与常见包裹/分隔符
bool isStopByte(char c) {
    return isAsciiSpace(c) || c == '"' || c == '\'' || c == '<' || c == '>' || c == '|' ||
           c == '`';
}

bool isTrailingAsciiPunct(char c) {
    return c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == ')' ||
           c == ']' || c == '}';
}

// 尾部要连续剥掉的全角标点（各 3 字节 UTF-8）：，。；：！？）】
const char* const kTrailingCjkPunct[] = {
    "\xEF\xBC\x8C", // ，
    "\xE3\x80\x82", // 。
    "\xEF\xBC\x9B", // ；
    "\xEF\xBC\x9A", // ：
    "\xEF\xBC\x81", // ！
    "\xEF\xBC\x9F", // ？
    "\xEF\xBC\x89", // ）
    "\xE3\x80\x91", // 】
};

size_t stripTrailingPunct(const std::string& text, size_t begin, size_t end) {
    while (end > begin) {
        if (isTrailingAsciiPunct(text[end - 1])) {
            --end;
            continue;
        }
        bool stripped = false;
        if (end - begin >= 3) {
            for (const char* punct : kTrailingCjkPunct) {
                if (std::memcmp(text.data() + end - 3, punct, 3) == 0) {
                    end -= 3;
                    stripped = true;
                    break;
                }
            }
        }
        if (!stripped) break;
    }
    return end;
}

size_t scanEntityEnd(const std::string& text, size_t begin) {
    size_t end = begin;
    while (end < text.size() && !isStopByte(text[end])) ++end;
    return stripTrailingPunct(text, begin, end);
}

// —— 前缀判定 ——

bool isAsciiWordByte(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// 左侧紧邻 ASCII 词字节 → 候选是更长单词的一部分（如相对路径 a/b），不识别
bool embeddedInWord(const std::string& text, size_t at) {
    return at > 0 && isAsciiWordByte(text[at - 1]);
}

bool isAsciiLetter(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool startsWithNoCase(const std::string& text, size_t at, const char* prefix) {
    for (size_t i = 0; prefix[i] != '\0'; ++i) {
        if (at + i >= text.size()) return false;
        char a = text[at + i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

bool containsPathSeparator(const std::string& text, size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
        if (text[i] == '/' || text[i] == '\\') return true;
    }
    return false;
}

// 字节偏移 → 终端列（与 textDisplayColumns / wordBoundsAt 同口径）
int columnAtByte(const std::string& text, size_t byteIndex) {
    if (byteIndex == 0) return 0;
    if (byteIndex >= text.size()) return textDisplayColumns(text);
    return textDisplayColumns(text.substr(0, byteIndex));
}

} // namespace

std::vector<LinkSpan> findLinks(const std::string& lineText) {
    std::vector<LinkSpan> links;
    size_t i = 0;
    while (i < lineText.size()) {
        LinkKind kind = LinkKind::Url;
        size_t schemeLen = 0;
        bool matched = false;

        if (startsWithNoCase(lineText, i, "https://")) {
            matched = true;
            schemeLen = 8;
        } else if (startsWithNoCase(lineText, i, "http://")) {
            matched = true;
            schemeLen = 7;
        }

        if (!matched) {
            const char c0 = lineText[i];
            const char c1 = (i + 1 < lineText.size()) ? lineText[i + 1] : '\0';
            const char c2 = (i + 2 < lineText.size()) ? lineText[i + 2] : '\0';
            const bool wordBefore = embeddedInWord(lineText, i);
            bool isPath = false;
            if (c0 == '~' && c1 == '/') {
                isPath = !wordBefore;
            } else if (isAsciiLetter(c0) && c1 == ':' && (c2 == '\\' || c2 == '/')) {
                isPath = !wordBefore;
            } else if (c0 == '/' && i + 1 < lineText.size() && c1 != '/' && c1 != '*' &&
                       (i == 0 || (lineText[i - 1] != '/' && lineText[i - 1] != '*'))) {
                // '//' 注释与 '/*' 块注释开头不算路径
                isPath = !wordBefore;
            }
            if (isPath) {
                kind = LinkKind::Path;
                matched = true;
            }
        }

        if (!matched) {
            ++i;
            continue;
        }

        const size_t begin = i;
        const size_t end = scanEntityEnd(lineText, begin);
        bool valid = false;
        if (kind == LinkKind::Url) {
            valid = end > begin + schemeLen; // scheme 之后至少一个字节实体
        } else {
            valid = end > begin + 1 && containsPathSeparator(lineText, begin, end);
        }
        if (valid) {
            LinkSpan span;
            span.colBegin = columnAtByte(lineText, begin);
            span.colEnd = columnAtByte(lineText, end);
            span.target = lineText.substr(begin, end - begin);
            span.kind = kind;
            if (span.colEnd > span.colBegin) links.push_back(std::move(span));
        }
        i = (end > i) ? end : i + 1;
    }
    return links;
}

const LinkSpan* linkAtColumn(const std::vector<LinkSpan>& links, int col) {
    for (const auto& span : links) {
        if (col >= span.colBegin && col < span.colEnd) return &span;
    }
    return nullptr;
}

} // namespace terminal
