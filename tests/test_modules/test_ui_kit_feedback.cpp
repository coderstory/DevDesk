// tests/test_modules/test_ui_kit_feedback.cpp
// W2d 轻提示 + EmptyState + 问号字形验证无头断言（spec §4.10 / §4.12 / §4.9）。
// 每条场景注明防什么事故——场景缺口是真机三连爆的根源（交付口径纪律）。
//
// 断言三层（spec §7.1）：
//   - 元素存在性（ui.find != nullptr）
//   - 几何数值（width / height / radius / fontSize）
//   - 颜色 hex 精确（* 令牌值；黑半透明 bg 例外用语义常量）
//
// 色值走 wechat 令牌（core::theme::current().colors.*）；黑半透明 bg 例外
// （wechat 无黑半透明槽位——§4.10 spec 钉「黑色半透明」，故用 eui::Color{0,0,0,0.7}
// 语义常量，alpha=0.7 ≈ 178/255）。
//
// resetCaseGlobals 默认 setDark()，wechat 令牌在深浅两模式均初始化（theme.cpp）。
#include "doctest.h"
#include "core/theme.h"
#include "core/component_theme.h"
#include "ui_kit/light_toast.h"
#include "ui_kit/toast.h"
#include "ui_kit/empty_state.h"
#include "ui_kit/button.h"
#include <eui/dsl.h>
#include <eui/types.h>

#include <chrono>
#include <cmath>
#include <string>

namespace {

// float [0,1] → 0-255 字节值（hex 精确比对，同 test_theme_accents / test_ui_kit_button 口径）
int toHexByte(float v) { return static_cast<int>(std::round(v * 255.0f)); }
// 浮点颜色近似比较（±0.01 容差，深浅模式自适应）
auto sameColor = [](const eui::Color& x, const eui::Color& y) {
    return std::fabs(x.r - y.r) < 0.01f && std::fabs(x.g - y.g) < 0.01f &&
           std::fabs(x.b - y.b) < 0.01f;
};

} // namespace

// 场景1（§4.10 防「轻提示不黑底/非半透明」）：showLightToast 首次触发后 bg 节点
// 存在 + color=黑半透明 (0,0,0,0.7) + 圆角=radiusWeChat(6)。
// 事故：轻提示用白底/纯黑底致可读性塌陷或违和（§4.10 spec 钉「黑色半透明」；
// 违反即整条 toast 视觉与微信基线脱节）。
REGISTER_TEST(ui_kit_light_toast_black_translucent_bg,
              "ui_kit lightToast: bg=黑半透明(0,0,0,0.7) + radius=6 首次触发", {
    ui_kit::resetLightToastForTests();  // 防单实例 state 跨测试用例泄漏
    const auto& t = core::theme::current();
    eui::Ui ui;
    ui.begin("w02d_lt_black");
    ui_kit::showLightToast(ui, "lt", "已复制", 100.0f, 100.0f, t);
    ui.end();

    const auto* bg = ui.find("lt.bg");
    DOCTEST_REQUIRE(bg != nullptr);
    // bg = 黑半透明 (0, 0, 0, 0.7)——语义常量（wechat 无对应槽位，见 light_toast.h 注释）
    DOCTEST_CHECK_EQ(toHexByte(bg->color.r), 0x00);
    DOCTEST_CHECK_EQ(toHexByte(bg->color.g), 0x00);
    DOCTEST_CHECK_EQ(toHexByte(bg->color.b), 0x00);
    // alpha 0.7 → std::round(0.7×255)=std::round(178.5)=179（std::round 半远离零）
    DOCTEST_CHECK_EQ(toHexByte(bg->color.a), 179);
    // 圆角 = radiusWeChat (6px，§3.2 D-12 统一)
    DOCTEST_CHECK_EQ(bg->radius, t.sizes.radiusWeChat);
    DOCTEST_CHECK_EQ(bg->radius, 6.0f);
    return true;
})

