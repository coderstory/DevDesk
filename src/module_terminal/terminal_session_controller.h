// src/module_terminal/terminal_session_controller.h
// 多会话管理（need241 v2 设计 §4.1/§6）：每 TAB 一会话（独立 backend handle + vterm
// 帧 + scrollback 偏移记忆 + 三态状态机）。activeIndex 承载页面渲染焦点；关闭 =
// 右邻优先激活（无右邻则左邻）；会话序号进程内递增不复用。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "platform/terminal/terminal_backend.h"

namespace terminal {

enum class SessionState {
    Idle,     // 尚未启动（首个会话页面进入时自动 start）
    Running,
    Exited,   // 子进程已退出，exitCode 有效；最后一帧内容保留展示
    Failed,   // spawn 失败，errorMessage 可达，可重试
};

// 单会话全部运行态（页面/网格只经 controller 间接读）
struct Session {
    std::unique_ptr<platform::TerminalSessionHandle> handle;
    SessionState state = SessionState::Idle;
    int exitCode = 0;
    std::string error;
    platform::TerminalFrame frame;
    int lastGeneration = -1;
    int rows = 0;
    int cols = 0;
    int scrollOffset = 0;      // scrollback 回滚偏移（每会话记忆，0=贴底）
    float scrollRemainder = 0.0f;
    int titleNumber = 0;       // 「终端 N」，进程内递增不复用
};

class SessionController {
public:
    void setBackend(platform::TerminalBackend* backend);

    // 新建会话追加到序列尾并激活，返回新会话 index。backend 不可用 / spawn 失败
    // 不抛不崩：会话以 Failed 态占位（TAB 圆点红 + 覆盖卡错误详情）。
    int startSession();
    // 关闭第 i 个会话（终止进程，无确认；语义=终止）。激活右邻优先，无右邻则左邻；
    // 关最后一个 → activeIndex=-1（空态）。
    void closeSession(int index);
    void setActive(int index);
    int activeIndex() const { return activeIndex_; }
    int count() const { return static_cast<int>(sessions_.size()); }
    bool empty() const { return sessions_.empty(); }

    Session* active();
    const Session* active() const;
    Session* at(int index);
    const Session* at(int index) const;

    // 每帧调用：全会话拉新快照（generation 变化才拷贝）+ 退出收割——后台 TAB
    // 的状态点/覆盖卡也要随会话态变化。
    void pollAll();

    // 激活会话的输入/resize/选区路由（无激活会话时静默丢弃）。
    // 滚动偏移不在此路由：折算需要 cell 真高（page 持 metrics），直接写 Session.scrollOffset。
    void writeToActive(const uint8_t* bytes, int32_t count, bool paste = false);
    void resizeActive(int rows, int cols);
    int activeScrollbackCount();
    bool activeScrollbackLine(int index, std::string& out);
    bool activeSelectionText(int rowStart, int colStart, int rowEnd, int colEnd, std::string& out);

    // 重试/重新开始：把第 i 个会话回到 Running（Exited/Failed 复位重建）
    void restartSession(int index);

private:
    int spawnInto_(Session& s);

    platform::TerminalBackend* backend_ = nullptr;
    std::vector<Session> sessions_;
    int activeIndex_ = -1;
    int nextTitleNumber_ = 1;
};

} // namespace terminal
