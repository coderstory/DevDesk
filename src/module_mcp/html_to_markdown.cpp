#include "html_to_markdown.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace module::mcp {
namespace {

constexpr std::size_t kMaxInputBytes = 2 * 1024 * 1024;  // spec §7:输入上限

std::string lowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string rstripSpaces(const std::string& s) {
    std::size_t end = s.size();
    while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t')) --end;
    return s.substr(0, end);
}

std::string trimAsciiCopy(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    };
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

void appendUtf8(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
        out += static_cast<char>(cp);
    } else if (cp <= 0x7FF) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 常见命名实体(spec §7);表外实体原样保留,不做完整 HTML5 实体表。
const struct NamedEntity {
    const char* name;
    const char* utf8;
} kNamedEntities[] = {
    {"amp", "&"},    {"lt", "<"},      {"gt", ">"},
    {"quot", "\""}, {"apos", "'"},    {"nbsp", "\xC2\xA0"},
    {"copy", "\xC2\xA9"}, {"reg", "\xC2\xAE"}, {"trade", "\xE2\x84\xA2"},
    {"ndash", "\xE2\x80\x93"}, {"mdash", "\xE2\x80\x94"},
    {"hellip", "\xE2\x80\xA6"}, {"laquo", "\xC2\xAB"}, {"raquo", "\xC2\xBB"},
    {"ldquo", "\xE2\x80\x9C"}, {"rdquo", "\xE2\x80\x9D"},
    {"lsquo", "\xE2\x80\x98"}, {"rsquo", "\xE2\x80\x99"},
    {"middot", "\xC2\xB7"}, {"deg", "\xC2\xB0"}, {"plusmn", "\xC2\xB1"},
    {"times", "\xC3\x97"}, {"divide", "\xC3\xB7"},
};

bool digitsOnly(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

std::string decodeEntities(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        if (in[i] != '&') {
            out += in[i++];
            continue;
        }
        const std::size_t semi = in.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) {
            out += '&';
            ++i;
            continue;
        }
        const std::string body = in.substr(i + 1, semi - i - 1);
        bool decoded = false;
        if (!body.empty() && body[0] == '#') {
            long long cp = 0;
            bool valid = false;
            if (body.size() >= 3 && (body[1] == 'x' || body[1] == 'X')) {
                valid = true;
                for (const char c : body.substr(2)) {
                    const int v = hexValue(c);
                    if (v < 0) {
                        valid = false;
                        break;
                    }
                    cp = cp * 16 + v;
                    if (cp > 0x10FFFF) {
                        valid = false;
                        break;
                    }
                }
            } else if (digitsOnly(body.substr(1))) {
                valid = true;
                for (const char c : body.substr(1)) {
                    cp = cp * 10 + (c - '0');
                    if (cp > 0x10FFFF) {
                        valid = false;
                        break;
                    }
                }
            }
            if (valid && (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF))) valid = false;
            if (valid) {
                appendUtf8(out, static_cast<unsigned int>(cp));
                decoded = true;
            }
        } else {
            const std::string lower = lowerAscii(body);
            for (const NamedEntity& e : kNamedEntities) {
                if (body == e.name || lower == e.name) {
                    out += e.utf8;
                    decoded = true;
                    break;
                }
            }
        }
        if (!decoded) out += in.substr(i, semi - i + 1);
        i = semi + 1;
    }
    return out;
}

// 单趟容错扫描器:行模型 = lines_("" = 块间空行),行内片段攒进 inline_,块边界
// (p/div/标题/列表/表格/pre)触发冲刷。引用/列表前缀在冲刷时套用,嵌套结构天然逐行生效。
class Converter {
public:
    std::string run(const std::string& html) {
        parse(html);
        return assemble();
    }

private:
    struct ListFrame {
        bool ordered = false;
        int next = 1;            // 有序表下一个序号
        std::string prefix;      // 本层缩进 = 2 空格 × (深度-1)
        std::string liPrefix;    // 当前 li 行前缀 = prefix + 标记
        bool liOpen = false;
    };

    enum class Mode { Normal, Head, RawSkip, Pre };

    Mode mode_ = Mode::Normal;
    std::string skipTag_;        // RawSkip 的结束标签名(script/style)
    bool inTitle_ = false;
    std::string titleText_;
    std::string preBuf_;
    int quoteDepth_ = 0;
    int headingLevel_ = 0;       // 0 = 不在标题内
    std::string href_;           // 当前 <a> 的目标
    bool inTable_ = false;
    bool cellOpen_ = false;
    std::vector<std::string> row_;
    std::vector<std::vector<std::string>> rows_;
    std::vector<ListFrame> lists_;

