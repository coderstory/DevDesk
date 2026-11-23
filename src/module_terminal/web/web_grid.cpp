// src/module_terminal/web/web_grid.cpp
#include "web_grid.h"

#include <algorithm>
#include <cstring>

#include "core/logging.h"
#include "module_terminal/terminal_grid_widget.h"

namespace terminal::web {

namespace {

void appendU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

void appendU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
}

bool readU16(const std::vector<uint8_t>& in, size_t& pos, uint16_t& v) {
    if (pos + 2 > in.size()) return false;
    v = static_cast<uint16_t>(in[pos] | (in[pos + 1] << 8));
    pos += 2;
    return true;
}

bool readU32(const std::vector<uint8_t>& in, size_t& pos, uint32_t& v) {
    if (pos + 4 > in.size()) return false;
    v = static_cast<uint32_t>(in[pos]) | (static_cast<uint32_t>(in[pos + 1]) << 8) |
        (static_cast<uint32_t>(in[pos + 2]) << 16) | (static_cast<uint32_t>(in[pos + 3]) << 24);
    pos += 4;
    return true;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xc0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xe0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    }
}

uint64_t fnv1aAppend(uint64_t h, const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

void hashU32(uint64_t& h, uint32_t v) {
    uint8_t b[4] = {static_cast<uint8_t>(v & 0xff), static_cast<uint8_t>((v >> 8) & 0xff),
                    static_cast<uint8_t>((v >> 16) & 0xff),
                    static_cast<uint8_t>((v >> 24) & 0xff)};
    h = fnv1aAppend(h, b, 4);
}

// run 文本超 u16 长度时截断（单 run 64K 列不可能，防御性上限）。
constexpr size_t kMaxRunText = 0xffff;

} // namespace

uint64_t rowHash(const platform::TerminalCell* row, int cols) {
    uint64_t h = 1469598103934665603ull;
    if (row == nullptr) return h;
    for (int i = 0; i < cols; i++) {
        hashU32(h, row[i].cp);
        hashU32(h, row[i].fg);
        hashU32(h, row[i].bg);
        hashU32(h, static_cast<uint32_t>(row[i].attrs) |
                       (static_cast<uint32_t>(row[i].width) << 16));
    }
    return h;
}

std::vector<WebRun> encodeRowRuns(const platform::TerminalCell* row, int cols, bool darkTheme) {
    std::vector<WebRun> runs;
    if (row == nullptr || cols <= 0) return runs;
    WebRun cur;
    bool open = false;
    int i = 0;
    while (i < cols) {
        const platform::TerminalCell& c = row[i];
        // 宽字右侧占位格（cp==0 && width==0）：无字符、不占额外列，并入前 run。
        if (c.cp == 0 && c.width == 0) {
            i++;
            continue;
        }
        const CellColor fg = decodeCellColor(c.fg, darkTheme, /*isForeground=*/true);
        const CellColor bg = decodeCellColor(c.bg, darkTheme, /*isForeground=*/false);
        const uint8_t width = std::max<uint8_t>(c.width, 1);
        const bool sameRun = open && cur.width == width && cur.attrsRaw == c.attrs &&
                             cur.fgRgba == fg.rgba && cur.bgRgba == bg.rgba;
        if (!sameRun) {
            if (open) runs.push_back(std::move(cur));
            cur = WebRun{};
            open = true;
            cur.width = width;
            cur.attrsRaw = c.attrs;
            cur.fgRgba = fg.rgba;
            cur.bgRgba = bg.rgba;
            cur.flags = static_cast<uint8_t>((fg.isDefault ? kRunFgDefault : 0) |
                                             (bg.isDefault ? kRunBgDefault : 0) |
                                             ((c.attrs & 0x01) ? kRunBold : 0) |
                                             ((c.attrs & 0x08) ? kRunUnderline : 0));
        }
        if (c.cp == 0) {
            cur.text.push_back(' ');
            cur.cells = static_cast<uint16_t>(cur.cells + 1);
        } else {
            appendUtf8(cur.text, c.cp);
            cur.cells = static_cast<uint16_t>(cur.cells + width);
        }
        i++;
    }
    if (open) runs.push_back(std::move(cur));
    return runs;
}

