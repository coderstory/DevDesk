// tests/test_modules/test_ui_kit_jelly.cpp
// need201 果冻 v2：jellyPose 时间线无头断言（纯函数吃 elapsedMs，不碰 clock）。
//
// 基线照抄简报（定稿勿自创）：
//   P1 压扁 [0,90]     (1,1)→(1.04,0.90) easeOutQuad，radius 12→10，w=0
//   P2 拉伸 (90,240]   →(0.96,1.07) easeInOut，radius→17，扭摆叠加
//   P3 回摆 (240,420]  →(1.02,0.99) 线性（基线未指定缓动），radius→12
//   P4 回正 (420,560)  →(1,1) easeOutCubic，radius 保持 12
//   扭摆 P2 起：w(t)=0.04·sin(2π·2.2·t)·exp(−3.5t)（t 秒，自 hover 起算），
//   scaleX=s·(1+w)、scaleY=s·(1−w)（差分反相）；P1 期 w=0（压扁已含水平鼓出 1.04）。
// 事故防线：关键帧漂移（压扁/拉伸幅度被误改）、扭摆差分同相（左右扭摆变呼吸）、
// active 窗口错位（zIndex 盖过邻卡窗口与时间线脱节）。
#include "doctest.h"
#include "core/theme.h"
#include "ui_kit/jelly.h"

#include <chrono>
#include <cmath>
#include <ratio>

namespace {

// 浮点近似（关键帧容差 0.01：t=240 落在扭摆第 1 周期 sin 相位上，合成值离
// 基线关键帧约 0.003，0.01 容差既盖住浮点噪声又不放过参数漂移）
bool nearF(float a, float b, float tol = 0.01f) {
    return std::fabs(a - b) <= tol;
}

// 基线插值的测试侧复算（spec 公式，非 jelly.h 内部实现）——供扭摆差分反相断言
float specEaseOutQuad(float p) { return 1.0f - (1.0f - p) * (1.0f - p); }
float specEaseInOutQuad(float p) {
    return p < 0.5f ? 2.0f * p * p
                    : 1.0f - (-2.0f * p + 2.0f) * (-2.0f * p + 2.0f) * 0.5f;
}

}  // namespace

// 关键帧：t=0 常态起表 / t=90 压扁到位 / t=240 拉伸峰值 / t=560 回正常态。
// 事故：时间线参数被误改（幅度、相位端点）后 hover 观感与设计基线脱节。
REGISTER_TEST(ui_kit_jelly_pose_keyframes,
              "ui_kit jelly: jellyPose 关键帧 t=0/90/240/560 命中基线值", {
    const ui_kit::JellyPose p0 = ui_kit::jellyPose(0.0f);
    DOCTEST_CHECK(nearF(p0.sx, 1.0f, 0.005f));
    DOCTEST_CHECK(nearF(p0.sy, 1.0f, 0.005f));
    DOCTEST_CHECK(nearF(p0.radius, 12.0f, 0.005f));

    const ui_kit::JellyPose p90 = ui_kit::jellyPose(90.0f);
    DOCTEST_CHECK(nearF(p90.sx, 1.04f, 0.005f));   // P1 右端点闭：w=0 精确命中
    DOCTEST_CHECK(nearF(p90.sy, 0.90f, 0.005f));
    DOCTEST_CHECK(nearF(p90.radius, 10.0f, 0.005f));

    const ui_kit::JellyPose p240 = ui_kit::jellyPose(240.0f);
    DOCTEST_CHECK(nearF(p240.sx, 0.96f));          // 扭摆已在叠加，容差 0.01
    DOCTEST_CHECK(nearF(p240.sy, 1.07f));
    DOCTEST_CHECK(nearF(p240.radius, 17.0f, 0.005f));  // radius 不吃扭摆

    const ui_kit::JellyPose p560 = ui_kit::jellyPose(560.0f);
    DOCTEST_CHECK(nearF(p560.sx, 1.0f, 0.005f));   // 回正常态 12/1.0（非旧版 1.02）
    DOCTEST_CHECK(nearF(p560.sy, 1.0f, 0.005f));
    DOCTEST_CHECK(nearF(p560.radius, 12.0f, 0.005f));
    return true;
})

