#include "module_terminal/web/qr/qr_encode.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <utility>

namespace module::terminal::web {
namespace {

constexpr int kMaxVersion = 10;

// L 级每版本：数据码字数 / 每块纠错码字数 / 块数（ISO/IEC 18004 表 9）。
// 多块时数据码字按 base / base+1 两组切分，末尾 extra 块多 1 码字（V10 起多块）。
struct VersionEccL {
    int dataCodewords;
    int ecPerBlock;
    int blocks;
};
constexpr std::array<VersionEccL, kMaxVersion + 1> kVersionEccL{{
    {0, 0, 0},  // V0 占位（版本从 1 起）
    {19, 7, 1},   {34, 10, 1}, {55, 15, 1}, {80, 20, 1},  {108, 26, 1},
    {136, 18, 2}, {156, 20, 2}, {194, 24, 2}, {232, 30, 2}, {274, 18, 4},
}};

// 校正图形中心坐标（V1 无）。V7+ 含 6 会压住时序线，按标准覆盖。
std::vector<int> alignmentPositions(int version) {
    switch (version) {
        case 2: return {6, 18};
        case 3: return {6, 22};
        case 4: return {6, 26};
        case 5: return {6, 30};
        case 6: return {6, 34};
        case 7: return {6, 22, 38};
        case 8: return {6, 24, 42};
        case 9: return {6, 26, 46};
        case 10: return {6, 28, 50};
        default: return {};
    }
}

int byteCapacity(int version) {
    const int countBits = (version >= 10) ? 16 : 8;
    return (kVersionEccL[version].dataCodewords * 8 - 4 - countBits) / 8;
}

// GF(256)，本原多项式 0x11D。
struct Gf256 {
    std::array<uint8_t, 512> exp{};
    std::array<uint8_t, 256> log{};
    Gf256() {
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[static_cast<std::size_t>(i)] = static_cast<uint8_t>(x);
            log[static_cast<std::size_t>(x)] = static_cast<uint8_t>(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; ++i)
            exp[static_cast<std::size_t>(i)] = exp[static_cast<std::size_t>(i - 255)];
    }
    uint8_t mul(uint8_t a, uint8_t b) const {
        if (a == 0 || b == 0) return 0;
        return exp[static_cast<std::size_t>(log[a]) + static_cast<std::size_t>(log[b])];
    }
};
const Gf256& gf() {
    static const Gf256 inst;
    return inst;
}

std::vector<uint8_t> genPoly(int degree) {
    const Gf256& g = gf();
    std::vector<uint8_t> poly{1};
    for (int i = 0; i < degree; ++i) {
        std::vector<uint8_t> next(poly.size() + 1, 0);
        const uint8_t factor = g.exp[static_cast<std::size_t>(i)];
        for (std::size_t j = 0; j < poly.size(); ++j) {
            next[j] ^= poly[j];
            next[j + 1] ^= g.mul(poly[j], factor);
        }
        poly.swap(next);
    }
    return poly;
}

std::vector<uint8_t> ecBytes(const std::vector<uint8_t>& data, int degree) {
    const Gf256& g = gf();
    const std::vector<uint8_t> gen = genPoly(degree);
    std::vector<uint8_t> rem(data.begin(), data.end());
    rem.resize(data.size() + static_cast<std::size_t>(degree), 0);
    for (std::size_t i = 0; i < data.size(); ++i) {
        const uint8_t factor = rem[i];
        if (factor == 0) continue;
        for (std::size_t j = 0; j < gen.size(); ++j) {
            rem[i + j] ^= g.mul(gen[j], factor);
        }
    }
    return std::vector<uint8_t>(rem.begin() + static_cast<std::ptrdiff_t>(data.size()), rem.end());
}

bool buildDataCodewords(const std::string& payload, int version, int dataCodewords,
                        std::vector<uint8_t>& out) {
    const int countBits = (version >= 10) ? 16 : 8;
    const int capacityBits = dataCodewords * 8;
    std::vector<int> bits;
    bits.reserve(static_cast<std::size_t>(capacityBits));
    auto pushBits = [&](uint32_t value, int n) {
        for (int i = n - 1; i >= 0; --i) bits.push_back(static_cast<int>((value >> i) & 1u));
    };
    pushBits(0b0100, 4);  // byte 模式指示符
    pushBits(static_cast<uint32_t>(payload.size()), countBits);
    for (unsigned char ch : payload) pushBits(ch, 8);
    if (static_cast<int>(bits.size()) > capacityBits) return false;
    const int term = std::min(4, capacityBits - static_cast<int>(bits.size()));
    for (int i = 0; i < term; ++i) bits.push_back(0);
    while (bits.size() % 8 != 0) bits.push_back(0);

    out.clear();
    out.reserve(static_cast<std::size_t>(dataCodewords));
    for (std::size_t i = 0; i + 8 <= bits.size(); i += 8) {
        uint8_t b = 0;
        for (int k = 0; k < 8; ++k)
            b = static_cast<uint8_t>((b << 1) | bits[i + static_cast<std::size_t>(k)]);
        out.push_back(b);
    }
    const uint8_t pads[2] = {0xEC, 0x11};
    int p = 0;
    while (static_cast<int>(out.size()) < dataCodewords) {
        out.push_back(pads[p % 2]);
        ++p;
    }
    return true;
}

std::vector<uint8_t> interleave(const std::vector<uint8_t>& data, int ecPerBlock, int blocks) {
    const int base = static_cast<int>(data.size()) / blocks;
    const int extra = static_cast<int>(data.size()) % blocks;  // 末尾 extra 块多 1 码字
    std::vector<std::vector<uint8_t>> dataBlocks;
    std::vector<std::vector<uint8_t>> ecBlocks;
    std::size_t offset = 0;
    for (int b = 0; b < blocks; ++b) {
        const int len = base + (b >= blocks - extra ? 1 : 0);
        dataBlocks.emplace_back(data.begin() + static_cast<std::ptrdiff_t>(offset),
                                data.begin() + static_cast<std::ptrdiff_t>(offset) + len);
        offset += static_cast<std::size_t>(len);
        ecBlocks.push_back(ecBytes(dataBlocks.back(), ecPerBlock));
    }
    std::vector<uint8_t> stream;
    stream.reserve(data.size() +
                   static_cast<std::size_t>(ecPerBlock) * static_cast<std::size_t>(blocks));
    const int maxData = base + (extra > 0 ? 1 : 0);
    for (int i = 0; i < maxData; ++i)
        for (int b = 0; b < blocks; ++b)
            if (i < static_cast<int>(dataBlocks[static_cast<std::size_t>(b)].size()))
                stream.push_back(dataBlocks[static_cast<std::size_t>(b)][static_cast<std::size_t>(i)]);
    for (int i = 0; i < ecPerBlock; ++i)
        for (int b = 0; b < blocks; ++b)
            stream.push_back(ecBlocks[static_cast<std::size_t>(b)][static_cast<std::size_t>(i)]);
    return stream;
}

struct Grid {
    int size;
    std::vector<uint8_t> modules;  // 0/1
    std::vector<uint8_t> func;     // 1=功能模块（不被掩码/数据改写）

