// tests/test_modules/test_hot_parse.cpp
// 微博热榜纯解析契约（无网络 / 无 DB / 无时钟）：
//   1) 正常响应过滤广告与 hotgov，按 realpos 重排 1-based rank；
//   2) word_scheme 优先，`#`/中文按 UTF-8 percent-encode，可解码回原文；
//   3) num 缺失 / 非 number → hot=nullopt，不让整条失败；
//   4) 空 realtime → 成功空列表；403 / 截断 / 结构变化 → Result 失败（不伪装空榜）；
//   5) takeTopN(0/3/100) 截取且不重排；有效项不足 10 时返回全部。
// fixture 用 inline raw string：仓库现有测试均不读 tests/fixtures/，保持一致。
#include "doctest.h"

#include "core/result.h"
#include "module_agent/hot_parse.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using module::agent::HotItem;
using module::agent::parseWeiboHot;
using module::agent::takeTopN;

// 12 条有效项（realpos 1..12，响应内故意乱序）+ 2 条广告（is_ad=1 且无 realpos）
// + 2 条单条坏项（无 realpos / 空标题，应跳过）+ 独立 hotgov/hotgovs（不得并入）。
const char* const kWeiboOk = R"({
  "ok": 1,
  "data": {
    "hotgov": {"word": "#政务置顶#", "num": 9999999, "realpos": 1},
    "hotgovs": [{"word": "#政务第二#", "num": 8888888}],
    "realtime": [
      {"word": " 第十条\t", "num": 300000, "realpos": 10, "word_scheme": "#第十条#"},
      {"word": "广告甲", "num": 2000000, "is_ad": 1},
      {"word": "第一条", "num": 1100000, "realpos": 1, "word_scheme": "#第一条#", "note": "第一条"},
      {"word": "第二条 ", "num": 1050000, "realpos": 2, "word_scheme": ""},
      {"word": "第三条", "realpos": 3},
      {"word": "第四条", "num": 900000, "realpos": 4, "word_scheme": "#第四条#"},
      {"word": "第五条", "num": 800000, "realpos": 5},
      {"word": "第六条", "num": "600000", "realpos": 6},
      {"word": "  #话题七#  ", "num": 600000, "realpos": 7},
      {"word": "第八条", "num": 500000, "realpos": 8},
      {"word": "第九条", "num": 400000, "realpos": 9},
      {"word": "广告乙", "num": 1900000, "is_ad": 1},
      {"word": "第十一条", "num": 350000, "realpos": 11},
      {"word": "第十二条", "num": 250000, "realpos": 12},
      {"word": "无排名条目", "num": 123, "is_ad": 0},
      {"word": "   ", "num": 456, "realpos": 13}
    ]
  }
})";

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// 测试侧独立 percent-decode（不复用实现内部函数，避免自证）。
bool percentDecode(const std::string& text, std::string& out) {
    out.clear();
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            out.push_back(text[i]);
            continue;
        }
        if (i + 2 >= text.size()) return false;
        const int hi = hexValue(text[i + 1]);
        const int lo = hexValue(text[i + 2]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
    }
    return true;
}

bool decodeWeiboQuery(const std::string& url, std::string& out) {
    const std::string prefix = "https://s.weibo.com/weibo?q=";
    if (url.rfind(prefix, 0) != 0) return false;
    return percentDecode(url.substr(prefix.size()), out);
}

} // namespace

