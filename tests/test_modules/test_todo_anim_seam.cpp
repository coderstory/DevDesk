// tests/test_modules/test_todo_anim_seam.cpp
// 池15 归档折叠动画观察缝：快照查询 + 「在途重复触发收编回插」帧边界断言。
// 守卫实义以方案卡 §一 为准（H20 权威口径）：在途重触发被状态机收编为单一动画段
//（目标翻转、进度连续不回跳），完成后瞬时重触发放行新动画。
// 触发入口走真实 UI：折叠态起步展开 = 归档列整列命中层 todo.col.N.expand.bg onClick
//（列下标动态扫描不猜序号）；反向重触发 = Esc（onKeyEvent 无弹窗 + 展开态分支）。
// 帧推进 = 无头 Frame::refresh()（一帧一次 advanceArchiveAnim 壁钟推进）。
// 时序纪律：条件轮询（等状态迁移）+ 容差带，禁固定 sleep（PRD 自查清单口径）。
#include "doctest.h"
#include "test_support.h"

#include "core/module_interface.h"
#include "module_todo/todo_observability.h"

#include <eui/dsl.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

extern "C" ::core::Module* create_todo_module();

namespace {

using module::todo::obs::archiveAnimSnapshot;
using module::todo::obs::setArchiveAnimProvider;

constexpr const char* kDbPrefix = "devdesk_test_todoanim_";
constexpr int kPollTimeoutMs = 2000;   // ≫ 动画上限 240ms；CI 慢机裕量

// 卸槽守卫：任何退出路径（含断言 fail-fast 早退）都切断 provider 对已析构 module 的
// 悬垂。必须先于任何断言构造（toggle_bridge 同款纪律）。
struct SeamGuard {
    SeamGuard() = default;   // deleted 拷贝会抑制隐式默认构造，显式补回（toggle_bridge 同款）
    ~SeamGuard() { setArchiveAnimProvider(nullptr); }
    SeamGuard(const SeamGuard&) = delete;
    SeamGuard& operator=(const SeamGuard&) = delete;
};

// 无头 compose 一帧（test_todo_ui Frame 同构；本文件自带避免跨 TU 依赖匿名设施）
struct Frame {
    eui::Ui ui;
    eui::Screen screen;
    ::core::Module* mod = nullptr;

    explicit Frame(::core::Module* m) : mod(m) {
        screen.width = 1600.0f;
        screen.height = 1000.0f;
        refresh();
    }
    void refresh() {
        ui.begin("todo_anim_frame");
        mod->compose(ui, screen);
        ui.end();
    }
};

// 归档列折叠态整列命中层（列下标动态扫描：固定列序是 schema 内部事实，测试不猜）
const core::dsl::Element* findArchiveExpandBg(eui::Ui& ui) {
    for (int n = 0; n < 16; ++n) {
        if (const auto* el = ui.find("todo.col." + std::to_string(n) + ".expand.bg")) {
            return el;
        }
    }
    return nullptr;
}

// 折叠态起步展开动画（真实入口：整列命中层 onClick）。起步前 Frame 已 compose ≥1 帧
//（折叠态才渲染 .expand.bg；Frame 构造即首帧）。
bool startExpandViaUi(Frame& f) {
    const auto* el = findArchiveExpandBg(f.ui);
    if (el == nullptr || el->onClick == nullptr) return false;
    el->onClick();
    return true;
}

eui::KeyEvent escPress() {
    eui::KeyEvent e;
    e.key = eui::InputKey::Escape;
    e.action = eui::KeyAction::Press;
    return e;
}

// 条件轮询：每轮 compose 一帧（= 一次 advanceArchiveAnim），至谓词真或超时。
// 超时回 false，由 DOCTEST_REQUIRE fail-fast 红；末次快照进诊断输出。
template <typename Pred>
bool pollFrame(Frame& f, Pred&& pred) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kPollTimeoutMs);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) {
            const auto s = archiveAnimSnapshot();
            std::cerr << "[todo_anim] 轮询超时: animating=" << s.animating
                      << " target=" << s.targetExpanded
                      << " progress=" << s.progress << "\n";
            return false;
        }
        f.refresh();
    }
    return true;
}