    std::vector<std::string> lines_;
    std::string inline_;
    bool pendingSpace_ = false;

    void appendText(const std::string& raw) {
        const std::string decoded = decodeEntities(raw);
        for (const char c : decoded) {
            const bool space = c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
                               c == '\f' || c == '\v';
            if (space) {
                pendingSpace_ = true;
                continue;
            }
            // ASCII 空白折叠为单空格;nbsp(U+00A0) 非 ASCII 空白,不折叠
            if (pendingSpace_ && !inline_.empty() && inline_.back() != '\n') {
                inline_ += ' ';
            }
            pendingSpace_ = false;
            inline_ += c;
        }
    }

    // 行内标记符([ * ` )落位前先消费挂起空格:保证 "a <strong>x" → "a **x**"
    // 而非 "a** x**";标记本身不引入也不吞掉空格。
    void applyPending() {
        if (pendingSpace_ && !inline_.empty() && inline_.back() != '\n') {
            inline_ += ' ';
        }
        pendingSpace_ = false;
    }

    std::string quotePrefix() const {
        std::string p;
        for (int i = 0; i < quoteDepth_; ++i) p += "> ";
        return p;
    }

    std::string listPrefix() const {
        if (lists_.empty() || !lists_.back().liOpen) return "";
        return lists_.back().liPrefix;
    }

    void pushLine(const std::string& line) { lines_.push_back(rstripSpaces(line)); }
    void pushRawLine(const std::string& line) { lines_.push_back(line); }  // pre 内容不整饰

    void pushBlockBreak() {
        if (!lines_.empty() && !lines_.back().empty()) lines_.push_back("");
    }

    // 行级冲刷:不产生块间空行(列表项/引用行延续用)
    void flushInlineLines() {
        if (headingLevel_ > 0) {
            flushHeading();
            return;
        }
        const std::string body = std::move(inline_);
        inline_.clear();
        pendingSpace_ = false;
        std::size_t start = 0;
        while (start <= body.size()) {
            const std::size_t nl = body.find('\n', start);
            const bool last = nl == std::string::npos;
            std::string seg = body.substr(start, (last ? body.size() : nl) - start);
            seg = rstripSpaces(seg);
            if (!seg.empty()) pushLine(quotePrefix() + listPrefix() + seg);
            if (last) break;
            start = nl + 1;
        }
    }

    void flushHeading() {
        const std::string text = trimAsciiCopy(inline_);
        inline_.clear();
        pendingSpace_ = false;
        const int level = headingLevel_;
        headingLevel_ = 0;
        if (text.empty()) return;
        pushLine(std::string(static_cast<std::size_t>(level), '#') + " " + text);
    }

    void flushBlock() {
        if (headingLevel_ > 0) {
            flushHeading();
        } else {
            flushInlineLines();
        }
        pushBlockBreak();
    }

    void openList(const bool ordered) {
        if (lists_.empty()) {
            flushBlock();
        } else {
            flushInlineLines();  // 嵌套列表:先收掉外层 li 当前行,不产生空行
        }
        ListFrame f;
        f.ordered = ordered;
        f.next = 1;
        f.prefix = std::string(2 * lists_.size(), ' ');
        lists_.push_back(f);
    }

    void closeList() {
        if (lists_.empty()) return;
        if (lists_.back().liOpen) flushInlineLines();
        lists_.pop_back();
        if (lists_.empty()) pushBlockBreak();
    }

    void openListItem() {
        if (lists_.empty()) openList(false);  // 容错:裸 <li>
        ListFrame& f = lists_.back();
        if (f.liOpen) flushInlineLines();
        f.liOpen = true;
        f.liPrefix = f.prefix + (f.ordered ? std::to_string(f.next++) + ". " : "- ");
        inline_.clear();
        pendingSpace_ = false;
    }

    void finishCell() {
        if (!cellOpen_) return;
        cellOpen_ = false;
        std::string cell = std::move(inline_);
        inline_.clear();
        pendingSpace_ = false;
        for (char& c : cell) {
            if (c == '\n') c = ' ';
        }
        std::string escaped;
        escaped.reserve(cell.size());
        for (const char c : cell) {
            if (c == '|') escaped += '\\';  // 单元格内 | 转义(spec §7)
            escaped += c;
        }
        row_.push_back(rstripSpaces(escaped));
    }

    void finishTable() {
        finishCell();
        if (!row_.empty()) {
            rows_.push_back(row_);
            row_.clear();
        }
        inTable_ = false;
        emitTable();
    }