REGISTER_TEST(weibo_hot_filters_and_ranks,
              "微博热榜: 正常响应过滤广告/置顶、按 realpos 重排 rank 并编码 URL", {
    auto parsed = parseWeiboHot(kWeiboOk);
    DOCTEST_REQUIRE(parsed.isOk());
    const std::vector<HotItem>& items = parsed.value();

    // 12 条有效项：2 条广告、无 realpos、空标题均被跳过；hotgov 不并入。
    DOCTEST_CHECK_EQ(items.size(), static_cast<std::size_t>(12));
    const char* expectedTitles[] = {
        "第一条", "第二条", "第三条", "第四条", "第五条", "第六条",
        "#话题七#", "第八条", "第九条", "第十条", "第十一条", "第十二条",
    };
    for (std::size_t i = 0; i < items.size(); ++i) {
        DOCTEST_CHECK_EQ(items[i].rank, static_cast<int>(i + 1));
        DOCTEST_CHECK_EQ(items[i].title, std::string(expectedTitles[i]));
        DOCTEST_CHECK(items[i].title.find("政务") == std::string::npos);
        DOCTEST_CHECK(items[i].title.find("广告") == std::string::npos);
    }

    // num number → hot；num 缺失 / 字符串 → nullopt；3.5e5 的 double 也认。
    DOCTEST_REQUIRE(items[0].hot.has_value());
    DOCTEST_CHECK_EQ(*items[0].hot, static_cast<int64_t>(1100000));
    DOCTEST_REQUIRE(items[1].hot.has_value());
    DOCTEST_CHECK_EQ(*items[1].hot, static_cast<int64_t>(1050000));
    DOCTEST_CHECK_FALSE(items[2].hot.has_value());
    DOCTEST_CHECK_FALSE(items[5].hot.has_value());
    DOCTEST_REQUIRE(items[10].hot.has_value());
    DOCTEST_CHECK_EQ(*items[10].hot, static_cast<int64_t>(350000));

    // 标题 trim；`#` 原样展示（来自 word，word_scheme 缺失）。
    DOCTEST_CHECK_EQ(items[1].title, std::string("第二条"));
    DOCTEST_CHECK_EQ(items[6].title, std::string("#话题七#"));
    DOCTEST_CHECK_EQ(items[9].title, std::string("第十条"));

    // 链接优先 word_scheme：`#` → %23，中文按 UTF-8 编码。
    DOCTEST_CHECK_EQ(
        items[0].url,
        std::string("https://s.weibo.com/weibo?q=%23%E7%AC%AC%E4%B8%80%E6%9D%A1%23"));
    std::string decoded;
    DOCTEST_REQUIRE(decodeWeiboQuery(items[0].url, decoded));
    DOCTEST_CHECK_EQ(decoded, std::string("#第一条#"));
    DOCTEST_CHECK_EQ(items[9].url,
                     std::string("https://s.weibo.com/weibo?q=%23%E7%AC%AC%E5%8D%81"
                                 "%E6%9D%A1%23"));

    // word_scheme 为空 → 退回 word；无 `#` 的普通词直接编码中文。
    DOCTEST_REQUIRE(decodeWeiboQuery(items[1].url, decoded));
    DOCTEST_CHECK_EQ(decoded, std::string("第二条"));

    // 工具层取 top10：恰好 10 条且 rank 连续 1..10（第 11/12 条被截掉）。
    const std::vector<HotItem> top = takeTopN(items, 10);
    DOCTEST_REQUIRE_EQ(top.size(), static_cast<std::size_t>(10));
    for (std::size_t i = 0; i < top.size(); ++i) {
        DOCTEST_CHECK_EQ(top[i].rank, static_cast<int>(i + 1));
    }
    DOCTEST_CHECK_EQ(top.back().title, std::string("第十条"));
    return true;
})

REGISTER_TEST(weibo_hot_take_top_n,
              "微博热榜: takeTopN(0/3/100/12) 截取且不重排", {
    auto parsed = parseWeiboHot(kWeiboOk);
    DOCTEST_REQUIRE(parsed.isOk());
    const std::vector<HotItem>& all = parsed.value();

    const std::vector<HotItem> zero = takeTopN(all, 0);
    DOCTEST_CHECK(zero.empty());

    const std::vector<HotItem> three = takeTopN(all, 3);
    DOCTEST_REQUIRE_EQ(three.size(), static_cast<std::size_t>(3));
    DOCTEST_CHECK_EQ(three[0].title, std::string("第一条"));
    DOCTEST_CHECK_EQ(three[1].title, std::string("第二条"));
    DOCTEST_CHECK_EQ(three[2].title, std::string("第三条"));
    DOCTEST_CHECK_EQ(three[0].rank, 1);
    DOCTEST_CHECK_EQ(three[2].rank, 3);

    const std::vector<HotItem> hundred = takeTopN(all, 100);
    DOCTEST_REQUIRE_EQ(hundred.size(), all.size());
    DOCTEST_CHECK_EQ(hundred.back().rank, 12);

    const std::vector<HotItem> exact = takeTopN(all, all.size());
    DOCTEST_REQUIRE_EQ(exact.size(), all.size());
    DOCTEST_CHECK_EQ(exact.back().rank, 12);

    // 传值语义：原列表不受影响。
    DOCTEST_CHECK_EQ(all.size(), static_cast<std::size_t>(12));
    return true;
})

