// notepad_protect.cpp — 未保存保护纯决策层实现（need:263 批 4 棒 A）。
#include "notepad_protect.h"

#include "notepad_session.h"  // detail::sessionRegistry（quit guard 扫描）

namespace module::notepad {

NotepadProtectOutcome notepadProtectOutcomeFor(NotepadProtectChoice choice,
                                               bool saveSucceeded,
                                               bool quitContext) {
    NotepadProtectOutcome out;
    switch (choice) {
        case NotepadProtectChoice::Save:
            if (!saveSucceeded) {
                return out;  // 保存失败（mtime 拦截/写失败）：留窗 + 退出链中止
            }
            if (quitContext) {
                out.rearmExit = true;  // 该窗已干净，下一帧 guard 继续下一窗
            } else {
                out.closeWindow = true;
            }
            return out;
        case NotepadProtectChoice::Discard:
            out.armExempt = true;
            if (quitContext) {
                out.rearmExit = true;  // guard 下一帧按豁免跳过，继续下一窗
            } else {
                out.closeWindow = true;
            }
            return out;
        case NotepadProtectChoice::Cancel:
            return out;  // 留窗；退出语境不 rearm = 中止整个退出（D-3）
    }
    return out;
}

bool notepadCloseShouldIntercept(NotepadSession& s) {
    if (!s.dirty) return false;  // 干净窗直通放行
    if (s.skipCloseConfirm) {
        s.skipCloseConfirm = false;  // 一次性豁免：本次放行即消费
        return false;
    }
    return true;  // 脏窗 → 拦截（调用方弹三选）
}

NotepadQuitScan notepadQuitScan(const std::vector<NotepadSession*>& ordered) {
    for (size_t i = 0; i < ordered.size(); ++i) {
        const NotepadSession& s = *ordered[i];
        if (s.protectOpen) {
            return {NotepadQuitScan::Kind::WaitDialog, i};  // 待决弹窗等用户
        }
        if (s.dirty && !s.skipCloseConfirm) {
            return {NotepadQuitScan::Kind::AskAt, i};  // 首个待决脏窗
        }
    }
    return {NotepadQuitScan::Kind::Allow, 0};
}

bool notepadQuitGuard() {
    std::vector<NotepadSession*> ordered;
    for (const auto& session : detail::sessionRegistry()) {
        ordered.push_back(session.get());
    }
    const NotepadQuitScan scan = notepadQuitScan(ordered);
    if (scan.kind == NotepadQuitScan::Kind::Allow) {
        return true;  // 全干净/全豁免/注册表空 → 放行
    }
    if (scan.kind == NotepadQuitScan::Kind::WaitDialog) {
        return false;  // 已有待决弹窗，主循环继续（请求已消费，由弹窗回调再入队）
    }
    NotepadSession& s = *ordered[scan.index];
    s.protectOpen = true;
    s.protectQuitContext = true;  // 弹窗回调走退出语境（rearmExit 路径）
    return false;
}

}  // namespace module::notepad