// 场景2（§4.10 防「轻提示非白字」）：text 节点 color=onPrimary(#FFFFFF)
// + 字号 14px（§3.3 主文字）。
// 事故：黑底非白字致对比度塌陷（§4.10 spec 钉「白色文字」；onPrimary=#FFFFFF
// 在 wechat 令牌里是主色上的前景色，黑底白字同义复用）。
REGISTER_TEST(ui_kit_light_toast_white_text_on_primary,
              "ui_kit lightToast: text color=onPrimary(#FFFFFF) + fontSize=14", {
    ui_kit::resetLightToastForTests();  // 防单实例 state 跨测试用例泄漏
    const auto& t = core::theme::current();
    eui::Ui ui;
    ui.begin("w02d_lt_white");
    ui_kit::showLightToast(ui, "lt", "保存成功", 100.0f, 100.0f, t);
    ui.end();

    const auto* txt = ui.find("lt.text");
    DOCTEST_REQUIRE(txt != nullptr);
    // text color = onPrimary (#FFFFFF)
    DOCTEST_CHECK_EQ(toHexByte(txt->textColor.r), 0xFF);
    DOCTEST_CHECK_EQ(toHexByte(txt->textColor.g), 0xFF);
    DOCTEST_CHECK_EQ(toHexByte(txt->textColor.b), 0xFF);
    // 字号 14（§3.3 主文字；spec 无 14 档 token，钉 spec 值）
    DOCTEST_CHECK_EQ(txt->fontSize, 14.0f);
    // text 内容正确（防「空文案渲染」）
    DOCTEST_CHECK_EQ(txt->text, std::string{"保存成功"});
    return true;
})

// 场景3（§4.10 防「轻提示不消失」）：手动把 state.startTime 推到 2s 前 → 函数
// 内部判 elapsed >= 1.5s 关闭，bg 节点不再渲染。
// 事故：轻提示永不消失致遮挡 UI（§4.10 spec 钉「1.5s 自动消失」；违反即用户
// 被永久挡住底层操作）。
// mock 方式：经 ui_kit::lightToastStateForTests() 直接改 startTime——
// light_toast.cpp 用 function-local static state（见 header 注释），测试经 seam
// 访问同一份 state（不依赖 ui.state<T> 的跨 TU COMDAT fold 行为）。
REGISTER_TEST(ui_kit_light_toast_auto_dismiss_after_1_5s,
              "ui_kit lightToast: elapsed>=1.5s 后不渲染（自动消失）", {
    ui_kit::resetLightToastForTests();
    const auto& t = core::theme::current();
    eui::Ui ui;
    // 第 1 帧：首次触发，open=true，bg 渲染（前提：toast 真渲染了，否则后续 nullptr 无判别力）
    ui.begin("w02d_lt_dismiss_f1");
    ui_kit::showLightToast(ui, "lt", "已复制", 100.0f, 100.0f, t);
    ui.end();
    DOCTEST_CHECK(ui.find("lt.bg") != nullptr);
    // 经 seam 改 startTime 到 2 秒前，模拟时间流逝（> 1.5s 阈值）
    ui_kit::lightToastStateForTests().startTime =
        std::chrono::steady_clock::now() - std::chrono::seconds(2);
    // 第 2 帧：begin 清树；showLightToast 判 elapsed>=1.5s→open=false→early return 不建 bg
    ui.begin("w02d_lt_dismiss_f2");
    ui_kit::showLightToast(ui, "lt", "已复制", 100.0f, 100.0f, t);
    ui.end();
    DOCTEST_CHECK(ui.find("lt.bg") == nullptr);  // 第 2 帧无 bg（dismiss 生效）
    DOCTEST_CHECK(!ui_kit::lightToastStateForTests().open);  // open=false（dismiss 逻辑）
    return true;
})

