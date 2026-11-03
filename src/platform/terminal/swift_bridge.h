// src/platform/terminal/swift_bridge.h
// Swift/CortaBridge interop 封装（macOS 专用，无 posix 兜底族）。
// 隐藏生成头与 Swift 类型：对外只出 std C++ 面。边界按 clue #24 实测限制全部走 void*。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "terminal_backend.h"

namespace platform {

// 一个 Swift 会话的 C++ 侧持有者。所有方法主线程调用（技术卡 §5.3）。
class SwiftBridgeSession {
public:
    // spawn 失败返回 nullptr（corta-exec 缺失/非存在 shell/PTY 失败）
    static std::unique_ptr<SwiftBridgeSession> spawn(
        const std::string& shell, int rows, int cols);
    // 最近一次 spawn 的核心错误描述（UTF-8 写出；false=无记录/容量不足）
    static bool lastSpawnError(uint8_t* out, int32_t capacity);
    ~SwiftBridgeSession();
    SwiftBridgeSession(const SwiftBridgeSession&) = delete;
    SwiftBridgeSession& operator=(const SwiftBridgeSession&) = delete;

    bool write(const uint8_t* bytes, int32_t count, bool paste);
    void resize(int32_t rows, int32_t cols);
    // 快照平铺缓冲。返回 false=网格超容量（不部分拷贝）。out 按 rows*cols 重排。
    bool snapshotCells(int32_t* rows, int32_t* cols, int32_t* cursorRow,
                       int32_t* cursorCol, std::vector<TerminalCell>& out);
    // exited=true 时 exitCode 有效
    bool exited(int32_t* exitCode) const;
    int32_t pendingGeneration() const;
    int32_t scrollbackCount();
    bool scrollbackLineText(int32_t index, std::string& out);
    bool selectionText(int rowStart, int colStart, int rowEnd, int colEnd, std::string& out);
    // 系统剪贴板写入（NSPasteboard；会话无关静态面）
    static bool copyToClipboard(const uint8_t* bytes, int32_t count);
    void stop();

private:
    SwiftBridgeSession();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace platform
