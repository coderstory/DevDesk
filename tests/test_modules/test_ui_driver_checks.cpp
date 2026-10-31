// tests/test_modules/test_ui_driver_checks.cpp
// 债-18 UI 驱动入口：driver_checks.h 无头单测——命中判定四态 + 几何谓词 +
// 文本匹配。树视图喂合成数据（不 compose、不开窗）。
#include "doctest.h"
#include "ui_driver/driver_checks.h"

using namespace ui_drive;
using core::dsl::Element;

namespace {

// 合成树：target 为根，child 为其子（命中判定「目标子树」分支）；
// outsider 为独立根（截胡分支）。
TreeView makeSampleTree() {
    static Element target;
    static Element child;
    static Element outsider;
    target.id = "app.page.target";
    child.id = "app.page.target.btn.bg";
    outsider.id = "app.page.toast";
    TreeView tree;
    tree.byId[target.id] = &target;
    tree.byId[child.id] = &child;
    tree.byId[outsider.id] = &outsider;
    tree.parentOf[target.id] = "app.page";
    tree.parentOf[child.id] = target.id;
    tree.parentOf[outsider.id] = "app.page";
    tree.leaves = {&child, &outsider};
    return tree;
}

} // namespace

REGISTER_TEST(uiDriverChecksHitVerdictFourStates, "uidrv/checks: 命中判定四态", {
    const TreeView tree = makeSampleTree();
    const std::string target = "app.page.target";
    DOCTEST_CHECK(classifyHit(tree, target, "app.page.target") == HitVerdict::Target);
    DOCTEST_CHECK(classifyHit(tree, target, "app.page.target.btn.bg") ==
                  HitVerdict::Descendant);  // 组件内部 hit 区都是这种
    DOCTEST_CHECK(classifyHit(tree, target, "app.page.toast") == HitVerdict::Hijacked);
    DOCTEST_CHECK(classifyHit(tree, target, "") == HitVerdict::Empty);
    // 未知 id（不在树里）= 截胡档（报告会显示它的 id 供诊断）
    DOCTEST_CHECK(classifyHit(tree, target, "app.other.stuff") == HitVerdict::Hijacked);
    return true;
})

REGISTER_TEST(uiDriverChecksGeometryWithinWindow, "uidrv/checks: within-window 几何谓词（相切/窗内外）", {
    // 窗口 1200x1000
    DOCTEST_CHECK(rectWithinWindow({0.0f, 0.0f, 1200.0f, 1000.0f}, 1200.0f, 1000.0f));
    DOCTEST_CHECK(rectWithinWindow({10.0f, 20.0f, 100.0f, 50.0f}, 1200.0f, 1000.0f));
    // 边界相切（右/下边贴齐窗口边）算在内
    DOCTEST_CHECK(rectWithinWindow({1100.0f, 950.0f, 100.0f, 50.0f}, 1200.0f, 1000.0f));
    // 越右 / 越下 / 负坐标 = 出窗
    DOCTEST_CHECK_FALSE(rectWithinWindow({1101.0f, 0.0f, 100.0f, 50.0f}, 1200.0f, 1000.0f));
    DOCTEST_CHECK_FALSE(rectWithinWindow({0.0f, 951.0f, 100.0f, 50.0f}, 1200.0f, 1000.0f));
    DOCTEST_CHECK_FALSE(rectWithinWindow({-1.0f, 0.0f, 100.0f, 50.0f}, 1200.0f, 1000.0f));
    return true;
})

REGISTER_TEST(uiDriverChecksGeometryIntersects, "uidrv/checks: intersects 谓词（相交/含于/分离/相切）", {
    // 相交
    DOCTEST_CHECK(rectIntersectsRect({0.0f, 0.0f, 10.0f, 10.0f}, {5.0f, 5.0f, 10.0f, 10.0f}));
    // 含于（子集也算相交）
    DOCTEST_CHECK(rectIntersectsRect({0.0f, 0.0f, 100.0f, 100.0f}, {10.0f, 10.0f, 5.0f, 5.0f}));
    DOCTEST_CHECK(rectIntersectsRect({10.0f, 10.0f, 5.0f, 5.0f}, {0.0f, 0.0f, 100.0f, 100.0f}));
    // 分离（x 或 y 方向都不碰）
    DOCTEST_CHECK_FALSE(rectIntersectsRect({0.0f, 0.0f, 10.0f, 10.0f}, {20.0f, 0.0f, 10.0f, 10.0f}));
    DOCTEST_CHECK_FALSE(rectIntersectsRect({0.0f, 0.0f, 10.0f, 10.0f}, {0.0f, 20.0f, 10.0f, 10.0f}));
    // 边相切 = 不相交（与 probe_harness::detail::framesIntersect 同口径）
    DOCTEST_CHECK_FALSE(rectIntersectsRect({0.0f, 0.0f, 10.0f, 10.0f}, {10.0f, 0.0f, 10.0f, 10.0f}));
    return true;
})

REGISTER_TEST(uiDriverChecksElementExists, "uidrv/checks: exists/missing 条件评估", {
    const TreeView tree = makeSampleTree();
    DOCTEST_CHECK(elementExists(tree, "app.page.target"));
    DOCTEST_CHECK(elementExists(tree, "app.page.toast"));
    DOCTEST_CHECK_FALSE(elementExists(tree, "app.page.absent"));
    DOCTEST_CHECK_FALSE(elementExists(tree, ""));  // 空 id 恒 false（防假绿）
    return true;
})

REGISTER_TEST(uiDriverChecksTextPresent, "uidrv/checks: 文本 contains/equals（定点/全页扫描）", {
    static Element card;
    card.id = "app.page.card";
    card.text = "待办：缴费提醒";
    static Element other;
    other.id = "app.page.other";
    other.text = "无关键文本";
    TreeView tree = makeSampleTree();
    tree.byId[card.id] = &card;
    tree.byId[other.id] = &other;
    tree.leaves.push_back(&card);
    tree.leaves.push_back(&other);

    // contains
    DOCTEST_CHECK(textPresent(tree, card.id, TextOp::Contains, "缴费"));
    // equals（全值相等才真）
    DOCTEST_CHECK(textPresent(tree, card.id, TextOp::Equals, "待办：缴费提醒"));
    DOCTEST_CHECK_FALSE(textPresent(tree, card.id, TextOp::Equals, "缴费"));
    // 定点 miss：元素文本不含
    DOCTEST_CHECK_FALSE(textPresent(tree, other.id, TextOp::Contains, "缴费"));
    // 全页扫描（id 空）：任一叶子命中即真
    DOCTEST_CHECK(textPresent(tree, "", TextOp::Contains, "缴费"));
    DOCTEST_CHECK(textPresent(tree, "", TextOp::Contains, "无关键"));
    DOCTEST_CHECK_FALSE(textPresent(tree, "", TextOp::Contains, "不存在的字样"));
    // id 不在树里 → false
    DOCTEST_CHECK_FALSE(textPresent(tree, "app.page.absent", TextOp::Contains, "缴费"));
    // 空期望文本恒 false（防 "assert text=''" 恒真）
    DOCTEST_CHECK_FALSE(textPresent(tree, card.id, TextOp::Contains, ""));
    return true;
})
