// notepad_find.cpp — 查找/替换纯函数引擎实现（接口语义见 notepad_find.h）。
#include "notepad_find.h"

#include <algorithm>

namespace module::notepad {
namespace {

inline bool isWordByte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

// ASCII 折叠等值（>=0x80 字节原样比较，禁 locale）。
inline bool asciiFoldEq(unsigned char a, unsigned char b) {
    if (a == b) return true;
    if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a + 32);
    if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b + 32);
    return a == b;
}

inline bool byteEq(unsigned char a, unsigned char b, bool caseSensitive) {
    return caseSensitive ? a == b : asciiFoldEq(a, b);
}

}  // namespace

std::vector<FindHit> findAll(const std::string& text, const std::string& query,
                             const FindOptions& opts) {
    std::vector<FindHit> hits;
    const size_t n = text.size();
    const size_t q = query.size();
    if (q == 0 || q > n) return hits;
    const bool cs = opts.caseSensitive;
    const unsigned char first = static_cast<unsigned char>(query[0]);
    for (size_t pos = 0; pos + q <= n;) {
        if (!byteEq(static_cast<unsigned char>(text[pos]), first, cs)) {
            ++pos;
            continue;
        }
        bool match = true;
        for (size_t i = 1; i < q; ++i) {
            if (!byteEq(static_cast<unsigned char>(text[pos + i]),
                        static_cast<unsigned char>(query[i]), cs)) {
                match = false;
                break;
            }
        }
        if (match && opts.wholeWord) {
            // 全词边界：首尾外侧不得邻接词字符（>=0x80 字节按词字符计）
            const bool okBefore =
                pos == 0 || !isWordByte(static_cast<unsigned char>(text[pos - 1]));
            const bool okAfter = pos + q >= n ||
                                 !isWordByte(static_cast<unsigned char>(text[pos + q]));
            if (!okBefore || !okAfter) match = false;
        }
        if (match) {
            hits.push_back({pos, pos + q});
            pos += q;  // 非重叠：命中后跳过整段
        } else {
            ++pos;
        }
    }
    return hits;
}

size_t nextFrom(const std::vector<FindHit>& hits, size_t fromByte) {
    for (size_t i = 0; i < hits.size(); ++i) {
        if (hits[i].start > fromByte) return i;
    }
    return hits.empty() ? kFindNoHit : 0;  // 环绕回首个
}

size_t prevFrom(const std::vector<FindHit>& hits, size_t fromByte) {
    for (size_t i = hits.size(); i > 0; --i) {
        if (hits[i - 1].start < fromByte) return i - 1;
    }
    return hits.empty() ? kFindNoHit : hits.size() - 1;  // 环绕回末个
}

ReplaceOneResult replaceOneAt(const std::string& text, const FindHit& hit,
                              const std::string& repl,
                              const std::string& query, const FindOptions& opts) {
    ReplaceOneResult r;
    r.text = text;
    r.text.replace(hit.start, hit.end - hit.start, repl);
    const size_t afterByte = hit.start + repl.size();  // 替换文本末尾
    // F-07：下一个命中 = 首个 start >= afterByte（含环绕）。nextFrom 是严格
    // start > fromByte，故探测点退一格；afterByte==0（空替换且命中在文首）
    // 时探测点 0，start==0 命中由环绕路径兜回，结果一致。
    const std::vector<FindHit> hits = findAll(r.text, query, opts);
    const size_t probe = afterByte > 0 ? afterByte - 1 : 0;
    const size_t idx = nextFrom(hits, probe);
    if (idx != kFindNoHit) {
        r.cursor = hits[idx].start;
        r.selStart = hits[idx].start;
        r.selEnd = hits[idx].end;
    } else {
        // 无下一命中（替换后剩余命中集为空）→ 替换文本末尾，零宽选区
        r.cursor = afterByte;
        r.selStart = afterByte;
        r.selEnd = afterByte;
    }
    return r;
}

