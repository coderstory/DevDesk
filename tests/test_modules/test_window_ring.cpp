// tests/test_modules/test_window_ring.cpp
// 池168/need184③：窗口 ring 最大化隐藏分支无头断言（接口层契约）。
// 探针无法真最大化主窗口（锁屏交互桌面受限，GLFW 最大化对合成窗口行为不可控），
// 分支判据在单测层锁死：maximized=false 恒绘 ring 三件套；true 一个元素都不落。
// 真机最大化态由收口 QA 走视觉复查（ring 消失 + 直角窗缘）。
#include "doctest.h"
#include "core/theme.h"
#include "ui_kit/window_ring.h"
#include <eui/dsl.h>

REGISTER_TEST(window_ring_maximized_branch,
              "ui_kit window_ring: 最大化隐藏分支——true 不落任何 ring 元素（need184③）", {
    const auto& t = core::theme::current();
    const float w = 1200.0f, h = 760.0f;   // 常规逻辑窗（≤760 取证口径）

    // 常态：底环 + 四边绿段首段齐落
    {
        eui::Ui ui;
        ui.begin("ring_normal");
        ui_kit::drawWindowRing(ui, w, h, t, false);
        ui.end();
        ui.layout(w, h);
        DOCTEST_CHECK(ui.find("app.ring.base") != nullptr);
        DOCTEST_CHECK(ui.find("app.ring.top.0") != nullptr);
        DOCTEST_CHECK(ui.find("app.ring.left.0") != nullptr);
    }
    // 最大化：分支早退，整个 ring（含底环与绿段）都不存在
    {
        eui::Ui ui;
        ui.begin("ring_maximized");
        ui_kit::drawWindowRing(ui, w, h, t, true);
        ui.end();
        ui.layout(w, h);
        DOCTEST_CHECK(ui.find("app.ring.base") == nullptr);
        DOCTEST_CHECK(ui.find("app.ring.top.0") == nullptr);
        DOCTEST_CHECK(ui.find("app.ring.left.0") == nullptr);
    }
    return true;
})
