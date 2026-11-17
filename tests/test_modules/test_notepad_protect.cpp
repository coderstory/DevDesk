// tests/test_modules/test_notepad_protect.cpp
// need:263 批 4 棒 A —— 未保存保护纯决策层无头直测（F-05，实现
// src/module_notepad/notepad_protect.{h,cpp}）：三选动作→后续动作映射表 /
// 关窗 veto 裁决与豁免旗标一次性消费 / quit guard 扫描三态（clue #269
// 三条穷尽路径的纯函数面）。对话框本体 UI 与 guard 全链路 = probe G 段。
#include "doctest.h"

#include "module_notepad/notepad_protect.h"
#include "module_notepad/notepad_session.h"  // sessionSetDirty（豁免作废单点）

#include <vector>

namespace np = module::notepad;

namespace {

np::NotepadSession makeSession(bool dirty) {
    np::NotepadSession s;
    if (dirty) s.dirty = true;
    return s;
}

}  // namespace

// 三选动作 → 后续动作映射表（关窗语境）。
REGISTER_TEST(notepad_protect_outcome_close_ctx, "notepad_protect: 三选映射（关窗语境）", {
    using C = np::NotepadProtectChoice;
    // 保存成功 → 关窗；保存失败 → 留窗（mtime 拦截/写失败不关）
    const np::NotepadProtectOutcome saveOk =
        np::notepadProtectOutcomeFor(C::Save, true, false);
    DOCTEST_CHECK(saveOk.closeWindow);
    DOCTEST_CHECK_FALSE(saveOk.rearmExit);
    DOCTEST_CHECK_FALSE(saveOk.armExempt);
    const np::NotepadProtectOutcome saveFail =
        np::notepadProtectOutcomeFor(C::Save, false, false);
    DOCTEST_CHECK_FALSE(saveFail.closeWindow);
    DOCTEST_CHECK_FALSE(saveFail.rearmExit);
    DOCTEST_CHECK_FALSE(saveFail.armExempt);
    // 不保存 → 置豁免 + 关窗；取消 → 全 false 留窗
    const np::NotepadProtectOutcome discard =
        np::notepadProtectOutcomeFor(C::Discard, true, false);
    DOCTEST_CHECK(discard.armExempt);
    DOCTEST_CHECK(discard.closeWindow);
    DOCTEST_CHECK_FALSE(discard.rearmExit);
    const np::NotepadProtectOutcome cancel =
        np::notepadProtectOutcomeFor(C::Cancel, true, false);
    DOCTEST_CHECK_FALSE(cancel.closeWindow);
    DOCTEST_CHECK_FALSE(cancel.rearmExit);
    DOCTEST_CHECK_FALSE(cancel.armExempt);
    return true;
})

// 三选动作 → 后续动作映射表（退出语境：不逐窗关窗，统一 requestExit 再入队）。
REGISTER_TEST(notepad_protect_outcome_quit_ctx, "notepad_protect: 三选映射（退出语境 rearm）", {
    using C = np::NotepadProtectChoice;
    // 保存成功 → rearmExit（该窗已干净，下一帧 guard 跳过继续下一窗）
    const np::NotepadProtectOutcome saveOk =
        np::notepadProtectOutcomeFor(C::Save, true, true);
    DOCTEST_CHECK(saveOk.rearmExit);
    DOCTEST_CHECK_FALSE(saveOk.closeWindow);
    // 保存失败 → 链中止（不 rearm，用户处理后重新发起退出）
    const np::NotepadProtectOutcome saveFail =
        np::notepadProtectOutcomeFor(C::Save, false, true);
    DOCTEST_CHECK_FALSE(saveFail.rearmExit);
    DOCTEST_CHECK_FALSE(saveFail.closeWindow);
    // 不保存 → 豁免 + rearm；取消 → 不 rearm = 中止整个退出（D-3）
    const np::NotepadProtectOutcome discard =
        np::notepadProtectOutcomeFor(C::Discard, true, true);
    DOCTEST_CHECK(discard.armExempt);
    DOCTEST_CHECK(discard.rearmExit);
    DOCTEST_CHECK_FALSE(discard.closeWindow);
    const np::NotepadProtectOutcome cancel =
        np::notepadProtectOutcomeFor(C::Cancel, true, true);
    DOCTEST_CHECK_FALSE(cancel.rearmExit);
    return true;
})

