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

namespace {

using module::agent::HotSource;
using module::agent::parseBaiduHot;
using module::agent::parseHotList;
using module::agent::parseToutiaoHot;
using module::agent::parseWeiboBand;

// 第一张卡不是 hotList；topContent 是独立置顶专题（不得并入）；
// 热榜内 index 乱序，hotScore 覆盖字符串 / number / 缺失 / 非数字，
// url 覆盖正常 / 空→appUrl / javascript:。
const char* const kBaiduOk = R"JSON({
  "success": true,
  "data": {
    "topContent": [
      {"word": "置顶专题", "hotScore": "99999999", "url": "https://top.baidu.com/board?tab=realtime"}
    ],
    "cards": [
      {"component": "banner", "content": [{"word": "不是热榜"}]},
      {"component": "hotList", "content": [
        {"word": " 百度第三 ", "hotScore": "300", "url": "https://www.baidu.com/s?wd=3", "index": 2},
        {"word": "百度第一", "hotScore": 100, "url": "", "appUrl": "https://m.baidu.com/s?wd=1", "index": 0},
        {"word": "百度第二", "hotScore": "abc", "url": "javascript:alert(1)", "index": 1},
        {"word": "百度第四", "hotScore": "400", "url": "https://www.baidu.com/s?wd=4", "index": 3},
        {"word": "百度第五", "index": 4},
        {"word": "   ", "hotScore": "500", "index": 5}
      ]}
    ]
  }
})JSON";

const char* const kToutiaoOk = R"JSON({
  "status": "success",
  "fixed_top_data": [
    {"Title": "置顶专题", "Url": "https://www.toutiao.com/trending/0/"}
  ],
  "data": [
    {"Title": " 头条一 ", "HotValue": "1000", "Url": "https://www.toutiao.com/trending/1/?a=1&b=2&c=3"},
    {"Title": "头条二", "HotValue": 2000, "Url": "javascript:alert(1)"},
    {"Title": "头条三", "HotValue": "not-a-number", "Url": "http://www.toutiao.com/trending/3/"},
    {"Title": "   ", "HotValue": "4000"},
    {"Title": "头条四", "HotValue": "4e3"}
  ]
})JSON";

const char* const kWeiboBandOk = R"({
  "data": {
    "hotgov": {"word": "#政务置顶#", "num": 9999999, "realpos": 1},
    "band_list": [
      {"word": "榜三", "num": 300, "realpos": 3},
      {"word": "广告", "num": 9999, "is_ad": 1, "realpos": 1},
      {"word": " 榜一 ", "num": 100, "realpos": 1, "word_scheme": "#榜一#"},
      {"word": "榜二", "num": 200, "realpos": 2},
      {"word": "无排名", "num": 50}
    ]
  }
})";

} // namespace

REGISTER_TEST(baidu_hot_selects_hot_list_and_sorts,
              "百度热榜: 按 component 选卡、忽略 topContent、index 重排、字段降级", {
    auto parsed = parseBaiduHot(kBaiduOk);
    DOCTEST_REQUIRE(parsed.isOk());
    const std::vector<HotItem>& items = parsed.value();

    // 空标题跳过；topContent / 非 hotList 卡片不并入。
    DOCTEST_REQUIRE_EQ(items.size(), static_cast<std::size_t>(5));
    const char* expectedTitles[] = {"百度第一", "百度第二", "百度第三", "百度第四",
                                    "百度第五"};
    for (std::size_t i = 0; i < items.size(); ++i) {
        DOCTEST_CHECK_EQ(items[i].title, std::string(expectedTitles[i]));
        DOCTEST_CHECK_EQ(items[i].rank, static_cast<int>(i + 1));
        DOCTEST_CHECK(items[i].title.find("置顶") == std::string::npos);
        DOCTEST_CHECK(items[i].title.find("不是热榜") == std::string::npos);
    }

    // hotScore：number 100 / 字符串 "300" / 字符串 "400" 转换；"abc" 与缺失 → nullopt。
    DOCTEST_REQUIRE(items[0].hot.has_value());
    DOCTEST_CHECK_EQ(*items[0].hot, static_cast<int64_t>(100));
    DOCTEST_CHECK_FALSE(items[1].hot.has_value());
    DOCTEST_REQUIRE(items[2].hot.has_value());
    DOCTEST_CHECK_EQ(*items[2].hot, static_cast<int64_t>(300));
    DOCTEST_REQUIRE(items[3].hot.has_value());
    DOCTEST_CHECK_EQ(*items[3].hot, static_cast<int64_t>(400));
    DOCTEST_CHECK_FALSE(items[4].hot.has_value());

    // url 为空 → appUrl；javascript: → 空 URL。
    DOCTEST_CHECK_EQ(items[0].url, std::string("https://m.baidu.com/s?wd=1"));
    DOCTEST_CHECK(items[1].url.empty());
    DOCTEST_CHECK_EQ(items[2].url, std::string("https://www.baidu.com/s?wd=3"));
    return true;
})

