// tests/test_modules/test_notepad_find.cpp
// need263 批 2 棒 2：查找/替换纯函数引擎无头直测（断言面 = 技术方案卡 §十一）。
// findAll（基本/大小写/全词/CJK 字节词字符）/环绕跳转（N-3）/F-07 替换落点锁
// 全边界（缩短文本/命中在文末/0 剩余/下一命中贴替换末）/replaceAll 计数
// /mergeRuns 命中层切分与当前命中合成。
#include "doctest.h"
#include "ui_kit/text_find.h"

#include <string>
#include <vector>

using ui_kit::text_find::FindHit;
using ui_kit::text_find::FindOptions;
using ui_kit::text_find::findAll;
using ui_kit::text_find::kFindNoHit;
using ui_kit::text_find::mergeRuns;
using ui_kit::text_find::nextFrom;
using ui_kit::text_find::prevFrom;
using ui_kit::text_find::replaceAll;
using ui_kit::text_find::replaceOneAt;

namespace {

std::vector<size_t> starts(const std::vector<FindHit>& hits) {
    std::vector<size_t> out;
    for (const FindHit& h : hits) out.push_back(h.start);
    return out;
}

// 三原色判别用（合成色断言走 r/g/b/a 精确比较）
const eui::Color kColBase{1.0f, 0.0f, 0.0f, 1.0f};  // 语法层
const eui::Color kColHit{0.0f, 1.0f, 0.0f, 1.0f};   // 普通命中
const eui::Color kColCur{0.0f, 0.0f, 1.0f, 1.0f};   // 当前命中

}  // namespace

REGISTER_TEST(notepad_find_findall_basic,
              "notepad_find: findAll 基本命中/非重叠/空 query/空命中", {
    const auto hits = findAll("hello\nworld\n", "l", FindOptions{});
    // h0 e1 l2 l3 o4 \n5 w6 o7 r8 l9 d10 \n11
    DOCTEST_CHECK_EQ(hits.size(), size_t(3));
    DOCTEST_CHECK_EQ(hits[0].start, size_t(2));
    DOCTEST_CHECK_EQ(hits[0].end, size_t(3));
    DOCTEST_CHECK_EQ(hits[1].start, size_t(3));
    DOCTEST_CHECK_EQ(hits[2].start, size_t(9));
    DOCTEST_CHECK(findAll("abc", "z", FindOptions{}).empty());
    DOCTEST_CHECK(findAll("abc", "", FindOptions{}).empty());   // 空 query
    DOCTEST_CHECK(findAll("", "a", FindOptions{}).empty());     // 空文本
    // 相邻命中（"aaaa" 找 "aa"）：非重叠贪心 → [0,2) [2,4)
    const auto adj = findAll("aaaa", "aa", FindOptions{});
    DOCTEST_CHECK_EQ(adj.size(), size_t(2));
    DOCTEST_CHECK_EQ(adj[1].start, size_t(2));
    return true;
})

REGISTER_TEST(notepad_find_findall_case,
              "notepad_find: 大小写 = 仅 ASCII 折叠（禁 locale；>=0x80 原样比较）", {
    DOCTEST_CHECK_EQ(starts(findAll("aBc AbC", "abc", FindOptions{})).size(),
                     size_t(2));
    DOCTEST_CHECK(
        findAll("aBc AbC", "abc", FindOptions{true, false}).empty());
    // ASCII 折叠不越界到拉丁扩展字节（C3 89 = É UTF-8 首字节，原样比较）
    const std::string latin = std::string("\xC3\x89") + "a";
    DOCTEST_CHECK(findAll(latin, "\xC3\xA9", FindOptions{}).empty());  // é ≠ É
    DOCTEST_CHECK_EQ(findAll(latin, "a", FindOptions{}).size(), size_t(1));
    return true;
})

REGISTER_TEST(notepad_find_findall_wholeword,
              "notepad_find: 全词边界（词字符 = 字母数字下划线 ∪ >=0x80 字节）", {
    const auto hits = findAll("cat catalog cat", "cat", FindOptions{false, true});
    DOCTEST_CHECK_EQ(hits.size(), size_t(2));
    DOCTEST_CHECK_EQ(hits[0].start, size_t(0));
    DOCTEST_CHECK_EQ(hits[1].start, size_t(12));
    // 下划线/数字 = 词字符
    DOCTEST_CHECK(
        findAll("cat_1 cat", "cat", FindOptions{false, true}).size() == 1);
    // CJK 字节视作词字符：query 词尾邻接 CJK 字节 → 非全词
    DOCTEST_CHECK(findAll("记事本", "记事", FindOptions{false, true}).empty());
    DOCTEST_CHECK(findAll("记事本", "记事本", FindOptions{false, true}).size() ==
                  1);
    DOCTEST_CHECK(findAll("记事", "记事", FindOptions{false, true}).size() == 1);
    return true;
})

