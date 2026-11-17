// notepad_encoding.cpp — 编码 / 行尾纯函数探测实现（口径见头注释，need:273 批 1）。
#include "notepad_encoding.h"

#include <algorithm>  // std::min（二进制扫描窗口截断；std include 守卫要求显式声明）

namespace module::notepad {

const char kUtf8Bom[3] = {'\xEF', '\xBB', '\xBF'};

namespace {

// 二进制扫描窗口：首 8KiB 含 NUL 即判二进制（module_git git_content_model 同口径）。
constexpr size_t kBinaryScanBytes = 8192;

// 严格 UTF-8 校验：拒绝代理区 / 超 U+10FFFF / 过长编码 / 悬挂 continuation
//（算法照抄 module_git git_content_model.cpp isValidUtf8——模块隔离不能 include，
// 逐行重写同口径）。
bool isValidUtf8Impl(const std::string& s) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        int extra = 0;
        unsigned codepoint = 0;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            codepoint = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            codepoint = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            codepoint = c & 0x07u;
        } else {
            return false;  // 0x80~0xBF 悬挂 continuation / 0xF8+ 非法首字节
        }
        if (i + static_cast<size_t>(extra) >= n) return false;  // 截断尾序列
        for (int k = 1; k <= extra; ++k) {
            const unsigned char cc =
                static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
            if ((cc & 0xC0) != 0x80) return false;  // 续字节缺位
            codepoint = (codepoint << 6) | (cc & 0x3Fu);
        }
        if (extra == 1 && codepoint < 0x80) return false;  // 过长
        if (extra == 2 && codepoint < 0x800) return false;
        if (extra == 3 && codepoint < 0x10000) return false;
        if (codepoint >= 0xD800 && codepoint <= 0xDFFF) return false;  // 代理区
        if (codepoint > 0x10FFFF) return false;
        i += static_cast<size_t>(extra) + 1;
    }
    return true;
}

// GB18030 双字节启发：lead 0x81-0xFE + 第二字节 0x40-0xFE（排除 0x7F）。
// 命中占非 ASCII 字节过半 → 推测 GB18030；否则 Latin-1 兜底（单字节全可解释）。
const char* guessNonUtf8Label(const std::string& bytes) {
    size_t nonAscii = 0;
    size_t gbPairs = 0;
    size_t i = 0;
    const size_t n = bytes.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        ++nonAscii;
        if (c >= 0x81 && c <= 0xFE && i + 1 < n) {
            const unsigned char c2 = static_cast<unsigned char>(bytes[i + 1]);
            if (c2 >= 0x40 && c2 <= 0xFE && c2 != 0x7F) {
                ++gbPairs;
                ++i;  // 第二字节一并消耗
            }
        }
        ++i;
    }
    if (nonAscii > 0 && gbPairs * 2 >= nonAscii) return "GB18030（推测）";
    return "Latin-1（推测）";
}

}  // namespace

NotepadEncodingProbe notepadProbeEncoding(const std::string& bytes) {
    NotepadEncodingProbe out;
    // 1. BOM 三型（UTF-16 标注精确到 LE/BE；无转换层 → validUtf8=false 只读降级）
    if (bytes.size() >= 2) {
        const unsigned char b0 = static_cast<unsigned char>(bytes[0]);
        const unsigned char b1 = static_cast<unsigned char>(bytes[1]);
        if (b0 == 0xFE && b1 == 0xFF) {
            out.bom = NotepadBom::Utf16Be;
            out.validUtf8 = false;
            return out;
        }
        if (b0 == 0xFF && b1 == 0xFE) {
            out.bom = NotepadBom::Utf16Le;
            out.validUtf8 = false;
            return out;
        }
    }
    std::string body = bytes;
    if (bytes.size() >= 3 && bytes.compare(0, 3, kUtf8Bom, 3) == 0) {
        out.bom = NotepadBom::Utf8;
        body = bytes.substr(3);
    }
    // 2. 二进制：首 8KiB 含 NUL（git 同口径；BOM 剥离后的正文窗口）
    const size_t scan = std::min(body.size(), kBinaryScanBytes);
    for (size_t i = 0; i < scan; ++i) {
        if (body[i] == '\0') {
            out.binary = true;
            break;
        }
    }
    // 3. 严格 UTF-8 → 推测标签
    out.validUtf8 = isValidUtf8Impl(body);
    if (!out.validUtf8) out.guessLabel = guessNonUtf8Label(body);
    return out;
}

bool notepadIsValidUtf8(const std::string& s) { return isValidUtf8Impl(s); }

NotepadEol notepadDetectEol(const std::string& text) {
    bool crlf = false;
    bool lf = false;
    bool cr = false;
    const size_t n = text.size();
    for (size_t i = 0; i < n; ++i) {
        if (text[i] == '\r') {
            if (i + 1 < n && text[i + 1] == '\n') {
                crlf = true;
                ++i;
            } else {
                cr = true;
            }
        } else if (text[i] == '\n') {
            lf = true;
        }
    }
    const int kinds = (crlf ? 1 : 0) + (lf ? 1 : 0) + (cr ? 1 : 0);
    if (kinds > 1) return NotepadEol::Mixed;
    if (crlf) return NotepadEol::Crlf;
    if (cr) return NotepadEol::Cr;
    return NotepadEol::Lf;
}

std::string notepadNormalizeToLf(const std::string& text, bool* mixedOut) {
    if (mixedOut != nullptr) *mixedOut = false;
    const NotepadEol detected = notepadDetectEol(text);
    if (mixedOut != nullptr) *mixedOut = (detected == NotepadEol::Mixed);
    if (detected == NotepadEol::Lf) return text;
    std::string out;
    out.reserve(text.size());
    const size_t n = text.size();
    for (size_t i = 0; i < n; ++i) {
        if (text[i] == '\r') {
            out.push_back('\n');
            if (i + 1 < n && text[i + 1] == '\n') ++i;  // CRLF 整组消耗
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::string notepadApplyEol(const std::string& textLf, NotepadEol eol) {
    if (eol == NotepadEol::Lf) return textLf;
    if (eol == NotepadEol::Cr) {
        std::string out;
        out.reserve(textLf.size());
        for (const char c : textLf) {
            out.push_back(c == '\n' ? '\r' : c);
        }
        return out;
    }
    // CRLF（Mixed 视作 CRLF 回写，见头注释）
    std::string out;
    out.reserve(textLf.size() + textLf.size() / 8 + 8);
    const size_t n = textLf.size();
    for (size_t i = 0; i < n; ++i) {
        if (textLf[i] == '\n') out.push_back('\r');
        out.push_back(textLf[i]);
    }
    return out;
}

std::string notepadStripBom(const std::string& bytes) {
    if (bytes.size() >= 3 && bytes.compare(0, 3, kUtf8Bom, 3) == 0) {
        return bytes.substr(3);
    }
    return bytes;
}

int notepadByteOffsetToLine(const std::string& text, size_t offset) {
    if (offset > text.size()) offset = text.size();
    int line = 0;
    for (size_t i = 0; i < offset; ++i) {
        if (text[i] == '\n') ++line;
    }
    return line;
}

size_t notepadLineStartOffset(const std::string& text, int lineNo) {
    if (lineNo <= 0) return 0;
    int line = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\n') continue;
        if (++line == lineNo) return i + 1;
    }
    return text.size();
}

size_t notepadUtf8Length(const std::string& text) {
    size_t n = 0;
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

}  // namespace module::notepad
