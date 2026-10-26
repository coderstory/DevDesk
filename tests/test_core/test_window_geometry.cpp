// tests/test_core/test_window_geometry.cpp
// 窗体记忆几何账本单测（UAT6「记住窗体大小」）。两层：
// ① 账本行为——最大化帧只刷标志不记尺寸（= 保还原态，防「记住的其实是全屏值」）、
//   非最大化帧更新尺寸、播种兜底启动即最大化会话、脏帧不落账、快照取整；
// ② 生产接线源码契约——main.cpp 不进测试二进制，无头套件只能读源码钉接线
//   （先例 test_splash_contract.cpp）：恢复链经 restoreInitialWindow、退出经
//   atexit 落盘、compose 每帧经 noteFrame 记账、SettingsStore 三键写读。
#include "doctest.h"
#include "core/window_geometry.h"
#include "core/window_layout.h"  // 恢复链（账本快照的消费侧，往返联调用）

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace {

// 源码契约读源文件（先例：test_splash_contract.cpp）。
// 根定位走 test_temp::repoRoot()（doctest.h）：__FILE__ 相对形态 + ctest 的
// build/tests cwd 下，裸 __FILE__ 上溯三级会指向不存在的路径 → 空串假红。
std::string readSourceFile(const std::string& relPath) {
    namespace fs = std::filesystem;
    const fs::path sourceRoot = test_temp::repoRoot();
    const fs::path target = sourceRoot / relPath;
    std::ifstream in(target, std::ios::binary);
    if (!in.is_open()) return std::string{};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

REGISTER_TEST(window_geometry_ledger_baseline, "window_geometry: 账本空态 + 播种成快照", {
    core::window_geometry::resetForTest();
    // 空态：未播种也没见过帧 → valid=false（退出落盘侧据此「不覆盖上次记录」）
    DOCTEST_CHECK_FALSE(core::window_geometry::current().valid);
    // 播种（启动请求值）→ 快照可用且取整回传
    core::window_geometry::seedRestoreSize(1600, 1000);
    const auto seeded = core::window_geometry::current();
    DOCTEST_CHECK(seeded.valid);
    DOCTEST_CHECK_EQ(seeded.logicalWidth, 1600);
    DOCTEST_CHECK_EQ(seeded.logicalHeight, 1000);
    DOCTEST_CHECK_FALSE(seeded.maximized);
    core::window_geometry::resetForTest();
    return true;
})

REGISTER_TEST(window_geometry_maximized_keeps_restore_size,
              "window_geometry: 最大化帧不记尺寸（保还原态），非最大化帧更新（取整）", {
    core::window_geometry::resetForTest();
    core::window_geometry::seedRestoreSize(1600, 1000);
    // 最大化帧：铺满值（2540×1320）不得进账本，只刷最大化标志
    core::window_geometry::noteFrame(2540.0f, 1320.0f, true);
    auto snap = core::window_geometry::current();
    DOCTEST_CHECK(snap.valid);
    DOCTEST_CHECK(snap.maximized);
    DOCTEST_CHECK_EQ(snap.logicalWidth, 1600);  // 还原态尺寸保留
    DOCTEST_CHECK_EQ(snap.logicalHeight, 1000);
    // 非最大化帧：更新尺寸（四舍五入）并清标志
    core::window_geometry::noteFrame(1234.6f, 777.4f, false);
    snap = core::window_geometry::current();
    DOCTEST_CHECK_FALSE(snap.maximized);
    DOCTEST_CHECK_EQ(snap.logicalWidth, 1235);
    DOCTEST_CHECK_EQ(snap.logicalHeight, 777);
    core::window_geometry::resetForTest();
    return true;
})

REGISTER_TEST(window_geometry_junk_frames_ignored, "window_geometry: 脏帧（0/负/NaN）不落账不伪造记录", {
    core::window_geometry::resetForTest();
    // 全程只有脏帧 → 账本保持无记录（不能因一帧 0/NaN 把「无记录」翻成有）
    core::window_geometry::noteFrame(0.0f, 600.0f, false);
    core::window_geometry::noteFrame(-5.0f, 700.0f, false);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    core::window_geometry::noteFrame(nan, 600.0f, false);
    core::window_geometry::noteFrame(800.0f, nan, false);
    DOCTEST_CHECK_FALSE(core::window_geometry::current().valid);
    // 脏帧之后播种仍有效
    core::window_geometry::seedRestoreSize(1000, 640);
    DOCTEST_CHECK(core::window_geometry::current().valid);
    core::window_geometry::resetForTest();
    return true;
})

REGISTER_TEST(window_geometry_seed_does_not_override_frames,
              "window_geometry: 已有帧记录时播种不回退（播种只兜全程最大化会话）", {
    core::window_geometry::resetForTest();
    core::window_geometry::noteFrame(800.0f, 600.0f, false);
    core::window_geometry::seedRestoreSize(1600, 1000);  // 迟到的播种不得覆盖帧记录
    const auto snap = core::window_geometry::current();
    DOCTEST_CHECK_EQ(snap.logicalWidth, 800);
    DOCTEST_CHECK_EQ(snap.logicalHeight, 600);
    core::window_geometry::resetForTest();
    return true;
})

REGISTER_TEST(window_geometry_snapshot_to_restore_chain_roundtrip,
              "window_geometry: 快照 → SavedWindowGeometry → restoreInitialWindow 无头往返", {
    // 消费侧联调：账本快照喂恢复链——同屏档原样恢复、最大化标志透传（保存/恢复
    // 往返的纯计算段；DB 往返由 test_settings.cpp 钉，main.cpp 接线由下方源码契约钉）
    core::window_geometry::resetForTest();
    core::window_geometry::seedRestoreSize(1500, 900);
    core::window_geometry::noteFrame(1500.0f, 900.0f, true);  // 最大化退出形态
    const auto snap = core::window_geometry::current();
    const core::window_layout::SavedWindowGeometry saved{
        snap.logicalWidth, snap.logicalHeight, snap.maximized};
    const core::window_layout::InitialWindowDefaults defaults{1600, 1000, 1000, 640};
    const auto plan = core::window_layout::restoreInitialWindow(
        saved, defaults, core::window_layout::WorkArea{0, 0, 1920, 1080}, 1.0f,
        core::window_layout::frameInsetsForScale(1.0f));
    DOCTEST_CHECK(plan.fromSavedRecord);
    DOCTEST_CHECK_EQ(plan.initial.width, 1500);
    DOCTEST_CHECK_EQ(plan.initial.height, 900);
    DOCTEST_CHECK(plan.initial.maximize);
    core::window_geometry::resetForTest();
    return true;
})

REGISTER_TEST(window_geometry_wiring_contract,
              "window_geometry: 生产接线源码契约（恢复链/播种/退出落盘/compose 记账）", {
    // main.cpp：恢复决策必须经 restoreInitialWindow（禁旁路直调 decideInitialWindow
    // 绕开记忆）、启动播种、atexit 落盘（晚于 armExitHook 注册 → LIFO 先于 stopAll）
    const std::string main = readSourceFile("src/main.cpp");
    DOCTEST_CHECK(!main.empty());
    DOCTEST_CHECK(contains(main, "restoreInitialWindow("));
    DOCTEST_CHECK(contains(main, "seedRestoreSize("));
    DOCTEST_CHECK(contains(main, "saveWindowGeometryOnExit"));
    DOCTEST_CHECK(contains(main, "std::atexit([] { saveWindowGeometryOnExit(); })"));
    // app_sidebar.cpp：compose 每帧记账，且在 splash 早退之前（splash 帧也是主窗帧）
    const std::string sidebar = readSourceFile("src/app_sidebar.cpp");
    DOCTEST_CHECK(!sidebar.empty());
    DOCTEST_CHECK(contains(sidebar, "window_geometry::noteFrame("));
    DOCTEST_CHECK(contains(sidebar, "app::mainWindowMaximized()"));
    // settings_config.cpp：三键进 save 行清单（缺一行 = 退出落盘永远写不出该键）
    const std::string cfg = readSourceFile("src/module_settings/settings_config.cpp");
    DOCTEST_CHECK(!cfg.empty());
    DOCTEST_CHECK(contains(cfg, "kKeyWindowRestoreWidth"));
    DOCTEST_CHECK(contains(cfg, "kKeyWindowRestoreHeight"));
    DOCTEST_CHECK(contains(cfg, "kKeyWindowMaximized"));
    return true;
})
