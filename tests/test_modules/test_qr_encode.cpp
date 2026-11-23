#include "doctest.h"
#include "module_terminal/web/qr/qr_encode.h"

#include <cstddef>
#include <string>

namespace {

using module::terminal::web::QrEcLevel;
using module::terminal::web::QrEncodeOptions;
using module::terminal::web::QrMatrix;
using module::terminal::web::encode;

struct GoldenCase {
    const char* payload;
    int version;
    int mask;
    int size;
    const char* hex;  // 行优先，bit0=(0,0)，每字节 MSB 先，末字节低位补 0
};

// golden 由 evidence/need490-qr-poc/qr.py 生成（该参考实现的输出已被 Apple Vision
// 逐字节解码），逐位内嵌；运行时零 Python 依赖。V4/V5 全 8 掩码。
const GoldenCase kGolden[] = {
    {"http://192.168.1.100:8080/#b=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", 4, 0, 33, "fe49e33fc11fd8d06ebae4ebb74e3c25dba2185aec123afd07faaaaafe01dcbd00efb619e27edb3e1b62f10aa828a2afe1a3a8e1820deef3e396bdf39b36111e8afada561931ecc93e1963e8bba970868ce0ad72f1835b4bdfe1a67ee802fecbdeaaaabe5319fc0048fe47ffa1226bf0522ef18baa918fcdd2efee6aeae91127051d99bafe9208b180"},
    {"http://192.168.1.100:8080/#b=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", 4, 1, 33, "fe9cb63fc17572506e8fb1abb7449685dbaf4d0aec14905507faaaaafe01761700e6e34cf9d47194b1f7a45ffd6208054b06fdb4d75f4459493fe8a6ce62bbb420508f034c64c66394b3f6bdeefc3a2c264a0827a4d609e1754b0f2bbd57aa61740000eb064cf9006254c57f94772ab05884512ba7c4df9dd04544c2ebbc447305b73310fec75de480"},
    {"http://192.168.1.100:8080/#b=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", 4, 2, 33, "fe2a6d3fc151e0106e82076bb75db215dba420baec16d97107faaaaafe00e45e00fbd597d51c5506f8ecc9e926100121d94026d96183c6106dae5e7da3d59b266974e2b597090e4706faedd05827482502d84efcc960d5633c6f9e9d663a1d41e6492486b097fc8046c6c47fb9c1ebd041a0d16baca96fc5d60c6052eb6729c505257a34fef1868900"},
    {"http://192.168.1.100:8080/#b=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", 4, 3, 33, "feaa6d3fc13c56d06eb96acbb75db215dba99662ec11b4c507faaaaafe00528500f2b8214e9c5506f8c1ff324bab2c97022026d9618660cb00193bcb78b99b266974566efabfe2f1dd976dd058275ebe6f6e936a120d65633c6f9c4e0b8cc6f33d249286b097fc806b70473f82ac6b1041a0d16ba11fbfadd561d68aeb6729c50593a159fe9c305200"},
    {"http://192.168.1.100:8080/#b=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", 4, 4, 33, "feed713fc1726e106eac3f8bb75aae65dba0517aec14e19107faaaaafe00959900ce128b97a4b688c00fc7d1c581863da8873aa8a69848288e206e9e2dec8357ae6893728b78b6a488c20ede60c4d9a21ea989e0b8a7ceed048c10ad85b42459978e38f7778bfd006548c4bf97f92a5056bcb1aba8d8afddd03483dee984a7fd0554bd28feb69af880"},
    {"http://192.168.1.100:8080/#b=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", 4, 5, 33, "fe1cb63fc13162106e82076bb751c3d5dbac20baec10d15107faaaaafe00665600c755978c0024c1e4acc9e9261209015b46fdb4d75b44184d2e5e7da3d5eae17505e2b59709066784f2f6bdeefc2a2d225a4efcc960d4a4201e5a9d663a1d61644104eb064cf9006644c47fb9c1ebd05dd1117ba4a96fc5d00440d2e9bc447305a77214fef1868900"},
    {"http://192.168.1.100:8080/#b=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", 4, 6, 33, "fe9cb63fc1326e106e8b23ebb741c3d5dba5042aec11574907faaaaafe016a6600da7105a08024c1e4a5ed7b6f338f193ac6fdb4d75848288e237aefeaf1eae17505c627de2d807fe574f6bdeefc261de1567a6e804444a4201e5a0f2f1e8f7905c71ceb064cf9006548c4bf90e56b904dd1117bad8dff8dd58258b2e9bc447305ab42d7fed514c000"},
    {"http://192.168.1.100:8080/#b=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", 4, 7, 33, "fe49e33fc14d91d06ebe76abb74e3c25dba8517aec16a8b507faaaaafe01959900d32450bb7edb3e1b70b82e3a6c30e6c533a8e1820fa7d771de2fbabfa4111e8afa93728b78fe801a8b23e8bba979a21ea98f3bd511134bdfe1a75a7a4bda82fa38e3be5319fc005ab7477fa5b02ad0422ef18ba0d8afddd67da74ee8e911270554bd28fe80419500"},
    {"http://10.0.0.123:49152/#b=KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK", 5, 0, 37, "fe4e6663fc11ccccd06eba6666bb74ecccc5dba266662ec120cccd07faaaaaafe01dcccc00efb1999e27698cccd9dba9999992602ccccbbdae9999b066dccccd95697999853489cccc2a8a71999b49e68cccda4f8199989e536cccc2aa7e9999b5ae4ccccda72d79999ee099cccf2ad2d1999318618ccc196ef1999a86c06ccccda8ea9998fc006ccccc6ffb7999eaf05dcccd1aba99998f8dd08ccc6deea1999a2705ccccf6afef9998cd80"},
    {"http://10.0.0.123:49152/#b=KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK", 5, 1, 37, "fe9b3333fc176666506e8f3332bb74466665dbaf33332ec14a666507faaaaaafe017666600e6e4cccf9dc32666734efcccccc6ca86666118fbcccce54c7666673c3c2cccd07e23666680df24ccce1b4c266670dad4cccdcaf9c666680f2bcccce084e666670e782ccccbaa336665808784ccc64acb2666b3fba4cccfd26ac666670dbfcccdf90046666447f82cccabb057666710ba4cccdfddd22666c76eb4cccf730566665c0feacccd9880"},
    {"http://10.0.0.123:49152/#b=KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK", 5, 2, 37, "fe2de85bfc152f42d06e8285e8bb75d42f45dba45e85aec16ef42d07faaaaaafe00e42f400fbd217a551416f42e115917a17abea142f458e20a17a3e0452f42e198af7a166ac2a42f4c9b29217a3afce6f42e281b97a16a7d9542f4c99f0a17a3bccc2f42e2bcef7a17d783a42f7c9ea3217abfe496f4221a0c97a14bf4a542f439b64a17bfa0062f42c63faf7a12b704e42f519bafa17bfedd46f42552eb97a141f05f42f789fe9a17b4380"},
    {"http://10.0.0.123:49152/#b=KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK", 5, 3, 37, "feade85bfc13f42f506eb9e85ebb75d42f45dba9e85eaec1182f4107faaaaaafe0052f4200f2bfa17ce9416f42e138a7a17a1c58cf42f35e20a17a3e5e3f42f576ef417a0b0c2a42f4c906497a15710502f439c1b97a16a6b0e2f42124667a178d4cc2f42e291d9a17a60317f42ca4ea3217abfbefb42f97569217a2654a542f43902917a0ff80542f4457f8f7a12b70452f4312ba17a16f85d46f42552eafa179a9052f42ce4fe9a17b4380"},
    {"http://10.0.0.123:49152/#b=KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK", 5, 4, 37, "feeaf42bfc1717a1506eacbd0abb75a5e855dba02f42aec14d7a1507faaaaaafe0095e8500ce150bd17acf57a16f369f42f424f265e859f93cd0bd223cb17a16fbba142f5e5dad5e850ec3550bd26c4057a16ca2b742f528c125e850eeecd0bd27f4217a16c9fe142f4589bd5e860e9bf50bda3dc757a1af83c742f7305225e85fec78d0bcfe00417a1443f8142f2b90595e851ebabd0bcfadd257a1db2e9742f7910585e864efedd0bc5f80"},
    {"http://10.0.0.123:49152/#b=KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK", 5, 5, 37, "fe1b3333fc132762506e8285e8bb75133335dbac5e85aec10e762507faaaaaafe006627600c75217a0c08673332615917a17abca96276508fbcccce50c727626398af7a166ab363333d5b29217a3af4c6762609ad4cccdcbf9d6276c19f0a17a3bd0b333325bcef7a17d7a326275c18784ccc64ecb6762a3a0c97a14bf3b9333325b64a17bfa0042762443fa2cccabb056627711ba7a17bfedd37333922e997a141f057627581feacccd9880"},
    {"http://10.0.0.123:49152/#b=KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK", 5, 6, 37, "fe9b3333fc1317a1506e8ba17abb74133335dba57a17aec11617a107faaaaaafe016a17a00da7685ea08867333261cb5e85e8fd2f7a17d68fbcccce53cb17a16f8ae65e8422b363333d596005e873f2de17a011ad4cccdcb3ada17af1d62e85ea9d0b333325b5cbe85ef3bb47a14478784ccc64dc757a1afb2805e86f73b933332592d85e9fb005a17a45bf82cccabb046a17b11bade85ffcdd77333922e9de85d3b0517a1407feacccd9880"},
    {"http://10.0.0.123:49152/#b=KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK", 5, 7, 37, "fe4e6663fc14e85e906ebef42ebb74ecccc5dba82f42aec169e85d07faaaaaafe0195e8500d323d0bbb7698cccd9c9e0bd0bda29085e829dae9999b0424e85e905fb30bd177489cccc2ac3550bd268c21e85fecf8199989ec125e850e837bd0bfcae4ccccda609ebd0ba640b85ebb8d2d199931a28a85e5067d50bd3a2c06ccccdac78d0bcfe0065e85c67fb7999eaf0495e851eba0bd0af9dd48ccc6dee88bd086f05e85ebf8fef9998cd80"},
};

int hexNibble(char ch) { return (ch >= '0' && ch <= '9') ? (ch - '0') : (ch - 'a' + 10); }

int goldenBit(const GoldenCase& c, int idx) {
    const int byteIndex = idx / 8;  // hex 串每字节两字符
    const int byte = (hexNibble(c.hex[byteIndex * 2]) << 4) | hexNibble(c.hex[byteIndex * 2 + 1]);
    return (byte >> (7 - idx % 8)) & 1;
}

int firstMismatch(const QrMatrix& m, const GoldenCase& c) {
    for (int i = 0; i < c.size * c.size; ++i) {
        const int actual = m.modules[static_cast<std::size_t>(i)] ? 1 : 0;
        if (actual != goldenBit(c, i)) return i;
    }
    return -1;
}

const GoldenCase* findGolden(const std::string& payload, int version, int mask) {
    for (const GoldenCase& c : kGolden) {
        if (payload == c.payload && version == c.version && mask == c.mask) return &c;
    }
    return nullptr;
}

std::string repeat(char ch, int n) { return std::string(static_cast<std::size_t>(n), ch); }

bool checkFinder(const QrMatrix& m, int r0, int c0) {
    bool ok = true;
    for (int dr = 0; dr < 7; ++dr) {
        for (int dc = 0; dc < 7; ++dc) {
            const bool border = dr == 0 || dr == 6 || dc == 0 || dc == 6;
            const bool center = dr >= 2 && dr <= 4 && dc >= 2 && dc <= 4;
            const int expected = (border || center) ? 1 : 0;
            if (static_cast<int>(m.at(r0 + dr, c0 + dc)) != expected) ok = false;
        }
    }
    return ok;
}

const char* const kPayloadA =
    "http://192.168.1.100:8080/#b=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

}  // namespace

