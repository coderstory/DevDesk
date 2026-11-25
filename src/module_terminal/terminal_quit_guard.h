// src/module_terminal/terminal_quit_guard.h
// M2 Task 3：终端会话的关窗/退出确认判定（纯函数，可单测）。
// 口径：只有 Running（子进程在跑、可能丢输出）需要确认；Idle/Starting/Exited/Failed 直通。
#pragma once

#include "terminal_session_controller.h"

namespace terminal {

inline bool stateNeedsCloseConfirm(SessionState state) {
    return state == SessionState::Running;
}

inline bool anyRunningSession(const SessionController& c) {
    for (int i = 0; i < c.count(); ++i) {
        const Session* s = c.at(i);
        if (s != nullptr && stateNeedsCloseConfirm(s->state)) return true;
    }
    return false;
}

inline int firstRunningSession(const SessionController& c) {
    for (int i = 0; i < c.count(); ++i) {
        const Session* s = c.at(i);
        if (s != nullptr && stateNeedsCloseConfirm(s->state)) return i;
    }
    return -1;
}

// 应用退出守卫（app::setQuitGuard 组合入口；实现见 module_terminal.cpp）
bool terminalQuitGuard();

} // namespace terminal