// 场景4（§4.10 防「新 message 不覆盖旧 message」）：同一 id 先调「已复制」，
// 立即再调「保存成功」→ 应展示后一条（覆盖），state.message=「保存成功」。
// 事故：新反馈点被旧 toast 卡住不显示（§4.10 单实例无队列语义；违反即用户
// 复制后立即保存，只看到「已复制」而看不到「保存成功」）。
REGISTER_TEST(ui_kit_light_toast_new_message_overrides,
              "ui_kit lightToast: 同 id 新 message 覆盖旧 message", {
    ui_kit::resetLightToastForTests();
    const auto& t = core::theme::current();
    eui::Ui ui;
    ui.begin("w02d_lt_override");
    ui_kit::showLightToast(ui, "lt", "已复制", 100.0f, 100.0f, t);
    ui_kit::showLightToast(ui, "lt", "保存成功", 100.0f, 100.0f, t);
    ui.end();

    const auto* txt = ui.find("lt.text");
    DOCTEST_REQUIRE(txt != nullptr);
    // 展示后一条（覆盖）
    DOCTEST_CHECK_EQ(txt->text, std::string{"保存成功"});
    return true;
})

// 场景5（§4.12 防「空状态插画式/无入口」）：emptyState 渲染极简文字
// （textSecondary #666666）+ 就近按钮（Secondary 级 浅灰底 + 深灰字）。
// 事故：空状态用插画/情绪化文案/无按钮致用户卡死（§4.12 禁止项 + §5.6 按钮就近
// 原则；违反即用户面对空列表无操作入口）。
REGISTER_TEST(ui_kit_empty_state_minimal_text_with_nearby_action,
              "ui_kit emptyState: 文字灰(#666666) + 就近按钮 Secondary 级", {
    const auto& t = core::theme::current();
    eui::Ui ui;
    ui.begin("w02d_es_action");
    ui_kit::emptyState(ui, "es", "暂无任务",
                      std::make_pair(std::string{"+ 添加任务"},
                                     std::function<void()>([]{})),
                      0.0f, 0.0f, 300.0f, 200.0f, t);
    ui.end();

    // 文字节点存在 + color=textSecondary #4B5563（S2.8 标题 textSecondary） + fontSize=14
    const auto* msg = ui.find("es.message");
    DOCTEST_REQUIRE(msg != nullptr);
    DOCTEST_CHECK(sameColor(msg->textColor, t.colors.textSecondary));
    DOCTEST_CHECK_EQ(msg->fontSize, 14.0f);
    DOCTEST_CHECK_EQ(msg->text, std::string{"暂无任务"});

    // 按钮节点存在（W2b button helper Secondary 级，id="es.action"）
    const auto* btnBg = ui.find("es.action.bg");
    DOCTEST_REQUIRE(btnBg != nullptr);
    // Secondary bg = hover #F3F4F6（S1.2 #13）
    DOCTEST_CHECK(sameColor(btnBg->color, t.colors.hover));
    // 按钮高度 = kButtonHeight（40，S1.2 #35 / S2.1）
    DOCTEST_CHECK_EQ(btnBg->height.value, ui_kit::kButtonHeight);
    DOCTEST_CHECK_EQ(btnBg->height.value, 40.0f);

    // 按钮文字节点存在 + 内容正确
    const auto* btnTxt = ui.find("es.action.text");
    DOCTEST_REQUIRE(btnTxt != nullptr);
    DOCTEST_CHECK_EQ(btnTxt->text, std::string{"+ 添加任务"});
    return true;
})