// P1 无扭摆（w=0）+ P2 扭摆差分反相：wx=wxBase 同幅、wy 反号。
// 事故：扭摆误挂成 scaleX/scaleY 同向（呼吸而非扭摆），或 P1 就开始抖（压扁被污染）。
REGISTER_TEST(ui_kit_jelly_pose_wobble_antiphase,
              "ui_kit jelly: 扭摆 P1 期 w=0，P2 期 scaleX/scaleY 差分反相同幅", {
    // P1 中点 t=45：e=easeOutQuad(0.5)=0.75，w=0 → pose 与基线插值逐位一致
    const ui_kit::JellyPose p45 = ui_kit::jellyPose(45.0f);
    const float e45 = specEaseOutQuad(45.0f / 90.0f);
    DOCTEST_CHECK(nearF(p45.sx, 1.0f + 0.04f * e45, 0.001f));
    DOCTEST_CHECK(nearF(p45.sy, 1.0f - 0.10f * e45, 0.001f));

    // P2 中前段 t=150：p=0.4 → e=2p²=0.32；w(0.15s)≈+0.0207
    const ui_kit::JellyPose p150 = ui_kit::jellyPose(150.0f);
    const float e150 = specEaseInOutQuad((150.0f - 90.0f) / 150.0f);
    const float sxBase = 1.04f + (0.96f - 1.04f) * e150;   // 1.0144
    const float syBase = 0.90f + (1.07f - 0.90f) * e150;   // 0.9544
    const float wx = p150.sx / sxBase - 1.0f;
    const float wy = p150.sy / syBase - 1.0f;
    DOCTEST_CHECK_MESSAGE(wx * wy < 0.0f,
                          "扭摆差分应反相 wx=" + std::to_string(wx) +
                              " wy=" + std::to_string(wy));
    DOCTEST_CHECK(std::fabs(wx + wy) < 1e-3f);  // 同幅反相（s·(1±w)）
    DOCTEST_CHECK(std::fabs(wx) > 0.01f);       // 扭摆真实在动（非零噪声）
    return true;
})

// active 窗口边界：[0,560) 内 true，t=560 起 false（zIndex 盖过邻卡的窗口）。
// 事故：动画结束后 active 不落 false → zIndex 永久 20 盖住邻卡交互。
REGISTER_TEST(ui_kit_jelly_pose_active_window,
              "ui_kit jelly: active 窗口 [0,560) 真区间，560ms 起回常态", {
    DOCTEST_CHECK(ui_kit::jellyPose(0.0f).active);
    DOCTEST_CHECK(ui_kit::jellyPose(90.0f).active);
    DOCTEST_CHECK(ui_kit::jellyPose(559.9f).active);
    DOCTEST_CHECK(!ui_kit::jellyPose(560.0f).active);
    DOCTEST_CHECK(!ui_kit::jellyPose(1000.0f).active);
    // 窗外恒常态（1,1,12）
    const ui_kit::JellyPose out = ui_kit::jellyPose(600.0f);
    DOCTEST_CHECK(nearF(out.sx, 1.0f, 0.005f));
    DOCTEST_CHECK(nearF(out.sy, 1.0f, 0.005f));
    DOCTEST_CHECK(nearF(out.radius, 12.0f, 0.005f));
    return true;
})

