// src/platform/terminal/terminal_vterm_core.cpp
// 转译层实现（契约与线程模型见 terminal_vterm_core.h）。
// libvterm 调用面全部收在本 TU；头文件零 vterm.h 泄漏（pimpl）。
#include "terminal_vterm_core.h"

#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include <vterm.h>

namespace platform {

// C 回调桥（定义在文件尾匿名 ns；user=VtermCore::Impl*）。前置声明供 Impl::init
// 的函数指针表取址——声明与定义必须在同一匿名 ns（同 TU 内嵌于 platform）。
namespace {
int damageCbTramp(VTermRect rect, void* user);
int movecursorScreenTramp(VTermPos pos, VTermPos old, int visible, void* user);
int settermpropTramp(VTermProp prop, VTermValue* val, void* user);
int screenResizeTramp(int rows, int cols, void* user);
int pushlineTramp(int cols, const VTermScreenCell* cells, void* user);
int poplineTramp(int cols, VTermScreenCell* cells, void* user);
void outputTramp(const char* s, size_t len, void* user);
} // namespace

namespace {

// Corta Color.rawValue kind（Color.swift：kind 高 8 位 + payload 低 24 位）
constexpr uint32_t kColorKindDefault = 0u << 24;
constexpr uint32_t kColorKindIndexed = 1u << 24;
constexpr uint32_t kColorKindRgb = 2u << 24;

// Corta CellAttributes.rawValue 位面（CellAttributes.swift）
constexpr uint16_t kAttrBold = 1 << 0;
constexpr uint16_t kAttrItalic = 1 << 2;
constexpr uint16_t kAttrUnderline = 1 << 3;
constexpr uint16_t kAttrBlink = 1 << 4;
constexpr uint16_t kAttrReverse = 1 << 5;
constexpr uint16_t kAttrInvisible = 1 << 6;
constexpr uint16_t kAttrStrikethrough = 1 << 7;
constexpr uint16_t kAttrDoubleUnderline = 1 << 10;
constexpr uint16_t kAttrCurlyUnderline = 1 << 11;

// 规范化：libvterm 内置默认 pen 是 RGB(240,240,240)（pen.c:162，非 default tag）——
// 等于 state 默认色的 pen 翻译成 default kind 0，widget 侧主题化契约才成立
uint32_t translateColor(const VTermColor& c, const VTermColor& def) {
    if (VTERM_COLOR_IS_DEFAULT_FG(&c) || VTERM_COLOR_IS_DEFAULT_BG(&c))
        return kColorKindDefault;
    if (vterm_color_is_equal(&c, &def)) return kColorKindDefault;
    if (VTERM_COLOR_IS_INDEXED(&c)) return kColorKindIndexed | c.indexed.idx;
    if (VTERM_COLOR_IS_RGB(&c))
        return kColorKindRgb | (uint32_t(c.rgb.red) << 16) | (uint32_t(c.rgb.green) << 8) |
               uint32_t(c.rgb.blue);
    return kColorKindDefault;
}

uint16_t translateAttrs(const VTermScreenCellAttrs& a) {
    uint16_t v = 0;
    if (a.bold) v |= kAttrBold;
    if (a.italic) v |= kAttrItalic;
    if (a.blink) v |= kAttrBlink;
    if (a.reverse) v |= kAttrReverse;
    if (a.conceal) v |= kAttrInvisible;
    if (a.strike) v |= kAttrStrikethrough;
    switch (a.underline) {
        case VTERM_UNDERLINE_SINGLE: v |= kAttrUnderline; break;
        case VTERM_UNDERLINE_DOUBLE: v |= kAttrUnderline | kAttrDoubleUnderline; break;
        case VTERM_UNDERLINE_CURLY: v |= kAttrUnderline | kAttrCurlyUnderline; break;
        default: break;
    }
    return v;
}

void appendCodepointUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(char(cp));
    } else if (cp < 0x800) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

} // namespace

struct VtermCore::Impl {
    static constexpr size_t kScrollbackMax = 10000; // 行上限，超限丢最旧（FIFO）

