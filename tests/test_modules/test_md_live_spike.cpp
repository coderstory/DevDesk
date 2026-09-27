// tests/test_modules/test_md_live_spike.cpp
//
// Markdown Phase 3 · M0 技术验证 spike（门禁）
// 依据：docs/superpowers/specs/2026-09-27-markdown-phase3-block-render-design.md
//       §2.2（待验证项 V1~V4）/ §4.1（选型 D1/D2/D7）/ §5（性能目标 #4）
//
// M0 是门禁：V1~V4 任一项不成立就要触发 R1 兜底方案重估、不进入 M1。本文件把四项
// 拆成可复跑的 REGISTER_TEST 用例（用例名统一 mdspike 前缀，便于
// `./devdesk_tests.exe mdspike` 子串过滤），长期留作回归证据；结论表与手工点验
// 清单在 docs/superpowers/plans/2026-09-27-md-phase3-m0-spike.md。
//
// 无头边界（诚实声明，勿把本文件当端到端验证）：
//  1) 焦点协议可全量驱动：Ui::requestFocus 的帧末消费端就在 Runtime::compose
//     （core/runtime/runtime_lifecycle.h:38-42），故本文件用 core::dsl::Runtime
//     逐帧 compose，不伪造焦点状态。
//  2) 键/指针的「事件队列 → dispatch」段需要真窗口（事件由
//     core::window::consumeKeyEvents/PointerEvents 从窗口队列取，入口是
//     Runtime::update），无头不可驱动。因此：
//       · 键用例直接调用 updateKeyInput 内部的同一调用点
//         `focusedElement->onKeyEvent(key)`（core/runtime/runtime_input.h:303-305）；
//       · 指针用例直接调用 onPress/onScroll，并按 Runtime 的算法把 scrollView 滚动
//         位移折算进 bounds（runtime_update.h:374-382 → hitMatrixForElement →
//         runtime_state_bindings.h:157-178 的 translate.y -= offset；子元素在命中
//         测试里继承父级 renderTransform，runtime_input.h:156-167）。
//     「队列段 + 真实 IME 候选框 + 真实击键节奏」由手工点验清单补。
//  3) 不依赖真实 ~/.claude、不起 app、不联网、不碰 sqlite。
//
// 结论（详细见计划文件 V1~V4 表）：
//  V1 成立（有条件）：同 id input 跨帧换位/换父级 → 焦点、StateStore、几何、键派发
//     目标全部存活；条件 = 焦点 id 必须是 `<input id>.hit`（见 v1b）。
//  V2 不成立（spec 的担心方向反了）：元素消失时 focusedId_ **不会**被清除，而是
//     变成"陈旧焦点"（isFocused 只比字符串）；真正会清焦点的是 releaseStateScope
//     覆盖该 id —— 兜底与约束见 v2a/v2c。
//  V3 成立：input 作为 scrollView 流式 content，尺寸参与布局与内容测量；滚动偏移下
//     pointer→caret 落在点击行；imeRect 在元素局部坐标系（候选框随元素变换）。
//  V4 条件成立：贴合高度（D2 默认）下无残余跳位；仅「封顶块 + followCaret=false」
//     同时成立时换文本首帧跳位，按 R5 同帧归零消除。

#include "doctest.h"

#include <eui/dsl.h>
#include <eui/types.h>

#include <components/input.h>
#include <components/scrollview.h>
#include <core/dsl.h>
#include <core/dsl_runtime.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace {

using Ui = eui::Ui;
using Element = core::dsl::Element;
using InputState = components::input_detail::InputModel::InputState;

// 所有用例共用同一个 pageId：Ui 的 state/焦点前缀以 pageId 起头，换名等于换命名空间
constexpr const char* kPage = "mdspike";

// 单输入方案（D1）里的稳定 id：input 元素 id 与"真正可聚焦元素"id（input.h:113）
constexpr const char* kInput = "live.input";
constexpr const char* kInputHit = "live.input.hit";

constexpr float kLineH = 24.0f;   // input.h:128 → fontSize(20) × 1.2
constexpr float kInset = 8.0f;    // 显式 inset，避免默认 metrics 未知
constexpr int kLines = 30;        // 合成文本行数（内容高 = 30×24 = 720）
constexpr float kFitH = kInset * 2.0f + static_cast<float>(kLines) * kLineH;  // 736 = D2 贴合高
constexpr int kLineBytes = 3;     // 每行 "L00" 三个字节
constexpr int kLineStride = 4;    // 含行尾 '\n'

