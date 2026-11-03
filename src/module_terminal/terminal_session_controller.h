// src/module_terminal/terminal_session_controller.h
// 会话三态状态机（技术卡 §8 F-07）：Running / Exited(N) / Failed(重试)；页面切走保活。
#pragma once

#include <memory>
#include <string>

#include "platform/terminal/terminal_backend.h"

namespace terminal {

enum class SessionState {
    Idle,     // 尚未启动（页面首帧自动 startNew）
    Running,
    Exited,   // 子进程已退出，exitCode 有效；最后一帧内容保留展示
    Failed,   // spawn 失败，errorMessage 可达，可重试
};

class SessionController {
public:
    void setBackend(platform::TerminalBackend* backend);

    // Idle/Exited/Failed → Running（新会话；旧句柄先 terminate 再丢弃）
    void startNew();
    // 主动终止 → Idle（新建入口恢复可用）
    void stop();

    SessionState state() const { return state_; }
    int exitCode() const { return exitCode_; }
    const std::string& errorMessage() const { return error_; }

    // 每帧调用：generation 变化才拉新快照（macOS 主路径；主队列 trampoline 仅 POC/未来 timer）
    void poll();
    const platform::TerminalFrame& frame() const { return frame_; }

    // 输出（keyencoder/TextInputEvent 编码产物）与 resize（widget 量化后回填）
    void writeToSession(const uint8_t* bytes, int32_t count, bool paste = false);
    void resizeGrid(int rows, int cols);
    int sessionRows() const { return sessionRows_; }
    int sessionCols() const { return sessionCols_; }

    // 回滚访问（grid/页面读）
    int scrollbackCount();
    bool scrollbackLine(int index, std::string& out);
    // 选区文本提取（Cmd+C；仅 Running 态）
    bool selectionText(int rowStart, int colStart, int rowEnd, int colEnd, std::string& out);

private:
    platform::TerminalBackend* backend_ = nullptr;
    std::unique_ptr<platform::TerminalSessionHandle> handle_;
    SessionState state_ = SessionState::Idle;
    int exitCode_ = 0;
    std::string error_;
    platform::TerminalFrame frame_;
    int lastGeneration_ = -1;
    int sessionRows_ = 0;
    int sessionCols_ = 0;
};

} // namespace terminal