REGISTER_TEST(baidu_hot_empty_and_bad_structure,
              "百度热榜: 空 content 成功；success/缺 hotList/坏 content → 失败带路径", {
    auto empty = parseBaiduHot(
        R"({"success":true,"data":{"cards":[{"component":"hotList","content":[]}]}})");
    DOCTEST_REQUIRE(empty.isOk());
    DOCTEST_CHECK(empty.value().empty());

    struct Case {
        const char* json;
        const char* path;
    };
    const Case cases[] = {
        {R"({"success":false,"data":{"cards":[]}})", "success"},
        {R"({"success":"true","data":{"cards":[]}})", "success"},
        {R"({"success":true})", "data"},
        {R"({"success":true,"data":{}})", "data.cards"},
        {R"({"success":true,"data":{"cards":[{"component":"banner","content":[]}]}})",
         "hotList"},
        {R"({"success":true,"data":{"cards":[{"component":"hotList","content":{}}]}})",
         "content"},
    };
    for (const Case& c : cases) {
        auto parsed = parseBaiduHot(c.json);
        DOCTEST_REQUIRE_MESSAGE(parsed.isFail(), c.json);
        DOCTEST_CHECK_MESSAGE(parsed.errorMessage().find(c.path) != std::string::npos,
                              parsed.errorMessage());
    }
    return true;
})

REGISTER_TEST(toutiao_hot_keeps_order_and_long_url,
              "今日头条热榜: 忽略置顶、按数组顺序赋 rank、长 URL 原样保留", {
    auto parsed = parseToutiaoHot(kToutiaoOk);
    DOCTEST_REQUIRE(parsed.isOk());
    const std::vector<HotItem>& items = parsed.value();

    // 空标题跳过；fixed_top_data 不并入。
    DOCTEST_REQUIRE_EQ(items.size(), static_cast<std::size_t>(4));
    const char* expectedTitles[] = {"头条一", "头条二", "头条三", "头条四"};
    for (std::size_t i = 0; i < items.size(); ++i) {
        DOCTEST_CHECK_EQ(items[i].title, std::string(expectedTitles[i]));
        DOCTEST_CHECK_EQ(items[i].rank, static_cast<int>(i + 1));
        DOCTEST_CHECK(items[i].title.find("置顶") == std::string::npos);
    }

    DOCTEST_REQUIRE(items[0].hot.has_value());
    DOCTEST_CHECK_EQ(*items[0].hot, static_cast<int64_t>(1000));
    DOCTEST_REQUIRE(items[1].hot.has_value());
    DOCTEST_CHECK_EQ(*items[1].hot, static_cast<int64_t>(2000));
    DOCTEST_CHECK_FALSE(items[2].hot.has_value());   // "not-a-number"
    DOCTEST_CHECK_FALSE(items[3].hot.has_value());   // "4e3" 非整数文本

    // 长 URL 连 & 参数完整保留；javascript: → 空。
    DOCTEST_CHECK_EQ(
        items[0].url,
        std::string("https://www.toutiao.com/trending/1/?a=1&b=2&c=3"));
    DOCTEST_CHECK(items[1].url.empty());
    DOCTEST_CHECK_EQ(items[2].url,
                     std::string("http://www.toutiao.com/trending/3/"));
    return true;
})

REGISTER_TEST(toutiao_hot_empty_and_bad_status,
              "今日头条热榜: 空 data 成功；status 非 success / data 非数组 → 失败", {
    auto empty = parseToutiaoHot(R"({"status":"success","data":[]})");
    DOCTEST_REQUIRE(empty.isOk());
    DOCTEST_CHECK(empty.value().empty());

    auto badStatus = parseToutiaoHot(R"({"status":"error","data":[]})");
    DOCTEST_REQUIRE(badStatus.isFail());
    DOCTEST_CHECK(badStatus.errorMessage().find("status") != std::string::npos);

    auto missingData = parseToutiaoHot(R"({"status":"success"})");
    DOCTEST_REQUIRE(missingData.isFail());
    DOCTEST_CHECK(missingData.errorMessage().find("data") != std::string::npos);

    auto badData = parseToutiaoHot(R"({"status":"success","data":{}})");
    DOCTEST_REQUIRE(badData.isFail());
    DOCTEST_CHECK(badData.errorMessage().find("data") != std::string::npos);
    return true;
})

