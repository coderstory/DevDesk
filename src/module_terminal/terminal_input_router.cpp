// src/module_terminal/terminal_input_router.cpp
#include "terminal_input_router.h"

#include <algorithm>

namespace terminal {

void TerminalInputRouter::press(CellPos p) {
    pressing_ = true;
    hasSelection_ = false;
    anchor_ = p;
    head_ = p;
}

void TerminalInputRouter::dragTo(CellPos p) {
    if (!pressing_) return;
    head_ = p;
    hasSelection_ = true;
}

void TerminalInputRouter::release(CellPos p) {
    if (!pressing_) return;
    pressing_ = false;
    head_ = p;
    hasSelection_ = !(anchor_.row == head_.row && anchor_.col == head_.col);
}

void TerminalInputRouter::clear() {
    pressing_ = false;
    hasSelection_ = false;
}

void TerminalInputRouter::selectRange(CellPos a, CellPos b) {
    pressing_ = false;
    anchor_ = a;
    head_ = b;
    hasSelection_ = !(a.row == b.row && a.col == b.col);
}

void TerminalInputRouter::reset() {
    clear();
    scrollRemainder_ = 0.0f;
}

int TerminalInputRouter::scrollBy(float notches, int currentOffset, int maxOffset) {
    scrollRemainder_ += notches * kWheelLinesPerNotch;
    const int lines = static_cast<int>(scrollRemainder_);
    if (lines == 0) return currentOffset;
    scrollRemainder_ -= static_cast<float>(lines);
    clear(); // 滚动即清选区
    return std::clamp(currentOffset + lines, 0, maxOffset);
}

} // namespace terminal
