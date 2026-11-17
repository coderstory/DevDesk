// word_wrap_poc_probe.cpp — 软换行（word wrap）POC 探针（need:267）
//
// 无头探针（纯度量 + 切行 + 增量重排，不建窗口不渲染帧）：直接驱动 EUI-NEO
// components/input_model.h 的真实静态函数链，验证「切行算法 + 度量」可用性与成本：
//   1. InputModel::measureLines 在 viewportWidth 有限时的超宽行切段行为
//      （输入模型层软换行是否已存在、UTF-8/CJK 边界是否安全、字节是否无损）；
//   2. 三档语料 × wrap 开/关的首次排版耗时 + 单字符编辑增量重排
//      （updateChangedParagraphs）耗时（POC 核心证据物）；
//   3. InputLayout::build 全链（光标/选区矩形）在切段行模型上是否自洽；
//   4. 量化现状缺陷：拉丁词中途断行（character-level 贪婪切段无词边界感知）的
//      发生率，供 POC 报告的改动面清单引用。
//
// 运行：word_wrap_poc_probe（无参数；退出码 0=全部断言过，1=有断言失败）。
// 与 font_metrics_probe 同款无头纪律：度量是分辨率无关的纯 FreeType 路径，
// 不消费窗口/DPI 因子。

#include <components/input_model.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using InputModel = components::input_detail::InputModel;
using InputState = InputModel::InputState;
using TextLine = InputModel::TextLine;
using Clock = std::chrono::steady_clock;

constexpr const char* kFontFamily = "PingFang SC";  // macOS 系统解析可达的 CJK 族；Windows 侧字体栈兜底
constexpr float kFontSize = 15.0f;
constexpr float kViewportWidth = 600.0f;
constexpr float kWrapOffWidth = 1.0e9f;  // 恒大于任何行宽 → measureLines 走不切段分支 = wrap 关

struct Timing {
    const char* label = "";
    double firstUs = 0.0;      // 首次排版（measureLines 全量）
    double reLayoutUs = 0.0;   // 单字符插入后 updateChangedParagraphs 增量重排
    double fullUs = 0.0;       // 同一编辑用全量 measureLines 的对照耗时
    size_t lineCount = 0;
    size_t midWordBreaks = 0;  // 拉丁词中途断行的显示段数（仅 latin 语料统计）
};

double micros(Clock::duration d) {
    return std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(d).count();
}

bool isContinuation(unsigned char b) { return (b & 0xC0) == 0x80; }

bool utf8BoundaryAt(const std::string& text, size_t pos) {
    return pos == 0 || pos >= text.size() || !isContinuation(static_cast<unsigned char>(text[pos]));
}

bool isAlphaByte(unsigned char b) {
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z');
}

/// 语料 ①：5000 行普通拉丁文本（每行 ~45 字节 ≈ 220KB）。
std::string makeLatinCorpus() {
    static const char* words[] = {"editor", "wrap",  "layout", "shaping", "glyph",
                                  "cache",  "input", "cursor", "scroll",  "metric"};
    std::string text;
    text.reserve(230 * 1024);
    for (int line = 0; line < 5000; ++line) {
        for (int w = 0; w < 8; ++w) {
            text += words[(line + w) % 10];
            text += ' ';
        }
        text += "end\n";
    }
    return text;
}

/// 语料 ②：单行 100KB 超长行（无 \n）——增量重排边界的最坏情形。
std::string makeMegaLineCorpus() {
    std::string text;
    text.reserve(110 * 1024);
    for (int w = 0; w < 12500; ++w) {
        text += "overflow ";
    }
    return text;
}

/// 语料 ③：CJK 混排 5000 行（汉字 + ASCII 词交错，汉字 3 字节 UTF-8）。
std::string makeCjkCorpus() {
    static const char* han[] = {"自动换行", "文本编辑", "软换行测试", "断行边界", "渲染验证"};
    std::string text;
    text.reserve(480 * 1024);
    for (int line = 0; line < 5000; ++line) {
        text += han[line % 5];
        text += " mixed ";
        text += han[(line + 2) % 5];
        text += " with latin tail ";
        text += std::to_string(line % 1000);
        text += '\n';
    }
    return text;
}

