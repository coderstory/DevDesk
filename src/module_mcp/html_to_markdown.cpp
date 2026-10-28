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

// ── 正文主干提取(正文提取批;蓝图 = docs/superpowers/research/2026-10-27-
// mcp-content-extraction-review.md,词表与阈值逐条对照 Readability/trafilatura
// 一手核对)。调参只动本常量区。规则全部受 main_content 开关整体关断,关断时
// 输出与关断前基线逐字节一致(差分保底测试锁死)。
constexpr std::size_t kSuppressByteLimit = 64 * 1024;  // R1 双保险之二:抑制区字节上限
constexpr std::size_t kLinkDensityBlockLimit = 150;    // R2:块长上限(字节;蓝图自定值)
constexpr double kLinkDensityThreshold = 0.5;          // R2:密度阈值(Readability 高权重端)
constexpr double kAnchorLinkWeight = 0.3;  // R2:锚链接(#开头)文字计重(Readability 同款)

// R1:渲染性/交互性语义标签,整块剔除(svg 不在列则 <svg><text> 会漏进正文)
const char* const kSuppressTags[] = {"header", "footer", "nav", "aside", "form",
                                     "noscript", "iframe", "svg", "template",
                                     "dialog", "button", "select"};
// R1b:class/id 负面词表(Readability unlikelyCandidates 高置信子集 + 国内命名)
const char* const kUnlikelyTokens[] = {
    "footer", "header", "nav", "menu", "sidebar", "banner", "breadcrumb",
    "comment", "related", "share", "social", "sponsor", "promo", "ads",
    "advert", "pagination", "pager", "widget", "copyright", "newsletter",
    "subscribe", "cookie", "gdpr", "drawer", "toolbar"};
// R1b:正词保险丝(词表冲突时存疑不删;docs/code/highlight/snippet 为本仓扩展)
const char* const kMaybeTokens[] = {"article", "content", "main", "post", "entry",
                                    "body", "column", "story", "docs", "doc",
                                    "markdown", "readme", "code", "highlight",
                                    "snippet"};
// R4:role 词表(Readability UNLIKELY_ROLES + banner/contentinfo/search 本仓扩展)
const char* const kSuppressRoles[] = {"menu", "menubar", "complementary",
                                      "navigation", "alert", "alertdialog",
                                      "dialog", "banner", "contentinfo", "search"};

template <std::size_t N>
bool containsAnyToken(const std::string& s, const char* const (&tokens)[N]) {
    for (std::size_t i = 0; i < N; ++i) {
        if (s.find(tokens[i]) != std::string::npos) return true;
    }
    return false;
}

bool isSuppressTag(const std::string& name) {
    return containsAnyToken(name, kSuppressTags);
}

bool isSuppressRole(const std::string& role) {
    for (std::size_t i = 0; i < sizeof(kSuppressRoles) / sizeof(kSuppressRoles[0]);
         ++i) {
        if (role == kSuppressRoles[i]) return true;
    }
    return false;
}

// R1b/R4 容器集:必有闭合配对的块级标签(void/自闭合标签不做 Suppress,防栈滞留)
bool isExtractionContainer(const std::string& name) {
    return name == "div" || name == "section" || name == "ul" || name == "ol" ||
           name == "table" || name == "p";
}

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