bool near(float a, float b, float eps = 0.5f) {
    return std::fabs(a - b) <= eps;
}

// 合成等宽文本：count 行、每行 3 字节、无尾换行（measureLines 按 '\n' 切 → 恰好 count 行）
std::string spikeLines(int count, const char* tag) {
    std::string out;
    for (int i = 0; i < count; ++i) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%s%02d", tag, i);
        if (i > 0) out.push_back('\n');
        out += buf;
    }
    return out;
}

int lineOfByte(int byte) {
    return byte / kLineStride;
}

// 无头帧驱动：唯一入口是 Runtime::compose（requestFocus 的帧末消费端）
struct SpikeEnv {
    core::dsl::Runtime rt;
    Ui* ui = nullptr;

    template <typename Fn>
    void frame(Fn&& body) {
        rt.compose(kPage, 1600.0f, 1000.0f, [&](Ui& u, const eui::Screen&) {
            ui = &u;
            body(u);
        });
    }
};

// 单输入（multiline）构建：与 Live 的接线同形——稳定 id、显式 content-fit 高度
void spikeBuildInput(Ui& ui, const std::string& text, float height) {
    components::input(ui, kInput)
        .multiline(true)
        .fontSize(20.0f)
        .fontFamily("Segoe UI")
        .inset(kInset)
        .size(600.0f, height)
        .value(text)
        .build();
}

// 直接驱动 input.h:199 的 onPress（queue→dispatch 段需真窗口，见文件头说明）
void spikePress(const Element* hit, const eui::Rect& bounds, double x, double y) {
    eui::PointerEvent down;
    down.action = eui::PointerAction::Press;
    down.button = eui::PointerButton::Left;
    down.x = x;
    down.y = y;
    hit->onPress(down, bounds);
}

} // namespace

// ============================================================
// V1：稳定 id 的 input 跨帧换位 / 换父级
// ============================================================