    void emitTable() {
        if (rows_.empty()) {
            pushBlockBreak();
            return;
        }
        std::size_t cols = 1;
        for (const auto& r : rows_) cols = std::max(cols, r.size());
        auto emitRow = [&](const std::vector<std::string>& r) {
            std::string line = "|";
            for (std::size_t c = 0; c < cols; ++c) {
                line += " ";
                line += (c < r.size() ? r[c] : std::string());
                line += " |";
            }
            pushLine(line);
        };
        emitRow(rows_.front());  // 首行为表头(spec §7)
        std::string sep = "|";
        for (std::size_t c = 0; c < cols; ++c) sep += " --- |";
        pushLine(sep);
        for (std::size_t i = 1; i < rows_.size(); ++i) emitRow(rows_[i]);
        pushBlockBreak();
    }

    void startPre() {
        flushBlock();
        pushLine("```");
        preBuf_.clear();
        mode_ = Mode::Pre;
    }

    void endPre() {
        std::string raw = std::move(preBuf_);
        preBuf_.clear();
        if (!raw.empty() && raw.front() == '\n') raw.erase(raw.begin());
        if (!raw.empty() && raw.back() == '\n') raw.pop_back();
        std::size_t start = 0;
        while (start <= raw.size()) {
            const std::size_t nl = raw.find('\n', start);
            if (nl == std::string::npos) {
                pushRawLine(raw.substr(start));
                break;
            }
            pushRawLine(raw.substr(start, nl - start));
            start = nl + 1;
        }
        pushRawLine("```");
        mode_ = Mode::Normal;
        pushBlockBreak();
    }

    void emitTextRun(const std::string& run) {
        switch (mode_) {
            case Mode::RawSkip: return;
            case Mode::Head:
                if (inTitle_) titleText_ += decodeEntities(run);
                return;
            case Mode::Pre:
                preBuf_ += decodeEntities(run);  // pre 内空白保持原文,实体仍解码
                return;
            case Mode::Normal:
                appendText(run);
                return;
        }
    }

