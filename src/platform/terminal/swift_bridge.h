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

// need328：spawn 目录解析唯一入口——非空直传；空 → $HOME（与 win 侧 %USERPROFILE%
// 语义对齐，均不依赖进程 cwd）。自由函数供单测直测传参链。
std::string resolveSpawnWorkDir(const std::string& workDir);

// bug508：网格尺寸钳制结果（rows/cols 已限幅；clamped=true 表示发生过钳制）
struct GridSize {
    int32_t rows;
    int32_t cols;
    bool clamped;
};

// 钳制到快照镜像上限（640 列 × 240 行，与 Swift BridgeLimits 镜像同值）。纯函数可单测。
GridSize clampGridSize(int32_t rows, int32_t cols);

// 一个 Swift 会话的 C++ 侧持有者。所有方法主线程调用（技术卡 §5.3）。
class SwiftBridgeSession {
public:
    // spawn 失败返回 nullptr（corta-exec 缺失/非存在 shell/PTY 失败）
    // workDir 语义（need328）：空 → $HOME、非空原值（解析唯一入口 resolveSpawnWorkDir）
    static std::unique_ptr<SwiftBridgeSession> spawn(
        const std::string& shell, int rows, int cols, const std::string& workDir);
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
    // bell 边沿（Corta takeBell：一次一真）。M2 Task 6 补齐 mac 侧。
    bool takeBell();
    int32_t pendingGeneration() const;
    int32_t scrollbackCount();
    bool scrollbackLineText(int32_t index, std::string& out);
    bool selectionText(int rowStart, int colStart, int rowEnd, int colEnd, std::string& out);

    // 鼠标上报（0 基行列，页面契约见 terminal_backend.h:74-92）。
    // Corta 只读状态给出「应用是否要鼠标」与「是否 SGR 编码」，没有事件注入 API，
    // 因此事件在 C++ 侧按 SGR（?1006）规则编码后复用 write 通道写入 PTY。
    // 非 SGR 编码（X10 / UTF8）不产出：SGR 未开时 Corta 复合值即 off，等价于不上报。
    int32_t mouseTrackingMode();  // 0=off 1000=normal 1002=buttonEvent 1003=anyEvent
    bool sgrMouseEncodingEnabled();
    bool mouseReporting();
    bool altscreen();
    // button：0=左 1=中 2=右 4=上滚 5=下滚；pressed 按下/松开各一次
    //（滚轮只发按下形态，松开被吞——见实现注释）。
    void mouseMove(int row, int col, bool buttonDown);
    void mouseButton(int row, int col, int button, bool pressed);
    // 系统剪贴板写入（NSPasteboard；会话无关静态面）
    static bool copyToClipboard(const uint8_t* bytes, int32_t count);
    void stop();

private:
    SwiftBridgeSession();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace platform