    std::mutex mutex;
    VTerm* vt = nullptr;
    VTermScreen* screen = nullptr;
    VTermState* state = nullptr;
    int rows = 0;
    int cols = 0;
    std::vector<TerminalCell> cells;      // rows*cols 平铺，自有缓冲（single-writer）
    int cursorRow = 0;
    int cursorCol = 0;
    int generation = 0;
    bool altscreen = false;
    bool bracketedPaste = false;
    std::string* currentReply = nullptr;  // feed 期间非空（单 writer 线程内）
    // damage 合并矩形（end 端排他；dirty 才有效）
    bool dirty = false;
    VTermRect pending{0, 0, 0, 0};
    std::deque<std::vector<VTermScreenCell>> scrollback;
    // bracketed paste 字节级跟踪残留（≤7 字节候选前缀）
    std::string bpTail;

    ~Impl() {
        if (vt) vterm_free(vt);
    }

    void init(int r, int c) {
        rows = r;
        cols = c;
        vt = vterm_new(rows, cols);
        vterm_set_utf8(vt, 1);
        state = vterm_obtain_state(vt);
        screen = vterm_obtain_screen(vt);
        // 官方顺序（pangoterm 同款）：hard reset 必须在 obtain_screen **之后**——
        // reset 的 setpenattr 会转发初始化 screen 侧 pen（默认 fg=RGB(240,240,240)），
        // screen 未建时该转发丢失 → ScreenCell pen 停在脏内存（fg=RGB(0,0,0) 实证）；
        // 且 G0/G1 编码表也只在 reset 初始化（state.c:2141），漏 reset 则首喂 ASCII 崩
        vterm_state_reset(state, 1);
        vterm_state_get_default_colors(state, &defaultFg, &defaultBg);
        // ⚠ 不注册 VTermStateCallbacks：obtain_screen 时 screen 层已把内部回调表挂进
        // state（screen.c:903，含 putglyph→screen 转发），而 vterm_state_set_callbacks
        // 是**整表替换**——再挂自定义表会覆盖转发链，glyph 不进网格、damage 恒零
        // （实证：damageCbTramp 零命中、cells 全零）。宿主回调面 = VTermScreenCallbacks
        //（damage/movecursor/settermprop/resize/sb_*）——见下方 screenCbs。
        static const VTermScreenCallbacks screenCbs = {
            /* damage      */ damageCbTramp,
            /* moverect    */ nullptr,
            /* movecursor  */ movecursorScreenTramp,
            /* settermprop */ settermpropTramp,
            /* bell        */ nullptr,
            /* resize      */ screenResizeTramp,
            /* sb_pushline */ pushlineTramp,
            /* sb_popline  */ poplineTramp,
            /* sb_clear    */ nullptr,
            /* sb_pushline4*/ nullptr,
        };
        vterm_screen_set_callbacks(screen, &screenCbs, this);
        vterm_screen_enable_reflow(screen, true);
        vterm_screen_enable_altscreen(screen, 1);
        vterm_screen_set_damage_merge(screen, VTERM_DAMAGE_SCROLL);
        vterm_output_set_callback(vt, outputTramp, this);
        cells.assign(size_t(rows) * cols, TerminalCell{});
    }

    // ---- 网格转译（全部在持锁 feed/resize 栈内）----

    TerminalCell translateCell(const VTermScreenCell& sc) const {
        TerminalCell t;
        // 宽字占位：libvterm 对 width>1 glyph 的右侧格写 chars[0]=(uint32_t)-1
        // （screen.c:197）——规范化为 cp=0 spacer（Corta BridgeCell 契约）
        t.cp = sc.chars[0] == 0xFFFFFFFFu ? 0u : sc.chars[0]; // 首 scalar；组合序列 chars[1..] 降级
        t.width = sc.width == 2 ? uint8_t(2) : uint8_t(1);
        t.attrs = translateAttrs(sc.attrs);
        t.fg = translateColor(sc.fg, defaultFg);
        t.bg = translateColor(sc.bg, defaultBg);
        return t;
    }

    void unionPending(const VTermRect& r) {
        VTermRect clamped = r;
        if (clamped.start_row < 0) clamped.start_row = 0;
        if (clamped.start_col < 0) clamped.start_col = 0;
        if (clamped.end_row > rows) clamped.end_row = rows;
        if (clamped.end_col > cols) clamped.end_col = cols;
        if (clamped.start_row >= clamped.end_row || clamped.start_col >= clamped.end_col)
            return;
        if (!dirty) {
            pending = clamped;
            dirty = true;
            return;
        }
        if (clamped.start_row < pending.start_row) pending.start_row = clamped.start_row;
        if (clamped.start_col < pending.start_col) pending.start_col = clamped.start_col;
        if (clamped.end_row > pending.end_row) pending.end_row = clamped.end_row;
        if (clamped.end_col > pending.end_col) pending.end_col = clamped.end_col;
    }