std::vector<uint8_t> encodeWebFrame(const WebFrame& frame) {
    std::vector<uint8_t> out;
    out.reserve(kGridHeaderBytes + 2 + frame.rows.size() * 4 + 64);
    const WebFrameHeader& h = frame.header;
    out.push_back(kGridMagic0);
    out.push_back(kGridMagic1);
    out.push_back(kGridVersion);
    uint8_t flags = 0;
    if (h.cursorVisible) flags |= kHeaderCursorVisible;
    if (h.full) flags |= kHeaderFull;
    out.push_back(flags);
    appendU16(out, h.rows);
    appendU16(out, h.cols);
    appendU16(out, h.cursorRow);
    appendU16(out, h.cursorCol);
    appendU32(out, h.scrollOffset);
    appendU32(out, h.scrollbackCount);
    appendU32(out, h.generation);
    appendU16(out, static_cast<uint16_t>(std::min<size_t>(frame.rows.size(), 0xffff)));
    for (const WebRow& row : frame.rows) {
        appendU16(out, row.rowIndex);
        appendU16(out, static_cast<uint16_t>(std::min<size_t>(row.runs.size(), 0xffff)));
        for (const WebRun& run : row.runs) {
            const size_t textLen = std::min(run.text.size(), kMaxRunText);
            appendU16(out, run.cells);
            out.push_back(run.width);
            out.push_back(run.flags);
            appendU32(out, run.fgRgba);
            appendU32(out, run.bgRgba);
            appendU16(out, run.attrsRaw);
            appendU16(out, static_cast<uint16_t>(textLen));
            out.insert(out.end(), run.text.begin(), run.text.begin() + textLen);
        }
    }
    return out;
}

bool decodeWebFrame(const std::vector<uint8_t>& bytes, WebFrame& out) {
    if (bytes.size() < static_cast<size_t>(kGridHeaderBytes) + 2) return false;
    if (bytes[0] != kGridMagic0 || bytes[1] != kGridMagic1) return false;
    if (bytes[2] != kGridVersion) return false;
    size_t pos = 3;
    const uint8_t flags = bytes[pos++];
    WebFrame frame;
    frame.header.full = (flags & kHeaderFull) != 0;
    frame.header.cursorVisible = (flags & kHeaderCursorVisible) != 0;
    if (!readU16(bytes, pos, frame.header.rows) || !readU16(bytes, pos, frame.header.cols) ||
        !readU16(bytes, pos, frame.header.cursorRow) ||
        !readU16(bytes, pos, frame.header.cursorCol) ||
        !readU32(bytes, pos, frame.header.scrollOffset) ||
        !readU32(bytes, pos, frame.header.scrollbackCount) ||
        !readU32(bytes, pos, frame.header.generation)) {
        return false;
    }
    uint16_t rowCount = 0;
    if (!readU16(bytes, pos, rowCount)) return false;
    frame.rows.reserve(rowCount);
    for (uint16_t r = 0; r < rowCount; r++) {
        WebRow row;
        uint16_t runCount = 0;
        if (!readU16(bytes, pos, row.rowIndex) || !readU16(bytes, pos, runCount)) return false;
        row.runs.reserve(runCount);
        for (uint16_t k = 0; k < runCount; k++) {
            WebRun run;
            uint16_t textLen = 0;
            if (!readU16(bytes, pos, run.cells)) return false;
            if (pos >= bytes.size()) return false;
            run.width = bytes[pos++];
            if (pos >= bytes.size()) return false;
            run.flags = bytes[pos++];
            if (!readU32(bytes, pos, run.fgRgba) || !readU32(bytes, pos, run.bgRgba) ||
                !readU16(bytes, pos, run.attrsRaw) || !readU16(bytes, pos, textLen)) {
                return false;
            }
            if (pos + textLen > bytes.size()) return false;
            run.text.assign(reinterpret_cast<const char*>(bytes.data() + pos), textLen);
            pos += textLen;
            row.runs.push_back(std::move(run));
        }
        frame.rows.push_back(std::move(row));
    }
    out = std::move(frame);
    return true;
}