// 强制版本+掩码：V4/V5 全 8 掩码（含 0/1/5/7）逐位对齐 Python 参考实现。
REGISTER_TEST(qr_encode_golden_forced_mask,
              "qr: 强制版本+掩码与 Python 参考矩阵逐位一致（V4/V5 全 8 掩码）", {
    for (const GoldenCase& c : kGolden) {
        QrEncodeOptions opt;
        opt.version = c.version;
        opt.mask = c.mask;
        QrMatrix m;
        DOCTEST_CHECK(encode(c.payload, QrEcLevel::L, m, opt));
        DOCTEST_CHECK_EQ(m.size, c.size);
        DOCTEST_CHECK_EQ(firstMismatch(m, c), -1);
    }
    return true;
})

// 自动选版本+掩码：65B→V4、96B→V5，且最终矩阵须等于参考实现同掩码的输出。
REGISTER_TEST(qr_encode_auto_select,
              "qr: 自动选版本(65B→V4/96B→V5)与掩码后矩阵与参考一致", {
    const std::string payloadB = std::string("http://10.0.0.123:49152/#b=") + repeat('K', 69);
    struct Case {
        std::string payload;
        int version;
    } cases[] = {{std::string(kPayloadA), 4}, {payloadB, 5}};
    for (const Case& cs : cases) {
        QrMatrix m;
        DOCTEST_CHECK(encode(cs.payload, QrEcLevel::L, m));
        DOCTEST_CHECK_EQ(m.version, cs.version);
        DOCTEST_CHECK(m.mask >= 0 && m.mask <= 7);
        const GoldenCase* g = findGolden(cs.payload, m.version, m.mask);
        DOCTEST_CHECK(g != nullptr);
        if (g != nullptr) DOCTEST_CHECK_EQ(firstMismatch(m, *g), -1);
    }
    return true;
})