    explicit Grid(int s)
        : size(s),
          modules(static_cast<std::size_t>(s) * static_cast<std::size_t>(s), 0),
          func(static_cast<std::size_t>(s) * static_cast<std::size_t>(s), 0) {}

    bool isFunc(int r, int c) const {
        return func[static_cast<std::size_t>(r) * static_cast<std::size_t>(size) +
                    static_cast<std::size_t>(c)] != 0;
    }
    void place(int r, int c, int v) {
        modules[static_cast<std::size_t>(r) * static_cast<std::size_t>(size) +
                static_cast<std::size_t>(c)] = static_cast<uint8_t>(v);
    }
    void setFunc(int r, int c, int v) {
        if (r < 0 || c < 0 || r >= size || c >= size) return;
        place(r, c, v);
        func[static_cast<std::size_t>(r) * static_cast<std::size_t>(size) +
             static_cast<std::size_t>(c)] = 1;
    }
    int get(int r, int c) const {
        return modules[static_cast<std::size_t>(r) * static_cast<std::size_t>(size) +
                       static_cast<std::size_t>(c)];
    }
};

void drawFinders(Grid& g) {
    const int s = g.size;
    const int corners[3][2] = {{0, 0}, {0, s - 7}, {s - 7, 0}};
    for (const auto& corner : corners) {
        for (int dr = -1; dr <= 7; ++dr) {
            for (int dc = -1; dc <= 7; ++dc) {
                const bool inner = dr >= 0 && dr < 7 && dc >= 0 && dc < 7;
                const bool dark = inner && (dr == 0 || dr == 6 || dc == 0 || dc == 6 ||
                                            (dr >= 2 && dr <= 4 && dc >= 2 && dc <= 4));
                g.setFunc(corner[0] + dr, corner[1] + dc, dark ? 1 : 0);
            }
        }
    }
}

void drawTiming(Grid& g) {
    for (int i = 8; i <= g.size - 9; ++i) {
        const int v = (i % 2 == 0) ? 1 : 0;
        g.setFunc(6, i, v);
        g.setFunc(i, 6, v);
    }
}

void drawAlignment(Grid& g, int version) {
    const std::vector<int> pos = alignmentPositions(version);
    if (pos.size() < 2) return;
    const int last = pos.back();
    for (int r : pos) {
        for (int c : pos) {
            if ((r == 6 && c == 6) || (r == 6 && c == last) || (r == last && c == 6)) continue;
            for (int dr = -2; dr <= 2; ++dr) {
                for (int dc = -2; dc <= 2; ++dc) {
                    const int v = (std::max(std::abs(dr), std::abs(dc)) != 1) ? 1 : 0;
                    g.setFunc(r + dr, c + dc, v);
                }
            }
        }
    }
}

void reserveFormat(Grid& g) {
    const int s = g.size;
    for (int i = 0; i <= 8; ++i) {
        g.setFunc(8, i, g.get(8, i));
        g.setFunc(i, 8, g.get(i, 8));
    }
    for (int i = 0; i < 8; ++i) g.setFunc(8, s - 1 - i, g.get(8, s - 1 - i));
    for (int i = 0; i < 7; ++i) g.setFunc(s - 1 - i, 8, g.get(s - 1 - i, 8));
}

void drawVersionInfo(Grid& g, int version) {
    if (version < 7) return;
    int rem = version;
    for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    const int bits = (version << 12) | rem;
    for (int i = 0; i < 18; ++i) {
        const int bit = (bits >> i) & 1;
        const int a = g.size - 11 + i % 3;
        const int b = i / 3;
        g.setFunc(b, a, bit);  // 右上：行 i/3，列 size-11+i%3
        g.setFunc(a, b, bit);  // 左下：行 size-11+i%3，列 i/3
    }
}

void drawData(Grid& g, const std::vector<uint8_t>& stream) {
    const int s = g.size;
    const int total = static_cast<int>(stream.size()) * 8;
    auto bitAt = [&](int idx) {
        return static_cast<int>((stream[static_cast<std::size_t>(idx / 8)] >> (7 - idx % 8)) & 1);
    };
    int bi = 0;
    int col = s - 1;
    bool up = true;
    while (col > 0) {
        if (col == 6) --col;
        for (int step = 0; step < s; ++step) {
            const int r = up ? (s - 1 - step) : step;
            for (int k = 0; k < 2; ++k) {
                const int c = col - k;
                if (!g.isFunc(r, c) && bi < total) {
                    g.place(r, c, bitAt(bi));
                    ++bi;
                }
            }
        }
        up = !up;
        col -= 2;
    }
}

bool maskBit(int mask, int r, int c) {
    switch (mask) {
        case 0: return (r + c) % 2 == 0;
        case 1: return r % 2 == 0;
        case 2: return c % 3 == 0;
        case 3: return (r + c) % 3 == 0;
        case 4: return (r / 2 + c / 3) % 2 == 0;
        case 5: return (r * c) % 2 + (r * c) % 3 == 0;
        case 6: return ((r * c) % 2 + (r * c) % 3) % 2 == 0;
        case 7: return ((r + c) % 2 + (r * c) % 3) % 2 == 0;
        default: return false;
    }
}

void applyMask(Grid& g, int mask) {
    for (int r = 0; r < g.size; ++r)
        for (int c = 0; c < g.size; ++c)
            if (!g.isFunc(r, c) && maskBit(mask, r, c)) g.place(r, c, g.get(r, c) ^ 1);
}

int formatBits(int mask) {
    const int fmt = (0b01 << 3) | mask;  // L=01
    int v = fmt << 10;
    for (int i = 4; i >= 0; --i)
        if (v & (1 << (i + 10))) v ^= 0x537 << i;
    return ((fmt << 10) | v) ^ 0x5412;
}

void drawFormat(Grid& g, int mask) {
    const int s = g.size;
    const int bits = formatBits(mask);
    for (int i = 0; i < 15; ++i) {
        const int b = (bits >> i) & 1;
        if (i < 6) g.place(i, 8, b);
        else if (i == 6) g.place(7, 8, b);
        else if (i == 7) g.place(8, 8, b);
        else if (i == 8) g.place(8, 7, b);
        else g.place(8, 14 - i, b);
        if (i < 8) g.place(8, s - 1 - i, b);
        else g.place(s - 15 + i, 8, b);
    }
    g.place(s - 8, 8, 1);  // 固定暗模块
}

long penaltyScore(const Grid& g) {
    const int s = g.size;
    long score = 0;
    auto runPenalty = [](int run) { return run >= 5 ? 3 + (run - 5) : 0; };
    for (int r = 0; r < s; ++r) {
        int run = 1;
        for (int c = 1; c < s; ++c) {
            if (g.get(r, c) == g.get(r, c - 1)) ++run;
            else { score += runPenalty(run); run = 1; }
        }
        score += runPenalty(run);
    }
    for (int c = 0; c < s; ++c) {
        int run = 1;
        for (int r = 1; r < s; ++r) {
            if (g.get(r, c) == g.get(r - 1, c)) ++run;
            else { score += runPenalty(run); run = 1; }
        }
        score += runPenalty(run);
    }
    for (int r = 0; r < s - 1; ++r)
        for (int c = 0; c < s - 1; ++c) {
            const int v = g.get(r, c);
            if (v == g.get(r, c + 1) && v == g.get(r + 1, c) && v == g.get(r + 1, c + 1)) score += 3;
        }
    const int p1[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
    const int p2[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
    std::vector<int> seq(static_cast<std::size_t>(s));
    auto scanRule3 = [&](const std::vector<int>& line) {
        for (int i = 0; i + 11 <= s; ++i) {
            bool a = true;
            bool b = true;
            for (int k = 0; k < 11; ++k) {
                if (line[static_cast<std::size_t>(i + k)] != p1[k]) a = false;
                if (line[static_cast<std::size_t>(i + k)] != p2[k]) b = false;
            }
            if (a) score += 40;
            if (b) score += 40;
        }
    };
    for (int r = 0; r < s; ++r) {
        for (int c = 0; c < s; ++c) seq[static_cast<std::size_t>(c)] = g.get(r, c);
        scanRule3(seq);
    }
    for (int c = 0; c < s; ++c) {
        for (int r = 0; r < s; ++r) seq[static_cast<std::size_t>(r)] = g.get(r, c);
        scanRule3(seq);
    }
    long dark = 0;
    for (int r = 0; r < s; ++r)
        for (int c = 0; c < s; ++c) dark += g.get(r, c);
    const int total = s * s;
    const int darkPercent = static_cast<int>(dark * 100 / total);
    score += 10 * (std::abs(darkPercent - 50) / 5);
    return score;
}

QrMatrix toMatrix(Grid&& g, int version, int mask) {
    QrMatrix m;
    m.size = g.size;
    m.version = version;
    m.mask = mask;
    m.modules = std::move(g.modules);
    return m;
}

}  // namespace

bool encode(const std::string& payload, QrEcLevel level, QrMatrix& out,
            const QrEncodeOptions& options) {
    if (level != QrEcLevel::L) return false;
    if (options.mask < -1 || options.mask > 7) return false;

    int version = options.version;
    if (version == 0) {
        for (int v = 1; v <= kMaxVersion; ++v) {
            if (static_cast<int>(payload.size()) <= byteCapacity(v)) {
                version = v;
                break;
            }
        }
        if (version == 0) return false;  // 超出 V10-L 容量
    } else if (version < 1 || version > kMaxVersion) {
        return false;
    }
    if (static_cast<int>(payload.size()) > byteCapacity(version)) return false;

    const VersionEccL spec = kVersionEccL[static_cast<std::size_t>(version)];
    std::vector<uint8_t> dataCodewords;
    if (!buildDataCodewords(payload, version, spec.dataCodewords, dataCodewords)) return false;
    const std::vector<uint8_t> stream = interleave(dataCodewords, spec.ecPerBlock, spec.blocks);

    Grid base(17 + 4 * version);
    drawFinders(base);
    drawTiming(base);
    drawAlignment(base, version);
    reserveFormat(base);
    base.setFunc(base.size - 8, 8, 1);  // 固定暗模块，先占位
    drawVersionInfo(base, version);
    drawData(base, stream);

    if (options.mask >= 0) {
        Grid g = base;
        applyMask(g, options.mask);
        drawFormat(g, options.mask);
        out = toMatrix(std::move(g), version, options.mask);
        return true;
    }

    long best = std::numeric_limits<long>::max();
    int bestMask = 0;
    for (int m = 0; m < 8; ++m) {
        Grid g = base;
        applyMask(g, m);
        drawFormat(g, m);
        const long score = penaltyScore(g);
        if (score < best) {
            best = score;
            bestMask = m;
        }
    }
    Grid g = base;
    applyMask(g, bestMask);
    drawFormat(g, bestMask);
    out = toMatrix(std::move(g), version, bestMask);
    return true;
}

}  // namespace module::terminal::web