// ---- V1 主用例：换位 + 换父级后，元素身份/焦点/状态/几何/键派发全部存活 ----
REGISTER_TEST(mdspike_v1_relocate_keeps_identity,
              "mdspike V1: 同 id input 换位换父级 → 焦点+状态+几何+键派发存活", {
    SpikeEnv env;
    const std::string text = spikeLines(kLines, "A");

    // 帧 1：根 column = [a, input, b]，帧末 requestFocus（照"点击渲染块→聚焦活动块"接线）
    auto layout1 = [&](Ui& ui) {
        ui.column("spike.root").gap(10.0f).content([&] {
            ui.rect("spike.a").size(600.0f, 40.0f).build();
            spikeBuildInput(ui, text, 200.0f);
            ui.rect("spike.b").size(600.0f, 40.0f).build();
        }).build();
        ui.requestFocus(kInputHit);
    };
    env.frame(layout1);
    DOCTEST_REQUIRE(env.ui != nullptr);
    DOCTEST_REQUIRE(env.ui->find(kInputHit) != nullptr);
    DOCTEST_CHECK(env.ui->find(kInputHit)->focusable);   // 可聚焦元素 = <id>.hit（input.h:197）
    DOCTEST_CHECK(env.ui->isFocused(kInputHit));         // requestFocus 帧末消费 → 焦点已建立
    // 焦点在帧末消费（runtime_lifecycle.h:38-42），故"聚焦态才画"的 caret 元素次帧才出现
    env.frame(layout1);
    DOCTEST_CHECK(env.ui->find("live.input.cursor") != nullptr);

    const float y1 = env.ui->find(kInput)->frame.y;
    unsigned long long rev1 = 0;
    {
        InputState& st = env.ui->state<InputState>(kInput);
        st.cursor = 5;
        st.followCaret = false;
        st.dragAnchor = 7;
        st.styleRevision = 42;      // build() 从不触碰的字段 = 状态对象身份标记
        st.lastBounds = eui::Rect(1.0f, 2.0f, 3.0f, 4.0f);
        rev1 = st.textRevision;
    }

    // 帧 2：同一个 id 搬进**另一个父级**（嵌套 column）且换位次
    env.frame([&](Ui& ui) {
        ui.column("spike.root").gap(10.0f).content([&] {
            ui.rect("spike.a").size(600.0f, 40.0f).build();
            ui.column("spike.nest").gap(4.0f).content([&] {
                ui.rect("spike.nx").size(600.0f, 30.0f).build();
                spikeBuildInput(ui, text, 200.0f);
            }).build();
        }).build();
    });
    DOCTEST_REQUIRE(env.ui->find(kInput) != nullptr);
    DOCTEST_REQUIRE(env.ui->find(kInputHit) != nullptr);
    const Element* nx = env.ui->find("spike.nx");
    DOCTEST_REQUIRE(nx != nullptr);
    const Element* in2 = env.ui->find(kInput);
    const float y2 = in2->frame.y;
    // ① 元素身份：同 id 仍在帧树中（id 不随树位置/父级变化——不含显式 scope 时）
    DOCTEST_CHECK(env.ui->find(kInputHit)->focusable);
    // ② 焦点保持：focusedId_ 是 id 字符串，每帧 compose 重新灌回 Ui
    DOCTEST_CHECK(env.ui->isFocused(kInputHit));
    DOCTEST_CHECK(env.ui->find("live.input.cursor") != nullptr);   // caret 仍在画
    // ③ 状态存活：StateStore 以 id 键控，跨帧不重建（未释放 scope）
    {
        const InputState& st = env.ui->state<InputState>(kInput);
        DOCTEST_CHECK_EQ(st.text, text);
        DOCTEST_CHECK_EQ(st.cursor, 5);
        DOCTEST_CHECK_FALSE(st.followCaret);
        DOCTEST_CHECK_EQ(st.dragAnchor, 7);
        DOCTEST_CHECK_EQ(st.styleRevision, 42ULL);
        DOCTEST_CHECK_EQ(st.textRevision, rev1);        // 未被当"外部改文本"重灌
        DOCTEST_CHECK(near(st.lastBounds.y, 2.0f));     // 瞬态字段也活着
    }
    // ④ 几何按新位置重算（不是残留旧坐标）：流式排在前一个兄弟之下
    DOCTEST_CHECK(near(y2, nx->frame.y + nx->frame.height + 4.0f));
    DOCTEST_CHECK(!near(y2, y1));
    // ⑤ 键派发目标 = 焦点元素（updateKeyInput 同一调用点）：事件仍进该实例
    const Element* hit2 = env.ui->find(kInputHit);
    DOCTEST_REQUIRE(hit2->onKeyEvent != nullptr);
    eui::KeyEvent right;
    right.key = eui::InputKey::Right;
    right.action = eui::KeyAction::Press;
    DOCTEST_CHECK(hit2->onKeyEvent(right));             // 未聚焦元素不会被这样调用
    DOCTEST_CHECK_EQ(env.ui->state<InputState>(kInput).cursor, 6);

    // 帧 3：再搬回根 column 末位 → 反复搬家幂等（焦点/状态仍存活）
    env.frame([&](Ui& ui) {
        ui.column("spike.root").gap(10.0f).content([&] {
            ui.rect("spike.a").size(600.0f, 40.0f).build();
            ui.rect("spike.b").size(600.0f, 40.0f).build();
            spikeBuildInput(ui, text, 200.0f);
        }).build();
    });
    DOCTEST_CHECK(env.ui->isFocused(kInputHit));
    DOCTEST_CHECK_EQ(env.ui->state<InputState>(kInput).cursor, 6);
    DOCTEST_CHECK_EQ(env.ui->state<InputState>(kInput).styleRevision, 42ULL);
    DOCTEST_CHECK(!near(env.ui->find(kInput)->frame.y, y2));
    return true;
})