// turndown 行内转义表移植(npm turndown@7.2.0 官方 escapes 逐条对照)：
//   任意位置：\ ` * [ ] _
//   片段起点(^ = 文本节点起点，与 turndown 逐节点 escape 同口径)：
//     - → \-   "+ " → "\+ "   "=+" → "\=+"   "#{1,6} " → "\#… "
//     "~~~" → "\~~~"   ">" → "\>"   "N. " → "N\. "
// 只碰 ASCII 标记符，中文字符零触碰。正文零转义会让网页原文的「# 标题」「3. 步骤」
// 「[链接](x)」被下游渲染器重新解释——对 LLM 消费场景即 markdown 注入面。
std::string escapeMarkdownText(const std::string& s, const bool chunkStart) {
    if (s.empty()) return s;
    auto asciiDigit = [](const char c) { return c >= '0' && c <= '9'; };
    std::string out;
    out.reserve(s.size() + 8);
    std::size_t i = 0;
    if (chunkStart) {
        std::size_t mark = 0;      // 起点标记符消费长度
        bool backslash = false;    // true = 整个前缀前补一个 '\'
        switch (s[0]) {
            case '~':
                if (s.compare(0, 3, "~~~") == 0) {
                    mark = 3;
                    backslash = true;
                }
                break;
            case '=':
                while (mark < s.size() && s[mark] == '=') ++mark;
                backslash = true;
                break;
            case '#': {
                while (mark < s.size() && mark < 6 && s[mark] == '#') ++mark;
                if (mark < s.size() && s[mark] == ' ') {
                    backslash = true;
                } else {
                    mark = 0;  // 井号后无空格不构成 ATX 标记
                }
                break;
            }
            case '>':
                mark = 1;
                backslash = true;
                break;
            case '+':
                if (s.size() > 1 && s[1] == ' ') {
                    mark = 1;
                    backslash = true;
                }
                break;
            case '-':
                mark = 1;
                backslash = true;
                break;
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9': {
                std::size_t d = 0;
                while (d < s.size() && asciiDigit(s[d])) ++d;
                // 有序表标记 "N. " → "N\. "：数字原样,反斜杠落在点前,空格保持
                if (d < s.size() && s[d] == '.' && d + 1 < s.size() &&
                    s[d + 1] == ' ') {
                    out.append(s, 0, d);
                    out += '\\';
                    mark = d;
                }
                break;
            }
            default:
                break;
        }
        if (backslash) {
            out += '\\';
            out.append(s, 0, mark);
        }
        i = mark;
    }
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\\' || c == '`' || c == '*' || c == '[' || c == ']' || c == '_') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

// 单趟容错扫描器:行模型 = lines_("" = 块间空行),行内片段攒进 inline_,块边界
// (p/div/标题/列表/表格/pre)触发冲刷。引用/列表前缀在冲刷时套用,嵌套结构天然逐行生效。
class Converter {
public:
    explicit Converter(bool extractMainContent) : mainContent_(extractMainContent) {}

    std::string run(const std::string& html) {
        parse(html);
        return assemble();
    }

private:
    struct ListFrame {
        bool ordered = false;
        int next = 1;            // 有序表下一个序号
        std::string prefix;      // 本层缩进 = 4 空格 × 深度（turndown listItem 口径;
                                 // CommonMark 里 2 空格子列表会被严格渲染器展平）
        std::string liPrefix;    // 当前 li 行前缀 = prefix + 标记
        bool liOpen = false;
    };

    enum class Mode { Normal, Head, RawSkip, Pre, Suppress };

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
    bool inCodeSpan_ = false;  // code/kbd/span 内容不走转义表(CommonMark 码 span 内反斜杠不生效)

    // ── 正文主干提取状态(正文提取批;蓝图 §3)─────────────────────
    bool mainContent_ = true;                     // 总开关(main_content 参数)
    std::vector<std::string> suppressStack_;      // R1:抑制中的标签栈(嵌套计数)
    std::size_t suppressBytes_ = 0;               // R1:抑制区字节计(64KiB 硬释放)
    std::size_t blockChars_ = 0;                  // R2:当前块字符数(转义后,字节)
    double blockLinkChars_ = 0.0;                 // R2:当前块链接文字计重(锚链接 0.3)
    int linkDepth_ = 0;                           // R2:未闭合 <a> 深度
    bool linkIsAnchor_ = false;                   // R2:当前链接 href 是否 # 开头
    bool blockHasVisual_ = false;                 // R2:块内含 <img>(含图块不判)
    int anchorDepth_ = 0;                         // R3:<main>/<article> 锚定深度

    std::vector<std::string> lines_;
    std::string inline_;
    bool pendingSpace_ = false;

    void appendText(const std::string& raw) {
        // 每个文本节点 = 一个转义片段：^ 规则以节点起点为锚（同 turndown 逐节点
        // escape 口径）；转义只插入反斜杠（非空白），不影响随后的空白折叠。
        // 码 span 内容不转义——CommonMark 里反斜杠转义在 code span 内不生效。
        const std::string decoded = decodeEntities(raw);
        const std::string escaped =
            inCodeSpan_ ? decoded : escapeMarkdownText(decoded, /*chunkStart=*/true);
        for (const char c : escaped) {
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
            // R2 计数漏斗:块字符与链接文字(锚链接按 0.3 计重,Readability 同款);
            // 表格单元格不经块模型(flushInlineLines 不裁表格行),计数一并关闭
            if (!cellOpen_) {
                blockChars_ += 1;
                if (linkDepth_ > 0) {
                    blockLinkChars_ += linkIsAnchor_ ? kAnchorLinkWeight : 1.0;
                }
            }
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

    // R2 计数器随块复位(丢弃/保留/标题三路共用,不变量:块边界外计数恒为 0)
    void resetBlockCounters() {
        blockChars_ = 0;
        blockLinkChars_ = 0.0;
        blockHasVisual_ = false;
    }

    // 行级冲刷:不产生块间空行(列表项/引用行延续用)
    void flushInlineLines() {
        if (headingLevel_ > 0) {
            flushHeading();
            return;
        }
        const std::string body = std::move(inline_);

        // R2(正文提取批):短块高链接密度且无图 → 判样板,整块丢弃。
        // 决策点在 body 取走之后(不污染输出);锚定区(R3)/引用块暂停;
        // 标题块已在函数头分流,不经过这里。未闭合 <a>(linkDepth_>0)= 病理
        // 输入,按"存疑不删"保险丝放行。
        const double density =
            blockChars_ > 0
                ? blockLinkChars_ / static_cast<double>(blockChars_)
                : 0.0;
        const bool r2Drop = mainContent_ && anchorDepth_ == 0 &&
                            quoteDepth_ == 0 && linkDepth_ == 0 &&
                            blockChars_ <= kLinkDensityBlockLimit &&
                            density > kLinkDensityThreshold &&
                            !blockHasVisual_;
        if (r2Drop) {
            inline_.clear();  // move 后复位(moved-from 未指定)
            pendingSpace_ = false;
            // 交互点①:半截码 span / 未闭合链接随块复位,防状态漏进后续正文
            inCodeSpan_ = false;
            href_.clear();
            linkDepth_ = 0;
            resetBlockCounters();
            // 交互点②:丢弃有序列表项 → 回退编号,防跳号
            if (!lists_.empty() && lists_.back().liOpen &&
                lists_.back().ordered && lists_.back().next > 1) {
                --lists_.back().next;
            }
            pushBlockBreak();
            return;
        }
        resetBlockCounters();
        inline_.clear();  // move 后复位(moved-from 未指定,必须显式清)

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
        resetBlockCounters();  // 标题块不走 R2 判定,计数不得带入下一块
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
        // 4 空格/层（turndown listItem 续行同款）：有序项 "N. " 标记宽 3 起,
        // 2 空格子列表在严格 CommonMark 渲染器下会与父项平级（被展平）
        f.prefix = std::string(4 * lists_.size(), ' ');
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

    // R1 双保险之二:抑制区超 64KiB → 认定抑制起点残缺,清栈恢复 Normal
    void releaseSuppress() {
        suppressStack_.clear();
        suppressBytes_ = 0;
        mode_ = Mode::Normal;
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
            case Mode::Suppress:
                // R1:抑制区文本整体吞掉;字节计逼近上限即硬释放(双保险之二)
                suppressBytes_ += run.size();
                if (suppressBytes_ > kSuppressByteLimit) releaseSuppress();
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

        // ── 正文主干提取(正文提取批;蓝图 §3;mainContent_=false 全部短路)──
        if (mode_ == Mode::Suppress) {
            // R1 双保险之一:抑制中遇正文信号(article/main/h1-h3)→ 判定抑制起点
            // 残缺,清栈恢复 Normal,该标签照常处理(trafilatura 对含标题节点网开一面)
            const bool headingSignal = name.size() == 2 && name[0] == 'h' &&
                                       name[1] >= '1' && name[1] <= '3';
            if (name == "article" || name == "main" || headingSignal) {
                releaseSuppress();
            } else {
                // 嵌套计数:抑制标签再入与同名容器链(div-in-div)各占一层,
                // 防内层同名闭合提前退栈漏出样板;栈非空是 mode_==Suppress 的不变量
                if (isSuppressTag(name) ||
                    (!suppressStack_.empty() && name == suppressStack_.back())) {
                    suppressStack_.push_back(name);
                }
                return;  // 抑制中其他标签照常吞掉,不产出
            }
        }
        if (mainContent_ && isSuppressTag(name)) {
            if (cellOpen_) return;  // 保险丝:表格单元格内不启动(Readability table 祖先)
            flushBlock();  // 先落盘当前块,行模型不脏
            mode_ = Mode::Suppress;
            suppressStack_.clear();
            suppressStack_.push_back(name);
            suppressBytes_ = 0;
            return;
        }
        if (mainContent_ && isExtractionContainer(name)) {
            // R1b/R4(正文提取批):class/id 负面词表 + role/隐藏惯例 → 整块抑制。
            // 保险丝:正词在身存疑不删(kMaybeTokens,Readability okMaybeItsACandidate
            // 同序前置);表格单元格内不启动;只对必有闭合配对的容器启动
            //(void/自闭合标签入栈必滞留,防漏释放)。
            std::string style = lowerAscii(attrOf(attrs, "style"));
            style.erase(std::remove(style.begin(), style.end(), ' '), style.end());
            const std::string signature =
                lowerAscii(attrOf(attrs, "class") + " " + attrOf(attrs, "id"));
            const bool hiddenByAttr = attrOf(attrs, "aria-hidden") == "true" ||
                                      attrs.find("hidden") != attrs.end() ||
                                      style.find("display:none") != std::string::npos ||
                                      style.find("visibility:hidden") !=
                                          std::string::npos;
            const bool negative = containsAnyToken(signature, kUnlikelyTokens) ||
                                  isSuppressRole(lowerAscii(attrOf(attrs, "role")));
            if (!containsAnyToken(signature, kMaybeTokens) && !cellOpen_ &&
                (negative || hiddenByAttr)) {
                flushBlock();
                mode_ = Mode::Suppress;
                suppressStack_.clear();
                suppressStack_.push_back(name);
                suppressBytes_ = 0;
                return;
            }
        }
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
            if (mainContent_ && !cellOpen_) blockHasVisual_ = true;  // R2 保险丝:含图块不判
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
        if (name == "main" || name == "article") ++anchorDepth_;  // R3 锚定入栈(不 return)
        if (name == "div" || name == "section" || name == "article" ||
            name == "header" || name == "footer" || name == "main" ||
            name == "nav" || name == "aside" || name == "figure") {
            if (!cellOpen_) flushBlock();
            return;
        }
        if (name == "a") {
            applyPending();
            href_ = attrOf(attrs, "href");
            linkIsAnchor_ = !href_.empty() && href_.front() == '#';  // R2:锚链接 0.3 计重
            ++linkDepth_;
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
            inCodeSpan_ = true;
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
        if (mode_ == Mode::Suppress) {
            // R1:乱序闭合一次弹到匹配层(含该层);栈空恢复 Normal
            for (std::size_t i = suppressStack_.size(); i > 0; --i) {
                if (suppressStack_[i - 1] == name) {
                    suppressStack_.resize(i - 1);
                    break;
                }
            }
            if (suppressStack_.empty()) {
                mode_ = Mode::Normal;
                suppressBytes_ = 0;
            }
            return;
        }
        if (name == "main" || name == "article") {
            if (anchorDepth_ > 0) --anchorDepth_;  // R3 锚定出栈
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
            if (linkDepth_ > 0) --linkDepth_;
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
            inCodeSpan_ = false;
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

std::string htmlToMarkdown(const std::string& html, bool extractMainContent) {
    std::string input = html;
    if (input.size() > kMaxInputBytes) input.resize(kMaxInputBytes);  // spec §7:超出先截断
    Converter converter(extractMainContent);
    return converter.run(input);
}

} // namespace module::mcp