    void flushDamageLocked() {
        if (!dirty) return;
        for (int r = pending.start_row; r < pending.end_row; ++r) {
            for (int c = pending.start_col; c < pending.end_col; ++c) {
                VTermScreenCell sc;
                VTermPos pos;
                pos.row = r;
                pos.col = c;
                vterm_screen_get_cell(screen, pos, &sc);
                cells[size_t(r) * cols + c] = translateCell(sc);
            }
        }
        dirty = false;
    }

    void syncFullLocked() {
        VTermRect all;
        all.start_row = 0;
        all.end_row = rows;
        all.start_col = 0;
        all.end_col = cols;
        unionPending(all);
        flushDamageLocked();
    }

    // ---- feed 侧 bracketed paste 跟踪 ----
    // shortcut: ~25 行字节级平行扫描（仅识别标准形 CSI ?2004 h/l）；升级触发=
    // libvterm 暴露 VTERM_PROP_PASTE 时整体删除（0.3.3 该模式存私有
    // state->mode.bracketpaste，state.c:861，不走 unrecognised fallback）。
    void trackBracketed(const uint8_t* b, int32_t n) {
        static const char kSeq[] = "\x1b[?2004";
        bpTail.append(reinterpret_cast<const char*>(b), size_t(n));
        size_t pos = 0;
        while (pos + sizeof(kSeq) /*含 \0，即 8；全序列=7 前缀+fin*/ <= bpTail.size()) {
            if (bpTail.compare(pos, sizeof(kSeq) - 1, kSeq, sizeof(kSeq) - 1) == 0 &&
                pos + sizeof(kSeq) - 1 < bpTail.size()) {
                const char fin = bpTail[pos + sizeof(kSeq) - 1];
                if (fin == 'h' || fin == 'l') {
                    bracketedPaste = fin == 'h';
                    pos += sizeof(kSeq); // 越过整个 8 字节序列
                    continue;
                }
            }
            ++pos;
        }
        if (pos > 0) bpTail.erase(0, pos);
        if (bpTail.size() > sizeof(kSeq) - 1) bpTail.erase(0, bpTail.size() - (sizeof(kSeq) - 1));
    }

    // ---- libvterm 回调（静态自由函数见文件尾桥接块）----

    VTermColor defaultFg;   // state 默认 pen 色（init 缓存，translateColor 规范化用）
    VTermColor defaultBg;

    void damageCbImpl(VTermRect rect) {
        unionPending(rect);
        generation++;
    }

    void movecursorCbImpl(VTermPos pos, VTermPos old, int visible) {
        (void)old;
        (void)visible;
        cursorRow = pos.row;
        cursorCol = pos.col;
    }

    void screenResizeCbImpl(int r, int c) {
        rows = r;
        cols = c;
        cells.assign(size_t(rows) * cols, TerminalCell{});
        generation++;
    }

    void settermpropCbImpl(VTermProp prop, VTermValue* val) {
        if (prop == VTERM_PROP_ALTSCREEN && val) altscreen = val->boolean != 0;
    }

    int pushlineCbImpl(int c, const VTermScreenCell* lineCells) {
        scrollback.emplace_back(lineCells, lineCells + c);
        if (scrollback.size() > kScrollbackMax) scrollback.pop_front();
        generation++;
        return 1;
    }

    int poplineCbImpl(int c, VTermScreenCell* lineCells) {
        if (scrollback.empty()) return 0;
        std::vector<VTermScreenCell> line = std::move(scrollback.front());
        scrollback.pop_front();
        const int take = int(line.size()) < c ? int(line.size()) : c;
        for (int i = 0; i < c; ++i) {
            if (i < take) {
                lineCells[i] = line[size_t(i)];
            } else {
                // padding：空白 + state 默认色（与 erase 填充同源）
                memset(&lineCells[i], 0, sizeof(VTermScreenCell));
                lineCells[i].width = 1;
                VTermColor dfg;
                VTermColor dbg;
                vterm_state_get_default_colors(state, &dfg, &dbg);
                lineCells[i].fg = dfg;
                lineCells[i].bg = dbg;
            }
        }
        return 1;
    }

    void outputCbImpl(const char* s, size_t len) {
        if (currentReply) currentReply->append(s, len);
    }
};

// ---- C 回调桥（libvterm 函数指针表只认 C 签名）----

