// fontScale / editorFontSize 是进程级全局态：每个用例结尾必须复位，否则后面的用例
// 读到的是上一个留下的值
#include "doctest.h"
#include "core/theme.h"

REGISTER_TEST(theme_fontscale_scales_size_tokens, "theme: setFontScale scales fontSize tokens only", {
    core::theme::setFontScale(1.0f);
    DOCTEST_CHECK_EQ(core::theme::fontScale(), 1.0f);
    DOCTEST_CHECK_EQ(core::theme::current().sizes.fontSizeBody, 14.0f);

    core::theme::setFontScale(1.2f);
    DOCTEST_CHECK_EQ(core::theme::fontScale(), 1.2f);
    DOCTEST_CHECK_EQ(core::theme::current().sizes.fontSizeBody, 14.0f * 1.2f);
    DOCTEST_CHECK_EQ(core::theme::current().sizes.fontSizeSmall, 12.0f * 1.2f);
    DOCTEST_CHECK_EQ(core::theme::current().sizes.fontSizePage, 24.0f * 1.2f);
    DOCTEST_CHECK_EQ(core::theme::current().sizes.fontSizeDisplay, 48.0f * 1.2f);
    DOCTEST_CHECK_EQ(core::theme::current().sizes.spacing, 8.0f);
    DOCTEST_CHECK_EQ(core::theme::current().sizes.radius, 8.0f);
    DOCTEST_CHECK_EQ(core::theme::current().sizes.radiusLarge, 12.0f);

    core::theme::setFontScale(1.0f);  // fontScale 是全局态，结尾复位，否则污染后面的用例
    return true;
})

REGISTER_TEST(theme_fontscale_survives_theme_switch, "theme: font scale survives setDark/setLight (no compounding)", {
    core::theme::setFontScale(1.15f);
    core::theme::setLight();
    DOCTEST_CHECK_EQ(core::theme::fontScale(), 1.15f);
    DOCTEST_CHECK_EQ(core::theme::current().sizes.fontSizeBody, 14.0f * 1.15f);
    // 再切回深色仍是 15 × 1.15：token 重建后统一施加，不叠加也不丢失
    core::theme::setDark();
    DOCTEST_CHECK_EQ(core::theme::current().sizes.fontSizeBody, 14.0f * 1.15f);
    core::theme::setFontScale(1.0f);
    DOCTEST_CHECK_EQ(core::theme::current().sizes.fontSizeBody, 14.0f);
    return true;
})

REGISTER_TEST(theme_fontscale_clamped, "theme: setFontScale clamps to [0.5, 2.0]", {
    core::theme::setFontScale(0.1f);
    DOCTEST_CHECK_EQ(core::theme::fontScale(), 0.5f);
    core::theme::setFontScale(99.0f);
    DOCTEST_CHECK_EQ(core::theme::fontScale(), 2.0f);
    core::theme::setFontScale(1.0f);
    return true;
})

// 编辑器字号是与 fontScale 并行的独立通道：settings 写入 → markdown compose 读取
REGISTER_TEST(theme_editor_font_channel,
              "theme: 编辑器字号通道默认 15 + 钳制 [10,32]（spec缩水 B）", {
    DOCTEST_CHECK_EQ(core::theme::editorFontSize(), 15.0f);
    core::theme::setEditorFontSize(20.0f);
    DOCTEST_CHECK_EQ(core::theme::editorFontSize(), 20.0f);
    core::theme::setEditorFontSize(5.0f);
    DOCTEST_CHECK_EQ(core::theme::editorFontSize(), 10.0f);
    core::theme::setEditorFontSize(99.0f);
    DOCTEST_CHECK_EQ(core::theme::editorFontSize(), 32.0f);
    core::theme::setEditorFontSize(15.0f);  // 恢复默认
    DOCTEST_CHECK_EQ(core::theme::editorFontSize(), 15.0f);
    // 与 fontScale 正交：改 token 缩放不动编辑器字号通道的值
    core::theme::setFontScale(1.15f);
    DOCTEST_CHECK_EQ(core::theme::editorFontSize(), 15.0f);
    core::theme::setFontScale(1.0f);
    return true;
})

// UAT6「字号设置无效，起码没实时生效」：setFontScale 此前是唯一不广播 change
// handler 的 theme mutator（setDark/setLight/setAccent 都通知）——档位变化对
// 下游联动（标题栏/窗口效果回查）无感。回退锁：设档必须广播。
REGISTER_TEST(theme_fontscale_notifies_change_handler,
              "theme: setFontScale 广播 change handler（与 setDark/setLight 同权）", {
    int notified = 0;
    core::theme::setChangeHandler([&notified](bool) { ++notified; });
    core::theme::setFontScale(1.15f);
    DOCTEST_CHECK_EQ(notified, 1);
    // 同档再设也广播（与 setDark/setLight 一致：无同值早退）
    core::theme::setFontScale(1.15f);
    DOCTEST_CHECK_EQ(notified, 2);
    // 复位全局 handler + 档位，防串扰后续用例（handler 是进程级全局态）
    core::theme::setChangeHandler(nullptr);
    core::theme::setFontScale(1.0f);
    DOCTEST_CHECK_EQ(core::theme::fontScale(), 1.0f);
    return true;
})

// H19（2026-10-23 用户拍板）：最小窗（640 逻辑高）自动落 85% 字号档；更大窗保持用户手动档。
// effective = min(用户档, 窗高封顶)；封顶只缩不放（禁替用户放大小字）。
// 先红后绿：在 setWindowHeight 为 no-op 时以下断言必红。
REGISTER_TEST(theme_fontscale_window_height_cap,
              "theme: H19 窗高驱动字号封顶——640 档落 85%、1000 档保持用户档", {
    core::theme::setFontScale(1.0f);
    // 640 档：cap=0.85 → effective = min(1.0, 0.85) = 0.85
    core::theme::setWindowHeight(640.0f);
    DOCTEST_CHECK_EQ(core::theme::fontScale(), 0.85f);
    DOCTEST_CHECK_EQ(core::theme::current().sizes.fontSizeBody, 14.0f * 0.85f);
    // 1000 档：不封顶 → 用户手动档原样
    core::theme::setWindowHeight(1000.0f);
    DOCTEST_CHECK_EQ(core::theme::fontScale(), 1.0f);
    DOCTEST_CHECK_EQ(core::theme::current().sizes.fontSizeBody, 14.0f);
    // 封顶只缩不放：用户 115% 档在 640 被 cap 到 85%，回到 1000 原样保留
    core::theme::setFontScale(1.15f);
    core::theme::setWindowHeight(640.0f);
    DOCTEST_CHECK_EQ(core::theme::fontScale(), 0.85f);
    core::theme::setWindowHeight(1000.0f);
    DOCTEST_CHECK_EQ(core::theme::fontScale(), 1.15f);
    // 复位（fontScale / windowHeight 都是进程级全局态）
    core::theme::setFontScale(1.0f);
    core::theme::setWindowHeight(1000.0f);
    return true;
})