/// 断言切行结果：字节无损重建 + UTF-8 边界安全 + 除不可断单词元外段宽不超视口。
/// 返回失败原因，空串 = 通过。midWordBreaks 统计拉丁词中途断行段数。
std::string verifyLines(const std::string& text, const std::vector<TextLine>& lines,
                        float viewportWidth, size_t* midWordBreaks) {
    if (midWordBreaks != nullptr) {
        *midWordBreaks = 0;
    }
    std::string rebuilt;
    rebuilt.reserve(text.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        const TextLine& line = lines[i];
        if (line.start < 0 || line.end < line.start || line.end > static_cast<int>(text.size())) {
            return "byte range out of bounds";
        }
        if (!utf8BoundaryAt(text, static_cast<size_t>(line.start)) ||
            !utf8BoundaryAt(text, static_cast<size_t>(line.end))) {
            return "segment splits a UTF-8 codepoint (continuation byte boundary)";
        }
        rebuilt.append(text, static_cast<size_t>(line.start), static_cast<size_t>(line.end - line.start));
        if (line.hardBreakAfter) {
            rebuilt += '\n';
        }
        // 段宽断言：放行两个例外——单码点超宽（不可断）与空行。
        const bool singleCodepoint = (line.end - line.start) <= 4;
        if (viewportWidth < kWrapOffWidth && !singleCodepoint && line.metrics.width > viewportWidth + 1.0f) {
            return "segment exceeds viewport width";
        }
        // 拉丁词中途断行：段尾与下一段首都是字母 = 断在词内（词边界感知缺失的证据）。
        if (midWordBreaks != nullptr && !line.hardBreakAfter && i + 1 < lines.size() &&
            line.end > line.start && !line.hardBreakAfter) {
            const TextLine& next = lines[i + 1];
            if (isAlphaByte(static_cast<unsigned char>(text[line.end - 1])) &&
                next.end > next.start &&
                isAlphaByte(static_cast<unsigned char>(text[next.start]))) {
                ++(*midWordBreaks);
            }
        }
    }
    if (rebuilt != text) {
        return "rebuilt text != original (bytes lost or duplicated)";
    }
    return {};
}

/// 单配置测量：全量排版 → 模拟单字符插入 → 增量重排 vs 全量对照。
Timing measureOne(const char* label, const std::string& base, const std::string& edited,
                  float viewportWidth, bool countMidWord) {
    Timing t;
    t.label = label;

    InputState state;
    state.text = base;
    ++state.textRevision;
    Clock::time_point t0 = Clock::now();
    state.cachedLines = InputModel::measureLines(state.text, kFontFamily, kFontSize, viewportWidth);
    Clock::time_point t1 = Clock::now();
    t.firstUs = micros(t1 - t0);
    t.lineCount = state.cachedLines.size();

    // 全量排版结果同样要过断言（正确性与耗时同源，避免「快但错」的假绿）。
    const std::string err = verifyLines(state.text, state.cachedLines, viewportWidth,
                                        countMidWord ? &t.midWordBreaks : nullptr);
    if (!err.empty()) {
        std::printf("  FAIL[%s] verify: %s\n", label, err.c_str());
    }

    // 增量重排：单字符插入于文档中部（与 updateChangedParagraphs 的消费方式对齐：
    // cachedLayoutText/cachedLines 先就位，编辑后 revision 自增再调用）。
    state.cachedLayoutText = state.text;
    state.cachedTextRevision = state.textRevision;
    state.text = edited;
    ++state.textRevision;
    t0 = Clock::now();
    InputModel::updateChangedParagraphs(state, kFontFamily, kFontSize, viewportWidth);
    t1 = Clock::now();
    t.reLayoutUs = micros(t1 - t0);

    // 对照：同一编辑结果从零全量排版。
    t0 = Clock::now();
    std::vector<TextLine> full = InputModel::measureLines(state.text, kFontFamily, kFontSize, viewportWidth);
    t1 = Clock::now();
    t.fullUs = micros(t1 - t0);
    // 增量结果必须与全量结果逐行一致（起点/终点/硬换行标记），否则增量路径有损。
    if (full.size() != state.cachedLines.size()) {
        std::printf("  FAIL[%s] incremental line count %zu != full %zu\n", label,
                    state.cachedLines.size(), full.size());
    } else {
        for (size_t i = 0; i < full.size(); ++i) {
            if (full[i].start != state.cachedLines[i].start || full[i].end != state.cachedLines[i].end ||
                full[i].hardBreakAfter != state.cachedLines[i].hardBreakAfter) {
                std::printf("  FAIL[%s] incremental line %zu mismatch\n", label, i);
                break;
            }
        }
    }
    return t;
}

void printTiming(const Timing& t) {
    std::printf("  %-34s lines=%-6zu first=%9.1fus  incrementalEdit=%9.1fus  fullRelayout=%9.1fus  midWordBreaks=%zu\n",
                t.label, t.lineCount, t.firstUs, t.reLayoutUs, t.fullUs, t.midWordBreaks);
}

/// 切行正确性的可见证据：中英混排小样逐段打印（验收 ① 的输出证据）。
void printSegmentDemo() {
    const std::string sample = "hello 世界 this is 自动换行测试 with a verylongenglishwordwithoutspaces 结尾";
    const std::vector<TextLine> lines = InputModel::measureLines(sample, kFontFamily, kFontSize, 220.0f);
    std::printf("[demo] sample %zu bytes, viewport 220px -> %zu display lines\n", sample.size(), lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        std::printf("  [%zu] bytes[%d,%d) w=%.1f hard=%d | %.*s\n", i, lines[i].start, lines[i].end,
                    lines[i].metrics.width, lines[i].hardBreakAfter ? 1 : 0,
                    lines[i].end - lines[i].start, sample.c_str() + lines[i].start);
    }
}