namespace {
int damageCbTramp(VTermRect rect, void* user) {
    static_cast<VtermCore::Impl*>(user)->damageCbImpl(rect);
    return 1;
}
int movecursorScreenTramp(VTermPos pos, VTermPos old, int visible, void* user) {
    static_cast<VtermCore::Impl*>(user)->movecursorCbImpl(pos, old, visible);
    return 1;
}
int settermpropTramp(VTermProp prop, VTermValue* val, void* user) {
    static_cast<VtermCore::Impl*>(user)->settermpropCbImpl(prop, val);
    return 1;
}
int screenResizeTramp(int rows, int cols, void* user) {
    static_cast<VtermCore::Impl*>(user)->screenResizeCbImpl(rows, cols);
    return 1;
}
int pushlineTramp(int cols, const VTermScreenCell* cells, void* user) {
    return static_cast<VtermCore::Impl*>(user)->pushlineCbImpl(cols, cells);
}
int poplineTramp(int cols, VTermScreenCell* cells, void* user) {
    return static_cast<VtermCore::Impl*>(user)->poplineCbImpl(cols, cells);
}
void outputTramp(const char* s, size_t len, void* user) {
    static_cast<VtermCore::Impl*>(user)->outputCbImpl(s, len);
}
} // namespace

// Impl 内引用的桥接符号（静态表成员初始化需要地址）
// 注意：上表以文件局部静态构造，Impl::init 中的 static 表与此同址。

VtermCore::VtermCore(int rows, int cols) : impl_(new Impl) {
    impl_->init(rows, cols);
}

VtermCore::~VtermCore() = default;

void VtermCore::feed(const uint8_t* bytes, int32_t count, std::string* replyOut) {
    if (count <= 0) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->currentReply = replyOut;
    vterm_input_write(impl_->vt, reinterpret_cast<const char*>(bytes), size_t(count));
    vterm_screen_flush_damage(impl_->screen);   // 幂等：merge 模式下冲出合并 damage
    impl_->flushDamageLocked();
    impl_->trackBracketed(bytes, count);
    impl_->currentReply = nullptr;
}

void VtermCore::resize(int rows, int cols) {
    if (rows <= 0 || cols <= 0) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    vterm_set_size(impl_->vt, rows, cols);
    vterm_screen_flush_damage(impl_->screen);
    impl_->flushDamageLocked();
    if (impl_->dirty || impl_->rows != rows || impl_->cols != cols) impl_->syncFullLocked();
}

TerminalFrame VtermCore::snapshot() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    TerminalFrame f;
    f.rows = impl_->rows;
    f.cols = impl_->cols;
    f.cursorRow = impl_->cursorRow;
    f.cursorCol = impl_->cursorCol;
    f.cells = impl_->cells;
    return f;
}

int VtermCore::pendingGeneration() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->generation;
}

int VtermCore::scrollbackCount() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return int(impl_->scrollback.size());
}

bool VtermCore::scrollbackLine(int index, std::string& out) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (index < 0 || size_t(index) >= impl_->scrollback.size()) return false;
    out.clear();
    for (const VTermScreenCell& sc : impl_->scrollback[size_t(index)]) {
        if (sc.chars[0] != 0) appendCodepointUtf8(out, sc.chars[0]);
    }
    return true;
}

bool VtermCore::selectionText(int rowStart, int colStart, int rowEnd, int colEnd,
                              std::string& out) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (rowStart < 0 || colStart < 0 || rowEnd < rowStart || colEnd < colStart ||
        rowStart >= impl_->rows || rowEnd >= impl_->rows || colStart >= impl_->cols ||
        colEnd >= impl_->cols)
        return false;
    out.clear();
    for (int r = rowStart; r <= rowEnd; ++r) {
        int end = r == rowEnd ? colEnd : impl_->cols - 1;
        std::string line;
        for (int c = colStart; c <= end; ++c) {
            const TerminalCell& cell = impl_->cells[size_t(r) * impl_->cols + c];
            if (cell.cp == 0)
                line.push_back(' ');
            else
                appendCodepointUtf8(line, cell.cp);
        }
        // 行尾 trim（所见即所得口径：尾空白不进剪贴板）
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (r > rowStart && (!out.empty() || !line.empty())) out.push_back('\n');
        out += line;
    }
    return true;
}

bool VtermCore::bracketedPasteEnabled() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->bracketedPaste;
}

} // namespace platform
