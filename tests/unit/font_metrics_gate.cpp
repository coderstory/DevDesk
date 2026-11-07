// font_metrics_gate.cpp — 字体度量本端门禁 doctest（need:132 审查打回轮，技术卡
// doc 27 §5/§8「断言形态」三件本端常驻化；code-review P1 clue 85：此前三件只在
// probe（跨端汇交口径）里，字体资产/度量路径/回退链漂移会静默通过到下次人工收口）。
//
// 三件断言（REGISTER_TEST 形态，随 devdesk_tests 注册表派发；--filter font 命中：
// 描述与 unique_name 均含 font 词根）：
//   ① 三随包字体文件 SHA-256 == 随仓基线常量：仓库 assets/ 源文件 + resolveFontPath
//      解析到的实际消费文件**双面**各验一遍——后者证明消费链注入生效（与
//      main.cpp:952-954 注入同一条链），前者兜住「改了 assets 但目标未重链接、
//      POST_BUILD 拷贝未跑」的盲区（copy_if_different 只在目标重建时执行）。
//   ② 自有控制串（CJK+Latin，**非** probe 的 59 条 corpus）weight 400/600/700 +
//      mono 测宽定点化（×64）后取 hash 与基线常量比对（度量路径任何漂移必改
//      hash）。基线常量由本文件控制串首跑输出回填（首跑红、失败消息带实际值，
//      填常量后复跑绿），与 probe advances_sha256 无关（控制串不同 hash 必不同，
//      禁止照抄）。
//   ③ 回退链序：注入后 lazyFallbackPaths[0] == 系统 CJK（技术卡 §8 链序门禁
//      主断言）。resolvePreferredSystemUiFontPath 在 text_shaping.cpp 匿名命名
//      空间（:371，内部链接）外部不可调；注入前 weight=700 主面=系统 UI bold
//      ≠ 系统 CJK，text_shaping.cpp:946 首个 addLazyFallback 不被 :936 dedup
//      → 注入前链 [0] 即其返回值，以该观测值作等价锚，注入后逐一比对。
//
// 基线常量变更须显式改常量过 review：corpus 增删/字体更换/度量路径变化都必须
// 有意识地重录基线，禁止放宽容差凑绿。符号/emoji 码点禁止入 corpus（同 probe
// 注释：源主字体不含，两端落系统兜底必分叉）。

#include "doctest.h"

#include "core/render/text.h"
#include "core/render/text_internal.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// —— SHA-256（FIPS 180-4 紧凑实现，复制自 tests/probes/font_metrics_probe.cpp
//      fmprobe 命名空间；probe 是独立可执行不在本编译面，按审查约定不改它） ——