REGISTER_TEST(weibo_band_parses_and_ranks,
              "微博 hot_band: 过滤广告/无排名、忽略 hotgov、按 realpos 连续 rank", {
    auto parsed = parseWeiboBand(kWeiboBandOk);
    DOCTEST_REQUIRE(parsed.isOk());
    const std::vector<HotItem>& items = parsed.value();

    DOCTEST_REQUIRE_EQ(items.size(), static_cast<std::size_t>(3));
    DOCTEST_CHECK_EQ(items[0].title, std::string("榜一"));
    DOCTEST_CHECK_EQ(items[1].title, std::string("榜二"));
    DOCTEST_CHECK_EQ(items[2].title, std::string("榜三"));
    for (std::size_t i = 0; i < items.size(); ++i) {
        DOCTEST_CHECK_EQ(items[i].rank, static_cast<int>(i + 1));
        DOCTEST_CHECK(items[i].title.find("政务") == std::string::npos);
        DOCTEST_CHECK(items[i].title.find("广告") == std::string::npos);
    }

    DOCTEST_REQUIRE(items[0].hot.has_value());
    DOCTEST_CHECK_EQ(*items[0].hot, static_cast<int64_t>(100));
    DOCTEST_REQUIRE(items[2].hot.has_value());
    DOCTEST_CHECK_EQ(*items[2].hot, static_cast<int64_t>(300));

    // word_scheme 优先，`#` 与中文 percent-encode。
    std::string decoded;
    DOCTEST_REQUIRE(decodeWeiboQuery(items[0].url, decoded));
    DOCTEST_CHECK_EQ(decoded, std::string("#榜一#"));
    DOCTEST_REQUIRE(decodeWeiboQuery(items[1].url, decoded));
    DOCTEST_CHECK_EQ(decoded, std::string("榜二"));
    return true;
})

REGISTER_TEST(weibo_band_empty_and_bad_structure,
              "微博 hot_band: 空 band_list 成功；缺 data / 坏 band_list → 失败带路径", {
    auto empty = parseWeiboBand(R"({"data":{"band_list":[]}})");
    DOCTEST_REQUIRE(empty.isOk());
    DOCTEST_CHECK(empty.value().empty());

    auto missingData = parseWeiboBand(R"({"band_list":[]})");
    DOCTEST_REQUIRE(missingData.isFail());
    DOCTEST_CHECK(missingData.errorMessage().find("data") != std::string::npos);

    auto badList = parseWeiboBand(R"({"data":{"band_list":{}}})");
    DOCTEST_REQUIRE(badList.isFail());
    DOCTEST_CHECK(badList.errorMessage().find("data.band_list") != std::string::npos);
    return true;
})

REGISTER_TEST(hot_list_dispatches_all_sources,
              "热榜分发: 四个 source 各自解析；未知 source → 失败", {
    struct Case {
        HotSource source;
        const char* json;
        const char* title;
    };
    const Case cases[] = {
        {HotSource::Weibo,
         R"({"ok":1,"data":{"realtime":[{"word":"微博榜","num":1,"realpos":1}]}})",
         "微博榜"},
        {HotSource::WeiboBand,
         R"({"data":{"band_list":[{"word":"微博 band 榜","num":2,"realpos":1}]}})",
         "微博 band 榜"},
        {HotSource::Baidu,
         R"({"success":true,"data":{"cards":[{"component":"hotList","content":[{"word":"百度榜","hotScore":"3","index":0}]}]}})",
         "百度榜"},
        {HotSource::Toutiao,
         R"({"status":"success","data":[{"Title":"头条榜","HotValue":"4","Url":"https://www.toutiao.com/x"}]})",
         "头条榜"},
    };
    for (const Case& c : cases) {
        auto parsed = parseHotList(c.source, c.json);
        DOCTEST_REQUIRE_MESSAGE(parsed.isOk(), c.json);
        DOCTEST_REQUIRE_EQ(parsed.value().size(), static_cast<std::size_t>(1));
        DOCTEST_CHECK_EQ(parsed.value()[0].title, std::string(c.title));
        DOCTEST_CHECK_EQ(parsed.value()[0].rank, 1);
    }

    auto unknown = parseHotList(static_cast<HotSource>(99), "{}");
    DOCTEST_REQUIRE(unknown.isFail());
    DOCTEST_CHECK(unknown.errorMessage().find("热榜源") != std::string::npos);
    return true;
})