// 装配样板：scratch 库（注册 app db）+ 真模块 + 首帧。调用方持 SeamGuard。
struct Rig {
    test_support::TestEnv env;
    std::unique_ptr<::core::Module> mod;
    Frame frame;

    explicit Rig(const char* tag)
        : env(kDbPrefix, tag, /*registerAppDb=*/true),
          mod(create_todo_module()),
          frame(mod.get()) {   // 声明序 env → mod → frame：frame 依赖 mod 指针
        mod->onStart();
        frame.refresh();   // onStart 后重 compose：成员首帧在 onStart 前，列尚未加载
    }
};

} // namespace

// ---- F-01 空态自查：无提供方 → 空闲快照，不崩溃 ----
REGISTER_TEST(todo_anim_idle_query_without_provider,
              "todo_anim: 未装配模块时查询 → 空闲快照不崩溃", {
    SeamGuard guard;
    setArchiveAnimProvider(nullptr);   // 模拟未装配（缝的复位公开语义）
    const auto snap = archiveAnimSnapshot();
    DOCTEST_CHECK(!snap.animating);
    DOCTEST_CHECK(!snap.targetExpanded);
    DOCTEST_CHECK_EQ(snap.progress, 0.0f);
    return true;
})

// ---- F-01 在途/完成派生态 + progress 推进 ----
REGISTER_TEST(todo_anim_snapshot_states,
              "todo_anim: 在途快照 animating/目标/progress 推进；完成落位可判", {
    SeamGuard guard;
    Rig rig("states");
    Frame& f = rig.frame;

    DOCTEST_REQUIRE(startExpandViaUi(f));
    const auto snap = archiveAnimSnapshot();
    DOCTEST_CHECK(snap.animating);          // 进行中
    DOCTEST_CHECK(snap.targetExpanded);     // 目标 = 展开
    DOCTEST_CHECK(snap.progress < 0.1f);    // 刚起步

    f.refresh();
    const auto snap2 = archiveAnimSnapshot();
    DOCTEST_CHECK(snap2.progress >= snap.progress);   // 壁钟推进单调

    // 已完成落位 = !animating 且 progress 贴目标（状态机无「从未播过」记忆，派生判定）
    DOCTEST_REQUIRE(pollFrame(f, [] {
        const auto s = archiveAnimSnapshot();
        return !s.animating && s.progress >= 0.999f;
    }));
    return true;
})

// ---- F-03 边界1 触发瞬间：行程为零的反向重触发 → 直接落位不播空动画 ----
//（触发瞬间进度仍在起点，反向回插行程=0，由 dist 早退守卫收口；与 30%/收尾的
//  「收编为在途段」边界互补，两会红靶点分别对应 todo_module.cpp 的早退块与回插行）
REGISTER_TEST(todo_anim_retrigger_at_start,
              "todo_anim: 触发瞬间反向重触发 → 零行程直接落位，目标翻转进度未动", {
    SeamGuard guard;
    Rig rig("start");
    Frame& f = rig.frame;

    DOCTEST_REQUIRE(startExpandViaUi(f));
    DOCTEST_CHECK(archiveAnimSnapshot().animating);

    // 触发瞬间（未推进任何帧）反向重触发
    DOCTEST_CHECK(rig.mod->onKeyEvent(escPress()));
    const auto snap = archiveAnimSnapshot();
    DOCTEST_CHECK(!snap.animating);          // 零行程不播空动画（dist 早退守卫）
    DOCTEST_CHECK(!snap.targetExpanded);     // 目标已翻转为折叠
    DOCTEST_CHECK_EQ(snap.progress, 0.0f);   // 进度未动
    return true;
})

