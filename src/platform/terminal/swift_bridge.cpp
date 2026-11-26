// src/platform/terminal/swift_bridge.cpp — SwiftBridgeSession 实现。
// 唯一 include 生成头的 TU；容量常量与 Swift BridgeLimits 镜像同值（POC P2-A 契约）。
#include "swift_bridge.h"

#include "CortaBridge-Swift.h"

#include <utility>

#include "core/logging.h"

#include <cstdlib>
#include <cstring>

namespace platform {

namespace {
// 与 Swift BridgeLimits（640 列 × 240 行）镜像同值（bug508）；改一侧必须同改另一侧。
constexpr int32_t kMaxCols = 640;
constexpr int32_t kMaxRows = 240;
constexpr int32_t kCellCapacity = kMaxCols * kMaxRows;

// SGR 鼠标编码（xterm ?1006）：`ESC [ < Cb ; Cx ; Cy {M|m}`，坐标 **1 基**。
// final='m' 表示松开，'M' 表示按下/移动/滚轮（与 X10 的「松开永远 Cb=3」不同）。
std::string encodeSgrMouse(int32_t code, int row, int col, bool releaseFinal) {
    std::string out;
    out.reserve(24);
    out += '\x1b';
    out += "[<";
    out += std::to_string(code);
    out += ';';
    out += std::to_string(col + 1);  // Cx 在前、Cy 在后（xterm 线序）
    out += ';';
    out += std::to_string(row + 1);
    out += releaseFinal ? 'm' : 'M';
    return out;
}

// 契约按钮号（terminal_backend.h:77）→ SGR Cb 低位：0/1/2 = 左/中/右；4/5 = 滚轮 64/65。
// -1 = 未定义的按钮号（调用方丢弃）。
int32_t sgrButtonCode(int button) {
    switch (button) {
        case 0: return 0;   // 左
        case 1: return 1;   // 中
        case 2: return 2;   // 右
        case 4: return 64;  // 上滚
        case 5: return 65;  // 下滚
        default: return -1;
    }
}
} // namespace

GridSize clampGridSize(int32_t rows, int32_t cols) {
    GridSize out{rows, cols, false};
    if (out.rows > kMaxRows) {
        out.rows = kMaxRows;
        out.clamped = true;
    }
    if (out.cols > kMaxCols) {
        out.cols = kMaxCols;
        out.clamped = true;
    }
    if (out.rows < 1) out.rows = 1;
    if (out.cols < 1) out.cols = 1;
    return out;
}

struct SwiftBridgeSession::Impl {
    // Swift 互操作对象不可默认构造（Optional/类默认 ctor 私有），经移动赋值持有
    explicit Impl(swift::Optional<CortaBridge::CortaBridgeSession> s) : session(std::move(s)) {}
    swift::Optional<CortaBridge::CortaBridgeSession> session;
    std::vector<CortaBridge::BridgeCell> cells =
        std::vector<CortaBridge::BridgeCell>(kCellCapacity, CortaBridge::BridgeCell::init());
    CortaBridge::BridgeCellMeta meta = CortaBridge::BridgeCellMeta::init();
    bool resizeWarned = false;
    // 鼠标上报的按下配对与移动去重。SGR Cb 只有单按钮位（多键同时按下无编码），
    // 故按按钮逐个配对；release 只对「上报过的按下」发，避免应用卡在按下态。
    bool mousePressed[3] = {false, false, false};
    int32_t lastMouseButton = -1;  // 最近按下未松的 0 基按钮号，-1=无
    int32_t lastMotionRow = -1;
    int32_t lastMotionCol = -1;
};

SwiftBridgeSession::SwiftBridgeSession() = default;
SwiftBridgeSession::~SwiftBridgeSession() {
    if (impl_ && !impl_->session.isNone()) {
        impl_->session.getSome().stop();
    }
}

std::string resolveSpawnWorkDir(const std::string& workDir) {
    if (!workDir.empty()) return workDir;
    const char* home = std::getenv("HOME");
    return home ? std::string(home) : std::string();
}

std::unique_ptr<SwiftBridgeSession> SwiftBridgeSession::spawn(
    const std::string& shell, int rows, int cols, const std::string& workDir) {
    auto s = std::unique_ptr<SwiftBridgeSession>(new SwiftBridgeSession());
    const std::string dir = resolveSpawnWorkDir(workDir);
    // need328：目录进 Swift Optional——空（连 $HOME 都缺失）传 none → 上游默认 cwd 兜底
    using OptString = swift::Optional<swift::String>;
    const OptString dirArg =
        dir.empty() ? OptString::none() : OptString::some(swift::String(dir));
    // bug508：尺寸先钳制到镜像上限，避免 spawn 出超过快照容量的网格（否则整帧丢弃=白屏）
    const GridSize grid = clampGridSize(rows, cols);
    if (grid.clamped) {
        LOG_WARN("Terminal bridge: spawn 尺寸超快照上限，已钳制到 " +
                 std::to_string(grid.cols) + "x" + std::to_string(grid.rows));
    }
    s->impl_ = std::make_unique<Impl>(
        CortaBridge::CortaBridgeSession::spawnInteractive(shell, grid.rows, grid.cols, dirArg));
    if (s->impl_->session.isNone()) return nullptr;
    return s;
}

bool SwiftBridgeSession::lastSpawnError(uint8_t* out, int32_t capacity) {
    return CortaBridge::CortaBridgeSession::lastSpawnError(out, capacity);
}

bool SwiftBridgeSession::write(const uint8_t* bytes, int32_t count, bool paste) {
    if (impl_->session.isNone()) return false;
    if (paste) impl_->session.getSome().writePaste(bytes, count);
    else impl_->session.getSome().write(bytes, count);
    return true;
}

void SwiftBridgeSession::resize(int32_t rows, int32_t cols) {
    if (impl_->session.isNone()) return;
    // bug508：窗口拖到超过镜像上限时钳制并只告警一次（会话内布尔防刷屏）
    const GridSize grid = clampGridSize(rows, cols);
    if (grid.clamped && !impl_->resizeWarned) {
        impl_->resizeWarned = true;
        LOG_WARN("Terminal bridge: resize 尺寸超快照上限，已钳制到 " +
                 std::to_string(grid.cols) + "x" + std::to_string(grid.rows));
    }
    impl_->session.getSome().resize(grid.rows, grid.cols);
}

bool SwiftBridgeSession::snapshotCells(int32_t* rows, int32_t* cols, int32_t* cursorRow,
                                       int32_t* cursorCol, std::vector<TerminalCell>& out) {
    if (impl_->session.isNone()) return false;
    if (!impl_->session.getSome().snapshotCells(&impl_->meta, impl_->cells.data(), kCellCapacity)) {
        LOG_WARN("Terminal bridge: 快照超出镜像容量（rows*cols > 153600），帧跳过");
        return false;
    }
    *rows = impl_->meta.getRows();
    *cols = impl_->meta.getCols();
    *cursorRow = impl_->meta.getCursorRow();
    *cursorCol = impl_->meta.getCursorCol();
    out.resize(static_cast<size_t>(*rows) * static_cast<size_t>(*cols));
    for (size_t i = 0; i < out.size(); i++) {
        const CortaBridge::BridgeCell& c = impl_->cells[i];
        // 池 bug248：宽字右侧占位格归一成 cp=0 spacer，与 libvterm 后端
        // （terminal_vterm_core.cpp:171 translateCell）保持同一契约。
        // Corta 侧 Swift 用「CJK 占两格」模型（CortaBridge.swift:188
        // bc.width = grapheme.isNone ? 1 : 2），宽字右侧那格不是 0xFFFFFFFF，
        // 而是真实空格 scalar(0x20)，于是两条后端产出的 cell 流不一致：
        //   libvterm：杭 / cp=0   / 州 / cp=0    → rowSliceText 拼出 "杭州"
        //   Corta   ：杭 / cp=0x20 / 州 / cp=0x20 → rowSliceText 拼出 "杭 州"
        // 下游 rowSliceText（terminal_grid_widget.cpp）按契约跳过 cp==0 的占位格，
        // 于是 Corta 机器（macOS 主力路径）上中文永远无法连续匹配。此处只对
        // width==0 的占位格置 0，不动真实半角空格。
        uint32_t cp = c.getCp();
        const uint8_t w = c.getWidth();
        if (w == 0 && cp != 0) cp = 0;
        out[i] = TerminalCell{cp, c.getFg(), c.getBg(), c.getAttrs(), w};
    }
    return true;
}

bool SwiftBridgeSession::exited(int32_t* exitCode) const {
    if (impl_->session.isNone()) return false;
    CortaBridge::BridgeStatus st = impl_->session.getSome().status();
    if (st.getPhase() != 1) return false;
    *exitCode = st.getExitCode();
    return true;
}

int32_t SwiftBridgeSession::pendingGeneration() const {
    if (impl_->session.isNone()) return 0;
    return impl_->session.getSome().pendingGeneration();
}

int32_t SwiftBridgeSession::scrollbackCount() {
    if (impl_->session.isNone()) return 0;
    return impl_->session.getSome().scrollbackCount();
}

bool SwiftBridgeSession::scrollbackLineText(int32_t index, std::string& out) {
    if (impl_->session.isNone()) return false;
    char buf[4096];
    if (!impl_->session.getSome().scrollbackLineText(index, reinterpret_cast<uint8_t*>(buf),
                                                     sizeof buf)) {
        LOG_WARN("Terminal bridge: 回滚行读取失败（越界或超 4K 缓冲）");
        return false;
    }
    out.assign(buf);
    return true;
}

bool SwiftBridgeSession::selectionText(int rowStart, int colStart, int rowEnd, int colEnd,
                                       std::string& out) {
    if (impl_->session.isNone()) return false;
    char buf[8192];
    if (!impl_->session.getSome().selectionText(rowStart, colStart, rowEnd, colEnd,
                                                reinterpret_cast<uint8_t*>(buf), sizeof buf)) {
        LOG_WARN("Terminal bridge: 选区文本提取失败（越界或超 8K 缓冲）");
        return false;
    }
    out.assign(buf);
    return true;
}

// ---- 鼠标上报 ----
// 读 Corta 状态决定「报不报」，事件本身按 SGR 规则编码后走 write 通道（PTY 输入）。
// 非 SGR 编码（X10/UTF8）本桥不产出：Corta 的复合模式在 SGR 未开时即 off。

int32_t SwiftBridgeSession::mouseTrackingMode() {
    if (impl_->session.isNone()) return 0;
    return impl_->session.getSome().mouseTrackingMode();
}

bool SwiftBridgeSession::sgrMouseEncodingEnabled() {
    if (impl_->session.isNone()) return false;
    return impl_->session.getSome().sgrMouseEncodingEnabled();
}

bool SwiftBridgeSession::mouseReporting() { return mouseTrackingMode() != 0; }

bool SwiftBridgeSession::altscreen() {
    if (impl_->session.isNone()) return false;
    return impl_->session.getSome().altscreenActive();
}

void SwiftBridgeSession::mouseMove(int row, int col, bool buttonDown) {
    if (impl_->session.isNone()) return;
    const int32_t mode = mouseTrackingMode();
    if (mode == 0) return;
    if (mode == 1000) return;  // normal：只报按键，移动一条不发
    // 拖动态：优先用 mouseButton 记下的按钮；没有则看页面给的 buttonDown（按下事件可能
    // 因 Shift 覆盖/首帧时序没上报）。1003=anyEvent 允许「无按键」位（Cb 低位 3）。
    int32_t held = impl_->lastMouseButton;
    if (held < 0) {
        if (buttonDown) held = 0;
        else if (mode == 1003) held = 3;
        else return;  // 1002=buttonEvent：未按住不报
    }
    // 同格去重（指针在一个 cell 内抖动不刷 PTY；与 Corta TerminalView 同口径）
    if (row == impl_->lastMotionRow && col == impl_->lastMotionCol) return;
    impl_->lastMotionRow = row;
    impl_->lastMotionCol = col;
    // 移动位 bit5（+32）：Cb = 32 + 按钮号（无按键=35=3|32）
    const std::string bytes = encodeSgrMouse(held | 32, row, col, false);
    write(reinterpret_cast<const uint8_t*>(bytes.data()), static_cast<int32_t>(bytes.size()),
          false);
}

void SwiftBridgeSession::mouseButton(int row, int col, int button, bool pressed) {
    if (impl_->session.isNone()) return;
    if (mouseTrackingMode() == 0) return;
    const int32_t code = sgrButtonCode(button);
    if (code < 0) return;
    if (code >= 64) {
        // 滚轮：只有按下形态，释放不上报（xterm 语义；页面按「按下+抬起」成对调用，
        // 与 libvterm 一致丢弃抬起）。
        if (!pressed) return;
        impl_->lastMotionRow = row;
        impl_->lastMotionCol = col;
        const std::string bytes = encodeSgrMouse(code, row, col, false);
        write(reinterpret_cast<const uint8_t*>(bytes.data()),
              static_cast<int32_t>(bytes.size()), false);
        return;
    }
    const size_t idx = static_cast<size_t>(button);
    if (pressed) {
        impl_->mousePressed[idx] = true;
        impl_->lastMouseButton = button;
    } else {
        // 未上报过的按下不凭空发 release（页面契约：配对才发）
        if (!impl_->mousePressed[idx]) return;
        impl_->mousePressed[idx] = false;
        if (impl_->lastMouseButton == button) {
            impl_->lastMouseButton = -1;
            for (size_t i = 0; i < 3; i++) {
                if (impl_->mousePressed[i]) {
                    impl_->lastMouseButton = static_cast<int32_t>(i);
                    break;
                }
            }
        }
    }
    impl_->lastMotionRow = row;
    impl_->lastMotionCol = col;
    // 按下 'M' / 松开 'm'，Cb 都是被操作按钮本身（SGR 与 X10 的关键差别）
    const std::string bytes = encodeSgrMouse(code, row, col, !pressed);
    write(reinterpret_cast<const uint8_t*>(bytes.data()), static_cast<int32_t>(bytes.size()),
          false);
}

bool SwiftBridgeSession::copyToClipboard(const uint8_t* bytes, int32_t count) {
    if (bytes == nullptr || count <= 0) return false;
    return CortaBridge::CortaBridgeSession::copyToClipboard(bytes, count);
}

void SwiftBridgeSession::stop() {
    if (impl_->session.isNone()) return;
    impl_->session.getSome().stop();
}

} // namespace platform