// ---- V1b：焦点 id 口径陷阱（requestFocus 必须打到 <id>.hit，否则静默失效）----
// 事故面：DevDesk 现有 4 处 `ui.requestFocus("md.editor.input")`
// （markdown_module.cpp:1611/2425/2453/2616）发的是**基 id**，而 input 组件认的
// 是 `<id>.hit`（input.h:114 `ui_.isFocused(id + ".hit")`）——本用例把两种口径的
// 可观测差异钉死，供 M3 接线前拍板。
REGISTER_TEST(mdspike_v1b_focus_target_must_be_hit_id,
              "mdspike V1b: requestFocus 必须指向 <input>.hit（基 id 静默失效）", {
    SpikeEnv env;
    const std::string text = spikeLines(kLines, "A");
    auto buildOne = [&](Ui& ui) {
        ui.column("spike.root").content([&] {
            spikeBuildInput(ui, text, 200.0f);
        }).build();
    };

    // ① 基 id 口径：焦点"看起来设上了"，但组件判定未聚焦 → 无 caret、按键落空
    env.frame([&](Ui& ui) {
        buildOne(ui);
        ui.requestFocus(kInput);
    });
    DOCTEST_CHECK(env.ui->isFocused(kInput));           // 焦点确实落在基 id 上
    DOCTEST_CHECK_FALSE(env.ui->isFocused(kInputHit));  // 组件认的是 .hit
    // 次帧（焦点已生效一帧）后仍然如此——差异不是"晚一帧"，而是口径不同
    env.frame([&](Ui& ui) { buildOne(ui); });
    DOCTEST_CHECK_FALSE(env.ui->isFocused(kInputHit));
    DOCTEST_CHECK(env.ui->find("live.input.cursor") == nullptr);   // 无 caret
    DOCTEST_CHECK(env.ui->find(kInput)->onKeyEvent == nullptr);    // 基 id 元素无键处理器

    // ② .hit 口径：组件与派发两侧同时成立
    env.frame([&](Ui& ui) {
        buildOne(ui);
        ui.requestFocus(kInputHit);
    });
    DOCTEST_CHECK(env.ui->isFocused(kInputHit));
    env.frame([&](Ui& ui) { buildOne(ui); });           // 焦点生效后的第一帧
    DOCTEST_CHECK(env.ui->find("live.input.cursor") != nullptr);
    DOCTEST_REQUIRE(env.ui->find(kInputHit)->onKeyEvent != nullptr);
    return true;
})

// ============================================================
// V2：焦点元素所在父级被合并 / 删除时的焦点归属
// ============================================================

// ---- V2a：元素从帧树消失 → focusedId_ 不清除（陈旧焦点，而非丢焦点）----
// spec §2.2 V2 假设"父级被删会清 focusedId_ → 打字丢失"；实测方向相反：
// focusedId_ 只在 ① releaseStateScope 前缀命中 ② 禁用子树 ③ 指针按下重定焦
// 三处变化（runtime_input.h:227-249 / runtime_lifecycle.h:28-45），元素消失不在其列。
REGISTER_TEST(mdspike_v2a_unmount_leaves_stale_focus,
              "mdspike V2a: 元素消失时焦点不被清除（陈旧焦点）+ 重挂载静默恢复", {
    SpikeEnv env;
    const std::string text = spikeLines(kLines, "A");
    auto withInput = [&](Ui& ui) {
        ui.column("spike.root").content([&] {
            spikeBuildInput(ui, text, 200.0f);
        }).build();
        ui.requestFocus(kInputHit);
    };

    env.frame(withInput);
    DOCTEST_REQUIRE(env.ui->isFocused(kInputHit));
    env.ui->state<InputState>(kInput).dragAnchor = 11;

    // 帧 2：input 整体不在帧树里（块合并瞬间 / 无活动块）
    env.frame([&](Ui& ui) {
        ui.rect("spike.other").size(100.0f, 100.0f).build();
    });
    DOCTEST_CHECK(env.ui->find(kInput) == nullptr);
    DOCTEST_CHECK(env.ui->find(kInputHit) == nullptr);
    DOCTEST_CHECK(env.ui->isFocused(kInputHit));   // ← 焦点未清（isFocused 只比字符串）

    // 帧 3：同 id 重新出现（块合并回来）→ 焦点 + 旧编辑状态静默恢复（无需补发 requestFocus）
    env.frame(withInput);
    DOCTEST_REQUIRE(env.ui->find(kInputHit) != nullptr);
    DOCTEST_CHECK(env.ui->isFocused(kInputHit));
    DOCTEST_CHECK(env.ui->find("live.input.cursor") != nullptr);
    DOCTEST_CHECK_EQ(env.ui->state<InputState>(kInput).dragAnchor, 11);  // StateStore 未释放
    return true;
})

// ---- V2b：目标不在本帧树中时 requestFocus 被丢弃（不排队）----
// 这条是兜底方案的前提：不能"提前"或"事后"补发，必须与元素同帧。
REGISTER_TEST(mdspike_v2b_requestfocus_when_absent_is_dropped,
              "mdspike V2b: 目标不在本帧树中 → requestFocus 丢弃不排队", {
    SpikeEnv env;
    const std::string text = spikeLines(kLines, "A");

    env.frame([&](Ui& ui) {
        ui.rect("spike.other").size(100.0f, 100.0f).build();
        ui.requestFocus(kInputHit);      // 本帧树里没有该元素
    });
    DOCTEST_CHECK_FALSE(env.ui->isFocused(kInputHit));

    // 下一帧元素就位，但请求没有排队 → 依然未聚焦
    env.frame([&](Ui& ui) {
        ui.column("spike.root").content([&] {
            spikeBuildInput(ui, text, 200.0f);
        }).build();
    });
    DOCTEST_CHECK_FALSE(env.ui->isFocused(kInputHit));
    return true;
})

