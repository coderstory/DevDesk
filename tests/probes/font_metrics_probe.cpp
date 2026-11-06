// font_metrics_probe.cpp — 字体度量门禁探针（need:132 技术卡 v4 §5，F-06）
//
// 运行：font_metrics_probe [--out <json 路径>]（其余 probe 参数被忽略；本 probe
// 纯度量不渲染帧，不支持 --screenshot）。缺省 --out = cwd 相对
// evidence/font-metrics/font_metrics_<os>.json（<os> 由 CMake 注入
// DEVDESK_PROBE_OS，源码零平台宏）。
//
// 门禁职责（技术卡 v4 §5）：
//   1. 100% DPI 基准：度量域显式 1.0（static_assert 钉死）。EUI measure/shaping
//      是分辨率无关的纯 FreeType 路径（fontSize 即像素，不消费窗口/DPI 因子），
//      窗口 contentScale（Retina=2.0 / Windows 125%=1.25 属正常显示环境）只记录
//      进 JSON 供收口对照——若度量路径未来混入 DPI 因子，两端 advances hash 必
//      分叉，门禁在收口比对处报红。
//   2. F-01 注入生效实证：注入前先采一份 resolveFontPath + 控制串测宽（系统字体
//      态），注入随包三 setter 后再采，断言解析路径从系统字体切到随包文件且控制
//      串宽度改变——证明主面真的是随包字体而非系统字体（resolveFontPath 是 EUI
//      公开可观测接口，text.h:115）。
//   3. corpus 固定常量：正文段（可打印 ASCII 全段 + GB2312 分层抽样 256 字 +
//      chips 工具名高频串 + MCP 代码样例）× weight 400/600/700 三档；mono 段
//      JetBrains Mono 样例。
//      **符号段显式排除**：✅✔✨ℹ⋮▾▸✎✕✗ 等 emoji/dingbat 码点源主字体
//      （Noto Sans CJK 子集）本就缺失（POC clue 54：源字体不含，非子集丢失），
//      两端落系统 emoji 兜底、advance 必然分叉（技术卡 §1.4 方案 b 已接受差异）。
//      **禁止把这类码点加进 corpus**，否则跨端 hash 比对恒红。
//   4. 度量双断言：measureTextWidth 与 shapeTextWithFontStack 逐 glyph advance
//      求和对照（同源 shapeTextWithFontStack 的两条消费路径互证，防未来另写估算
//      器破坏同源性）；定点整数化（×64，26.6 定点）后取序。
//
// JSON 契约（汇交比对=qa 收口）：font_sha256 / resolved_fonts / dpi_scale /
// f01_injection_proof / advances_sha256{400,600,700,mono} / entries 逐串明细。
// sha256 为 probe 本地 FIPS 180-4 紧凑实现（仓内无通用 sha256 工具，不为此引依赖）。

#include "probes/probe_harness.h"

#include "core/render/text.h"
#include "core/render/text_internal.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fmprobe {

// ==================== SHA-256（FIPS 180-4，probe 本地紧凑实现） ====================

