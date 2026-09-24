// src/module_markdown/md_keyword_highlight.cpp
// 代码关键字多色词法器（Phase 3 ⑤，接口语义与扩展路线见头文件）。
//
// 实现形态：单一手写状态机 + per-language LangSpec 数据表（关键字集合、
// 注释/字符串/三引号/预处理规则以 Lang 枚举开关表达——语言数 ≤3 时比
// vtable 干净，加 JS/Go 等仍在 langOf/词法开关两处扩展）。按行驱动：
// 跨行 token（C++ 块注释、Python 三引号串）用 mode_ 状态在行间传递，
// 每行独立产 run ⇒ 「空行无 run、普通 run 不含换行」结构与逐行 fallback
// 完全一致，跨行 token 按行切成同色相邻 run（换行恒为间隙）。
//
// 容错哲学（与 md_highlight 同源）：识别不确定 = 漏标回默认色，不错标：
//   - 单行字符串未闭合（行内无配对引号）→ 整段按默认色，状态清零
//   - 数字扫描过配（"1and" 之类畸形字面量）→ 宁多并一档，不影响可读
#include "md_keyword_highlight.h"

#include <cctype>
#include <unordered_set>
#include <utility>

namespace module::markdown {
namespace {

char toLowerAscii(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// a 与全小写的 lower 做大小写不敏感等值比较
bool ieq(std::string_view a, std::string_view lower) {
    if (a.size() != lower.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (toLowerAscii(a[i]) != lower[i]) return false;
    }
    return true;
}

enum class Lang { None, Cpp, Python, Js, Go, Rust, Shell, Java, Json };

Lang langOf(std::string_view lang) {
    if (ieq(lang, "cpp") || ieq(lang, "c++") || ieq(lang, "cc") ||
        ieq(lang, "cxx") || ieq(lang, "hpp") || ieq(lang, "hh") ||
        ieq(lang, "c")) {
        return Lang::Cpp;
    }
    if (ieq(lang, "python") || ieq(lang, "py") || ieq(lang, "python3")) {
        return Lang::Python;
    }
    if (ieq(lang, "js") || ieq(lang, "javascript") || ieq(lang, "mjs")) {
        return Lang::Js;
    }
    if (ieq(lang, "go") || ieq(lang, "golang")) {
        return Lang::Go;
    }
    if (ieq(lang, "rs") || ieq(lang, "rust")) {
        return Lang::Rust;
    }
    if (ieq(lang, "sh") || ieq(lang, "bash") || ieq(lang, "shell") ||
        ieq(lang, "zsh")) {
        return Lang::Shell;
    }
    if (ieq(lang, "java")) {
        return Lang::Java;
    }
    if (ieq(lang, "json")) {
        return Lang::Json;
    }
    return Lang::None;
}

const std::unordered_set<std::string_view>& cppKeywords() {
    static const std::unordered_set<std::string_view> kw = {
        "alignas",  "alignof", "and",        "and_eq",    "asm",
        "auto",     "bitand",  "bitor",      "bool",      "break",
        "case",     "catch",   "char",       "char8_t",   "char16_t",
        "char32_t", "class",   "compl",      "concept",   "const",
        "consteval", "constexpr", "constinit", "const_cast", "continue",
        "co_await", "co_return", "co_yield",  "decltype",  "default",
        "delete",   "do",      "double",     "dynamic_cast", "else",
        "enum",     "explicit", "export",    "extern",    "false",
        "float",    "for",     "friend",     "goto",      "if",
        "inline",   "int",     "long",       "mutable",   "namespace",
        "new",      "noexcept", "not",       "not_eq",    "nullptr",
        "operator", "or",      "or_eq",      "private",   "protected",
        "public",   "register", "reinterpret_cast", "requires", "return",
        "short",    "signed",  "sizeof",     "static",    "static_assert",
        "static_cast", "struct", "switch",   "template",  "this",
        "thread_local", "throw", "true",     "try",       "typedef",
        "typeid",   "typename", "union",     "unsigned",  "using",
        "virtual",  "void",    "volatile",   "wchar_t",   "while",
        "xor",      "xor_eq",  "final",      "override",  "import",
        "module",
    };
    return kw;
}

const std::unordered_set<std::string_view>& pyKeywords() {
    static const std::unordered_set<std::string_view> kw = {
        "False", "None",    "True",    "and",      "as",      "assert",
        "async", "await",   "break",   "class",    "continue", "def",
        "del",   "elif",    "else",    "except",   "finally", "for",
        "from",  "global",  "if",      "import",   "in",      "is",
        "lambda", "nonlocal", "not",   "or",       "pass",    "raise",
        "return", "try",    "while",   "with",     "yield",
    };
    return kw;
}


const std::unordered_set<std::string_view>& jsKeywords() {
    static const std::unordered_set<std::string_view> kw = {
        "var", "let", "const", "function", "return", "if", "else", "for",
        "while", "do", "switch", "case", "break", "continue", "new",
        "delete", "typeof", "instanceof", "in", "of", "this", "class",
        "extends", "super", "import", "export", "default", "try", "catch",
        "finally", "throw", "async", "await", "yield", "null", "true",
        "false", "undefined", "void",
    };
    return kw;
}

const std::unordered_set<std::string_view>& goKeywords() {
    static const std::unordered_set<std::string_view> kw = {
        "break", "case", "chan", "const", "continue", "default", "defer",
        "else", "fallthrough", "for", "func", "go", "goto", "if", "import",
        "interface", "map", "package", "range", "return", "select",
        "struct", "switch", "type", "var",
    };
    return kw;
}

const std::unordered_set<std::string_view>& rustKeywords() {
    static const std::unordered_set<std::string_view> kw = {
        "fn", "let", "mut", "const", "static", "if", "else", "for",
        "while", "loop", "match", "return", "break", "continue", "struct",
        "enum", "impl", "trait", "pub", "use", "mod", "crate", "self",
        "super", "Self", "as", "in", "ref", "where", "unsafe", "async",
        "await", "dyn", "box", "move", "true", "false",
    };
    return kw;
}

const std::unordered_set<std::string_view>& shellKeywords() {
    static const std::unordered_set<std::string_view> kw = {
        "if", "then", "elif", "else", "fi", "for", "in", "do", "done",
        "while", "case", "esac", "function", "return", "local", "export",
        "unset", "echo", "exit", "shift", "wait",
    };
    return kw;
}

const std::unordered_set<std::string_view>& javaKeywords() {
    static const std::unordered_set<std::string_view> kw = {
        "abstract", "assert", "boolean", "break", "byte", "case", "catch",
        "char", "class", "const", "continue", "default", "do", "double",
        "else", "enum", "extends", "final", "finally", "float", "for",
        "goto", "if", "implements", "import", "instanceof", "int",
        "interface", "long", "native", "new", "package", "private",
        "protected", "public", "record", "return", "sealed", "short",
        "static", "strictfp", "super", "switch", "synchronized", "this",
        "throw", "throws", "transient", "try", "var", "void", "volatile",
        "while", "yield", "true", "false", "null",
    };
    return kw;
}

// JSON 无语句关键字，仅三个字面量（键名与值都是字符串色、数字走数字色）
const std::unordered_set<std::string_view>& jsonKeywords() {
    static const std::unordered_set<std::string_view> kw = {
        "true", "false", "null",
    };
    return kw;
}

// 按 Lang 取对应关键字表（None 返回空集）
const std::unordered_set<std::string_view>& keywordsOf(Lang lang) {
    switch (lang) {
    case Lang::Cpp:    return cppKeywords();
    case Lang::Python: return pyKeywords();
    case Lang::Js:     return jsKeywords();
    case Lang::Go:     return goKeywords();
    case Lang::Rust:   return rustKeywords();
    case Lang::Shell:  return shellKeywords();
    case Lang::Java:   return javaKeywords();
    case Lang::Json:   return jsonKeywords();
    default: {
        static const std::unordered_set<std::string_view> empty;
        return empty;
    }
    }
}

// 语言特性开关（注释 / 反引号串）
bool hasLineComment(Lang lang) {
    return lang == Lang::Cpp || lang == Lang::Js ||
           lang == Lang::Go || lang == Lang::Rust || lang == Lang::Java;
}
bool hasBlockComment(Lang lang) {
    return lang == Lang::Cpp || lang == Lang::Js ||
           lang == Lang::Go || lang == Lang::Rust || lang == Lang::Java;
}
bool hasBacktickString(Lang lang) {
    return lang == Lang::Js || lang == Lang::Go;
}

// Python 字符串字面量前缀（r/b/u/f 及两两组合，≤2 字符且后随引号才算）
bool isPyStrPrefix(std::string_view w) {
    if (w.empty() || w.size() > 2) return false;
    for (const char c : w) {
        switch (c) {
        case 'b': case 'B': case 'r': case 'R': case 'u': case 'U':
        case 'f': case 'F': break;
        default: return false;
        }
    }
    return true;
}

bool isSpace(char c) { return c == ' ' || c == '\t'; }
bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\n'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isHexDigit(char c) {
    return isDigit(c) || (toLowerAscii(c) >= 'a' && toLowerAscii(c) <= 'f');
}
bool isIdentStart(char c) {
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool isIdentChar(char c) { return isIdentStart(c) || isDigit(c); }

constexpr bool colorEq(const eui::Color& a, const eui::Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// ── 行驱动状态机 ──────────────────────────────────────────────────────
class LineLexer {
public:
    LineLexer(std::string_view code, Lang lang, const MdHighlightColors& colors)
        : s_(code), lang_(lang), c_(colors) {}

    std::vector<eui::TextRun> run() {
        size_t lineStart = 0;
        const size_t n = s_.size();
        for (;;) {
            size_t nl = s_.find('\n', lineStart);
            const size_t lineEnd = (nl == std::string_view::npos) ? n : nl;
            lexLine(lineStart, lineEnd);
            if (nl == std::string_view::npos) break;
            lineStart = nl + 1;
        }
        return std::move(runs_);
    }

private:
    enum class Mode { Normal, BlockComment, TripleD, TripleS, Backtick };

    // 产 run（[a,b) 绝对坐标）。相邻同色自动合并；调用方保证 b ≥ 上一 run 端点
    void push(size_t a, size_t b, const eui::Color& color) {
        if (a >= b) return;
        if (!runs_.empty() && runs_.back().byteEnd == static_cast<int>(a) &&
            colorEq(runs_.back().color, color)) {
            runs_.back().byteEnd = static_cast<int>(b);
            return;
        }
        runs_.push_back({static_cast<int>(a), static_cast<int>(b), color});
    }

    void lexLine(size_t a, size_t b) {
        if (mode_ == Mode::BlockComment) {
            const size_t close = s_.find("*/", a);
            if (close == std::string_view::npos || close + 2 > b) {
                push(a, b, c_.codeComment);
                return;  // 仍未闭合，下一行继续
            }
            push(a, close + 2, c_.codeComment);
            mode_ = Mode::Normal;
            lexNormal(close + 2, b);
            return;
        }
        if (mode_ == Mode::TripleD || mode_ == Mode::TripleS) {
            const char q = (mode_ == Mode::TripleD) ? '"' : '\'';
            const size_t close = findTriple(q, a, b);
            if (close == std::string_view::npos) {
                push(a, b, c_.codeString);
                return;
            }
            push(a, close, c_.codeString);
            mode_ = Mode::Normal;
            lexNormal(close, b);
            return;
        }
        if (mode_ == Mode::Backtick) {
            const size_t close = s_.find('`', a);
            if (close == std::string_view::npos || close >= b) {
                push(a, b, c_.codeString);
                return;  // 反引号串仍未闭合，下一行继续
            }
            push(a, close + 1, c_.codeString);
            mode_ = Mode::Normal;
            lexNormal(close + 1, b);
            return;
        }
        lexNormal(a, b);
    }

    // 从 from 起找闭合三引号串（含引号本身）的「串后位置」；本行找不到 npos
    size_t findTriple(char q, size_t from, size_t b) {
        const char tri[3] = {q, q, q};
        const std::string_view marker(tri, 3);
        size_t j = from;
        for (;;) {
            const size_t k = s_.find(marker, j);
            if (k == std::string_view::npos || k + 3 > b) {
                return std::string_view::npos;
            }
            // 前置反斜杠转义（\ """）不算闭合——粗略扫描：数紧邻反斜杠奇偶
            size_t slashes = 0;
            for (size_t p = k; p > from && s_[p - 1] == '\\'; --p) ++slashes;
            if (slashes % 2 == 0) return k + 3;
            j = k + 3;
        }
    }

    // 尝试把 s_[pos]（须为引号字符）开始的串按字符串消费。
    // 返回串后位置；单行串未闭合 → npos（漏标回默认）。
    // 三引号未在本行闭合 → 置 mode_ 跨行，返回本行行尾。
    size_t lexString(size_t pos, size_t b) {
        const char q = s_[pos];
        if (lang_ == Lang::Python && pos + 2 < b && s_[pos + 1] == q &&
            s_[pos + 2] == q) {
            const size_t close = findTriple(q, pos + 3, b);
            if (close != std::string_view::npos) return close;
            mode_ = (q == '"') ? Mode::TripleD : Mode::TripleS;
            return b;  // 本行整行入串，下一行续
        }
        size_t j = pos + 1;
        while (j < b) {
            if (s_[j] == '\\') {
                j += 2;
                continue;
            }
            if (s_[j] == q) return j + 1;
            ++j;
        }
        return std::string_view::npos;  // 未闭合
    }

    // 数字字面量扫描（自 pos 起，pos 为数字字符）。返回数字段终点。
    // 容忍过配（后缀字母并入），宁多并不错断
    size_t lexNumber(size_t pos, size_t b) {
        size_t j = pos;
        bool radix = false;
        if (s_[pos] == '0' && pos + 1 < b) {
            const char t = toLowerAscii(s_[pos + 1]);
            if (t == 'x' || t == 'b' || t == 'o') {
                size_t k = pos + 2;
                while (k < b && (isHexDigit(s_[k]) || s_[k] == '_')) ++k;
                if (k > pos + 2) {  // "0x" 空尾 → 回退按十进制走
                    j = k;
                    radix = true;
                }
            }
        }
        if (!radix) {
            while (j < b && (isDigit(s_[j]) || s_[j] == '_')) ++j;
            if (j + 1 < b && s_[j] == '.' && isDigit(s_[j + 1])) {
                ++j;
                while (j < b && (isDigit(s_[j]) || s_[j] == '_')) ++j;
            }
            if (j < b && s_[j] == 'e') {  // 指数：e[+-]?digits（E 大写走标识符
                                          // 分支前的过配并入，够着色用）
                size_t k = j + 1;
                if (k < b && (s_[k] == '+' || s_[k] == '-')) ++k;
                if (k < b && isDigit(s_[k])) {
                    while (k < b && (isDigit(s_[k]) || s_[k] == '_')) ++k;
                    j = k;
                }
            }
        }
        while (j < b && isIdentChar(s_[j])) ++j;  // 后缀（f/LL/j/过配字母）
        return j;
    }

    void lexNormal(size_t a, size_t b) {
        size_t i = a;
        size_t def = std::string_view::npos;  // 默认段起点（trim 后成 run）
        const auto flushDef = [&](size_t end) {
            if (def == std::string_view::npos) return;
            size_t s = def;
            size_t e = end;
            while (s < e && isBlank(s_[s])) ++s;
            while (e > s && isBlank(s_[e - 1])) --e;
            if (s < e) push(s, e, c_.code);
            def = std::string_view::npos;
        };
        const auto extendDef = [&]() {
            if (def == std::string_view::npos) def = i;
        };

        while (i < b) {
            const char ch = s_[i];
            if (isSpace(ch)) {
                extendDef();
                ++i;
                continue;
            }
            // ── 注释（// 行注释、/* */ 块注释：C++/JS/Go/Rust）
            if (ch == '/' && i + 1 < b) {
                if (hasLineComment(lang_) && s_[i + 1] == '/') {
                    flushDef(i);
                    push(i, b, c_.codeComment);
                    return;
                }
                if (hasBlockComment(lang_) && s_[i + 1] == '*') {
                    flushDef(i);
                    const size_t close = s_.find("*/", i + 2);
                    if (close == std::string_view::npos || close + 2 > b) {
                        push(i, b, c_.codeComment);
                        mode_ = Mode::BlockComment;
                        return;
                    }
                    push(i, close + 2, c_.codeComment);
                    i = close + 2;
                    continue;
                }
            }
            if ((lang_ == Lang::Python || lang_ == Lang::Shell) &&
                ch == '#') {  // 行注释：到行尾（Python / Shell）
                flushDef(i);
                push(i, b, c_.codeComment);
                return;
            }
            if (lang_ == Lang::Cpp && ch == '#') {
                // 预处理指令词（#include/#define…）按关键字色（meta 惯例）；
                // 裸 #（lambda 捕获等）回默认
                size_t j = i + 1;
                while (j < b && isIdentChar(s_[j])) ++j;
                if (j > i + 1) {
                    flushDef(i);
                    push(i, j, c_.codeKeyword);
                    i = j;
                    continue;
                }
                extendDef();
                ++i;
                continue;
            }
            // ── 字符串 / 字符字面量
            if (ch == '"' || ch == '\'') {
                const size_t after = lexString(i, b);
                if (after != std::string_view::npos) {
                    flushDef(i);
                    push(i, after, c_.codeString);
                    if (mode_ != Mode::Normal) return;  // 三引号未闭合，跨行
                    i = after;
                    continue;
                }
                extendDef();  // 未闭合：引号按默认色
                ++i;
                continue;
            }
            // ── 反引号原始串 / 模板串（JS/Go，跨行）
            if (ch == '`' && hasBacktickString(lang_)) {
                const size_t close = s_.find('`', i + 1);
                if (close == std::string_view::npos || close >= b) {
                    flushDef(i);
                    push(i, b, c_.codeString);
                    mode_ = Mode::Backtick;
                    return;
                }
                flushDef(i);
                push(i, close + 1, c_.codeString);
                i = close + 1;
                continue;
            }
            // ── 数字
            if (isDigit(ch)) {
                flushDef(i);
                const size_t after = lexNumber(i, b);
                push(i, after, c_.codeNumber);
                i = after;
                continue;
            }
            // ── 标识符 / 关键字（含 Python 前缀串）
            if (isIdentStart(ch)) {
                size_t j = i;
                while (j < b && isIdentChar(s_[j])) ++j;
                const std::string_view w(s_.data() + i, j - i);
                if (lang_ == Lang::Python && isPyStrPrefix(w) && j < b &&
                    (s_[j] == '"' || s_[j] == '\'')) {
                    const size_t after = lexString(j, b);
                    if (after != std::string_view::npos) {
                        flushDef(i);
                        push(i, after, c_.codeString);  // 前缀并入串 run
                        if (mode_ != Mode::Normal) return;
                        i = after;
                        continue;
                    }
                    // 未闭合：前缀+引号整体落默认，逐字续扫（j 不动，
                    // 下面按普通标识符消费前缀两字，引号下轮再处理）
                }
                const bool keyword = keywordsOf(lang_).count(w) > 0;
                if (keyword) {
                    flushDef(i);
                    push(i, j, c_.codeKeyword);
                    i = j;
                    continue;
                }
                extendDef();
                i = j;
                continue;
            }
            // ── 标点/运算符
            extendDef();
            ++i;
        }
        flushDef(b);
    }

    std::string_view s_;
    Lang lang_;
    const MdHighlightColors& c_;
    std::vector<eui::TextRun> runs_;
    Mode mode_ = Mode::Normal;
};

} // namespace

bool mdKeywordLangSupported(std::string_view lang) {
    return langOf(lang) != Lang::None;
}

MdKeywordHighlight mdKeywordHighlight(std::string_view code,
                                      std::string_view lang,
                                      const MdHighlightColors& colors) {
    MdKeywordHighlight out;
    const Lang l = langOf(lang);
    if (l == Lang::None) return out;  // fallback：supported=false，调用方单色
    LineLexer lex(code, l, colors);
    out.runs = lex.run();
    out.supported = true;
    return out;
}

} // namespace module::markdown