// ---- V2c：真正会清焦点的是 releaseStateScope；同帧「释放 + 补发」可恢复 ----
// R7 要求块切换时释放旧块状态；若单输入被放进被释放的 scope 前缀下，焦点会被连带清掉，
// 同时 InputState（光标/undo）被销毁重建 —— 这是 Live 接线的硬约束。
// 缓解顺序成立：Runtime::compose 先消费 releasedStateScopes 再消费 pendingFocus
// （runtime_lifecycle.h:28-42），故同帧补发 requestFocus 会在释放之后生效。
REGISTER_TEST(mdspike_v2c_release_scope_clears_then_same_frame_request_restores,
              "mdspike V2c: releaseStateScope 清焦点；同帧补发 requestFocus 可恢复", {
    SpikeEnv env;
    const std::string text = spikeLines(kLines, "A");
    auto buildInput = [&](Ui& ui) {
        ui.column("spike.root").content([&] {
            spikeBuildInput(ui, text, 200.0f);
        }).build();
    };

    env.frame([&](Ui& ui) {
        buildInput(ui);
        ui.requestFocus(kInputHit);
    });
    DOCTEST_REQUIRE(env.ui->isFocused(kInputHit));
    env.ui->state<InputState>(kInput).cursor = 5;

    // ① 只释放（模拟块切换时释放覆盖 input 的 scope）→ 焦点被清、编辑状态被销毁
    env.frame([&](Ui& ui) {
        buildInput(ui);
        ui.releaseStateScope(kInput);
    });
    DOCTEST_CHECK_FALSE(env.ui->isFocused(kInputHit));
    DOCTEST_CHECK_EQ(env.ui->state<InputState>(kInput).cursor, 0);   // 光标/undo 归零
    DOCTEST_CHECK_EQ(env.ui->state<InputState>(kInput).styleRevision, 0ULL);

    // ② 重新聚焦
    env.frame([&](Ui& ui) {
        buildInput(ui);
        ui.requestFocus(kInputHit);
    });
    DOCTEST_REQUIRE(env.ui->isFocused(kInputHit));

    // ③ 同帧「先释放、后补发」→ 帧末消费时元素已在树中 → 焦点恢复（兜底可用）
    env.frame([&](Ui& ui) {
        buildInput(ui);
        ui.releaseStateScope(kInput);
        ui.requestFocus(kInputHit);
    });
    DOCTEST_CHECK(env.ui->isFocused(kInputHit));
    DOCTEST_CHECK(env.ui->find("live.input.cursor") != nullptr);
    return true;
})

// ============================================================
// V3：input 作为 scrollView content 内的流式元素
// ============================================================

namespace {

// scrollView 内容： [top(60)] + input(显式高) + [bottom(60)]，gap 20，视口 600×300
struct SpikeScrollPage {
    Ui ui;

    void frame(float inputH, float offset, const std::string& text) {
        ui.begin(kPage);
        components::scrollView(ui, "live.scroll")
            .position(0.0f, 0.0f)
            .size(600.0f, 300.0f)
            .offset(offset)
            .gap(20.0f)
            .content([&](Ui& u, float cw, float vh) {
                (void)vh;
                u.rect("spike.top").size(cw, 60.0f).build();
                components::input(u, kInput)
                    .multiline(true)
                    .fontSize(20.0f)
                    .fontFamily("Segoe UI")
                    .inset(kInset)
                    .size(cw, inputH)
                    .value(text)
                    .build();
                u.rect("spike.bottom").size(cw, 60.0f).build();
            })
            .build();
        ui.end();
        ui.layout(1600.0f, 1000.0f);
    }
};

} // namespace