REGISTER_TEST(notepad_find_wrap,
              "notepad_find: 环绕跳转 N-3（next 严格后继 / prev 严格前驱，回绕固定）", {
    const auto hits = findAll("aa bb aa", "aa", FindOptions{});  // {0,2} {6,8}
    DOCTEST_CHECK_EQ(nextFrom(hits, 0), size_t(1));    // 严格后继：跳过 [0,2)
    DOCTEST_CHECK_EQ(nextFrom(hits, 3), size_t(1));    // 中段仍有后继
    DOCTEST_CHECK_EQ(nextFrom(hits, 7), size_t(0));    // 末个之后回绕首个
    DOCTEST_CHECK_EQ(prevFrom(hits, 3), size_t(0));    // 严格前驱
    DOCTEST_CHECK_EQ(prevFrom(hits, 0), size_t(1));    // 首个之前回绕末个
    DOCTEST_CHECK_EQ(nextFrom({}, 0), kFindNoHit);     // 空表 = 无命中
    DOCTEST_CHECK_EQ(prevFrom({}, 0), kFindNoHit);
    return true;
})

REGISTER_TEST(notepad_find_f07_replace_one,
              "notepad_find: F-07 落点锁（下一命中 start/选区；含环绕与无下一命中）", {
    const FindOptions opts;
    // ① 基本形：替换后光标 = 下一命中 start，选区 = 该命中区间
    //   "foo bar foo" 替换 [0,3)→"X" 后 "X bar foo"：下一命中 "foo" 在字节 6
    {
        const auto r = replaceOneAt("foo bar foo", FindHit{0, 3}, "X", "foo", opts);
        DOCTEST_CHECK_EQ(r.text, std::string("X bar foo"));
        DOCTEST_CHECK_EQ(r.cursor, size_t(6));
        DOCTEST_CHECK_EQ(r.selStart, size_t(6));
        DOCTEST_CHECK_EQ(r.selEnd, size_t(9));
    }
    // ② 下一命中紧贴替换末字节（start == afterByte 也要选中）
    {
        const auto r = replaceOneAt("abab", FindHit{0, 2}, "ab", "ab", opts);
        DOCTEST_CHECK_EQ(r.text, std::string("abab"));
        DOCTEST_CHECK_EQ(r.cursor, size_t(2));
        DOCTEST_CHECK_EQ(r.selEnd, size_t(4));
    }
    // ③ 替换最后一个命中 → 环绕回首命中
    {
        const auto r = replaceOneAt("aa bb aa", FindHit{6, 8}, "X", "aa", opts);
        DOCTEST_CHECK_EQ(r.text, std::string("aa bb X"));
        DOCTEST_CHECK_EQ(r.cursor, size_t(0));
        DOCTEST_CHECK_EQ(r.selStart, size_t(0));
        DOCTEST_CHECK_EQ(r.selEnd, size_t(2));
    }
    // ④ 无下一命中（替换缩短文本且剩余命中集为空）→ 替换文本末尾零宽选区
    {
        const auto r = replaceOneAt("abcabc", FindHit{0, 6}, "x", "abcabc", opts);
        DOCTEST_CHECK_EQ(r.text, std::string("x"));
        DOCTEST_CHECK_EQ(r.cursor, size_t(1));
        DOCTEST_CHECK_EQ(r.selStart, size_t(1));
        DOCTEST_CHECK_EQ(r.selEnd, size_t(1));
    }
    // ⑤ 命中在文末 + 空替换 → 0 剩余，光标 = 替换文本末尾（= 新文末）
    {
        const auto r = replaceOneAt("xx aa", FindHit{3, 5}, "", "aa", opts);
        DOCTEST_CHECK_EQ(r.text, std::string("xx "));
        DOCTEST_CHECK_EQ(r.cursor, size_t(3));
        DOCTEST_CHECK_EQ(r.selEnd, size_t(3));
    }
    // ⑥ 命中在文末 + 非空替换 → 光标 = 替换文本末尾（= 新文末）
    {
        const auto r = replaceOneAt("xx aa", FindHit{3, 5}, "b", "aa", opts);
        DOCTEST_CHECK_EQ(r.text, std::string("xx b"));
        DOCTEST_CHECK_EQ(r.cursor, size_t(4));
        DOCTEST_CHECK_EQ(r.selEnd, size_t(4));
    }
    return true;
})

