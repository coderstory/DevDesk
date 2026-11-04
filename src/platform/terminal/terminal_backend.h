// src/platform/terminal/terminal_backend.h
// 终端会话平台中立接口（技术卡 §5.2）。概念面 {spawn, read=poll, write, resize, terminate}
// 对齐 ConPTY（win-terminal #16）；平台中立数据模型取「cell 网格 + 事件」而非裸字节——
// 两端终端核（Corta Grid / 未来 libvterm screen）产出形状一致。
// 键盘→VT 字节编码在 module 层（terminal_keyencoder），不在此接口。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/result.h"

namespace platform {

struct TerminalSize {
    int rows = 0;
    int cols = 0;
};

// 布局同桥接 BridgeCell（POC 镜像契约）。fg/bg 为核心 Color.rawValue 透传（kind 位+payload），
// ANSI/默认色到主题色的映射在 module 层 widget 做。
struct TerminalCell {
    uint32_t cp = 0;
    uint32_t fg = 0;
    uint32_t bg = 0;
    uint16_t attrs = 0;
    uint8_t width = 1;
};

// poll() 的产物：可视网格平铺快照。exited=true 时 cells 无效（终端内容仍在，最后一帧已返回过）。
struct TerminalFrame {
    int rows = 0;
    int cols = 0;
    int cursorRow = 0;
    int cursorCol = 0;
    std::vector<TerminalCell> cells;
};

class TerminalSessionHandle {
public:
    virtual ~TerminalSessionHandle() = default;
    // paste=true 走 bracketed paste 语义（核心 write(paste:closing:)）
    virtual core::Result<void> write(const uint8_t* bytes, int32_t count, bool paste) = 0;
    virtual core::Result<void> resize(const TerminalSize& size) = 0;
    virtual void terminate() = 0;
    // 子进程已退出时返回 true 并填 exitCode（1=正常退出码；信号=128+sig 约定在桥接层折算）
    virtual bool exited(int32_t* exitCode) = 0;
    // read 第一形态：非阻塞拉当前可视网格快照（主线程调）；exited 后仍返回末帧
    //（win=转译层网格保留、mac=核心快照保留——「会话已结束」态展示依赖此契约）
    virtual TerminalFrame poll() = 0;
    // 输出批次单调计数（渲染循环保养判断；macOS=子进程输出即增，Windows=libvterm
    // damage 聚合计数）
    virtual int pendingGeneration() = 0;
    virtual int scrollbackCount() = 0;
    // 读回滚行 UTF-8 文本（0=最旧已推入行）；false=越界/溢出
    virtual bool scrollbackLine(int index, std::string& out) = 0;
    // 选区矩形→文本（所见即所得，核心 Selection.text；行列=当前视口系）
    virtual bool selectionText(int rowStart, int colStart, int rowEnd, int colEnd,
                               std::string& out) = 0;
};

// 平台后端。macOS=Swift/Corta 桥；Windows=ConPTY 桥（available() 动态探测 1809+，
// need:127 真实现）。
class TerminalBackend {
public:
    virtual ~TerminalBackend() = default;
    virtual core::Result<std::unique_ptr<TerminalSessionHandle>> spawn(
        const std::string& shell, const TerminalSize& size) = 0;
    virtual bool available() const = 0;
    virtual const char* unavailableReason() const = 0;
};

// macOS 后端单例（terminal_backend_mac.cpp；Windows 占位走 terminal_backend_win.cpp，
// 两 TU 各自提供同名工厂与剪贴板函数，CMake 按平台选源）
TerminalBackend* terminalBackendMac();

// 选区文本写系统剪贴板（mac=NSPasteboard 经桥接；win=Win32 clipboard）
bool clipboardCopyText(const uint8_t* bytes, int32_t count);

// 键位惯例（技术卡 v2 决策 4/§5）：复制/粘贴主键的平台差异缝。
// mac 常量=现行为原样（Cmd+C 走 module 兜底、Cmd+V 走 pasteText，无二象性）；
// win=Ctrl+C 二象性（有选区复制/无选区 \x03，分派纯函数见 terminal_keyencoder
// encodeCtrlC）+ Ctrl+V 粘贴（EUI pasteText 链路天然承载，page 不拦截）。
struct TerminalKeyConventions {
    bool ctrlCIsCopy;  // true=Win 惯例：Ctrl+C 在有选区时复制
    bool ctrlVIsPaste; // true=Win 惯例：Ctrl+V 为粘贴主键
};
TerminalKeyConventions terminalKeyConventions();

} // namespace platform