    void handleOpen(const std::string& name,
                    const std::map<std::string, std::string>& attrs) {
        if (mode_ == Mode::RawSkip) return;
        if (mode_ == Mode::Head) {
            if (name == "title") inTitle_ = true;
            return;
        }
        if (mode_ == Mode::Pre) return;  // pre 内标签一律跳过,内容保持原文
        if (name == "head") {
            mode_ = Mode::Head;
            titleText_.clear();
            return;
        }
        if (name == "script" || name == "style") {
            mode_ = Mode::RawSkip;
            skipTag_ = name;
            return;
        }
        if (name == "pre") {
            startPre();
            return;
        }
        if (name == "br") {
            inline_ += '\n';
            pendingSpace_ = false;
            return;
        }
        if (name == "hr") {
            flushBlock();
            pushLine("---");
            pushBlockBreak();
            return;
        }
        if (name == "img") {
            applyPending();
            inline_ += "![" + attrOf(attrs, "alt") + "](" + attrOf(attrs, "src") + ")";
            return;
        }
        if (name == "meta" || name == "link" || name == "input" || name == "col" ||
            name == "area" || name == "source" || name == "wbr") {
            return;
        }
        if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
            if (cellOpen_) return;  // 单元格内拍平:不再分块
            flushBlock();
            headingLevel_ = name[1] - '0';
            return;
        }
        if (name == "p") {
            if (!cellOpen_) flushBlock();
            return;
        }
        if (name == "div" || name == "section" || name == "article" ||
            name == "header" || name == "footer" || name == "main" ||
            name == "nav" || name == "aside" || name == "figure") {
            if (!cellOpen_) flushBlock();
            return;
        }
        if (name == "a") {
            applyPending();
            href_ = attrOf(attrs, "href");
            inline_ += '[';
            return;
        }
        if (name == "strong" || name == "b") {
            applyPending();
            inline_ += "**";
            return;
        }
        if (name == "em" || name == "i") {
            applyPending();
            inline_ += '*';
            return;
        }
        if (name == "code" || name == "kbd" || name == "samp") {
            applyPending();
            inline_ += '`';
            return;
        }
        if (name == "blockquote") {
            flushBlock();
            ++quoteDepth_;
            return;
        }
        if (name == "ul") {
            openList(false);
            return;
        }
        if (name == "ol") {
            openList(true);
            return;
        }
        if (name == "li") {
            openListItem();
            return;
        }
        if (name == "table") {
            flushBlock();
            inTable_ = true;
            rows_.clear();
            row_.clear();
            cellOpen_ = false;
            return;
        }
        if (name == "tr") {
            if (!inTable_) return;
            finishCell();
            if (!row_.empty()) {
                rows_.push_back(row_);
                row_.clear();
            }
            return;
        }
        if (name == "td" || name == "th") {
            if (!inTable_) return;
            finishCell();
            cellOpen_ = true;
            inline_.clear();
            pendingSpace_ = false;
            return;
        }
        // 其余标签透明处理(span/font/tbody/caption 等):内容照常
    }

    void handleClose(const std::string& name) {
        if (mode_ == Mode::RawSkip) {
            if (name == skipTag_) mode_ = Mode::Normal;
            return;
        }
        if (mode_ == Mode::Head) {
            if (name == "title") {
                inTitle_ = false;
            } else if (name == "head") {
                mode_ = Mode::Normal;
            }
            return;
        }
        if (mode_ == Mode::Pre) {
            if (name == "pre") endPre();
            return;
        }
        if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
            if (headingLevel_ > 0) flushHeading();
            return;
        }
        if (name == "p" || name == "div" || name == "section" || name == "article" ||
            name == "header" || name == "footer" || name == "main" ||
            name == "nav" || name == "aside" || name == "figure") {
            if (!cellOpen_) flushBlock();
            return;
        }
        if (name == "a") {
            if (!href_.empty()) {
                inline_ += "](" + href_ + ")";
                href_.clear();
            }
            return;
        }
        if (name == "strong" || name == "b") {
            applyPending();
            inline_ += "**";
            return;
        }
        if (name == "em" || name == "i") {
            applyPending();
            inline_ += '*';
            return;
        }
        if (name == "code" || name == "kbd" || name == "samp") {
            applyPending();
            inline_ += '`';
            return;
        }
        if (name == "li") {
            if (!lists_.empty() && lists_.back().liOpen) flushInlineLines();
            return;
        }
        if (name == "ul" || name == "ol") {
            closeList();
            return;
        }
        if (name == "blockquote") {
            flushBlock();
            if (quoteDepth_ > 0) --quoteDepth_;
            return;
        }
        if (name == "td" || name == "th") {
            finishCell();
            return;
        }
        if (name == "tr") {
            if (!inTable_) return;
            finishCell();
            if (!row_.empty()) {
                rows_.push_back(row_);
                row_.clear();
            }
            return;
        }
        if (name == "table") {
            if (inTable_) finishTable();
            return;
        }
        // 散落的 </pre>/</script>/</head> 等闭合:容错忽略
    }

    static std::size_t skipUntilSeq(const std::string& s, std::size_t from,
                                    const std::string& seq) {
        const std::size_t p = s.find(seq, from);
        return p == std::string::npos ? s.size() : p + seq.size();
    }

    static std::size_t skipUntilChar(const std::string& s, std::size_t from, const char end) {
        const std::size_t p = s.find(end, from);
        return p == std::string::npos ? s.size() : p + 1;
    }

    static std::string tagName(const std::string& s, std::size_t begin, std::size_t end) {
        std::string name;
        for (std::size_t i = begin; i < end; ++i) {
            const char c = s[i];
            const bool ok = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' ||
                            c == ':';
            if (!ok) break;
            name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return name;
    }

    static std::string attrOf(const std::map<std::string, std::string>& attrs,
                              const std::string& key) {
        const auto it = attrs.find(key);
        return it == attrs.end() ? std::string() : it->second;
    }

    // 解析开标签:返回 '>' 之后位置,npos = EOF 截断(容错丢弃)
    std::size_t parseOpenTag(const std::string& html, std::size_t i,
                             std::string* nameOut,
                             std::map<std::string, std::string>* attrsOut) {
        const std::size_t n = html.size();
        std::size_t p = i + 1;
        const std::string name = tagName(html, p, n);
        if (name.empty()) return std::string::npos;
        *nameOut = name;
        p += name.size();
        while (p < n) {
            while (p < n && std::isspace(static_cast<unsigned char>(html[p])) != 0) ++p;
            if (p >= n) return std::string::npos;
            if (html[p] == '>') return p + 1;
            if (html[p] == '/') {
                std::size_t q = p + 1;  // 自闭合 "/>"(容错 "/ >")
                while (q < n && std::isspace(static_cast<unsigned char>(html[q])) != 0) ++q;
                if (q < n && html[q] == '>') return q + 1;
                ++p;
                continue;
            }
            std::size_t a = p;
            while (a < n && html[a] != '=' && html[a] != '>' && html[a] != '/' &&
                   std::isspace(static_cast<unsigned char>(html[a])) == 0) {
                ++a;
            }
            const std::string attrName = lowerAscii(html.substr(p, a - p));
            while (a < n && std::isspace(static_cast<unsigned char>(html[a])) != 0) ++a;
            std::string value;
            if (a < n && html[a] == '=') {
                ++a;
                while (a < n && std::isspace(static_cast<unsigned char>(html[a])) != 0) ++a;
                if (a < n && (html[a] == '"' || html[a] == '\'')) {
                    const char quote = html[a];
                    const std::size_t close = html.find(quote, a + 1);
                    if (close == std::string::npos) return std::string::npos;
                    value = html.substr(a + 1, close - a - 1);
                    a = close + 1;
                } else {
                    std::size_t b = a;
                    while (b < n && html[b] != '>' &&
                           std::isspace(static_cast<unsigned char>(html[b])) == 0) {
                        ++b;
                    }
                    value = html.substr(a, b - a);
                    a = b;
                }
            }
            if (!attrName.empty()) (*attrsOut)[attrName] = decodeEntities(value);
            p = a;
        }
        return std::string::npos;
    }

    std::size_t parseCloseTag(const std::string& html, std::size_t i,
                              std::string* nameOut) {
        const std::size_t n = html.size();
        std::size_t p = i + 2;
        const std::string name = tagName(html, p, n);
        if (name.empty()) return std::string::npos;
        *nameOut = name;
        while (p < n && html[p] != '>') ++p;
        if (p >= n) return std::string::npos;
        return p + 1;
    }

    void parse(const std::string& html) {
        const std::size_t n = html.size();
        std::size_t i = 0;
        while (i < n) {
            if (html[i] != '<') {
                const std::size_t nxt = html.find('<', i);
                emitTextRun(html.substr(i, (nxt == std::string::npos ? n : nxt) - i));
                i = nxt == std::string::npos ? n : nxt;
                continue;
            }
            if (html.compare(i, 4, "<!--") == 0) {
                i = skipUntilSeq(html, i + 4, "-->");
                continue;
            }
            if (html.compare(i, 2, "<!") == 0 || html.compare(i, 2, "<?") == 0) {
                i = skipUntilChar(html, i + 2, '>');
                continue;
            }
            if (i + 1 < n && html[i + 1] == '/') {
                std::string name;
                const std::size_t next = parseCloseTag(html, i, &name);
                if (next == std::string::npos) return;  // 未闭合容错:丢弃尾部
                handleClose(name);
                i = next;
                continue;
            }
            if (i + 1 < n && std::isalpha(static_cast<unsigned char>(html[i + 1])) != 0) {
                std::string name;
                std::map<std::string, std::string> attrs;
                const std::size_t next = parseOpenTag(html, i, &name, &attrs);
                if (next == std::string::npos) return;
                handleOpen(name, attrs);
                i = next;
                continue;
            }
            emitTextRun("<");  // 非标签的裸 '<' 按文本
            ++i;
        }
    }

    static bool hasH1Line(const std::string& text) {
        std::size_t start = 0;
        while (start <= text.size()) {
            const std::size_t nl = text.find('\n', start);
            const std::string line =
                text.substr(start, (nl == std::string::npos ? text.size() : nl) - start);
            if (line.rfind("# ", 0) == 0) return true;
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        return false;
    }

    std::string assemble() {
        if (mode_ == Mode::Pre) endPre();  // EOF 容错
        if (inTable_) finishTable();
        if (headingLevel_ > 0) flushHeading();
        flushInlineLines();
        std::string out;
        bool pendingBlank = false;
        for (const std::string& line : lines_) {
            if (line.empty()) {
                pendingBlank = pendingBlank || !out.empty();
                continue;
            }
            if (!out.empty()) out += pendingBlank ? "\n\n" : "\n";
            pendingBlank = false;
            out += line;
        }
        // 标题兜底:<title> 非空且正文没有 h1(spec §7)
        const std::string title = trimAsciiCopy(titleText_);
        if (!title.empty() && !hasH1Line(out)) {
            std::string withTitle = "# " + title;
            if (!out.empty()) withTitle += "\n\n";
            withTitle += out;
            out = std::move(withTitle);
        }
        return out;
    }
};

} // namespace

std::string htmlToMarkdown(const std::string& html) {
    std::string input = html;
    if (input.size() > kMaxInputBytes) input.resize(kMaxInputBytes);  // spec §7:超出先截断
    Converter converter;
    return converter.run(input);
}

} // namespace module::mcp
