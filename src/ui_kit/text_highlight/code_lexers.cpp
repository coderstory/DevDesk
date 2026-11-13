// 代码词法扫描器实现（接口语义见头文件）。
// 形态：单一手写状态机 + per-language 关键字表（md_keyword_highlight LineLexer
// 同模型）。按行驱动：跨行 token 用 mode_ 在行间传递，按行切同 kind span。
#include "code_lexers.h"

#include <cctype>
#include <unordered_set>

namespace ui_kit::text_highlight {

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

bool isSpace(char c) { return c == ' ' || c == '\t'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isHexDigit(char c) {
    return isDigit(c) || (toLowerAscii(c) >= 'a' && toLowerAscii(c) <= 'f');
}
bool isIdentStart(char c) {
    return c == '_' || c == '$' || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
}
bool isIdentChar(char c) { return isIdentStart(c) || isDigit(c); }

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
        "while", "yield", "permits", "true", "false", "null",
    };
    return kw;
}

const std::unordered_set<std::string_view>& jsKeywords() {
    static const std::unordered_set<std::string_view> kw = {
        "var", "let", "const", "function", "return", "if", "else", "for",
        "while", "do", "switch", "case", "break", "continue", "new",
        "delete", "typeof", "instanceof", "in", "of", "this", "class",
        "extends", "super", "import", "export", "default", "try", "catch",
        "finally", "throw", "async", "await", "yield", "static", "get",
        "set", "null", "true", "false", "undefined", "void",
    };
    return kw;
}

// TS = JS 超集 + 类型系统关键字（扫描时 JS 表先查，命中即 keyword）
const std::unordered_set<std::string_view>& tsKeywords() {
    static const std::unordered_set<std::string_view> kw = {
        "any", "boolean", "declare", "enum", "implements", "interface",
        "keyof", "namespace", "never", "number", "object", "private",
        "protected", "public", "readonly", "string", "symbol", "type",
        "unknown", "as", "is", "asserts", "override", "satisfies", "infer",
        "abstract", "module",
    };
    return kw;
}

// SQL 通用集（大小写不敏感；取 MySQL/ANSI 高频交集，宁少不错）
const std::unordered_set<std::string_view>& sqlKeywords() {
    static const std::unordered_set<std::string_view> kw = {
        "select", "from", "where", "insert", "into", "values", "update",
        "set", "delete", "create", "table", "drop", "alter", "add", "index",
        "view", "join", "inner", "left", "right", "outer", "full", "cross",
        "on", "group", "by", "order", "having", "limit", "offset", "union",
        "all", "distinct", "as", "and", "or", "not", "null", "is", "like",
        "in", "between", "case", "when", "then", "else", "end", "primary",
        "key", "foreign", "references", "default", "check", "unique",
        "cascade", "constraint", "exists", "asc", "desc", "truncate",
        "commit", "rollback", "begin", "transaction", "grant", "revoke",
        "if", "use", "true", "false",
    };
    return kw;
}

// JSON 无语句关键字，仅三个字面量（键名与字符串值同为字符串色）
const std::unordered_set<std::string_view>& jsonKeywords() {
    static const std::unordered_set<std::string_view> kw = {
        "true", "false", "null",
    };
    return kw;
}

bool isKeyword(CodeLang lang, std::string_view w) {
    switch (lang) {
    case CodeLang::Java: return javaKeywords().count(w) > 0;
    case CodeLang::JavaScript: return jsKeywords().count(w) > 0;
    case CodeLang::TypeScript:
        return jsKeywords().count(w) > 0 || tsKeywords().count(w) > 0;
    case CodeLang::Sql: {
        std::string lower;
        lower.reserve(w.size());
        for (const char c : w) lower += toLowerAscii(c);
        return sqlKeywords().count(lower) > 0;
    }
    case CodeLang::Json: return jsonKeywords().count(w) > 0;
    }
    return false;
}

} // namespace

CodeTokenColors codeTokenColors(const core::theme::Tokens& t) {
    return CodeTokenColors{
        t.colors.info,           // --hl-keyword（蓝系）
        t.colors.primaryText,    // --hl-string（绿系）
        t.colors.textTertiary,   // --hl-comment（灰）
        t.colors.warning,        // --hl-number（琥珀）
    };
}

eui::Color colorOf(HlKind kind, const CodeTokenColors& c) {
    switch (kind) {
    case HlKind::Keyword: return c.keyword;
    case HlKind::String: return c.string_;
    case HlKind::Comment: return c.comment;
    case HlKind::Number: return c.number;
    }
    return c.keyword;
}