// jellyFrame 公开缝（agent 卡手写段供值）：与 jellyCard 消费同一状态表/边沿检测，
// 同 id 同 hovered 序列 → 同 pose（jellyCard 写进 element 的目标值 = jellyFrame 返回值）。
// 事故：缝与卡路径状态表分叉（各记各的 t0/wasHovered）→ agent 卡形变与 jellyCard
// 时间线相位漂移；或 radius 口径不一致（基线 12 vs opt 平移后）→ agent 卡常态跳 16。
REGISTER_TEST(ui_kit_jelly_frame_same_source_as_card,
              "ui_kit jelly: jellyFrame 与 jellyCard 同 id 同 hovered → 同 pose", {
    ui_kit::jellyResetForTests();
    // 非动画态：确定值（常态 12/1.0/1.0/active=false）
    const ui_kit::JellyPose rest = ui_kit::jellyFrame("tjf.card", false);
    DOCTEST_CHECK(nearF(rest.sx, 1.0f, 0.0f));
    DOCTEST_CHECK(nearF(rest.sy, 1.0f, 0.0f));
    DOCTEST_CHECK(nearF(rest.radius, 12.0f, 0.0f));
    DOCTEST_CHECK(!rest.active);

    // 同帧：jellyFrame 先行供值，jellyCard 随后 compose（内部消费同一状态表）
    const auto& t = core::theme::current();
    eui::Ui ui;
    ui.begin("tjf");
    const ui_kit::JellyPose f = ui_kit::jellyFrame("tjf.card", true);
    ui_kit::jellyCard(ui, "tjf.card", 0.0f, 0.0f, 100.0f, 60.0f, t, {}, true);
    ui.end();

    const auto* el = ui.find("tjf.card");
    DOCTEST_REQUIRE(el != nullptr);
    // 默认 JellyOptions(radius=12) 无平移差：element 目标 == jellyFrame 返回
    DOCTEST_CHECK_MESSAGE(nearF(el->radius, f.radius, 0.01f),
                          "radius 分叉 el=" + std::to_string(el->radius) +
                              " frame=" + std::to_string(f.radius));
    DOCTEST_CHECK(nearF(el->transform.scale.x, f.sx, 0.005f));
    DOCTEST_CHECK(nearF(el->transform.scale.y, f.sy, 0.005f));
    // active 一致：动画期 jellyCard 写 zIndex(20)
    DOCTEST_CHECK(f.active);
    DOCTEST_CHECK_EQ(el->zIndex, 20);
    // 且 jellyFrame 值确为时间线值（自 hover 起算的 elapsed → jellyPose 同源）
    std::chrono::steady_clock::time_point t0;
    DOCTEST_REQUIRE(ui_kit::jellyAnimStartForTests("tjf.card", t0));
    const float elapsed =
        std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    const ui_kit::JellyPose expected = ui_kit::jellyPose(elapsed);
    DOCTEST_CHECK(nearF(f.radius, expected.radius, 0.05f));
    DOCTEST_CHECK(nearF(f.sx, expected.sx, 0.005f));
    DOCTEST_CHECK(nearF(f.sy, expected.sy, 0.005f));
    return true;
})

// hover 退出中断：jellyFrame 与 jellyCard 同步落回常态（animating=false → 常态 pose
// + 非动画 zIndex）。事故：缝路径中断不落账 → agent 卡松开后残留在途形变。
REGISTER_TEST(ui_kit_jelly_frame_interrupt_matches_card_reset,
              "ui_kit jelly: jellyFrame hover 退出中断 → 常态且 active=false", {
    ui_kit::jellyResetForTests();
    DOCTEST_CHECK(ui_kit::jellyFrame("tjf2.card", true).active);  // 起表
    const ui_kit::JellyPose cut = ui_kit::jellyFrame("tjf2.card", false);
    DOCTEST_CHECK(!cut.active);
    DOCTEST_CHECK(nearF(cut.sx, 1.0f, 0.005f));
    DOCTEST_CHECK(nearF(cut.sy, 1.0f, 0.005f));
    DOCTEST_CHECK(nearF(cut.radius, 12.0f, 0.005f));
    // 中断后 jellyCard 同 id compose：非动画目标（element zIndex 默认 0）
    const auto& t = core::theme::current();
    eui::Ui ui;
    ui.begin("tjf2");
    ui_kit::jellyCard(ui, "tjf2.card", 0.0f, 0.0f, 100.0f, 60.0f, t, {}, false);
    ui.end();
    const auto* el = ui.find("tjf2.card");
    DOCTEST_REQUIRE(el != nullptr);
    DOCTEST_CHECK_EQ(el->zIndex, 0);
    DOCTEST_CHECK(nearF(el->radius, 12.0f, 0.005f));
    return true;
})
