// tests/test_platform/test_utf8.cpp
// platform::utf8 覆盖。
//
// 这个头守的是一条**进程级**不变式：交给 platform::json 组装层的文本必须良构
// UTF-8，否则 yyjson 读侧的严格校验会让 AgentLoop 的 parseArrayOrDie 自检
// LOG_FATAL + std::abort（整应用死）。故用例分两层：
//   ① 判定口径 = RFC 3629（overlong / 代理区 / >U+10FFFF / 截断 / 孤立续字节
//      全拒；合法多字节序列全过）——与 yyjson 读侧对齐；
//   ② 清洗输出的**不变式**：任意字节序列 sanitize 后恒良构，且可读部分逐字保真。
// 交叉钉子：同批向量走一遍 stringify→parse 往返（等价 makeTextContent 的实际
// 链路）——避免「本头比 yyjson 宽」留下仍然可 abort 的缝隙，或「比 yyjson 严」
// 误伤正常笔记。
#include "doctest.h"

#include "platform/json.h"
#include "platform/utf8.h"

#include <string>
#include <string_view>
#include <vector>

namespace {

// 复刻 AgentLoop 回喂链路的判定：makeString(bytes) → stringify → parse。
// json 层对非法 UTF-8 是「stringify 放行、parse 拒绝」，故本函数返回 true
// ⇔ 该字节序列**不会**打到 parseArrayOrDie 的 abort。
bool survivesJsonRoundTrip(std::string_view bytes) {
    auto doc = platform::json::stringify(
        platform::json::makeString(std::string(bytes)));
    if (doc.isFail()) return false;
    return platform::json::parse(doc.value()).isOk();
}

} // namespace

REGISTER_TEST(utf8_valid_accepts_wellformed,
              "utf8: 良构序列（ASCII/2-3-4 字节边界/emoji）判 true", {
    for (const char* s : {"", "abc", "中文测试",
                          "\xC2\x80",          // U+0080 下界
                          "\xDF\xBF",          // U+07FF 上界
                          "\xE0\xA0\x80",      // U+0800 下界
                          "\xEF\xBF\xBF",      // U+FFFF 上界
                          "\xF0\x90\x80\x80",  // U+10000 下界
                          "\xF4\x8F\xBF\xBF",  // U+10FFFF 上界
                          "\xF0\x9F\x98\x80"}) {   // emoji
        DOCTEST_CHECK(platform::utf8::isValid(s));
    }
    // 与 json 读侧口径一致（良构 → 往返成功）
    DOCTEST_CHECK(survivesJsonRoundTrip("中文\xF0\x9F\x98\x80"));
    return true;
})

REGISTER_TEST(utf8_valid_rejects_malformed,
              "utf8: overlong/代理区/越界/截断/孤立续字节一律判 false", {
    std::vector<std::string> bad;
    bad.push_back(std::string("\x80", 1));          // 孤立续字节
    bad.push_back(std::string("\xBF", 1));          // 孤立续字节
    bad.push_back(std::string("\xC0\x80", 2));      // overlong NUL
    bad.push_back(std::string("\xC1\xBF", 2));      // overlong（C0/C1 前导）
    bad.push_back(std::string("\xE0\x9F\xBF", 3));  // overlong（< U+0800）
    bad.push_back(std::string("\xF0\x8F\xBF\xBF", 4)); // overlong（< U+10000）
    bad.push_back(std::string("\xED\xA0\x80", 3));  // 代理区 U+D800
    bad.push_back(std::string("\xED\xBF\xBF", 3));  // 代理区 U+DFFF
    bad.push_back(std::string("\xF4\x90\x80\x80", 4)); // > U+10FFFF
    bad.push_back(std::string("\xF5\x80\x80\x80", 4)); // 非法前导 F5
    bad.push_back(std::string("\xFF", 1));          // 非法前导 FF
    bad.push_back(std::string("\xE4\xB8", 2));      // 截断（3 字节少 1）
    bad.push_back(std::string("\xF0\x9F\x98", 3));  // 截断（4 字节少 1）
    bad.push_back(std::string("\xC2", 1));          // 截断（2 字节缺续）
    bad.push_back(std::string("\xE4\xB8" "A", 3));  // 第三字节不是续字节
    bad.push_back(std::string("ok\xD6\xD0\xCE\xC4", 6)); // GBK「中文」夹在合法文本里
    bad.push_back(std::string("\xE9", 1));          // Latin-1 单字节 'é'

    for (const auto& s : bad) {
        DOCTEST_CHECK_MESSAGE(platform::utf8::isValid(s) == false, s);
        // 同批向量走一遍 json 往返：非法 → 往返必失败（= 修复前会 abort 的形状）。
        DOCTEST_CHECK_MESSAGE(!survivesJsonRoundTrip(s), s);
    }
    return true;
})

REGISTER_TEST(utf8_sanitize_invariants,
              "utf8: 清洗后恒良构、可读部分逐字保真、良构输入零改写", {
    // 良构输入：逐字原样（不引入替换字符）
    const std::string good = "正常文本 abc\xF0\x9F\x98\x80";
    DOCTEST_CHECK_EQ(platform::utf8::sanitize(good), good);

    // 精确字节断言：每个非法字节各换一个 U+FFFD，前后可读字节保真。
    std::string four;
    for (int i = 0; i < 4; ++i) four += "\xEF\xBF\xBD";
    const std::string gbk = "A\xD6\xD0\xCE\xC4" "B";
    DOCTEST_CHECK_EQ(platform::utf8::sanitize(gbk), std::string("A") + four + "B");
    DOCTEST_CHECK(survivesJsonRoundTrip(platform::utf8::sanitize(gbk)));

    // 不变式（全字节扫描）：任意 0x00-0xFF 序列清洗后恒良构，且 json 往返继续成立。
    std::string allBytes;
    for (int i = 0; i < 256; ++i) allBytes.push_back(static_cast<char>(i));
    const std::string cleaned = platform::utf8::sanitize(allBytes);
    DOCTEST_CHECK(platform::utf8::isValid(cleaned));
    DOCTEST_CHECK(survivesJsonRoundTrip(cleaned));
    DOCTEST_CHECK(cleaned.find("\xEF\xBF\xBD") != std::string::npos);
    // ASCII 字节全部原样保真（0x00-0x7F 恒良构，不该被替换）
    for (int i = 0; i < 128; ++i)
        DOCTEST_CHECK(cleaned.find(static_cast<char>(i)) != std::string::npos);
    return true;
})