REGISTER_TEST(weibo_hot_empty_realtime_succeeds,
              "微博热榜: realtime 空数组 → 成功且为空", {
    auto parsed = parseWeiboHot(R"({"ok":1,"data":{"realtime":[]}})");
    DOCTEST_REQUIRE(parsed.isOk());
    DOCTEST_CHECK(parsed.value().empty());
    DOCTEST_CHECK(takeTopN(parsed.value(), 10).empty());
    return true;
})

REGISTER_TEST(weibo_hot_bad_response_fails_with_path,
              "微博热榜: 403/ok!=1/结构变化 → 失败且错误带字段路径", {
    struct Case {
        const char* json;
        const char* path;
    };
    const Case cases[] = {
        {R"({"error":"Forbidden"})", "ok"},                              // 403/无 ok
        {R"({"ok":0,"data":{"realtime":[]}})", "ok"},                    // ok != 1
        {R"({"ok":"1","data":{"realtime":[]}})", "ok"},                  // ok 类型错
        {R"({"ok":true,"data":{"realtime":[]}})", "ok"},                 // ok 非 number
        {R"({"ok":1})", "data"},                                         // 缺 data
        {R"({"ok":1,"data":[]})", "data"},                               // data 类型错
        {R"({"ok":1,"data":{}})", "data.realtime"},                      // 缺 realtime
        {R"({"ok":1,"data":{"realtime":{}}})", "data.realtime"},         // realtime 类型错
    };

    for (const Case& c : cases) {
        auto parsed = parseWeiboHot(c.json);
        DOCTEST_REQUIRE_MESSAGE(parsed.isFail(), c.json);
        DOCTEST_CHECK_MESSAGE(parsed.errorMessage().find(c.path) != std::string::npos,
                              parsed.errorMessage());
    }
    return true;
})

REGISTER_TEST(weibo_hot_truncated_json_fails,
              "微博热榜: 截断 JSON → 解析失败，不伪装成空榜", {
    auto parsed = parseWeiboHot(R"({"ok":1,"data":{"realtime":[{"word":"x"})");
    DOCTEST_REQUIRE(parsed.isFail());
    DOCTEST_CHECK(parsed.errorCode() == core::ErrorCode::JsonParseError);
    DOCTEST_CHECK_FALSE(parsed.errorMessage().empty());

    auto emptyInput = parseWeiboHot("");
    DOCTEST_CHECK(emptyInput.isFail());
    return true;
})

REGISTER_TEST(weibo_hot_fewer_than_ten_succeeds,
              "微博热榜: 有效项少于 10 时返回全部（成功）", {
    const char* json = R"({
      "ok": 1,
      "data": {
        "realtime": [
          {"word": "小榜第二", "num": 2, "realpos": 2},
          {"word": "小榜第一", "num": 1, "realpos": 1}
        ]
      }
    })";
    auto parsed = parseWeiboHot(json);
    DOCTEST_REQUIRE(parsed.isOk());
    const std::vector<HotItem>& items = parsed.value();
    DOCTEST_REQUIRE_EQ(items.size(), static_cast<std::size_t>(2));
    DOCTEST_CHECK_EQ(items[0].title, std::string("小榜第一"));
    DOCTEST_CHECK_EQ(items[0].rank, 1);
    DOCTEST_CHECK_EQ(items[1].title, std::string("小榜第二"));
    DOCTEST_CHECK_EQ(items[1].rank, 2);

    const std::vector<HotItem> top = takeTopN(items, 10);
    DOCTEST_REQUIRE_EQ(top.size(), static_cast<std::size_t>(2));
    DOCTEST_CHECK_EQ(top[1].rank, 2);
    return true;
})

REGISTER_TEST(weibo_hot_percent_encode_reserved,
              "微博热榜: percent-encode 保留 unreserved、编码保留字符", {
    const char* json = R"({
      "ok": 1,
      "data": {
        "realtime": [
          {"word": "A-B_C.D~E/F?G&H=I J", "realpos": 1}
        ]
      }
    })";
    auto parsed = parseWeiboHot(json);
    DOCTEST_REQUIRE(parsed.isOk());
    DOCTEST_REQUIRE_EQ(parsed.value().size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(
        parsed.value()[0].url,
        std::string("https://s.weibo.com/weibo?q=A-B_C.D~E%2FF%3FG%26H%3DI%20J"));
    DOCTEST_CHECK_FALSE(parsed.value()[0].hot.has_value());
    return true;
})