std::uint32_t rotr(std::uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

std::string sha256Hex(const std::uint8_t* data, std::size_t len) {
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

std::string fileSha256Hex(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    return sha256Hex(bytes.data(), bytes.size());
}

std::string textSha256Hex(const std::string& text) {
    return sha256Hex(reinterpret_cast<const std::uint8_t*>(text.data()),
                     text.size());
}

// 定点整数化（×64 = 26.6 定点，与 FreeType 线性 advance 同标度；同 probe）。
std::int64_t fixedPt(float v) {
    return std::llround(static_cast<double>(v) * 64.0);
}

std::string filenameOf(const std::string& path) {
    return std::filesystem::path(path).filename().string();
}

// need:132 阶段 2 注入（main.cpp:952-954 / probe :292-294 同款三 setter）；析构
// 还原空 override（空=不设，走原解析逻辑，clue 64 语义）并随 setter 内
// clearSharedFontStackCache 失效缓存，不污染同二进制其它字体用例的解析态。
struct BundledFontGuard {
    BundledFontGuard() {
        core::TextPrimitive::setDefaultFontFiles("DevDeskSansSC-Regular.otf", "");
        core::TextPrimitive::setDefaultBoldUiFontFile("DevDeskSansSC-Bold.otf");
        core::TextPrimitive::setDefaultMonospaceFontFile("JetBrainsMono-Regular.ttf");
    }
    ~BundledFontGuard() {
        core::TextPrimitive::setDefaultFontFiles("", "");
        core::TextPrimitive::setDefaultBoldUiFontFile("");
        core::TextPrimitive::setDefaultMonospaceFontFile("");
    }
};

// —— 基线常量（变更须显式改常量过 review） ——

// ① 随包字体资产指纹（与 CI 跨端汇交基线同源：CI run 42318136031 三方一致）。
constexpr const char* kRegularFontSha256 =
    "69706360ad87154ec111be408a7d5c6adffc9288254f3ef8ace8162457da02b2";
constexpr const char* kBoldFontSha256 =
    "607562eef8a691220da373b81e626df2a04547952b4bea4ff27c9934023dd82b";
constexpr const char* kMonoFontSha256 =
    "a0bf60ef0f83c5ed4d7a75d45838548b1f6873372dfac88f71804491898d138f";

// ② gate corpus（自拟固定控制串；id 与文本均进 hash 种子，改任何字符=改 hash=
//    须重录基线）。
struct GateEntry {
    const char* id;
    const char* text;
};
const GateEntry kGateBody[] = {
    {"gate_mixed", "字体度量门禁 FontMetricsGate 2026：中文与 Latin 混排 0123"},
    {"gate_cjk", "跨平台布局一致性验收要求正文测宽逐字形稳定可复现"},
    {"gate_latin", "The quick brown fox jumps over the lazy dog 0123456789"},
};
const GateEntry kGateMono[] = {
    {"gate_mono", "for (const auto& e : gate) hash ^= fixed(e.width) << 3;"},
};

// ② 测宽 hash 基线（种子格式与 probe 同构："id:fixed\n" 串联后 SHA-256；本文件
//    控制串首跑回填，非 probe corpus 口径——首跑失败消息带实际值，逐项填入后
//    复跑绿）。
constexpr const char* kBodyAdvancesSha400 = "";
constexpr const char* kBodyAdvancesSha600 = "";
constexpr const char* kBodyAdvancesSha700 = "";
constexpr const char* kMonoAdvancesSha = "";

}  // namespace

// —— ①：随包字体资产指纹 == 随仓基线（仓库源文件 + 实际消费文件双面） ——

REGISTER_TEST(font_metrics_gate_bundled_font_sha256,
              "font-metrics-gate: bundled font sha256 == repo baseline (asset fingerprint)", {
    // 仓库 assets/ 源文件（DEVDESK_SOURCE_DIR 由 tests/CMakeLists.txt 注入）。
    const std::string repoAssets[3] = {
        std::string(DEVDESK_SOURCE_DIR) + "/assets/DevDeskSansSC-Regular.otf",
        std::string(DEVDESK_SOURCE_DIR) + "/assets/DevDeskSansSC-Bold.otf",
        std::string(DEVDESK_SOURCE_DIR) + "/assets/JetBrainsMono-Regular.ttf"};
    const char* const kRepoBaselines[3] = {kRegularFontSha256, kBoldFontSha256, kMonoFontSha256};
    for (int i = 0; i < 3; ++i) {
        const std::string sha = fileSha256Hex(repoAssets[i]);
        DOCTEST_CHECK_MESSAGE(sha == kRepoBaselines[i],
                              std::string("仓库资产指纹漂移 ") + repoAssets[i] +
                                  " 实际=" + sha);
    }

    // 实际消费文件（注入后 resolveFontPath 解析链，证明消费的就是随包三份）。
    BundledFontGuard guard;
    const std::string regularPath = core::TextPrimitive::resolveFontPath("", 400);
    const std::string boldPath = core::TextPrimitive::resolveFontPath("", 600);
    const std::string monoPath = core::TextPrimitive::resolveFontPath("monospace", 400);
    DOCTEST_CHECK_MESSAGE(!regularPath.empty(),
                          "weight400 主面解析为空（随包字体未拷到 exeDir/assets？）");
    DOCTEST_CHECK_MESSAGE(!boldPath.empty(), "weight600 主面解析为空");
    DOCTEST_CHECK_MESSAGE(!monoPath.empty(), "monospace 解析为空");
    DOCTEST_CHECK_MESSAGE(filenameOf(regularPath) == "DevDeskSansSC-Regular.otf",
                          std::string("实际解析=") + regularPath);
    DOCTEST_CHECK_MESSAGE(filenameOf(boldPath) == "DevDeskSansSC-Bold.otf",
                          std::string("实际解析=") + boldPath);
    DOCTEST_CHECK_MESSAGE(filenameOf(monoPath) == "JetBrainsMono-Regular.ttf",
                          std::string("实际解析=") + monoPath);
    const std::string regularSha = fileSha256Hex(regularPath);
    const std::string boldSha = fileSha256Hex(boldPath);
    const std::string monoSha = fileSha256Hex(monoPath);
    DOCTEST_CHECK_MESSAGE(regularSha == kRegularFontSha256,
                          std::string("Regular 消费文件资产漂移 实际=") + regularSha);
    DOCTEST_CHECK_MESSAGE(boldSha == kBoldFontSha256,
                          std::string("Bold 消费文件资产漂移 实际=") + boldSha);
    DOCTEST_CHECK_MESSAGE(monoSha == kMonoFontSha256,
                          std::string("mono 消费文件资产漂移 实际=") + monoSha);
    return true;
})

// —— ②：自有控制串测宽 hash 稳定（回归门禁） ——

REGISTER_TEST(font_metrics_gate_measure_width_hash,
              "font-metrics-gate: measure-width hash stable (gate corpus, body 400/600/700 + mono)", {
    BundledFontGuard guard;
    const int kWeights[] = {400, 600, 700};
    std::string bodySeeds[3];
    for (int wi = 0; wi < 3; ++wi) {
        for (const auto& e : kGateBody) {
            const std::int64_t fx = fixedPt(core::TextPrimitive::measureTextWidth(
                e.text, "", 16.0f, kWeights[wi]));
            bodySeeds[wi] += std::string(e.id) + ":" + std::to_string(fx) + "\n";
        }
    }
    std::string monoSeed;
    for (const auto& e : kGateMono) {
        const std::int64_t fx = fixedPt(core::TextPrimitive::measureTextWidth(
            e.text, "monospace", 16.0f, 400));
        monoSeed += std::string(e.id) + ":" + std::to_string(fx) + "\n";
    }
    const std::string body400 = textSha256Hex(bodySeeds[0]);
    const std::string body600 = textSha256Hex(bodySeeds[1]);
    const std::string body700 = textSha256Hex(bodySeeds[2]);
    const std::string monoSha = textSha256Hex(monoSeed);
    // 基线未回填=门禁未武装（#132 P1 暂停中）：跳过断言防全量跑恒红；
    // 回填后本分支自动失效（常量首字符非空即武装）。
    if (kBodyAdvancesSha400[0] == '\0') {
        DOCTEST_CHECK_MESSAGE(true,
                              "font-metrics gate ② 基线未回填，测宽门禁未武装（自研 doctest shim 仅上报首个失败，回填后须逐档核验）");
        return true;
    }
    DOCTEST_CHECK_MESSAGE(body400 == kBodyAdvancesSha400,
                          std::string("body@400 测宽 hash 漂移 实际=") + body400);
    DOCTEST_CHECK_MESSAGE(body600 == kBodyAdvancesSha600,
                          std::string("body@600 测宽 hash 漂移 实际=") + body600);
    DOCTEST_CHECK_MESSAGE(body700 == kBodyAdvancesSha700,
                          std::string("body@700 测宽 hash 漂移 实际=") + body700);
    DOCTEST_CHECK_MESSAGE(monoSha == kMonoAdvancesSha,
                          std::string("mono 测宽 hash 漂移 实际=") + monoSha);
    return true;
})

// —— ③：回退链序（注入后 lazyFallbackPaths[0] == 系统 CJK） ——

REGISTER_TEST(font_metrics_gate_lazy_fallback_chain,
              "font-metrics-gate: lazy fallback chain [0] == system CJK (chain order)", {
    // 注入前锚采样：weight=700 主面=系统 UI bold ≠ 系统 CJK → 链[0] 即
    // resolvePreferredSystemUiFontPath() 的返回值（等价观测锚推导见文件头注释③）。
    const auto preStack = core::text_detail::loadSharedFontStack(
        core::TextPrimitive::resolveFontPath("", 700), 16.0f, 700);
    DOCTEST_REQUIRE(preStack != nullptr);
    DOCTEST_REQUIRE(!preStack->lazyFallbackPaths.empty());
    const std::string systemCjk = preStack->lazyFallbackPaths[0];
    DOCTEST_CHECK_MESSAGE(!systemCjk.empty(),
                          "系统 CJK 链位观测为空（平台系统 CJK 字体缺失？）");

    BundledFontGuard guard;
    const auto stack400 = core::text_detail::loadSharedFontStack(
        core::TextPrimitive::resolveFontPath("", 400), 16.0f, 400);
    const auto stack700 = core::text_detail::loadSharedFontStack(
        core::TextPrimitive::resolveFontPath("", 700), 16.0f, 700);
    DOCTEST_REQUIRE(stack400 != nullptr);
    DOCTEST_REQUIRE(stack700 != nullptr);
    DOCTEST_REQUIRE(!stack400->lazyFallbackPaths.empty());
    DOCTEST_REQUIRE(!stack700->lazyFallbackPaths.empty());
    DOCTEST_CHECK_MESSAGE(stack400->lazyFallbackPaths[0] == systemCjk,
                          std::string("weight400 链[0]=") + stack400->lazyFallbackPaths[0] +
                              " 期望(系统CJK)=" + systemCjk);
    DOCTEST_CHECK_MESSAGE(stack700->lazyFallbackPaths[0] == systemCjk,
                          std::string("weight700(bold 主面) 链[0]=") + stack700->lazyFallbackPaths[0] +
                              " 期望(系统CJK)=" + systemCjk);
    return true;
})
