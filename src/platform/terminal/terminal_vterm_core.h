// src/platform/terminal/terminal_vterm_core.h
// libvterm → TerminalFrame 转译层（need:127 Phase 1，技术卡 v2 §1/§3.3）。
// 平台中立：mac 只被 devdesk_tests 链接（录制 VT 字节流 doctest 直测），
// WIN32 被 ConPTY 后端（terminal_backend_win.cpp）链接；不进 mac app 链接面（F-06）。
// 线程模型（single-writer，技术卡 §3.1）：feed()/resize() 由会话读线程独占；
// 快照/回滚/选区读由 UI 主线程锁内拷贝**自有缓冲**（锁内零 libvterm 调用——
// 网格变更全留 feed 线程，v1「Grid 留核心线程」同构）。
// 编码契约（技术卡 v2 P3-2）：TerminalCell.fg/bg = Corta 核心 Color.rawValue 复刻
// ——kind 高 8 位（default=0 / indexed=1 / rgb=2）+ payload 低 24 位（indexed=idx、
// rgb=r<<16|g<<8|b，见 third_party/Corta .../Color.swift）；attrs = CellAttributes
// .rawValue 位面复刻（bold=bit0 italic=bit2 underline=bit3 blink=bit4 reverse=bit5
// invisible=bit6 strikethrough=bit7 doubleUnderline=bit10 curlyUnderline=bit11）。
// widget 侧 ANSI/默认色→主题色映射零改动。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "terminal_backend.h"

namespace platform {

class VtermCore {
public:
    VtermCore(int rows, int cols);
    ~VtermCore();
    VtermCore(const VtermCore&) = delete;
    VtermCore& operator=(const VtermCore&) = delete;

    // 会话读线程独占：喂 VT 字节流。libvterm 产出的应答字节（DSR/DA/焦点/鼠标
    // 上报等）追加到 *replyOut（可空=丢弃）；内部同步 flush damage → 转译网格。
    void feed(const uint8_t* bytes, int32_t count, std::string* replyOut);

    // 经 vterm_set_size（reflow 由 screen 层处理）；完成后按 damage 重转译。
    void resize(int rows, int cols);

    // UI 线程：锁内拷自有缓冲（零 libvterm 调用）。
    TerminalFrame snapshot() const;
    int pendingGeneration() const;

    // 回滚 ring（0=最旧；差一语义对齐 corta #22 实测口径：尾部空行推送差一非缺陷）。
    int scrollbackCount() const;
    bool scrollbackLine(int index, std::string& out) const;

    // 选区矩形→UTF-8 文本（所见即所得；行列=当前视口系，双端闭区间，端点须已
    // 规范化 start<=end——反向拖选的规范化由 widget 侧做，Phase 2 接线）。
    bool selectionText(int rowStart, int colStart, int rowEnd, int colEnd,
                       std::string& out) const;

    // bracketed paste 模式（DECSET 2004 跟踪，feed 内平行小状态机——libvterm
    // 0.3.3 把该模式存在私有 state->mode.bracketpaste（state.c:861）不外露，
    // 详见 .cpp 内 shortcut 注释）。
    bool bracketedPasteEnabled() const;

    // pimpl 状态体（定义收在 .cpp；public 嵌套因 C 回调桥需 cast 访问）
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace platform