ReplaceAllResult replaceAll(const std::string& text, const std::string& query,
                            const std::string& repl, const FindOptions& opts) {
    ReplaceAllResult r;
    const std::vector<FindHit> hits = findAll(text, query, opts);
    r.count = hits.size();
    if (hits.empty()) {
        r.text = text;
        return r;
    }
    r.text.reserve(text.size() + hits.size() * repl.size());
    size_t last = 0;
    for (const FindHit& hit : hits) {
        r.text.append(text, last, hit.start - last);
        r.text.append(repl);
        last = hit.end;
    }
    r.text.append(text, last, std::string::npos);
    return r;
}

std::vector<eui::TextRun> mergeRuns(const std::vector<eui::TextRun>& base,
                                    const std::vector<FindHit>& hits,
                                    const FindHit* currentHit,
                                    const std::string& lineText,
                                    size_t lineStartByte, eui::Color hitColor,
                                    eui::Color currentHitColor,
                                    bool hitBold) {
    std::vector<eui::TextRun> out;
    const size_t lineStart = lineStartByte;
    const size_t lineEnd = lineStartByte + lineText.size();
    if (lineEnd <= lineStart) return out;

    // 命中层段（裁剪进本行；findAll 输出已按 start 升序且互不重叠）
    struct Overlay {
        size_t start;
        size_t end;
        eui::Color color;
        bool bold;
    };
    std::vector<Overlay> overlays;
    overlays.reserve(hits.size());
    for (const FindHit& hit : hits) {
        if (hit.end <= lineStart || hit.start >= lineEnd) continue;
        const bool isCurrent =
            currentHit != nullptr && currentHit->start == hit.start &&
            currentHit->end == hit.end;
        overlays.push_back({std::max(hit.start, lineStart),
                            std::min(hit.end, lineEnd),
                            isCurrent ? currentHitColor : hitColor,
                            hitBold});
    }

    // 切点集合：行界 + 全部层段边界 → 任意切出的段要么整体在某层段内，要么整体不在
    std::vector<size_t> cuts;
    cuts.reserve(2 + 2 * (base.size() + overlays.size()));
    cuts.push_back(lineStart);
    cuts.push_back(lineEnd);
    for (const eui::TextRun& run : base) {
        const size_t s = std::max(static_cast<size_t>(run.byteStart), lineStart);
        const size_t e = std::min(static_cast<size_t>(run.byteEnd), lineEnd);
        if (s < e) {
            cuts.push_back(s);
            cuts.push_back(e);
        }
    }
    for (const Overlay& ov : overlays) {
        cuts.push_back(ov.start);
        cuts.push_back(ov.end);
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    auto coveringColor = [](const Overlay& ov, size_t a) {
        return a >= ov.start && a < ov.end;
    };

    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        const size_t a = cuts[i];
        const size_t b = cuts[i + 1];
        if (b <= a) continue;
        // 命中层覆盖语法层（冲突消解 = 命中色覆盖语法色）
        const Overlay* ov = nullptr;
        for (const Overlay& cand : overlays) {
            if (coveringColor(cand, a)) {
                ov = &cand;
                break;
            }
        }
        eui::Color color{};
        bool bold = false;
        bool hasColor = false;
        if (ov != nullptr) {
            color = ov->color;
            bold = ov->bold;
            hasColor = true;
        } else {
            for (const eui::TextRun& run : base) {
                if (static_cast<size_t>(run.byteStart) <= a &&
                    a < static_cast<size_t>(run.byteEnd)) {
                    color = run.color;
                    bold = run.bold;
                    hasColor = true;
                    break;
                }
            }
        }
        if (!hasColor) continue;  // 语法层未覆盖 = 默认正文色，不产 run
        // 相邻同样式合并（如 "aaaa" 找 "aa" 产生的相邻命中段）
        if (!out.empty() && out.back().byteEnd == static_cast<int>(a) &&
            out.back().color.r == color.r && out.back().color.g == color.g &&
            out.back().color.b == color.b && out.back().color.a == color.a &&
            out.back().bold == bold) {
            out.back().byteEnd = static_cast<int>(b);
        } else {
            out.push_back({static_cast<int>(a), static_cast<int>(b), color, bold});
        }
    }
    return out;
}

}  // namespace module::notepad