bool codeLangFromExtension(std::string_view fileName, CodeLang& out) {
    const size_t dot = fileName.rfind('.');
    if (dot == std::string_view::npos) return false;
    const std::string_view ext = fileName.substr(dot + 1);
    if (ieq(ext, "java")) { out = CodeLang::Java; return true; }
    if (ieq(ext, "js") || ieq(ext, "mjs") || ieq(ext, "cjs") ||
        ieq(ext, "jsx")) {
        out = CodeLang::JavaScript;
        return true;
    }
    if (ieq(ext, "ts") || ieq(ext, "tsx")) {
        out = CodeLang::TypeScript;
        return true;
    }
    if (ieq(ext, "sql")) { out = CodeLang::Sql; return true; }
    if (ieq(ext, "json")) { out = CodeLang::Json; return true; }
    return false;
}

namespace {

// 行扫描状态机（跨行 mode 由外层 CodeHighlighter 持有）
class LineScanner {
public:
    LineScanner(CodeLang lang, CodeHighlighter::Mode mode,
                std::vector<HlSpan>& out)
        : lang_(lang), mode_(mode), out_(out) {}

    CodeHighlighter::Mode mode() const { return mode_; }

    void scan(std::string_view line) {
        const size_t n = line.size();
        switch (mode_) {
        case CodeHighlighter::Mode::BlockComment: {
            const size_t close = line.find("*/");
            if (close == std::string_view::npos) {
                if (n > 0) push(0, n, HlKind::Comment);
                return;  // 仍未闭合，下一行继续
            }
            push(0, close + 2, HlKind::Comment);
            mode_ = CodeHighlighter::Mode::Normal;
            scanNormal(line, close + 2);
            return;
        }
        case CodeHighlighter::Mode::Backtick: {
            const size_t close = line.find('`');
            if (close == std::string_view::npos) {
                if (n > 0) push(0, n, HlKind::String);
                return;  // 模板串仍未闭合，下一行继续
            }
            push(0, close + 1, HlKind::String);
            mode_ = CodeHighlighter::Mode::Normal;
            scanNormal(line, close + 1);
            return;
        }
        case CodeHighlighter::Mode::TripleQuote: {
            // Java 文本块闭合："""（前置转义粗略按奇偶反斜杠排除）
            size_t j = 0;
            for (;;) {
                const size_t k = line.find("\"\"\"", j);
                if (k == std::string_view::npos) {
                    if (n > 0) push(0, n, HlKind::String);
                    return;
                }
                size_t slashes = 0;
                for (size_t p = k; p > 0 && line[p - 1] == '\\'; --p) ++slashes;
                if (slashes % 2 == 0) {
                    push(0, k + 3, HlKind::String);
                    mode_ = CodeHighlighter::Mode::Normal;
                    scanNormal(line, k + 3);
                    return;
                }
                j = k + 3;
            }
        }
        case CodeHighlighter::Mode::Normal:
            scanNormal(line, 0);
            return;
        }
    }

private:
    void push(size_t a, size_t b, HlKind kind) {
        if (a >= b) return;
        if (!out_.empty() && out_.back().end == static_cast<int>(a) &&
            out_.back().kind == kind) {
            out_.back().end = static_cast<int>(b);
            return;
        }
        out_.push_back({static_cast<int>(a), static_cast<int>(b), kind});
    }

    // 字符串消费：返回串后位置；本行未闭合 → npos（漏标回默认，不改跨行 mode——
    // 单引号/双引号串在五语言均不跨行；JS/TS 反引号跨行走独立分支）
    size_t lexQuoted(std::string_view line, size_t pos, char q) {
        const size_t n = line.size();
        size_t j = pos + 1;
        while (j < n) {
            if (lang_ != CodeLang::Sql && line[j] == '\\' && j + 1 < n) {
                j += 2;  // 转义（SQL 串无反斜杠转义，'' doubling 单独处理）
                continue;
            }
            if (lang_ == CodeLang::Sql && line[j] == q && j + 1 < n &&
                line[j + 1] == q) {
                j += 2;  // SQL '' doubling
                continue;
            }
            if (line[j] == q) return j + 1;
            ++j;
        }
        return std::string_view::npos;
    }

    // 数字字面量（0x/0b/0o 进制、小数、指数；后缀字母过配并入，宁多不错断）
    size_t lexNumber(std::string_view line, size_t pos) {
        const size_t n = line.size();
        size_t j = pos;
        bool radix = false;
        if (line[pos] == '0' && pos + 1 < n) {
            const char t = toLowerAscii(line[pos + 1]);
            if (t == 'x' || t == 'b' || t == 'o') {
                size_t k = pos + 2;
                while (k < n && (isHexDigit(line[k]) || line[k] == '_')) ++k;
                if (k > pos + 2) {
                    j = k;
                    radix = true;
                }
            }
        }
        if (!radix) {
            while (j < n && (isDigit(line[j]) || line[j] == '_')) ++j;
            if (j + 1 < n && line[j] == '.' && isDigit(line[j + 1])) {
                ++j;
                while (j < n && (isDigit(line[j]) || line[j] == '_')) ++j;
            }
            if (j < n && toLowerAscii(line[j]) == 'e') {
                size_t k = j + 1;
                if (k < n && (line[k] == '+' || line[k] == '-')) ++k;
                if (k < n && isDigit(line[k])) {
                    while (k < n && (isDigit(line[k]) || line[k] == '_')) ++k;
                    j = k;
                }
            }
        }
        while (j < n && isIdentChar(line[j])) ++j;  // 后缀（f/LL/过配字母）
        return j;
    }