void WebGridEncoder::reset() {
    havePrev_ = false;
    prevRows_ = -1;
    prevCols_ = -1;
    prevDark_ = false;
    prevHashes_.clear();
}

WebFrame WebGridEncoder::encode(const platform::TerminalFrame& frame, int generation,
                                uint32_t scrollOffset, uint32_t scrollbackCount,
                                bool cursorVisible, bool darkTheme) {
    WebFrame out;
    out.header.cursorVisible = cursorVisible;
    out.header.rows = static_cast<uint16_t>(std::max(frame.rows, 0));
    out.header.cols = static_cast<uint16_t>(std::max(frame.cols, 0));
    out.header.cursorRow = static_cast<uint16_t>(std::max(frame.cursorRow, 0));
    out.header.cursorCol = static_cast<uint16_t>(std::max(frame.cursorCol, 0));
    out.header.scrollOffset = scrollOffset;
    out.header.scrollbackCount = scrollbackCount;
    out.header.generation = static_cast<uint32_t>(std::max(generation, 0));

    const bool dimsChanged = !havePrev_ || frame.rows != prevRows_ || frame.cols != prevCols_;
    const bool themeChanged = havePrev_ && darkTheme != prevDark_;
    const bool full = dimsChanged || themeChanged;
    out.header.full = full;

    std::vector<uint64_t> hashes(static_cast<size_t>(std::max(frame.rows, 0)), 0);
    for (int r = 0; r < frame.rows; r++) {
        const platform::TerminalCell* row =
            frame.cells.size() >= static_cast<size_t>((r + 1) * frame.cols)
                ? frame.cells.data() + static_cast<size_t>(r) * frame.cols
                : nullptr;
        hashes[static_cast<size_t>(r)] = rowHash(row, frame.cols);
        const bool changed = full || hashes[static_cast<size_t>(r)] != prevHashes_[static_cast<size_t>(r)];
        if (!changed) continue;
        WebRow webRow;
        webRow.rowIndex = static_cast<uint16_t>(r);
        if (row != nullptr) webRow.runs = encodeRowRuns(row, frame.cols, darkTheme);
        out.rows.push_back(std::move(webRow));
    }

    // 超上限降级：整帧回退纯文本行（默认前景、单 run/行），记一次 WARN。
    size_t size = encodeWebFrame(out).size();
    if (size > kGridMaxFrameBytes) {
        WebFrame plain;
        plain.header = out.header;
        plain.header.full = true;
        for (int r = 0; r < frame.rows; r++) {
            if (frame.cells.size() < static_cast<size_t>((r + 1) * frame.cols)) break;
            WebRow row;
            row.rowIndex = static_cast<uint16_t>(r);
            WebRun run;
            run.width = 1;
            run.flags = kRunFgDefault | kRunBgDefault;
            run.bgRgba = 0;
            const CellColor fg = decodeCellColor(0, darkTheme, true);
            run.fgRgba = fg.rgba;
            std::string text;
            for (int c = 0; c < frame.cols; c++) {
                const platform::TerminalCell& cell =
                    frame.cells[static_cast<size_t>(r) * frame.cols + c];
                if (cell.cp == 0) {
                    text.push_back(' ');
                } else {
                    appendUtf8(text, cell.cp);
                }
            }
            run.cells = static_cast<uint16_t>(std::max(frame.cols, 0));
            run.text = std::move(text);
            row.runs.push_back(std::move(run));
            plain.rows.push_back(std::move(row));
        }
        LOG_WARN("Terminal web: 网格帧超过 256KB，降级纯文本行（" + std::to_string(size) +
                 " bytes）");
        out = std::move(plain);
        size = encodeWebFrame(out).size();
    }
    (void)size;

    prevRows_ = frame.rows;
    prevCols_ = frame.cols;
    prevDark_ = darkTheme;
    prevHashes_ = std::move(hashes);
    havePrev_ = true;
    return out;
}

} // namespace terminal::web