// 场景6（§4.12 防「无按钮时空状态渲染异常」）：emptyState 无 action 时只画
// 文字，不应崩、不应残留按钮节点。
// 事故：无 action 路径未测致空状态在「无操作入口」场景（如「无搜索结果」纯
// 提示）崩或画残按钮（§4.12「可选」操作按钮——nullopt 是合法调用）。
REGISTER_TEST(ui_kit_empty_state_no_action_text_only,
              "ui_kit emptyState: 无 action 时只画文字（不崩不残按钮）", {
    const auto& t = core::theme::current();
    eui::Ui ui;
    ui.begin("w02d_es_no_action");
    ui_kit::emptyState(ui, "es", "无搜索结果",
                      std::nullopt,
                      0.0f, 0.0f, 300.0f, 200.0f, t);
    ui.end();

    // 文字节点存在 + 内容正确
    const auto* msg = ui.find("es.message");
    DOCTEST_REQUIRE(msg != nullptr);
    DOCTEST_CHECK_EQ(msg->text, std::string{"无搜索结果"});

    // 不应残留按钮节点（action=nullopt → 不调 button helper）
    const auto* btnBg = ui.find("es.action.bg");
    DOCTEST_CHECK(btnBg == nullptr);
    return true;
})

// 场景7（§4.9 问号字形验证，FA7 Solid 0xF059=fa-circle-question）：
// ui.text().icon(0xF059) 后 Element.text 非空（含 utf8 编码）+ fontFamily="Icon"
// ——证字形绑定正确（非豆腐占位）。
//
// codepoint 0xF059 已通过 Python 解析 FA7 Solid otf 的 cmap 表实证存在
// （D:/project/gui/EUI-NEO/assets/Font Awesome 7 Free-Solid-900.otf 含 4 个
// subtable：format 4 Unicode BMP + format 12 full Unicode；0xF059/0xF128 均
// 在 format 4 覆盖段内）。.icon(codepoint) 实现见 eui/dsl.h:1246——把 codepoint
// 转 utf8 存进 Element.text + 设 fontFamily="Icon"。
//
// 事故：术语问号字形缺失致 §5.3「专业术语旁问号解释」无可用图标，用户无入口
// 理解术语（「迭代上限/1M tokens/FTS5」等）。
// 运行时渲染像素尺寸验证留待 probe（无头单测验绑定正确性，probe 验像素非豆腐）；
// 本场景在 devdesk_tests 默认门禁里跑，不依赖真窗口。
REGISTER_TEST(ui_kit_question_glyph_fa7_solid_0xF059_bound,
              "ui_kit 问号字形 0xF059: ui.text().icon() 后 text 非空 + fontFamily=Icon", {
    eui::Ui ui;
    ui.begin("w02d_q_glyph");
    ui.text("q").icon(eui::utf8(0xF059)).fontSize(14.0f).build();
    ui.end();

    const auto* q = ui.find("q");
    DOCTEST_REQUIRE(q != nullptr);
    // text 字段非空（.icon(0xF059) 把 codepoint 转 utf8 存进 text）
    DOCTEST_CHECK(!q->text.empty());
    // fontFamily = "Icon"（.icon() 侧效应，eui/dsl.h:1248）
    DOCTEST_CHECK_EQ(q->fontFamily, std::string{"Icon"});
    return true;
})

// 场景8（§4.9 备用问号字形 0xF128=fa-question）：同一 FA7 Solid otf 内另一问号
// codepoint，作 fallback 候选（若 P-02 图标口径拍板选裸问号而非圆圈问号）。
// 双 codepoint 都验证绑定正确，给 P-02 决策留余地。
REGISTER_TEST(ui_kit_question_glyph_fa7_solid_0xF128_bound,
              "ui_kit 问号字形 0xF128(fa-question): 绑定正确（P-02 备选）", {
    eui::Ui ui;
    ui.begin("w02d_q_glyph_alt");
    ui.text("q2").icon(eui::utf8(0xF128)).fontSize(14.0f).build();
    ui.end();

    const auto* q = ui.find("q2");
    DOCTEST_REQUIRE(q != nullptr);
    DOCTEST_CHECK(!q->text.empty());
    DOCTEST_CHECK_EQ(q->fontFamily, std::string{"Icon"});
    return true;
})