inline std::uint32_t rotr(std::uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

inline std::string sha256Hex(const std::uint8_t* data, std::size_t len) {
    static const std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<std::uint8_t> msg(data, data + len);
    const std::uint64_t bitLen = static_cast<std::uint64_t>(len) * 8u;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0x00);
    for (int i = 7; i >= 0; --i)
        msg.push_back(static_cast<std::uint8_t>(bitLen >> (i * 8)));
    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<std::uint32_t>(msg[off + 4 * i]) << 24) |
                   (static_cast<std::uint32_t>(msg[off + 4 * i + 1]) << 16) |
                   (static_cast<std::uint32_t>(msg[off + 4 * i + 2]) << 8) |
                   static_cast<std::uint32_t>(msg[off + 4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 =
                rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 =
                rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::string out;
    out.reserve(64);
    static const char* hex = "0123456789abcdef";
    for (int i = 0; i < 8; ++i)
        for (int j = 3; j >= 0; --j) {
            out.push_back(hex[(h[i] >> (j * 8 + 4)) & 0xF]);
            out.push_back(hex[(h[i] >> (j * 8)) & 0xF]);
        }
    return out;
}

inline std::string fileSha256Hex(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    return sha256Hex(bytes.data(), bytes.size());
}

inline std::string sha256HexOfText(const std::string& text) {
    return sha256Hex(reinterpret_cast<const std::uint8_t*>(text.data()),
                     text.size());
}

// ==================== corpus 固定常量 ====================

// 正文段条目（family 缺省=主面，按 weight 分派 Regular/Bold）。id 进 hash 序列
// 与 JSON 明细，文本逐字面常量——改任何一个字符都会改 advances_sha256（这正是
// 门禁意图：corpus 变更必须显式过 review，两端同步）。
struct CorpusEntry {
    const char* id;
    const char* text;
};

// GB2312 分层抽样 256 字（2026-11-06 由全 6763 字池每 1/256 档等距取样生成，
// 覆盖高频/中频/低频全部 GB2312 区段；脚本见 need:132 阶段 3 简报）。
const char* const kCjkSample256 =
    R"FONT(啊盎爸梆杯鼻扁秉哺灿岔唱衬侈躇吹葱搓掸盗帝钓恫锻剁罚肪坟敷腐甘镐羹够拐桂含浩横蝴桓灰豁缉济嫁鉴降较介睛纠聚觉坎刻挎溃澜镭莉廉辽零窿潞抡嘛毛闷绵名姆囊撵浓琶泡砒拼莆崎钎抢勤酋缺人孺散山勺神十嗜抒数撕嗽祟太膛锑铁投脱碗唯温屋西铣衔翔啸锌匈恤寻阉彦尧壹抑荫迎尤逾育怨杂灶摘张者争侄中蛛拽茁总做馗匾仨侃倜俎嬴诘谙陉郐劾圹埘懿芴苕荩萁葚蒡薤奚捭撖吆哒唢喃嗥噜帑岱嵋犰猕饷廨怿愦阄汩浈淅溱漉灞逅彐姊娼孀骝绠缛玢琮韬柙桀楂榕猷辁攴暄觌毹肼胼膑斐煳扈恁砜碲眙畎钏铄铫锛镏锺瓠鹑疴瘕窆裼耦颡蚺蜻蝙蠖笮箅臾袈糗赧醭跬蹊訾鼍鲑鲶靼魇麇)FONT";

// 符号段显式排除（技术卡 v4 §1.4 方案 b）：U+2705/2714/25B8/2728/2139/22EE/
// 25BE/270E/2715/2717 等 18 码点在源主字体（Noto Sans CJK 子集）cmap 中本就
// 缺失（源字体不含，非子集丢失——POC clue 54），运行时两端落系统 emoji/符号
// 兜底，advance 必然分叉=已接受差异。**不要往 corpus 里加这些码点**。

// chips 工具名高频串（agent UI chips 度量面，need:132 触发 bug 的同类面；
// 与 test_agent_tools.cpp:800 注册清单同源）。
const CorpusEntry kChips[] = {
    {"chip_fs_glob", "fs.glob"},       {"chip_fs_grep", "fs.grep"},
    {"chip_fs_read", "fs.read"},       {"chip_fs_write", "fs.write"},
    {"chip_fs_edit", "fs.edit"},       {"chip_bash_run", "bash.run"},
    {"chip_git_clone", "git.clone"},   {"chip_git_status", "git.status"},
    {"chip_git_log", "git.log"},       {"chip_git_diff", "git.diff"},
    {"chip_git_branch", "git.branch"}, {"chip_git_checkout", "git.checkout"},
    {"chip_git_pull", "git.pull"},     {"chip_git_commit", "git.commit"},
};

// MCP 代码样例（MCP 页代码块宽度面）。
const CorpusEntry kMcp[] = {
    {"mcp_jsonrpc", R"({"jsonrpc":"2.0","id":7,"method":"tools/call"})"},
    {"mcp_tool_call",
     R"({"name":"fs.grep","params":{"pattern":"need","mode":"content","lineNumbers":true}})"},
    {"mcp_shell", "grep -rn \"TODO\" src/ --include=*.cpp | wc -l"},
};

// mono 段（JetBrains Mono，家族 "monospace"；CJK 恒走回退=已接受差异 §1.2，
// mono corpus 纯 ASCII）。
const CorpusEntry kMono[] = {
    {"mono_code", "for (const auto& [k, v] : metrics) hash ^= fx(v) << 3;"},
    {"mono_box_ascii", "+--[ glyph advance ]--+ 0123456789 ABCDEF"},
};

std::string asciiPrintable() {
    std::string s;
    for (int c = 0x20; c <= 0x7E; ++c) s.push_back(static_cast<char>(c));
    return s;
}

// ==================== 度量与 JSON 辅助 ====================

// 定点整数化（×64 = 26.6 定点，与 FreeType 线性 advance 同标度）。
std::int64_t fixedPt(float v) { return std::llround(static_cast<double>(v) * 64.0); }

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

struct MeasureResult {
    std::int64_t measureFixed = 0;
    std::int64_t glyphSumFixed = 0;
};

// 双断言度量：measureTextWidth（消费路径 A）与 shapeTextWithFontStack 逐 glyph
// advance 求和（消费路径 B）同源互证；容差 1px（定点 64）吸收路径间浮点求和差。
// family/weight 必须与 holder 的构造字体一致（A、B 走同一 face 才是互证）。
MeasureResult measureDual(const std::string& text, const std::string& family,
                          core::text_detail::FontInfoHolder& holder, int weight) {
    MeasureResult r;
    r.measureFixed = fixedPt(core::TextPrimitive::measureTextWidth(text, family, 16.0f, weight));
    const std::vector<core::TextPrimitive::ShapedGlyph> glyphs =
        core::text_detail::shapeTextWithFontStack(holder, text, 16.0f);
    double sum = 0.0;
    for (const auto& g : glyphs) sum += g.advance;
    r.glyphSumFixed = fixedPt(static_cast<float>(sum));
    return r;
}

}  // namespace fmprobe