// ---- F-03 边界2 30% 处重触发（会红主载体：守卫破坏 → animating 断言红）----
REGISTER_TEST(todo_anim_retrigger_at_30pct,
              "todo_anim: 30% 处反向重触发 → 仍单一在途段、进度未回零", {
    SeamGuard guard;
    Rig rig("p30");
    Frame& f = rig.frame;

    DOCTEST_REQUIRE(startExpandViaUi(f));
    DOCTEST_REQUIRE(pollFrame(f, [] {
        const auto s = archiveAnimSnapshot();
        return s.animating && s.progress >= 0.25f && s.progress <= 0.45f;
    }));
    const float p1 = archiveAnimSnapshot().progress;

    DOCTEST_CHECK(rig.mod->onKeyEvent(escPress()));
    const auto snap = archiveAnimSnapshot();
    DOCTEST_CHECK(snap.animating);        // 收编为在途反向段（回插起点被破坏 → 早退 false → 红）
    DOCTEST_CHECK(snap.progress > 0.2f);  // 未瞬跳回零（同上红载体）
    DOCTEST_CHECK(!snap.targetExpanded);

    DOCTEST_REQUIRE(pollFrame(f, [] {
        const auto s = archiveAnimSnapshot();
        return !s.animating && s.progress <= 0.01f;
    }));
    (void)p1;
    return true;
})

// ---- F-03 边界3 收尾（≥95%）重触发：守卫仍消化 ----
REGISTER_TEST(todo_anim_retrigger_near_end,
              "todo_anim: 收尾 ≥95% 重触发 → 仍单一在途段、进度未回零", {
    SeamGuard guard;
    Rig rig("end");
    Frame& f = rig.frame;

    DOCTEST_REQUIRE(startExpandViaUi(f));
    DOCTEST_REQUIRE(pollFrame(f, [] {
        const auto s = archiveAnimSnapshot();
        return s.animating && s.progress >= 0.95f;   // easeOutCubic 尾部慢，窗口宽裕
    }));

    DOCTEST_CHECK(rig.mod->onKeyEvent(escPress()));
    const auto snap = archiveAnimSnapshot();
    DOCTEST_CHECK(snap.animating);        // 最后一帧前守卫仍拒（同 30% 红载体）
    DOCTEST_CHECK(snap.progress > 0.9f);
    DOCTEST_CHECK(!snap.targetExpanded);

    DOCTEST_REQUIRE(pollFrame(f, [] {
        const auto s = archiveAnimSnapshot();
        return !s.animating && s.progress <= 0.01f;
    }));
    return true;
})

// ---- F-03 边界4 刚完成瞬时重触发放行新动画 ----
REGISTER_TEST(todo_anim_retrigger_after_done,
              "todo_anim: 刚完成瞬时重触发 → 放行新动画（目标翻转、起步推进）", {
    SeamGuard guard;
    Rig rig("done");
    Frame& f = rig.frame;

    DOCTEST_REQUIRE(startExpandViaUi(f));
    DOCTEST_REQUIRE(pollFrame(f, [] {
        const auto s = archiveAnimSnapshot();
        return !s.animating && s.progress >= 0.999f;   // 已完成落位
    }));

    DOCTEST_CHECK(rig.mod->onKeyEvent(escPress()));
    const auto snap = archiveAnimSnapshot();
    DOCTEST_CHECK(snap.animating);           // 新动画放行
    DOCTEST_CHECK(!snap.targetExpanded);     // 目标翻转为折叠
    DOCTEST_CHECK(snap.progress > 0.5f);     // 从全展开处起步

    DOCTEST_REQUIRE(pollFrame(f, [] {
        const auto s = archiveAnimSnapshot();
        return !s.animating && s.progress <= 0.01f;
    }));
    return true;
})

// ---- F-01 错误自查：onStop 卸载（闸语义）→ 查询回空闲 ----
REGISTER_TEST(todo_anim_idle_query_after_stop,
              "todo_anim: onStop 后查询 → 空闲快照（provider 卸载）", {
    SeamGuard guard;
    Rig rig("stop");
    DOCTEST_REQUIRE(startExpandViaUi(rig.frame));
    DOCTEST_CHECK(archiveAnimSnapshot().animating);

    rig.mod->onStop();
    const auto snap = archiveAnimSnapshot();
    DOCTEST_CHECK(!snap.animating);
    DOCTEST_CHECK(!snap.targetExpanded);
    DOCTEST_CHECK_EQ(snap.progress, 0.0f);
    return true;
})