// 关窗 veto 裁决：干净直通 / 脏拦截 / 豁免放行且一次性消费。
REGISTER_TEST(notepad_protect_close_intercept, "notepad_protect: veto 裁决与豁免一次性消费", {
    np::NotepadSession clean = makeSession(false);
    DOCTEST_CHECK_FALSE(np::notepadCloseShouldIntercept(clean));

    np::NotepadSession dirty = makeSession(true);
    DOCTEST_CHECK_TRUE(np::notepadCloseShouldIntercept(dirty));  // 脏 → 拦截弹三选

    // 豁免旗标：置位后放行并**消费**（一次性）；下次脏关窗重新保护
    np::NotepadSession exempt = makeSession(true);
    exempt.skipCloseConfirm = true;
    DOCTEST_CHECK_FALSE(np::notepadCloseShouldIntercept(exempt));  // 放行 + 消费
    DOCTEST_CHECK_FALSE(exempt.skipCloseConfirm);
    DOCTEST_CHECK_TRUE(np::notepadCloseShouldIntercept(exempt));   // 已消费 → 再拦
    return true;
})

// 豁免旗标作废单点：任何新的脏化（含已脏再编辑的早返回路径）作废豁免。
REGISTER_TEST(notepad_protect_exempt_invalidated_on_edit, "notepad_protect: 再编辑作废豁免旗标", {
    np::NotepadSession s = makeSession(true);
    s.skipCloseConfirm = true;
    // 已脏再编辑：sessionSetDirty(true) 走早返回路径也要清旗标
    np::sessionSetDirty(s, true);
    DOCTEST_CHECK_FALSE(s.skipCloseConfirm);
    DOCTEST_CHECK_TRUE(np::notepadCloseShouldIntercept(s));
    return true;
})

// quit guard 扫描：Allow（全干净/全豁免/空）/ WaitDialog（待决弹窗不叠开）/ AskAt（首个待决脏窗）。
REGISTER_TEST(notepad_protect_quit_scan, "notepad_protect: quit guard 扫描三态", {
    using Kind = np::NotepadQuitScan::Kind;
    // 空注册表 → Allow（穷尽路径③：窗均已关闭）
    std::vector<np::NotepadSession*> empty;
    DOCTEST_CHECK_EQ(np::notepadQuitScan(empty).kind, Kind::Allow);

    // 全干净 → Allow（穷尽路径①：全部窗已处理完）
    np::NotepadSession clean = makeSession(false);
    std::vector<np::NotepadSession*> allClean{&clean};
    DOCTEST_CHECK_EQ(np::notepadQuitScan(allClean).kind, Kind::Allow);

    // 首个脏窗 AskAt(0)；豁免后跳到下一窗 AskAt(1)；全豁免 → Allow
    np::NotepadSession w1 = makeSession(true);
    np::NotepadSession w2 = makeSession(true);
    std::vector<np::NotepadSession*> both{&w1, &w2};
    const np::NotepadQuitScan ask0 = np::notepadQuitScan(both);
    DOCTEST_CHECK_EQ(ask0.kind, Kind::AskAt);
    DOCTEST_CHECK_EQ(ask0.index, size_t{0});
    w1.skipCloseConfirm = true;  // 豁免 w1（guard 侧不消费旗标，放行到窗关）
    const np::NotepadQuitScan ask1 = np::notepadQuitScan(both);
    DOCTEST_CHECK_EQ(ask1.kind, Kind::AskAt);
    DOCTEST_CHECK_EQ(ask1.index, size_t{1});
    w2.skipCloseConfirm = true;
    DOCTEST_CHECK_EQ(np::notepadQuitScan(both).kind, Kind::Allow);

    // 待决弹窗（任意语境）→ WaitDialog：不叠开第二张（w2 虽脏也等）
    np::NotepadSession w3 = makeSession(true);
    np::NotepadSession w4 = makeSession(true);
    w3.protectOpen = true;
    std::vector<np::NotepadSession*> wait{&w3, &w4};
    const np::NotepadQuitScan wd = np::notepadQuitScan(wait);
    DOCTEST_CHECK_EQ(wd.kind, Kind::WaitDialog);
    DOCTEST_CHECK_EQ(wd.index, size_t{0});
    return true;
})
