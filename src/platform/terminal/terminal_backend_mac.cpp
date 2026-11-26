// src/platform/terminal/terminal_backend_mac.cpp — macOS 后端：Swift/Corta 桥。
// ⚠ 无 posix 兜底族（mac/win 直配，win 侧 terminal_backend_win.cpp 为 Phase 2）。
#include "terminal_backend.h"

#include <cstdlib>
#include <pwd.h>
#include <utility>
#include <unistd.h>

#include "swift_bridge.h"

namespace platform {

namespace {

// 默认 shell 解析（技术卡决策 6）：env 测试注入口 → $SHELL → pw_shell → /bin/zsh 终兜底。
std::string resolveDefaultShell() {
    // 核心 Spawn 要求绝对路径（executablePathNotAbsolute）；各段一律绝对化校验
    if (const char* injected = std::getenv("DEVDESK_TERMINAL_SHELL")) {
        if (injected[0] == '/') return injected;
    }
    if (const char* sh = std::getenv("SHELL")) {
        if (sh[0] == '/' && access(sh, X_OK) == 0) return sh;
    }
    if (struct passwd* pw = getpwuid(getuid())) {
        if (pw->pw_shell && pw->pw_shell[0] == '/') return pw->pw_shell;
    }
    return "/bin/zsh";
}

// 设置面板 shell 值 → spawn 可执行（need330）：上游 Spawn 要求绝对路径
//（executablePathNotAbsolute），而 need250 选项表存的是裸名（""/"bash"/"sh"）。
// 裸名 → /bin/<名> 可执行即用；绝对路径校验可执行；任意分支落空回落默认解析链，
// 保证 spawn 永远拿到可执行的绝对路径。
std::string resolveShellExecutable(const std::string& shell) {
    if (shell.empty()) return resolveDefaultShell();
    if (shell[0] == '/') {
        if (access(shell.c_str(), X_OK) == 0) return shell;
        return resolveDefaultShell();
    }
    const std::string bin = "/bin/" + shell;
    if (access(bin.c_str(), X_OK) == 0) return bin;
    return resolveDefaultShell();
}

class MacSessionHandle : public TerminalSessionHandle {
public:
    explicit MacSessionHandle(std::unique_ptr<SwiftBridgeSession> s) : s_(std::move(s)) {}

    core::Result<void> write(const uint8_t* bytes, int32_t count, bool paste) override {
        if (!s_->write(bytes, count, paste)) {
            return core::Result<void>::fail(core::ErrorCode::Internal, "会话已停止");
        }
        return core::Result<void>::ok();
    }

    core::Result<void> resize(const TerminalSize& size) override {
        s_->resize(size.rows, size.cols);
        return core::Result<void>::ok();
    }

    void terminate() override { s_->stop(); }

    uint32_t takeBells() override {
        if (s_->takeBell()) ++pendingBells_;
        const uint32_t out = pendingBells_;
        pendingBells_ = 0;
        return out;
    }

    bool exited(int32_t* exitCode) override { return s_->exited(exitCode); }

    TerminalFrame poll() override {
        TerminalFrame frame;
        int32_t rows = 0, cols = 0, cr = 0, cc = 0;
        if (s_->snapshotCells(&rows, &cols, &cr, &cc, frame.cells)) {
            frame.rows = rows;
            frame.cols = cols;
            frame.cursorRow = cr;
            frame.cursorCol = cc;
        }
        return frame;
    }

    int pendingGeneration() override { return s_->pendingGeneration(); }
    int scrollbackCount() override { return s_->scrollbackCount(); }
    bool scrollbackLine(int index, std::string& out) override {
        return s_->scrollbackLineText(index, out);
    }
    bool selectionText(int rowStart, int colStart, int rowEnd, int colEnd,
                       std::string& out) override {
        return s_->selectionText(rowStart, colStart, rowEnd, colEnd, out);
    }

    // 鼠标上报：转发给 Swift 桥（Corta 只读状态给「是否要鼠标」，事件由桥按 SGR 编码
    // 写入 PTY）。只支持 SGR（?1006）：非 SGR 编码（X10 默认态 / UTF8 ?1005）暂不实现
    // ——opencode / vim / tmux 均启用 SGR，且 Corta 的复合模式在 SGR 未开时读作 off。
    void mouseMove(int row, int col, bool buttonDown) override {
        s_->mouseMove(row, col, buttonDown);
    }
    void mouseButton(int row, int col, int button, bool pressed) override {
        s_->mouseButton(row, col, button, pressed);
    }
    bool mouseReporting() const override { return s_->mouseReporting(); }
    // 备屏：Corta 公开 isAlternateScreenActive（桥读取），真机未验证（见汇报）
    bool altscreen() const override { return s_->altscreen(); }

private:
    std::unique_ptr<SwiftBridgeSession> s_;
    uint32_t pendingBells_ = 0; // takeBells 聚合（Corta 一次一真）
};

class MacTerminalBackend : public TerminalBackend {
public:
    core::Result<std::unique_ptr<TerminalSessionHandle>> spawn(
        const std::string& shell, const TerminalSize& size,
        const std::string& workDir) override {
        // need328：workDir 接线桥 spawn；空/非空语义在桥内 resolveSpawnWorkDir
        // 统一解析（空 → $HOME，与 win 侧 %USERPROFILE% 对齐）。shell 选名
        //（zsh/bash/sh）直作 exe 透传，空 = 默认解析（决策 6：env 注入 → $SHELL → pw_shell → /bin/zsh）
        const std::string exe = resolveShellExecutable(shell);
        auto s = SwiftBridgeSession::spawn(exe, size.rows, size.cols, workDir);
        if (!s) {
            char reason[512] = {0};
            const bool hasReason = platform::SwiftBridgeSession::lastSpawnError(
                reinterpret_cast<uint8_t*>(reason), sizeof reason);
            return core::Result<std::unique_ptr<TerminalSessionHandle>>::fail(
                core::ErrorCode::Internal,
                hasReason ? std::string(reason) : "终端启动失败（corta-exec 缺失或 shell 不可执行）");
        }
        return core::Result<std::unique_ptr<TerminalSessionHandle>>::ok(
            std::make_unique<MacSessionHandle>(std::move(s)));
    }

    bool available() const override { return true; }
    const char* unavailableReason() const override { return ""; }
};

} // namespace

TerminalBackend* terminalBackendMac() {
    static MacTerminalBackend backend;
    return &backend;
}

bool clipboardCopyText(const uint8_t* bytes, int32_t count) {
    return SwiftBridgeSession::copyToClipboard(bytes, count);
}

// mac 键位惯例=现行为原样（need:127 F-06 零行为变化）：Cmd+C 复制走 module 兜底、
// Cmd+V 粘贴走 pasteText 链路，Ctrl+C 无二象性、Ctrl+V 非粘贴主键
TerminalKeyConventions terminalKeyConventions() {
    return TerminalKeyConventions{.ctrlCIsCopy = false, .ctrlVIsPaste = false};
}

} // namespace platform