// ==================== main ====================

int main(int argc, char* argv[]) {
    // --out（probe::parseProbeArgs 不识别则忽略，这里单独取）
    std::string outPath;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--out" && i + 1 < argc) outPath = argv[++i];
    }
    if (outPath.empty()) {
        outPath = std::string("evidence/font-metrics/font_metrics_") +
                  DEVDESK_PROBE_OS + ".json";
    }

    // 100% DPI 基准（技术卡 v4 §5）：度量域显式 1.0，编译期钉死。
    constexpr float kMetricDpiScale = 1.0f;
    static_assert(kMetricDpiScale == 1.0f,
                  "font metrics must be collected at 100% DPI baseline");

    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("font_metrics_probe", 480, 320);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }
    float contentScaleX = 1.0f, contentScaleY = 1.0f;
    glfwGetWindowContentScale(static_cast<GLFWwindow*>(window), &contentScaleX,
                              &contentScaleY);
    const float windowContentScale = contentScaleX > 0.0f ? contentScaleX : 1.0f;

    bool passed = true;
    auto fail = [&passed](const std::string& msg) {
        std::cerr << "font_metrics_probe: FAIL — " << msg << "\n";
        passed = false;
    };

    // ---- F-01 实证第 1 步：注入前（系统字体态）采样 ----
    const std::string pathBefore =
        core::TextPrimitive::resolveFontPath("", 400);
    const std::string kControl = "统一字体测宽宽度一致性 0123";
    const std::int64_t controlBefore = fmprobe::fixedPt(
        core::TextPrimitive::measureTextWidth(kControl, "", 16.0f, 400));
    std::cout << "font_metrics_probe: 注入前主面=" << pathBefore
              << " 控制串宽(fixed)=" << controlBefore << "\n";

    // ---- 注入随包三 setter（与 main.cpp:952-954 同款） ----
    core::TextPrimitive::setDefaultFontFiles("DevDeskSansSC-Regular.otf", "");
    core::TextPrimitive::setDefaultBoldUiFontFile("DevDeskSansSC-Bold.otf");
    core::TextPrimitive::setDefaultMonospaceFontFile("JetBrainsMono-Regular.ttf");

    // ---- F-01 实证第 2 步：注入后断言解析路径与控制串宽度均改变 ----
    const std::string regularPath = core::TextPrimitive::resolveFontPath("", 400);
    const std::string boldPath = core::TextPrimitive::resolveFontPath("", 600);
    const std::string monoPath = core::TextPrimitive::resolveFontPath("monospace", 400);
    auto filenameOf = [](const std::string& p) {
        return std::filesystem::path(p).filename().string();
    };
    if (regularPath.empty() || filenameOf(regularPath) != "DevDeskSansSC-Regular.otf") {
        fail("主面 weight=400 未解析到 DevDeskSansSC-Regular.otf，实际=" + regularPath);
    }
    if (boldPath.empty() || filenameOf(boldPath) != "DevDeskSansSC-Bold.otf") {
        fail("主面 weight=600 未解析到 DevDeskSansSC-Bold.otf，实际=" + boldPath);
    }
    if (monoPath.empty() || filenameOf(monoPath) != "JetBrainsMono-Regular.ttf") {
        fail("monospace 未解析到 JetBrainsMono-Regular.ttf，实际=" + monoPath);
    }
    if (!pathBefore.empty() && pathBefore == regularPath) {
        fail("注入前后主面解析路径相同，注入未生效：" + regularPath);
    }
    const std::int64_t controlAfter =
        fmprobe::fixedPt(core::TextPrimitive::measureTextWidth(kControl, "", 16.0f, 400));
    if (controlAfter == controlBefore) {
        fail("注入前后控制串测宽相同，主面字体未切换（before=" +
             std::to_string(controlBefore) + " after=" + std::to_string(controlAfter) + "）");
    }
    std::cout << "font_metrics_probe: 注入后主面=" << regularPath
              << " bold=" << boldPath << " mono=" << monoPath
              << " 控制串宽(fixed)=" << controlAfter << "（注入前 "
              << controlBefore << "，已改变）\n";

    // ---- font_sha256（解析到的实际文件——证明消费的就是随包那三份） ----
    const std::string regularSha = fmprobe::fileSha256Hex(regularPath);
    const std::string boldSha = fmprobe::fileSha256Hex(boldPath);
    const std::string monoSha = fmprobe::fileSha256Hex(monoPath);
    if (regularSha.empty() || boldSha.empty() || monoSha.empty()) {
        fail("字体文件读取失败（sha256 为空）：" + regularPath + " / " + boldPath +
             " / " + monoPath);
    }

    // ---- corpus 采集：正文段 × weight 400/600/700 ----
    std::vector<fmprobe::CorpusEntry> body;
    static const std::string kAscii = fmprobe::asciiPrintable();
    body.push_back({"ascii_printable", kAscii.c_str()});
    body.push_back({"cjk_sample_256", fmprobe::kCjkSample256});
    for (const auto& e : fmprobe::kChips) body.push_back(e);
    for (const auto& e : fmprobe::kMcp) body.push_back(e);

    const int kWeights[] = {400, 600, 700};
    std::vector<fmprobe::MeasureResult> bodyByWeight[3];
    std::string hashSeedByWeight[3];
    for (int wi = 0; wi < 3; ++wi) {
        const std::string fontPath = core::TextPrimitive::resolveFontPath("", kWeights[wi]);
        auto holder = core::text_detail::loadSharedFontStack(fontPath, 16.0f, kWeights[wi]);
        if (!holder) {
            fail("font stack 加载失败 weight=" + std::to_string(kWeights[wi]) +
                 " path=" + fontPath);
            continue;
        }
        for (const auto& e : body) {
            const fmprobe::MeasureResult r = fmprobe::measureDual(e.text, "", *holder, kWeights[wi]);
            // 双断言：两消费路径定点差 >1px 记红
            const std::int64_t diff = r.measureFixed - r.glyphSumFixed;
            const std::int64_t absDiff = diff < 0 ? -diff : diff;
            if (absDiff > 64) {
                fail("双断言超差（>1px）weight=" + std::to_string(kWeights[wi]) +
                     " id=" + e.id + " measure=" + std::to_string(r.measureFixed) +
                     " glyphSum=" + std::to_string(r.glyphSumFixed));
            }
            bodyByWeight[wi].push_back(r);
            hashSeedByWeight[wi] += std::string(e.id) + ":" +
                                    std::to_string(r.measureFixed) + "\n";
        }
    }

    // ---- corpus 采集：mono 段 ----
    std::vector<fmprobe::MeasureResult> monoResults;
    std::string monoHashSeed;
    {
        auto holder = core::text_detail::loadSharedFontStack(monoPath, 16.0f, 400);
        if (!holder) {
            fail("mono font stack 加载失败 path=" + monoPath);
        } else {
            for (const auto& e : fmprobe::kMono) {
                const fmprobe::MeasureResult r = fmprobe::measureDual(e.text, "monospace", *holder, 400);
                const std::int64_t diff = r.measureFixed - r.glyphSumFixed;
                const std::int64_t absDiff = diff < 0 ? -diff : diff;
                if (absDiff > 64) {
                    fail(std::string("mono 双断言超差 id=") + e.id +
                         " measure=" + std::to_string(r.measureFixed) +
                         " glyphSum=" + std::to_string(r.glyphSumFixed));
                }
                monoResults.push_back(r);
                monoHashSeed += std::string(e.id) + ":" +
                                std::to_string(r.measureFixed) + "\n";
            }
        }
    }

    // ---- 写 JSON ----
    {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(outPath).parent_path(), ec);
        std::ostringstream o;
        o << "{\n";
        o << "  \"os\": \"" << DEVDESK_PROBE_OS << "\",\n";
        o << "  \"font_size\": 16,\n";
        o << "  \"font_sha256\": {\"regular\": \"" << regularSha
          << "\", \"bold\": \"" << boldSha << "\", \"mono\": \"" << monoSha << "\"},\n";
        o << "  \"resolved_fonts\": {\"regular\": \""
          << fmprobe::jsonEscape(regularPath) << "\", \"bold\": \""
          << fmprobe::jsonEscape(boldPath) << "\", \"mono\": \""
          << fmprobe::jsonEscape(monoPath) << "\"},\n";
        o << "  \"dpi_scale\": {\"window_content_scale\": " << windowContentScale
          << ", \"metric_scale\": 1.0},\n";
        o << "  \"f01_injection_proof\": {\"path_before\": \""
          << fmprobe::jsonEscape(pathBefore) << "\", \"path_after\": \""
          << fmprobe::jsonEscape(regularPath)
          << "\", \"control_text\": \"" << fmprobe::jsonEscape(kControl)
          << "\", \"control_width_fixed_before\": " << controlBefore
          << ", \"control_width_fixed_after\": " << controlAfter << "},\n";
        o << "  \"advances_sha256\": {\"400\": \""
          << fmprobe::sha256HexOfText(hashSeedByWeight[0]) << "\", \"600\": \""
          << fmprobe::sha256HexOfText(hashSeedByWeight[1]) << "\", \"700\": \""
          << fmprobe::sha256HexOfText(hashSeedByWeight[2]) << "\", \"mono\": \""
          << fmprobe::sha256HexOfText(monoHashSeed) << "\"},\n";
        o << "  \"entries\": [\n";
        std::size_t total = 0;
        for (int wi = 0; wi < 3; ++wi) {
            for (std::size_t i = 0; i < body.size(); ++i, ++total) {
                o << "    {\"id\": \"" << body[i].id << "@" << kWeights[wi]
                  << "\", \"family\": \"\", \"weight\": " << kWeights[wi]
                  << ", \"text\": \"" << fmprobe::jsonEscape(body[i].text)
                  << "\", \"measure_fixed\": " << bodyByWeight[wi][i].measureFixed
                  << ", \"glyph_sum_fixed\": " << bodyByWeight[wi][i].glyphSumFixed
                  << "},\n";
            }
        }
        constexpr std::size_t kMonoCount =
            sizeof(fmprobe::kMono) / sizeof(fmprobe::kMono[0]);
        for (std::size_t i = 0; i < kMonoCount && i < monoResults.size(); ++i, ++total) {
            o << "    {\"id\": \"" << fmprobe::kMono[i].id
              << "@mono\", \"family\": \"monospace\", \"weight\": 400, \"text\": \""
              << fmprobe::jsonEscape(fmprobe::kMono[i].text)
              << "\", \"measure_fixed\": " << monoResults[i].measureFixed
              << ", \"glyph_sum_fixed\": " << monoResults[i].glyphSumFixed
              << "},\n";
        }
        o.seekp(-2, std::ios_base::cur);  // 去掉末尾「},\n」的逗号
        o << "\n  ],\n";
        o << "  \"corpus_count\": " << total << "\n";
        o << "}\n";

        std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            fail("JSON 写入失败 path=" + outPath);
        } else {
            out << o.str();
            std::cout << "font_metrics_probe: JSON 已写 " << outPath << "\n";
        }
    }

    core::window::destroyWindow(window);
    probe::terminateWindowBackend();

    if (!passed) {
        std::cerr << "font_metrics_probe: FAIL\n";
        return 1;
    }
    std::cout << "font_metrics_probe: PASS（advances_sha256@400="
              << fmprobe::sha256HexOfText(hashSeedByWeight[0])
              << " @600=" << fmprobe::sha256HexOfText(hashSeedByWeight[1])
              << " @700=" << fmprobe::sha256HexOfText(hashSeedByWeight[2])
              << " mono=" << fmprobe::sha256HexOfText(monoHashSeed) << "）\n";
    return 0;
}