// 容量边界：恰好装满通过，超 1 字节明确失败（不越界不截断）；非法参数拒绝。
REGISTER_TEST(qr_encode_capacity_overflow,
              "qr: 容量边界（V4=78 满/79 溢出、自动 106→V5/107→V6、非法参数）", {
    QrEncodeOptions force4;
    force4.version = 4;
    force4.mask = 0;

    QrMatrix exact;
    DOCTEST_CHECK(encode(repeat('A', 78), QrEcLevel::L, exact, force4));
    DOCTEST_CHECK_EQ(exact.size, 33);

    QrMatrix overflow;
    DOCTEST_CHECK_FALSE(encode(repeat('A', 79), QrEcLevel::L, overflow, force4));

    QrMatrix auto106;
    DOCTEST_CHECK(encode(repeat('A', 106), QrEcLevel::L, auto106));
    DOCTEST_CHECK_EQ(auto106.version, 5);

    QrMatrix auto107;
    DOCTEST_CHECK(encode(repeat('A', 107), QrEcLevel::L, auto107));
    DOCTEST_CHECK_EQ(auto107.version, 6);

    QrMatrix tooBig;
    DOCTEST_CHECK_FALSE(encode(repeat('A', 272), QrEcLevel::L, tooBig));

    QrEncodeOptions badMask;
    badMask.mask = 8;
    QrMatrix bad;
    DOCTEST_CHECK_FALSE(encode("x", QrEcLevel::L, bad, badMask));

    QrEncodeOptions badVersion;
    badVersion.version = 11;
    QrMatrix badV;
    DOCTEST_CHECK_FALSE(encode("x", QrEcLevel::L, badV, badVersion));
    return true;
})

// 非 golden 覆盖版本的结构断言：尺寸、三角定位图形、固定暗模块。
REGISTER_TEST(qr_encode_structure_versions,
              "qr: V1/V3/V6/V10 结构断言（尺寸/三角定位/暗模块）", {
    const int versions[] = {1, 3, 6, 10};
    for (int v : versions) {
        QrEncodeOptions opt;
        opt.version = v;
        opt.mask = 0;
        QrMatrix m;
        DOCTEST_CHECK(encode("abcdefghij", QrEcLevel::L, m, opt));
        const int expectedSize = 17 + 4 * v;
        DOCTEST_CHECK_EQ(m.size, expectedSize);
        DOCTEST_CHECK(checkFinder(m, 0, 0));
        DOCTEST_CHECK(checkFinder(m, 0, expectedSize - 7));
        DOCTEST_CHECK(checkFinder(m, expectedSize - 7, 0));
        DOCTEST_CHECK_EQ(static_cast<int>(m.at(expectedSize - 8, 8)), 1);
    }
    return true;
})
