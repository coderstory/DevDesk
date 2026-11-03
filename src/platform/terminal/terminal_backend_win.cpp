// src/platform/terminal/terminal_backend_win.cpp — Windows 占位（F-06）：
// available()=false + 全部操作返回「未实现」Result，不伪造成功。
// v1 后续接 ConPTY + libvterm（need:123，平台中立数据模型 cell 网格已对齐）。
// ⚠ 平台归属：Windows 机收口时验证构建绿（本机 macOS 无法构建本分支）。
#include "terminal_backend.h"

namespace platform {

namespace {

class WinSessionHandle : public TerminalSessionHandle {
public:
    core::Result<void> write(const uint8_t*, int32_t, bool) override {
        return core::Result<void>::fail(core::ErrorCode::NotImplemented, "Windows 终端 v1 未实现");
    }
    core::Result<void> resize(const TerminalSize&) override {
        return core::Result<void>::fail(core::ErrorCode::NotImplemented, "Windows 终端 v1 未实现");
    }
    void terminate() override {}
    TerminalFrame poll() override { return TerminalFrame{}; }
    bool exited(int32_t*) override { return false; }
    int pendingGeneration() override { return 0; }
    int scrollbackCount() override { return 0; }
    bool scrollbackLine(int, std::string&) override { return false; }
    // TODO(need:123): ConPTY + libvterm 真实现
};

class WinTerminalBackend : public TerminalBackend {
public:
    core::Result<std::unique_ptr<TerminalSessionHandle>> spawn(
        const std::string&, const TerminalSize&) override {
        return core::Result<std::unique_ptr<TerminalSessionHandle>>::fail(
            core::ErrorCode::NotImplemented, "Windows 终端 v1 未实现（占位页）");
    }
    bool available() const override { return false; }
    const char* unavailableReason() const override {
        return "终端在 Windows 版即将支持（v1 占位）";
    }
};

} // namespace

TerminalBackend* terminalBackendMac() {
    // Windows 编译单元：同名工厂返回占位后端（接口名保持中立，避免 CMake 选源外的差异）
    static WinTerminalBackend backend;
    return &backend;
}

// 剪贴板占位（mac 实现在 terminal_backend_mac.cpp）
bool clipboardCopyText(const uint8_t*, int32_t) { return false; }

} // namespace platform