// ---- V3a：尺寸参与流式布局 + 进入内容测量（滚动范围）+ 贴合高度不注册滚轮 ----
REGISTER_TEST(mdspike_v3a_input_as_flow_content,
              "mdspike V3a: input 流式 content（布局/滚动范围/贴合无滚轮处理器）", {
    SpikeScrollPage page;
    const std::string text = spikeLines(kLines, "L");

    page.frame(kFitH, 0.0f, text);
    const Element* top = page.ui.find("spike.top");
    const Element* in = page.ui.find(kInput);
    const Element* bottom = page.ui.find("spike.bottom");
    const Element* sv = page.ui.find("live.scroll");
    DOCTEST_REQUIRE(top != nullptr);
    DOCTEST_REQUIRE(in != nullptr);
    DOCTEST_REQUIRE(bottom != nullptr);
    DOCTEST_REQUIRE(sv != nullptr);
    // ① 流式定位（非绝对定位）：input 的显式高度把后续兄弟推下去
    DOCTEST_CHECK(near(in->frame.y, top->frame.y + top->frame.height + 20.0f));
    DOCTEST_CHECK(near(bottom->frame.y, in->frame.y + kFitH + 20.0f));
    // ② input 高度进入 scrollView 的内容测量 → 滚动范围 = 内容高 − 视口高
    const float contentH = bottom->frame.y + bottom->frame.height;
    DOCTEST_CHECK(near(sv->scrollMaxOffset, contentH - 300.0f));
    DOCTEST_CHECK(sv->scrollMaxOffset > 0.0f);
    DOCTEST_CHECK(page.ui.find("live.scroll.scroll.thumb") != nullptr);   // 可滚 → 有滚动条
    // ③ D2 贴合高度 ⇒ maxVerticalScroll == 0 ⇒ 不注册 onScroll（滚轮穿透外层，spec §2.1）
    DOCTEST_CHECK(page.ui.find(kInputHit)->onScroll == nullptr);

    // ④ 同一布局换矮 input → 内容不再超高：滚动范围归零、滚动条消失（尺寸真的参与了测量）
    page.frame(40.0f, 0.0f, text);
    DOCTEST_CHECK(near(page.ui.find("live.scroll")->scrollMaxOffset, 0.0f));
    DOCTEST_CHECK(page.ui.find("live.scroll.scroll.thumb") == nullptr);
    // 非贴合高度（本例 40 < 内容 720）→ input 自己有内部滚动 → 注册滚轮处理器（吃滚轮）
    DOCTEST_CHECK(page.ui.find(kInputHit)->onScroll != nullptr);

    // ⑤ 偏移由框架钳制：正常值原样生效，超界钳到 maxOffset
    page.frame(kFitH, 120.0f, text);
    DOCTEST_CHECK(near(page.ui.find("live.scroll")->scrollOffset, 120.0f));
    page.frame(kFitH, 99999.0f, text);
    DOCTEST_CHECK(near(page.ui.find("live.scroll")->scrollOffset,
                       page.ui.find("live.scroll")->scrollMaxOffset));

    // ⑥ 对照：封顶块（R4，高 300 < 内容 720）→ maxVerticalScroll > 0 → 注册滚轮处理器
    //    （= spec §5 #5 的"块内滚动/轮盘死区"确实存在，属已接受取舍）
    page.frame(300.0f, 0.0f, text);
    DOCTEST_CHECK(page.ui.find(kInputHit)->onScroll != nullptr);
    return true;
})