REGISTER_TEST(notepad_find_replace_all,
              "notepad_find: replaceAll 计数/单批拼接/替换文本含 query 不重扫", {
    {
        const auto r = replaceAll("hello world hello", "hello", "hi", FindOptions{});
        DOCTEST_CHECK_EQ(r.count, size_t(2));
        DOCTEST_CHECK_EQ(r.text, std::string("hi world hi"));
    }
    // 替换文本含 query：不重扫（防自举死循环，官方编辑器同口径）
    {
        const auto r = replaceAll("aaa", "a", "aa", FindOptions{});
        DOCTEST_CHECK_EQ(r.count, size_t(3));
        DOCTEST_CHECK_EQ(r.text, std::string("aaaaaa"));
    }
    DOCTEST_CHECK(replaceAll("abc", "z", "q", FindOptions{}).count == 0);
    {
        const auto r = replaceAll("abc", "z", "q", FindOptions{});
        DOCTEST_CHECK_EQ(r.text, std::string("abc"));  // 0 命中原文返回
    }
    {
        const auto r = replaceAll("cat catalog", "cat", "dog", FindOptions{false, true});
        DOCTEST_CHECK_EQ(r.count, size_t(1));  // 全词过滤 catalog
        DOCTEST_CHECK_EQ(r.text, std::string("dog catalog"));
    }
    return true;
})

REGISTER_TEST(notepad_find_merge_runs_split,
              "notepad_find: mergeRuns 命中边界切分语法段 + 命中色覆盖语法色", {
    // base 覆盖 [0,10)，命中 [3,6) → 三段：语法/命中/语法
    {
        const std::vector<eui::TextRun> base = {{0, 10, kColBase}};
        const std::vector<FindHit> hits = {{3, 6}};
        const auto runs = mergeRuns(base, hits, nullptr, "0123456789", 0, kColHit, kColCur);
        DOCTEST_CHECK_EQ(runs.size(), size_t(3));
        DOCTEST_CHECK_EQ(runs[0].byteStart, 0);
        DOCTEST_CHECK_EQ(runs[0].byteEnd, 3);
        DOCTEST_CHECK(runs[0].color.r == kColBase.r);
        DOCTEST_CHECK_EQ(runs[1].byteStart, 3);
        DOCTEST_CHECK_EQ(runs[1].byteEnd, 6);
        DOCTEST_CHECK(runs[1].color.r == kColHit.r);
        DOCTEST_CHECK_EQ(runs[2].byteStart, 6);
        DOCTEST_CHECK_EQ(runs[2].byteEnd, 10);
        DOCTEST_CHECK(runs[2].color.r == kColBase.r);
    }
    // 当前命中合成色：cur 指向 {8,10)} → 普通命中 hitColor / 当前 curColor
    {
        const std::vector<FindHit> hits = {{3, 6}, {8, 10}};
        const auto runs =
            mergeRuns({}, hits, &hits[1], "0123456789", 0, kColHit, kColCur);
        DOCTEST_CHECK_EQ(runs.size(), size_t(2));
        DOCTEST_CHECK(runs[0].color.b == kColHit.b);
        DOCTEST_CHECK(runs[1].color.b == kColCur.b);
        DOCTEST_CHECK_EQ(runs[1].byteStart, 8);
    }
    // 相邻同色命中合并（"aaaa" 找 "aa"）→ 单段
    {
        const std::vector<FindHit> hits = {{0, 2}, {2, 4}};
        const auto runs = mergeRuns({}, hits, nullptr, "aaaa", 0, kColHit, kColCur);
        DOCTEST_CHECK_EQ(runs.size(), size_t(1));
        DOCTEST_CHECK_EQ(runs[0].byteStart, 0);
        DOCTEST_CHECK_EQ(runs[0].byteEnd, 4);
    }
    // 行裁剪：行首字节 5，命中 [3,8) 跨行 → 裁成 [5,8)；base 语法段裁到行内
    {
        const std::vector<eui::TextRun> base = {{0, 20, kColBase}};
        const std::vector<FindHit> hits = {{3, 8}};
        const auto runs =
            mergeRuns(base, hits, nullptr, "world", 5, kColHit, kColCur);
        DOCTEST_CHECK_EQ(runs.size(), size_t(2));
        DOCTEST_CHECK_EQ(runs[0].byteStart, 5);
        DOCTEST_CHECK_EQ(runs[0].byteEnd, 8);
        DOCTEST_CHECK(runs[0].color.r == kColHit.r);
        DOCTEST_CHECK_EQ(runs[1].byteStart, 8);
        DOCTEST_CHECK_EQ(runs[1].byteEnd, 10);
        DOCTEST_CHECK(runs[1].color.r == kColBase.r);
    }
    // 无 base 覆盖的命中段单独成段；行外命中剔除
    {
        const std::vector<FindHit> hits = {{2, 4}, {20, 22}};
        const auto runs = mergeRuns({}, hits, nullptr, "abcd", 0, kColHit, kColCur);
        DOCTEST_CHECK_EQ(runs.size(), size_t(1));
        DOCTEST_CHECK_EQ(runs[0].byteEnd, 4);
    }
    DOCTEST_CHECK(mergeRuns({}, {}, nullptr, "abc", 0, kColHit, kColCur).empty());
    return true;
})
