// tests/test_modules/test_window_ring.cpp
// 池168/need184③：窗口细黑边最大化隐藏分支无头断言（接口层契约）。
// 2026-11-04 老板撤销绿白虚框改细线黑边：常态=仅 app.ring.base（黑描边），
// 绿段 id（top.N/left.N）已随撤销移除、不得再出现；maximized=true 一个元素都不落。
// 探针无法真最大化主窗口（锁屏交互桌面受限，GLFW 最大化对合成窗口行为不可控），
// 分支判据在单测层锁死。真机最大化态由收口 QA 走视觉复查（边框消失 + 直角窗缘）。
#include "doctest.h"
#include "core/theme.h"
#include "ui_kit/window_ring.h"
#include <eui/dsl.h>

REGISTER_TEST(window_ring_maximized_branch,
              "ui_kit window_ring: 细黑边最大化隐藏分支——常态仅 base 黑描边、绿段 id 已撤销（need184③）", {
    const auto& t = core::theme::current();
    const float w = 1200.0f, h = 760.0f;   // 常规逻辑窗（≤760 取证口径）

    // 常态：仅细黑边底框；撤销的绿段 id 必须不存在
    {
        eui::Ui ui;
        ui.begin("ring_normal");
        ui_kit::drawWindowRing(ui, w, h, t, false);
        ui.end();
        ui.layout(w, h);
        const auto* base = ui.find("app.ring.base");
        DOCTEST_REQUIRE(base != nullptr);
        DOCTEST_CHECK(base->border.color.r < 0.05f);   // 黑（撤销绿白虚框）
        DOCTEST_CHECK(base->border.color.g < 0.05f);
        DOCTEST_CHECK(base->border.color.b < 0.05f);
        DOCTEST_CHECK(base->border.width <= ui_kit::kWindowRingBorder);  // 细线
        DOCTEST_CHECK(ui.find("app.ring.top.0") == nullptr);   // 绿段已撤销
        DOCTEST_CHECK(ui.find("app.ring.left.0") == nullptr);
    }
    // 最大化：分支早退，边框整体不存在
    {
        eui::Ui ui;
        ui.begin("ring_maximized");
        ui_kit::drawWindowRing(ui, w, h, t, true);
        ui.end();
        ui.layout(w, h);
        DOCTEST_CHECK(ui.find("app.ring.base") == nullptr);
    }
    return true;
})
