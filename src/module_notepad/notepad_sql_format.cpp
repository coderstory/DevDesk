// notepad_sql_format.cpp — SQL 格式化引擎实现（接口语义见头文件，need:380）。
// 形态：两遍扫描——先 tokenize（注释/字符串/词/数字/标点五类，空白丢弃但
// 行注释收行语义保留），再线性排版。识别不确定 = 原样搬运（词按原样输出），
// 不错改（与 md_keyword_highlight 同源容错纪律）。
#include "notepad_sql_format.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace module::notepad {
namespace {

bool isIdentStart(char c) {
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool isIdentChar(char c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

// 词元类别（空白不入流；行注释 = 自带收行义务的独立词元）
enum class TokKind { LineComment, BlockComment, String, Word, Number, Punct };

struct Token {
    TokKind kind;
    std::string text;  // String 含首尾引号；注释含全部原文
};

// 顶层断行关键字（子句起始；BY 不单断——GROUP BY/ORDER BY 断在 GROUP/ORDER）
const std::unordered_set<std::string_view>& clauseBreaks() {
    static const std::unordered_set<std::string_view> kw = {
        "SELECT", "FROM", "WHERE", "GROUP", "ORDER", "HAVING", "LIMIT",
        "OFFSET", "UNION", "INTERSECT", "EXCEPT", "INSERT", "UPDATE",
        "DELETE", "SET", "VALUES", "WITH", "JOIN", "INNER", "LEFT", "RIGHT",
        "FULL", "OUTER", "CROSS", "ON", "RETURNING",
    };
    return kw;
}

// 断行 + 一档缩进关键字（顶层表达式连接词）
const std::unordered_set<std::string_view>& indentBreaks() {
    static const std::unordered_set<std::string_view> kw = {
        "AND", "OR",
    };
    return kw;
}

// 全量识别表（规范化大写输出 + isSqlKeyword 对外单一源）
const std::unordered_set<std::string_view>& sqlKeywordSet() {
    static const std::unordered_set<std::string_view> kw = {
        "SELECT", "FROM", "WHERE", "GROUP", "BY", "HAVING", "ORDER", "LIMIT",
        "OFFSET", "INSERT", "INTO", "VALUES", "UPDATE", "SET", "DELETE",
        "CREATE", "DROP", "ALTER", "ADD", "COLUMN", "TABLE", "VIEW", "INDEX",
        "TRIGGER", "SCHEMA", "DATABASE", "IF", "EXISTS", "NOT", "DEFAULT",
        "PRIMARY", "KEY", "FOREIGN", "REFERENCES", "UNIQUE", "CHECK",
        "CONSTRAINT", "AUTO_INCREMENT", "AS", "DISTINCT", "ALL", "AND", "OR",
        "IN", "IS", "NULL", "LIKE", "ILIKE", "BETWEEN", "ESCAPE", "UNION",
        "INTERSECT", "EXCEPT", "WITH", "RECURSIVE", "CASE", "WHEN", "THEN",
        "ELSE", "END", "JOIN", "INNER", "LEFT", "RIGHT", "FULL", "OUTER",
        "CROSS", "ON", "USING", "NATURAL", "ASC", "DESC", "RETURNING",
        "CONFLICT", "DO", "NOTHING", "BEGIN", "COMMIT", "ROLLBACK",
        "TRANSACTION", "GRANT", "REVOKE", "EXPLAIN", "ANALYZE", "VACUUM",
        "PRAGMA", "ATTACH", "DETACH", "REINDEX", "CAST", "COLLATE", "OVER",
        "PARTITION", "WINDOW", "FILTER", "INT", "INTEGER", "SMALLINT",
        "BIGINT", "DECIMAL", "NUMERIC", "REAL", "FLOAT", "DOUBLE",
        "PRECISION", "CHAR", "VARCHAR", "NVARCHAR", "TEXT", "CLOB", "BLOB",
        "BOOLEAN", "BOOL", "DATE", "TIME", "DATETIME", "TIMESTAMP", "INTERVAL",
        "UUID", "JSON", "JSONB", "SERIAL", "BIGSERIAL", "TRUE", "FALSE",
        "UNKNOWN", "CURRENT_DATE", "CURRENT_TIME", "CURRENT_TIMESTAMP",
        "CURRENT_USER", "SESSION_USER", "LOCALTIMESTAMP", "TOP",
    };
    return kw;
}

std::string toUpperAscii(std::string_view w) {
    std::string out(w);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    }
    return out;
}

// 关键字命中（大小写不敏感折叠后查大写表）
bool keywordHit(std::string_view word) {
    if (word.empty() || word.size() > 24) return false;  // 超长词必非关键字（表内最长 16）
    return sqlKeywordSet().count(toUpperAscii(word)) > 0;
}

// ── 第一遍：tokenize ─────────────────────────────────────────────
std::vector<Token> tokenize(std::string_view s) {
    std::vector<Token> out;
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++i;
            continue;
        }
        // -- 行注释：吞到行尾（不含换行，换行由排版层给）
        if (c == '-' && i + 1 < n && s[i + 1] == '-') {
            size_t j = s.find('\n', i);
            if (j == std::string_view::npos) j = n;
            out.push_back({TokKind::LineComment, std::string(s.substr(i, j - i))});
            i = j;
            continue;
        }
        // /* */ 块注释：吞到 */（未闭合吞到串尾，原样搬运不错改）
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            const size_t close = s.find("*/", i + 2);
            const size_t end = close == std::string_view::npos ? n : close + 2;
            out.push_back({TokKind::BlockComment, std::string(s.substr(i, end - i))});
            i = end;
            continue;
        }
        // 字符串：'…'（'' 转义）、"…"、`…`（未闭合吞到串尾）
        if (c == '\'' || c == '"' || c == '`') {
            size_t j = i + 1;
            while (j < n) {
                if (s[j] == c) {
                    if (c == '\'' && j + 1 < n && s[j + 1] == '\'') {
                        j += 2;  // SQL '' 双写转义
                        continue;
                    }
                    ++j;
                    break;
                }
                ++j;
            }
            out.push_back({TokKind::String, std::string(s.substr(i, j - i))});
            i = j;
            continue;
        }
        // 词 / 数字
        if (isIdentStart(c)) {
            size_t j = i;
            while (j < n && isIdentChar(s[j])) ++j;
            out.push_back({TokKind::Word, std::string(s.substr(i, j - i))});
            i = j;
            continue;
        }
        if (isDigit(c)) {
            size_t j = i;
            while (j < n && (isIdentChar(s[j]) || s[j] == '.')) ++j;  // 容忍过配（1.5e3）
            out.push_back({TokKind::Number, std::string(s.substr(i, j - i))});
            i = j;
            continue;
        }
        // 标点（单字符；多字符运算符按相邻标点各自成元，排版层统一贴排）
        out.push_back({TokKind::Punct, std::string(1, c)});
        ++i;
    }
    return out;
}