// ---- V3b：滚动偏移下 pointer→caret 映射 + lastBounds + imeRect ----
REGISTER_TEST(mdspike_v3b_pointer_to_caret_under_scroll,
              "mdspike V3b: 滚动偏移下 pointer→caret 落点击行（含反控）", {
    SpikeScrollPage page;
    const std::string text = spikeLines(kLines, "L");
    page.frame(kFitH, 120.0f, text);

    const Element* in = page.ui.find(kInput);
    const Element* hit = page.ui.find(kInputHit);
    const Element* sv = page.ui.find("live.scroll");
    DOCTEST_REQUIRE(in != nullptr);
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_REQUIRE(sv != nullptr);
    const float offset = sv->scrollOffset;
    DOCTEST_REQUIRE(near(offset, 120.0f));

    // Runtime 侧：内容列 renderTransform.translate.y -= offset，命中测试把父级变换
    // 施加到子元素 → onPress 收到的是"屏幕坐标下"的 bounds（见文件头取证）
    const float hitW = in->frame.width;
    const eui::Rect shifted(in->frame.x, in->frame.y - offset, hitW, in->frame.height);
    const eui::Rect unshifted(in->frame.x, in->frame.y, hitW, in->frame.height);

    // 视口中部点（屏幕坐标）：内容坐标 270 → input 局部 190 → 第 7 行（0 基）
    // 反函数：lineIndexFromY(localY) = floor((localY − textTop) / lineHeight)（input_model.h:256-263）
    const double px = 300.0;
    const double py = 150.0;
    const int expectLine = static_cast<int>((py - shifted.y - kInset) / kLineH);
    DOCTEST_CHECK(expectLine == 7);

    spikePress(hit, shifted, px, py);
    {
        const InputState& s2 = page.ui.state<InputState>(kInput);
        DOCTEST_CHECK(lineOfByte(s2.cursor) == 7);
        DOCTEST_CHECK(s2.cursor >= 7 * kLineStride);
        DOCTEST_CHECK(s2.cursor <= 7 * kLineStride + kLineBytes);
        DOCTEST_CHECK(near(s2.lastBounds.y, shifted.y));   // lastBounds = 变换后的 bounds
    }

    // 反控：若 bounds 未折算滚动位移（=错误接线），落点会少 offset/lineH 行 → 断言不等，
    // 以证明"框架必须传折算后的 bounds"这一条是承重的
    spikePress(hit, unshifted, px, py);
    {
        const InputState& s3 = page.ui.state<InputState>(kInput);
        DOCTEST_CHECK(lineOfByte(s3.cursor) != expectLine);
        DOCTEST_CHECK(lineOfByte(s3.cursor) == expectLine - static_cast<int>(offset / kLineH));
    }

    // IME 候选框：imeRect 记在元素局部坐标系（input.h:198），平台侧再叠加元素变换
    // （runtime_input.h:318-360 → toPixelRect(frame) + hitMatrixForElement）→ 随滚动走
    page.frame(kFitH, 120.0f, text);
    const Element* hit2 = page.ui.find(kInputHit);
    DOCTEST_REQUIRE(hit2 != nullptr);
    DOCTEST_CHECK(hit2->hasImeRect);
    DOCTEST_CHECK(near(hit2->imeRect.height, kLineH));
    DOCTEST_CHECK(hit2->imeRect.y > 0.0f);          // 行首上方（元素局部，非屏幕）
    return true;
})

// ============================================================
// V4：换文本瞬间 verticalScroll / followCaret 残余
// ============================================================

// ---- V4a：封顶块 + followCaret=false → 首帧跳位成立；同帧归零（R5）消除 ----
// 首帧几何探针用 caret 元素：frame.y = inset + cursorLine×lineH − verticalScroll
// （input.h:487-492 → layout.cursorY = textTop + cursorLine×lineH − verticalScroll）。
// 换文本时 focused → 光标落到新文本末尾（input.h:143-147）→ cursorLine 固定为末行，
// 故 caret.y 直接读出残余 verticalScroll。
REGISTER_TEST(mdspike_v4a_swap_text_scroll_residual_capped_block,
              "mdspike V4a: 封顶块换文本首帧跳位 + R5 同帧归零消除", {
    SpikeEnv env;
    const std::string t1 = spikeLines(kLines, "A");
    const std::string t2 = spikeLines(kLines, "B");
    const float capH = 300.0f;                     // 封顶块：内容 720 > 视口 284
    const float lastLineY = kInset
        + static_cast<float>(kLines - 1) * kLineH;  // 光标在末行时的 caret.y（scroll=0）

    auto frameWith = [&](Ui& ui, const std::string& text) {
        ui.column("spike.root").content([&] {
            spikeBuildInput(ui, text, capH);
        }).build();
        ui.requestFocus(kInputHit);
    };

    env.frame([&](Ui& ui) { frameWith(ui, t1); });
    // 焦点在帧末消费 → caret 元素次帧才出现；光标显式钉到文末（等价 setLiveCursor 到块尾），
    // 使 caret.frame.y = inset + 末行号×lineH − verticalScroll 只随残余滚动变化
    {
        InputState& s = env.ui->state<InputState>(kInput);
        s.cursor = static_cast<int>(t1.size());
        s.selectionStart = s.cursor;
        s.selectionEnd = s.cursor;
        // followCaret=true 会把 caret 拉进视口（input_model.h:158-161），先关掉让滚动可读
        s.followCaret = false;
        s.verticalScroll = 0.0f;
    }
    env.frame([&](Ui& ui) { frameWith(ui, t1); });
    const Element* caret = env.ui->find("live.input.cursor");
    DOCTEST_REQUIRE(caret != nullptr);
    DOCTEST_CHECK(near(caret->frame.y, lastLineY));                 // 未滚动基准

    // 用户在封顶块内滚一格（followCaret 置 false，input.h:220-227）
    const Element* hit = env.ui->find(kInputHit);
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_REQUIRE(hit->onScroll != nullptr);
    core::ScrollEvent wheel;
    wheel.y = -3.0;
    hit->onScroll(wheel);
    const float step = 20.0f * 2.2f;                                // input.h:221
    const float residual = 3.0f * step;
    {
        const InputState& s = env.ui->state<InputState>(kInput);
        DOCTEST_CHECK(near(s.verticalScroll, residual));
        DOCTEST_CHECK_FALSE(s.followCaret);
    }

    env.frame([&](Ui& ui) { frameWith(ui, t1); });                  // 同文本：残余生效
    DOCTEST_CHECK(near(env.ui->find("live.input.cursor")->frame.y, lastLineY - residual));

    // ① 换文本（t1→t2）且 followCaret 仍 false → 首帧带着残余 → 一帧跳位成立
    env.frame([&](Ui& ui) { frameWith(ui, t2); });
    DOCTEST_CHECK_EQ(env.ui->state<InputState>(kInput).text, t2);
    DOCTEST_CHECK(near(env.ui->find("live.input.cursor")->frame.y, lastLineY - residual));

    // ② R5 缓解：切块帧（t2→t3）compose 前把 verticalScroll 归零（模块在 compose 回调内先
    //    归零再画 input）→ 新块首帧无跳位；与 ① 同为真·换文本帧，唯一差别是这一行归零
    const std::string t3 = spikeLines(kLines, "C");
    {
        InputState& s = env.ui->state<InputState>(kInput);
        s.verticalScroll = 0.0f;
        s.followCaret = false;
    }
    env.frame([&](Ui& ui) { frameWith(ui, t3); });
    DOCTEST_CHECK_EQ(env.ui->state<InputState>(kInput).text, t3);
    DOCTEST_CHECK(near(env.ui->find("live.input.cursor")->frame.y, lastLineY));
    DOCTEST_CHECK(near(env.ui->state<InputState>(kInput).verticalScroll, 0.0f));

    // ③ 打字路径自愈：followCaret=true 时框架同帧按光标重算滚动（input_model.h:158-161）
    {
        InputState& s = env.ui->state<InputState>(kInput);
        s.verticalScroll = residual;
        s.followCaret = true;
        s.cursor = 0;
    }
    env.frame([&](Ui& ui) { frameWith(ui, t3); });
    DOCTEST_CHECK(near(env.ui->state<InputState>(kInput).verticalScroll, 0.0f));
    DOCTEST_CHECK(near(env.ui->find("live.input.cursor")->frame.y, kInset));
    return true;
})

