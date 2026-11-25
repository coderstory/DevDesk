// src/module_terminal/terminal_input_router.h
// 终端「鼠标/选区/滚动」输入路由（M2 Task 1 抽缝）。
// 纯状态机、无 UI/无后端依赖：页面只做「指针坐标 → CellPos」换算后委托本组件。
// 语义与抽缝前逐字等价（拖选锚点/头、滚动清选区、滚轮 3 行/档 + 小数余数）。
#pragma once

namespace terminal {

struct CellPos {
    int row = 0;
    int col = 0;
};

class TerminalInputRouter {
public:
    // 滚轮每档行数（池#364：y 是档位不是像素）
    static constexpr float kWheelLinesPerNotch = 3.0f;

    void press(CellPos p);
    // 仅在 press 之后生效（拖动中）
    void dragTo(CellPos p);
    // 结束拖动：头更新为落点，选区有效性 = 锚点≠落点
    void release(CellPos p);
    void clear();
    // 切会话/重启：清选区并复位滚轮余数（抽缝前 page 的两处显式复位）
    void reset();

    bool pressing() const { return pressing_; }
    bool hasSelection() const { return hasSelection_; }
    CellPos anchor() const { return anchor_; }
    CellPos head() const { return head_; }

    // 滚轮：档位累积换行（余数保留给触控板小数增量），返回新 offset（已钳制）。
    // 滚动即清选区（终端惯例）。
    int scrollBy(float notches, int currentOffset, int maxOffset);

private:
    bool pressing_ = false;
    bool hasSelection_ = false;
    CellPos anchor_{};
    CellPos head_{};
    float scrollRemainder_ = 0.0f;
};

} // namespace terminal