// ---- UAT6 toast 内容驱动尺寸（ui_kit/toast.cpp，防两条真机事故）----
// 事故1（长文本跑出框）：toast 原固定 360×88，正文区 38px 只装 2 行，长文本第
//   3 行起画出框外。修后：长文本按最大宽换行，高 = 行数×18+边距 全装进框。
// 事故2（短文本空框）：todo setStatus 单行标题也配 88 高（两行高的空框）。
//   修后：仅标题 = 58 高；标题+一行正文 = 78 高。
// 三档（短/中/长换行）+ 仅标题的尺寸断言；几何公式见 ui_kit/toast.cpp 文件头
//（正文顶 44 / 行高 18 / 下留白 16 / 宽 = 内容+106，上限 360 下限 180）。
namespace {
struct ToastBox { float w, h; };
// 合成一帧 toast 并取根节点 frame（显式 layout 后 frame 才有值；Ui 每帧新建，
// 返回值拷贝避免悬垂指针）。
ToastBox measureToastBox(const char* tag, const std::string& title,
                         const std::string& body) {
    ui_kit::ToastState st;
    st.show(title, body, false);
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin(tag);
    ui_kit::drawToast(ui, screen, "tbox", st,
                      core::theme::componentTokensWeChat(
                          core::theme::current()));
    ui.end();
    ui.layout(screen.width, screen.height);
    const auto* root = ui.find("tbox");
    if (root == nullptr) return {-1.0f, -1.0f};
    return {root->frame.width, root->frame.height};
}
bool boxNear(float a, float b) { return std::fabs(a - b) <= 0.5f; }
}  // namespace

REGISTER_TEST(ui_kit_toast_size_content_driven_short,
              "ui_kit toast 尺寸: 短文本=一行高（78）不再配两行高空框", {
    // 防事故2：短正文一行就一行高（44+18+16=78）；原固定 88 高留 10px 死空。
    const ToastBox box = measureToastBox("w02d_t_short", "已保存", "已复制");
    DOCTEST_CHECK_MESSAGE(box.w > 0.0f, "toast 根节点缺失");
    DOCTEST_CHECK_MESSAGE(boxNear(box.h, 78.0f),
                          "短文本高应 78（44+18+16），实测 " +
                              std::to_string(box.h));
    // 宽内容驱动：短文本被 180 下限托住（原固定 360）
    DOCTEST_CHECK_MESSAGE(box.w >= 180.0f && box.w < 360.0f,
                          "短文本宽应落在 [180,360)，实测 " +
                              std::to_string(box.w));
    return true;
})

REGISTER_TEST(ui_kit_toast_size_title_only_compact,
              "ui_kit toast 尺寸: 仅标题（todo setStatus 型）= 58 高零空框", {
    // 防事故2 变体：无正文站点（todo setStatus 恒空 body）原也配 88 高；
    // 修后仅标题 = max(40+16, 图标下限 58) = 58。
    const ToastBox box = measureToastBox("w02d_t_tonly", "已同步完成", "");
    DOCTEST_CHECK_MESSAGE(boxNear(box.h, 58.0f),
                          "仅标题高应 58，实测 " + std::to_string(box.h));
    return true;
})

REGISTER_TEST(ui_kit_toast_size_long_wraps_inside_box,
              "ui_kit toast 尺寸: 长文本按最大宽换行全装进框（≥3 行 + 宽封顶 360）", {
    // 防事故1：60 个 CJK ≈ 840px 正文在 254px 最大宽下 ≥4 行；
    // 原固定 360×88 只装 2 行 → 第 3 行起跑出框。修后高 ≥44+3×18+16=114 全装下。
    std::string longBody;
    for (int i = 0; i < 60; ++i) longBody += "同步";
    const ToastBox box = measureToastBox("w02d_t_long", "导入完成", longBody);
    DOCTEST_CHECK_MESSAGE(box.h >= 114.0f,
                          "长文本高应 ≥114（≥3 行），实测 " +
                              std::to_string(box.h));
    // 宽封顶：超宽内容按 360 上限钳制（防巨框）
    DOCTEST_CHECK_MESSAGE(boxNear(box.w, 360.0f),
                          "长文本宽应封顶 360，实测 " + std::to_string(box.w));
    return true;
})
