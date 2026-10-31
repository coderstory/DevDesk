// tests/test_modules/test_ui_driver_keys.cpp
// 债-18 UI 驱动入口：driver_keys.h 全表无头单测。
// 语义键名 → GLFW 键码 + 修饰位（键码 → InputKey 层在 probe_harness 内，
// 不在无头单测射程——include probe_harness 即拉 png.h，见方案卡 §3 分层纪律）。
#include "doctest.h"
#include "ui_driver/driver_keys.h"

using namespace ui_drive;

REGISTER_TEST(uiDriverKeysFixedNames, "uidrv/keys: 固定键名表（Enter/Escape/Tab/...）", {
    DOCTEST_CHECK_EQ(lookupKeyName("Enter").key, GLFW_KEY_ENTER);
    DOCTEST_CHECK_EQ(lookupKeyName("Escape").key, GLFW_KEY_ESCAPE);
    DOCTEST_CHECK_EQ(lookupKeyName("Tab").key, GLFW_KEY_TAB);
    DOCTEST_CHECK_EQ(lookupKeyName("Backspace").key, GLFW_KEY_BACKSPACE);
    DOCTEST_CHECK_EQ(lookupKeyName("Space").key, GLFW_KEY_SPACE);
    DOCTEST_CHECK_EQ(lookupKeyName("Delete").key, GLFW_KEY_DELETE);
    DOCTEST_CHECK_EQ(lookupKeyName("Insert").key, GLFW_KEY_INSERT);
    DOCTEST_CHECK_EQ(lookupKeyName("Home").key, GLFW_KEY_HOME);
    DOCTEST_CHECK_EQ(lookupKeyName("End").key, GLFW_KEY_END);
    DOCTEST_CHECK_EQ(lookupKeyName("PageUp").key, GLFW_KEY_PAGE_UP);
    DOCTEST_CHECK_EQ(lookupKeyName("PageDown").key, GLFW_KEY_PAGE_DOWN);
    DOCTEST_CHECK_EQ(lookupKeyName("Left").key, GLFW_KEY_LEFT);
    DOCTEST_CHECK_EQ(lookupKeyName("Right").key, GLFW_KEY_RIGHT);
    DOCTEST_CHECK_EQ(lookupKeyName("Up").key, GLFW_KEY_UP);
    DOCTEST_CHECK_EQ(lookupKeyName("Down").key, GLFW_KEY_DOWN);
    return true;
})

REGISTER_TEST(uiDriverKeysLettersDigits, "uidrv/keys: 字母 / 数字键", {
    DOCTEST_CHECK_EQ(lookupKeyName("S").key, GLFW_KEY_S);
    DOCTEST_CHECK_EQ(lookupKeyName("A").key, GLFW_KEY_A);
    DOCTEST_CHECK_EQ(lookupKeyName("Z").key, GLFW_KEY_Z);
    DOCTEST_CHECK_EQ(lookupKeyName("0").key, GLFW_KEY_0);
    DOCTEST_CHECK_EQ(lookupKeyName("9").key, GLFW_KEY_9);
    // 小写不是合法键名（表只收大写——防静默错拼，miss 走相近候选）
    DOCTEST_CHECK_FALSE(lookupKeyName("s").known);
    return true;
})

REGISTER_TEST(uiDriverKeysFunctionRow, "uidrv/keys: F1..F24", {
    DOCTEST_CHECK_EQ(lookupKeyName("F1").key, GLFW_KEY_F1);
    DOCTEST_CHECK_EQ(lookupKeyName("F5").key, GLFW_KEY_F5);
    DOCTEST_CHECK_EQ(lookupKeyName("F12").key, GLFW_KEY_F12);
    DOCTEST_CHECK_EQ(lookupKeyName("F24").key, GLFW_KEY_F24);
    DOCTEST_CHECK_FALSE(lookupKeyName("F25").known);
    DOCTEST_CHECK_FALSE(lookupKeyName("FX").known);
    return true;
})

REGISTER_TEST(uiDriverKeysModifierCombos, "uidrv/keys: 修饰前缀位（Ctrl/Shift/Alt/Cmd/Win）", {
    const KeySpec ctrlS = lookupKeyName("Ctrl+S");
    DOCTEST_CHECK(ctrlS.known);
    DOCTEST_CHECK_EQ(ctrlS.key, GLFW_KEY_S);
    DOCTEST_CHECK_EQ(ctrlS.modifiers, GLFW_MOD_CONTROL);
    DOCTEST_CHECK_FALSE(ctrlS.shortcut);

    const KeySpec combo = lookupKeyName("Ctrl+Shift+Alt+Enter");
    DOCTEST_CHECK(combo.known);
    DOCTEST_CHECK_EQ(combo.modifiers,
                     GLFW_MOD_CONTROL | GLFW_MOD_SHIFT | GLFW_MOD_ALT);

    const KeySpec cmd = lookupKeyName("Cmd+P");
    DOCTEST_CHECK(cmd.known);
    DOCTEST_CHECK_EQ(cmd.modifiers, GLFW_MOD_SUPER);

    const KeySpec win = lookupKeyName("Win+E");
    DOCTEST_CHECK(win.known);
    DOCTEST_CHECK_EQ(win.modifiers, GLFW_MOD_SUPER);
    return true;
})

REGISTER_TEST(uiDriverKeysShortcutPrefix, "uidrv/keys: Shortcut+ 记标志位（执行期平台折算）", {
    const KeySpec spec = lookupKeyName("Shortcut+S");
    DOCTEST_CHECK(spec.known);
    DOCTEST_CHECK_EQ(spec.key, GLFW_KEY_S);
    DOCTEST_CHECK(spec.shortcut);
    DOCTEST_CHECK_EQ(spec.modifiers, 0);  // 修饰位不在表内折算（双平台一致键名）
    return true;
})

REGISTER_TEST(uiDriverKeysMissAndCandidates, "uidrv/keys: miss + 相近候选（≤5 个）", {
    DOCTEST_CHECK_FALSE(lookupKeyName("Entr").known);
    DOCTEST_CHECK_FALSE(lookupKeyName("").known);
    DOCTEST_CHECK_FALSE(lookupKeyName("Ctrl+").known);   // 裸前缀无键名段
    DOCTEST_CHECK_FALSE(lookupKeyName("Ctrl+Entr").known);  // 键名段 miss
    DOCTEST_CHECK_FALSE(lookupKeyName("Meta+S").known);  // 未知修饰前缀 → 整体 miss
    // 相近候选机制（前缀/子串口径）：Ente → Enter
    const std::vector<std::string> hits =
        nearCandidates("Ente", keyNameVocabulary());
    DOCTEST_CHECK_FALSE(hits.empty());
    bool hasEnter = false;
    for (const auto& h : hits) hasEnter = hasEnter || h == "Enter";
    DOCTEST_CHECK(hasEnter);
    DOCTEST_CHECK(hits.size() <= std::size_t(5));
    return true;
})