// ── 第二遍：排版 ─────────────────────────────────────────────────
std::string render(const std::vector<Token>& toks) {
    constexpr size_t kIndentStep = 4;
    std::string out;
    out.reserve(toks.empty() ? 0 : toks.size() * 8);
    int depth = 0;
    bool lineStart = true;      // 行首（待缩进，尚未写内容）
    bool selectList = false;    // 顶层 SELECT 列表域（遇 FROM 等主子句退出）
    bool needSpace = false;     // 行内已写内容 → 下个非贴排词元前置空格

    const auto writeIndent = [&] {
        if (lineStart) {
            for (size_t k = 0; k < kIndentStep * static_cast<size_t>(depth); ++k) {
                out.push_back(' ');
            }
            lineStart = false;
            needSpace = false;
        }
    };
    const auto newline = [&](size_t extraIndent) {
        out.push_back('\n');
        lineStart = true;
        for (size_t k = 0; k < extraIndent; ++k) out.push_back(' ');  // 缩进即写
        lineStart = false;
        needSpace = false;
    };

    for (size_t idx = 0; idx < toks.size(); ++idx) {
        const Token& tok = toks[idx];
        if (tok.kind == TokKind::Word && keywordHit(tok.text)) {
            const std::string upper = toUpperAscii(tok.text);
            const bool clause = clauseBreaks().count(upper) > 0;
            const bool indentB = indentBreaks().count(upper) > 0;
            if (depth == 0 && (clause || indentB)) {
                if (!out.empty()) {
                    newline(0);
                }
                writeIndent();
            } else {
                writeIndent();
                if (needSpace) out.push_back(' ');
            }
            out += upper;
            needSpace = true;
            if (depth == 0) {
                if (upper == "SELECT") {
                    selectList = true;
                } else if (clause && upper != "SELECT") {
                    selectList = false;
                }
            }
            continue;
        }
        if (tok.kind == TokKind::LineComment) {
            writeIndent();
            if (needSpace) out.push_back(' ');
            out += tok.text;
            newline(0);  // 行注释强制收行
            continue;
        }
        if (tok.kind == TokKind::BlockComment || tok.kind == TokKind::String) {
            writeIndent();
            if (needSpace) out.push_back(' ');
            out += tok.text;
            needSpace = true;
            continue;
        }
        if (tok.kind == TokKind::Number) {
            writeIndent();
            if (needSpace) out.push_back(' ');
            out += tok.text;
            needSpace = true;
            continue;
        }
        // 标点
        const char c = tok.text[0];
        if (c == '(') {
            writeIndent();
            if (needSpace) out.push_back(' ');
            out.push_back('(');
            ++depth;
            needSpace = false;
            continue;
        }
        if (c == ')') {
            if (depth > 0) --depth;
            out.push_back(')');  // 紧贴前置内容
            needSpace = true;
            continue;
        }
        if (c == ',') {
            out.push_back(',');
            needSpace = true;
            if (depth == 0 && selectList) {
                newline(kIndentStep);  // 顶层 SELECT 列表逐列断行
            }
            continue;
        }
        if (c == ';') {
            out.push_back(';');
            // 语句收尾：下一条语句起新行（0 缩进）
            if (idx + 1 < toks.size()) {
                newline(0);
            }
            needSpace = false;
            continue;
        }
        writeIndent();
        if (needSpace) out.push_back(' ');
        out.push_back(c);
        needSpace = c != '.';  // 成员访问点号紧贴右侧
    }
    return out;
}

}  // namespace

bool isSqlKeyword(std::string_view word) { return keywordHit(word); }

std::string formatSqlText(std::string_view sql) {
    if (sql.empty()) return std::string(sql);
    return render(tokenize(sql));
}

}  // namespace module::notepad