    void scanNormal(std::string_view line, size_t from) {
        const size_t n = line.size();
        size_t i = from;
        while (i < n) {
            const char ch = line[i];
            if (isSpace(ch)) {
                ++i;
                continue;
            }
            // ── 注释：// 行注释（java/js/ts）；/* */ 块注释（java/js/ts/sql）
            if (ch == '/' && i + 1 < n && line[i + 1] == '/' &&
                lang_ != CodeLang::Sql && lang_ != CodeLang::Json) {
                push(i, n, HlKind::Comment);
                return;
            }
            if (ch == '/' && i + 1 < n && line[i + 1] == '*' &&
                lang_ != CodeLang::Json) {
                const size_t close = line.find("*/", i + 2);
                if (close == std::string_view::npos) {
                    push(i, n, HlKind::Comment);
                    mode_ = CodeHighlighter::Mode::BlockComment;
                    return;
                }
                push(i, close + 2, HlKind::Comment);
                i = close + 2;
                continue;
            }
            // ── SQL 行注释 --
            if (lang_ == CodeLang::Sql && ch == '-' && i + 1 < n &&
                line[i + 1] == '-') {
                push(i, n, HlKind::Comment);
                return;
            }
            // ── Java 文本块 """（跨行）
            if (lang_ == CodeLang::Java && ch == '"' && i + 2 < n &&
                line[i + 1] == '"' && line[i + 2] == '"') {
                bool closed = false;
                size_t j = i + 3;
                while (j + 3 <= n) {
                    if (line[j] == '"' && line[j + 1] == '"' &&
                        line[j + 2] == '"') {
                        size_t slashes = 0;
                        for (size_t p = j; p > i + 3 && line[p - 1] == '\\';
                             --p) {
                            ++slashes;
                        }
                        if (slashes % 2 == 0) {
                            push(i, j + 3, HlKind::String);
                            i = j + 3;
                            closed = true;
                            break;
                        }
                        j += 3;
                    } else {
                        ++j;
                    }
                }
                if (!closed) {
                    push(i, n, HlKind::String);
                    mode_ = CodeHighlighter::Mode::TripleQuote;
                    return;
                }
                continue;
            }
            // ── 字符串/字符字面量（单引号/双引号均不跨行；未闭合 = 漏标回默认）
            if (ch == '"' || ch == '\'') {
                const size_t after = lexQuoted(line, i, ch);
                if (after != std::string_view::npos) {
                    push(i, after, HlKind::String);
                    i = after;
                    continue;
                }
                ++i;  // 未闭合：引号按普通字符推进
                continue;
            }
            // ── JS/TS 反引号模板串（跨行）
            if (ch == '`' && (lang_ == CodeLang::JavaScript ||
                              lang_ == CodeLang::TypeScript)) {
                const size_t close = line.find('`', i + 1);
                if (close == std::string_view::npos) {
                    push(i, n, HlKind::String);
                    mode_ = CodeHighlighter::Mode::Backtick;
                    return;
                }
                push(i, close + 1, HlKind::String);
                i = close + 1;
                continue;
            }
            // ── 数字
            if (isDigit(ch)) {
                const size_t after = lexNumber(line, i);
                push(i, after, HlKind::Number);
                i = after;
                continue;
            }
            // ── 标识符 / 关键字
            if (isIdentStart(ch)) {
                size_t j = i;
                while (j < n && isIdentChar(line[j])) ++j;
                if (isKeyword(lang_, line.substr(i, j - i))) {
                    push(i, j, HlKind::Keyword);
                }
                i = j;
                continue;
            }
            ++i;  // 标点/运算符（默认色）
        }
    }

    CodeLang lang_;
    CodeHighlighter::Mode mode_;
    std::vector<HlSpan>& out_;
};

} // namespace

std::vector<HlSpan> CodeHighlighter::spansForLine(std::string_view line) {
    std::vector<HlSpan> spans;
    LineScanner scanner(lang_, mode_, spans);
    scanner.scan(line);
    mode_ = scanner.mode();
    return spans;
}

} // namespace ui_kit::text_highlight