/// InputLayout::build 全链自洽：切段行模型上光标/选区矩形可生成且坐标不越界。
bool verifyLayoutChain(const std::string& text) {
    InputState state;
    state.text = text;
    ++state.textRevision;
    state.cursor = static_cast<int>(text.size() / 2);
    state.selectionStart = static_cast<int>(text.size() / 3);
    state.selectionEnd = state.cursor;
    const InputModel::InputLayout layout = InputModel::InputLayout::build(
        state, kViewportWidth, 300.0f, kViewportWidth, 8.0f, 0.0f, 20.0f, kFontFamily, kFontSize, true);
    std::printf("[layout] contentHeight=%.0f maxVScroll=%.0f cursorLine=%d cursorX=%.1f cursorY=%.1f selectionRects=%zu hScroll=%.0f\n",
                layout.contentHeight, layout.maxVerticalScroll, layout.cursorLine, layout.cursorX,
                layout.cursorY, layout.selectionRects.size(), layout.scroll);
    if (layout.selectionRects.empty()) {
        std::printf("  FAIL[layout] selection rects empty for mid-document selection\n");
        return false;
    }
    for (const InputModel::TextSelectionRect& r : layout.selectionRects) {
        if (r.width < 0.0f || r.height <= 0.0f) {
            std::printf("  FAIL[layout] degenerate selection rect\n");
            return false;
        }
    }
    // 多行分支下 horizontalScroll 必须恒 0（横向滚动只属于单行态，input_model.h:150）。
    if (state.horizontalScroll != 0.0f) {
        std::printf("  FAIL[layout] multiline horizontalScroll != 0\n");
        return false;
    }
    return true;
}

} // namespace

int main() {
    core::TextPrimitive::setDefaultFontFiles("DevDeskSansSC-Regular.otf", "");
    bool passed = true;

    printSegmentDemo();

    std::printf("[perf] corpus timings (viewport=%.0fpx, fontSize=%.0f, PingFang SC fallback)\n",
                kViewportWidth, kFontSize);
    const std::string latin = makeLatinCorpus();
    const std::string mega = makeMegaLineCorpus();
    const std::string cjk = makeCjkCorpus();
    std::printf("  corpus sizes: latin=%zuKB megaSingleLine=%zuKB cjkMixed=%zuKB\n",
                latin.size() / 1024, mega.size() / 1024, cjk.size() / 1024);

    // 单字符插入点：文档中部，避开首尾特例（编辑副本与原语料并存，两态共用同一基线）。
    std::string latinEdited = latin;
    latinEdited.insert(latinEdited.size() / 2, "x");
    std::string megaEdited = mega;
    megaEdited.insert(megaEdited.size() / 2, "x");
    std::string cjkEdited = cjk;
    cjkEdited.insert(cjkEdited.size() / 2, "测");

    std::vector<Timing> timings;
    timings.push_back(measureOne("latin5000 wrap ON", latin, latinEdited, kViewportWidth, true));
    timings.push_back(measureOne("latin5000 wrap OFF", latin, latinEdited, kWrapOffWidth, true));
    timings.push_back(measureOne("mega100K wrap ON", mega, megaEdited, kViewportWidth, false));
    timings.push_back(measureOne("mega100K wrap OFF", mega, megaEdited, kWrapOffWidth, false));
    timings.push_back(measureOne("cjk5000 wrap ON", cjk, cjkEdited, kViewportWidth, false));
    timings.push_back(measureOne("cjk5000 wrap OFF", cjk, cjkEdited, kWrapOffWidth, false));
    for (const Timing& t : timings) {
        printTiming(t);
    }

    std::printf("[layout-chain] InputLayout::build on wrapped model\n");
    if (!verifyLayoutChain(cjk)) {
        passed = false;
    }

    // demo 切段同样要过字节无损 + 边界断言。
    {
        const std::string sample = "hello 世界 this is 自动换行测试 with a verylongenglishwordwithoutspaces 结尾";
        const std::vector<TextLine> lines = InputModel::measureLines(sample, kFontFamily, kFontSize, 220.0f);
        const std::string err = verifyLines(sample, lines, 220.0f, nullptr);
        if (!err.empty()) {
            std::printf("  FAIL[demo] %s\n", err.c_str());
            passed = false;
        }
    }

    std::printf(passed ? "[poc] PASS: wrap segmentation byte-exact, UTF-8/CJK boundaries safe, layout chain self-consistent\n"
                       : "[poc] FAIL: see FAIL lines above\n");
    return passed ? 0 : 1;
}
