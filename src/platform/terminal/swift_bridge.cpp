// src/platform/terminal/swift_bridge.cpp — SwiftBridgeSession 实现。
// 唯一 include 生成头的 TU；容量常量与 Swift BridgeLimits 镜像同值（POC P2-A 契约）。
#include "swift_bridge.h"

#include "CortaBridge-Swift.h"

#include <utility>

#include "core/logging.h"

#include <cstring>

namespace platform {

namespace {
// 与 Swift BridgeLimits.cellCapacity（200 列 × 60 行）镜像同值；两侧由 POC overflow 模式锁定。
constexpr int32_t kCellCapacity = 200 * 60;
} // namespace

struct SwiftBridgeSession::Impl {
    // Swift 互操作对象不可默认构造（Optional/类默认 ctor 私有），经移动赋值持有
    explicit Impl(swift::Optional<CortaBridge::CortaBridgeSession> s) : session(std::move(s)) {}
    swift::Optional<CortaBridge::CortaBridgeSession> session;
    std::vector<CortaBridge::BridgeCell> cells =
        std::vector<CortaBridge::BridgeCell>(kCellCapacity, CortaBridge::BridgeCell::init());
    CortaBridge::BridgeCellMeta meta = CortaBridge::BridgeCellMeta::init();
};

SwiftBridgeSession::SwiftBridgeSession() = default;
SwiftBridgeSession::~SwiftBridgeSession() {
    if (impl_ && !impl_->session.isNone()) {
        impl_->session.getSome().stop();
    }
}

std::unique_ptr<SwiftBridgeSession> SwiftBridgeSession::spawn(
    const std::string& shell, int rows, int cols) {
    auto s = std::unique_ptr<SwiftBridgeSession>(new SwiftBridgeSession());
    s->impl_ = std::make_unique<Impl>(
        CortaBridge::CortaBridgeSession::spawnInteractive(shell, rows, cols));
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
    impl_->session.getSome().resize(rows, cols);
}

bool SwiftBridgeSession::snapshotCells(int32_t* rows, int32_t* cols, int32_t* cursorRow,
                                       int32_t* cursorCol, std::vector<TerminalCell>& out) {
    if (impl_->session.isNone()) return false;
    if (!impl_->session.getSome().snapshotCells(&impl_->meta, impl_->cells.data(), kCellCapacity)) {
        LOG_WARN("Terminal bridge: 快照超出镜像容量（rows*cols > 12000），帧跳过");
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

bool SwiftBridgeSession::copyToClipboard(const uint8_t* bytes, int32_t count) {
    if (bytes == nullptr || count <= 0) return false;
    return CortaBridge::CortaBridgeSession::copyToClipboard(bytes, count);
}

void SwiftBridgeSession::stop() {
    if (impl_->session.isNone()) return;
    impl_->session.getSome().stop();
}

} // namespace platform