// ---- V4b：贴合高度（D2 默认路径）→ 不注册滚轮、残余恒为 0、换文本无跳位 ----
REGISTER_TEST(mdspike_v4b_fit_height_has_no_residual,
              "mdspike V4b: 贴合高度换文本无残余跳位（D2 默认）", {
    SpikeEnv env;
    const std::string t1 = spikeLines(kLines, "A");
    const std::string t2 = spikeLines(kLines, "B");
    const float lastLineY = kInset + static_cast<float>(kLines - 1) * kLineH;

    auto frameWith = [&](Ui& ui, const std::string& text) {
        ui.column("spike.root").content([&] {
            spikeBuildInput(ui, text, kFitH);
        }).build();
        ui.requestFocus(kInputHit);
    };

    env.frame([&](Ui& ui) { frameWith(ui, t1); });
    DOCTEST_REQUIRE(env.ui->find(kInputHit) != nullptr);
    DOCTEST_CHECK(env.ui->find(kInputHit)->onScroll == nullptr);     // 无内部滚动 → 无残余来源
    {
        InputState& s = env.ui->state<InputState>(kInput);
        s.cursor = static_cast<int>(t1.size());
        s.selectionStart = s.cursor;
        s.selectionEnd = s.cursor;
    }
    env.frame([&](Ui& ui) { frameWith(ui, t1); });
    DOCTEST_REQUIRE(env.ui->find("live.input.cursor") != nullptr);
    DOCTEST_CHECK(near(env.ui->find("live.input.cursor")->frame.y, lastLineY));

    // 换文本（无任何缓解）→ 首帧无跳位
    env.frame([&](Ui& ui) { frameWith(ui, t2); });
    DOCTEST_CHECK_EQ(env.ui->state<InputState>(kInput).text, t2);
    DOCTEST_CHECK(near(env.ui->state<InputState>(kInput).verticalScroll, 0.0f));
    DOCTEST_CHECK(near(env.ui->find("live.input.cursor")->frame.y, lastLineY));
    return true;
})
