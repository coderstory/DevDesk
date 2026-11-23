// src/module_terminal/web/web_grid.h
// 网页网格帧编解码（need#490 技术卡 v9 §5.1）：TerminalFrame → run 编码 → 二进制帧。
// 帧 = 24B 定长头 + 行区（u16 rowCount，逐行 u16 rowIndex + u16 runCount + 逐 run）。
// run = 16B 定长头（cells/width/flags/fgRGBA/bgRGBA/attrsRaw/textLen）+ UTF-8 文本。
// 相邻格 width/attrs/fgRGBA/bgRGBA 任一不同即切 run；宽字占位格并入前 run。
// 行哈希 FNV-1a 64 驱动增量：首帧/full 清增量，之后只带变化行。seq 由桥在 SSE 层分配
// （帧内不序列化，见技术卡 §5.2）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "platform/terminal/terminal_backend.h"

namespace terminal::web {

inline constexpr uint8_t kGridMagic0 = 'D';
inline constexpr uint8_t kGridMagic1 = 'W';
inline constexpr uint8_t kGridVersion = 1;
inline constexpr int kGridHeaderBytes = 24;
inline constexpr int kRunHeaderBytes = 16;
// 单帧明文上限（技术卡 §5.3）：超过降级纯文本行。
inline constexpr size_t kGridMaxFrameBytes = 256 * 1024;
// 头 flags 位（第 3 字节）
inline constexpr uint8_t kHeaderCursorVisible = 1u << 0;
inline constexpr uint8_t kHeaderFull = 1u << 1;
// run flags 位（第 3 字节）
inline constexpr uint8_t kRunFgDefault = 1u << 0;
inline constexpr uint8_t kRunBgDefault = 1u << 1;
inline constexpr uint8_t kRunBold = 1u << 2;
inline constexpr uint8_t kRunUnderline = 1u << 3;

struct WebRun {
    uint16_t cells = 0;   // 覆盖列数（宽字 2、半角 1、占位格不另计）
    uint8_t width = 1;    // 1=半角 2=宽字
    uint8_t flags = 0;
    uint32_t fgRgba = 0;  // 0xRRGGBBAA
    uint32_t bgRgba = 0;  // 0xRRGGBBAA；默认背景 alpha=0
    uint16_t attrsRaw = 0;
    std::string text;     // UTF-8；空白格为空格
};

struct WebRow {
    uint16_t rowIndex = 0;
    std::vector<WebRun> runs;
};

struct WebFrameHeader {
    bool cursorVisible = true;
    bool full = true;
    uint16_t rows = 0;
    uint16_t cols = 0;
    uint16_t cursorRow = 0;
    uint16_t cursorCol = 0;
    uint32_t scrollOffset = 0;
    uint32_t scrollbackCount = 0;
    uint32_t generation = 0;
};

struct WebFrame {
    WebFrameHeader header;
    std::vector<WebRow> rows;
    uint64_t seq = 0;  // SSE 事件序号，桥分配；0 = 未分配
};

// FNV-1a 64（行内容指纹）：cp/fg/bg/attrs/width 全参与，判增量用。
uint64_t rowHash(const platform::TerminalCell* row, int cols);

// 单行 → run 列表（可直测）。空白格产空格文本，保证各 run cells 之和 == cols，
// 网页按列定位无需额外起始列字段。
std::vector<WebRun> encodeRowRuns(const platform::TerminalCell* row, int cols, bool darkTheme);

// WebFrame → 明文二进制（24B 头 + 行区）。
std::vector<uint8_t> encodeWebFrame(const WebFrame& frame);

// 明文二进制 → WebFrame（往返测试/协议客户端用）。magic/version/长度不合法 → false。
bool decodeWebFrame(const std::vector<uint8_t>& bytes, WebFrame& out);

// 有状态增量编码器：首帧全量；同维度后续只带行哈希变化行；维度/主题变化回全量。
class WebGridEncoder {
public:
    void reset();

    // generation 由调用方取自 pendingGeneration；不变也不影响正确性（行哈希判变）。
    WebFrame encode(const platform::TerminalFrame& frame, int generation,
                    uint32_t scrollOffset, uint32_t scrollbackCount, bool cursorVisible,
                    bool darkTheme);

private:
    bool havePrev_ = false;
    int prevRows_ = -1;
    int prevCols_ = -1;
    bool prevDark_ = false;
    std::vector<uint64_t> prevHashes_;
};

} // namespace terminal::web
