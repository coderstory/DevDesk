// Markdown 实时态纯函数层：活动块定位 / 贴合高度 / 跨块导航 / 边界删除 /
// 块级 undo/redo 快照栈 / 光标跟随判定 / 单块高度上限。真实行数测量由消费侧注入。
// 不 include 任何 EUI-NEO 组件头、不抛异常（同 md_block_model / md_sync_map）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>

#include "md_block_model.h"

namespace module::markdown {

// 活动 input 行高系数，与 components/input.h 的 textLineHeight = fontSize × 1.2
// 同源；改本值必须与上游同步（矮一像素 input 就会长出内部滚动吃掉外层滚轮）。
inline constexpr float kInputLineHeightFactor = 1.2f;

// 活动块下标：块身份锚 = 文档字节偏移（不用 stableId，拆/并块时会变）。
// 语义 = blockIndexAt：空文档 → 0；cursorByte ≥ 尾 → 末块；块边界字节归后一块。
size_t mdLiveActiveBlockIndex(const MdBlockModel& model, size_t cursorByte);

// 文档字节偏移 → 块局部偏移（钳到 [0, 块长]）。「文档偏移 ↔ input 光标」唯一换算点。
size_t mdLiveBlockLocalOffset(const MdBlock& block, size_t cursorByte);

// 行数测量注入：块文本 + 文本区宽 → 可视行数（≥1）。真实场景 =
// InputModel::measureLines(...).size()；单测可用假函数。
using MdLiveLineCountFn =
    std::function<int(const std::string& blockText, float textWidth)>;

// IME 冻结谓词：组字非空期间冻结块推导 + 拒绝一切切块入口（换块换文本会撕裂组字）。
// 消费侧纪律：① 冻结期间不重算锚；② 失焦必须清 compositionText（否则谓词恒真）。
bool mdLiveCompositionFrozen(const std::string& compositionText);

// 内容贴合高度：h = inset×2 + 行数 × fontSize × 1.2 + 1（+1px 余量保证
// maxVerticalScroll == 0）。textWidth 须与 input 内部排版同宽（= 声明宽 − inset×2）。
float mdLiveInputHeight(const std::string& blockText, float textWidth,
                        float fontSize, float inset,
                        const MdLiveLineCountFn& lineCount);

// 块边界键只做普通文本编辑，无状态。本层只回答「↑↓ 跳到哪个字节」与
// 「边界退格/删除删哪个范围」。

// 「不消费」哨兵：交回 input 内置。
inline constexpr size_t kMdLiveNoConsume = static_cast<size_t>(-1);

// 跨块 ↑↓ 目标：direction < 0 = ↑、> 0 = ↓；返回目标文档字节偏移或 kMdLiveNoConsume。
// ↑ 在首行 → 上一块末行；↓ 在末行 → 下一块首行；列尽量保持。
// 不消费：空块序列 / direction==0 / 首块 ↑ / 末块 ↓ / 光标不在边界行。
// 术语按源码行（软折行不参与）；列 = 行内字节偏移，落到目标行钳到该行内容长。
size_t mdLiveVerticalBlockTarget(const std::string& text,
                                 const std::vector<MdBlock>& blocks,
                                 size_t cursorByte, int direction);

// 块边界删除的待执行编辑：删 [eraseStart, eraseEnd) 后光标落 cursor（== 接缝字节）。
struct MdLiveBoundaryEdit {
    size_t eraseStart = 0;
    size_t eraseEnd = 0;
    size_t cursor = 0;
};

// 块首退格 / 块尾删除（direction < 0 = Backspace、> 0 = Delete）：
// 要求光标正好落在块边界字节上。Backspace 删光标前一个行终结符（CRLF 成对）；
// Delete 只删行终结符。返回 false = 不消费（含首块 Backspace / 文末 Delete /
// 有选区时调用侧不得进来）。
bool mdLiveBoundaryEdit(const std::string& text,
                        const std::vector<MdBlock>& blocks, size_t cursorByte,
                        int direction, MdLiveBoundaryEdit& out);

// 块级 undo / redo：快照 = (整篇文本, 文档光标偏移)，存编辑前状态——消费侧写文档前 push，
// 撤销 = 弹回。不用框架内置栈：per-instance 块局部快照在单输入下跨块混叠。
// 容量：常规 200；文档 > 2MB 降为 50（降级记日志由消费侧做）。
inline constexpr size_t kMdLiveUndoCap = 200;
inline constexpr size_t kMdLiveUndoCapLargeDoc = 50;
inline constexpr size_t kMdLiveUndoLargeDocBytes = 2u * 1024u * 1024u;
// 同块连续输入合并窗口：距上次推栈 < 600ms 视为同一次编辑
inline constexpr int64_t kMdLiveUndoMergeMs = 600;

class MdLiveUndoStack {
public:
// 基线换代（进实时态 / 换笔记 / 外部重载）：清栈并以 (text, cursorByte) 为基线，
// 撤销最多回到这一版。不换代会用旧文档快照覆盖新文档。cursorByte 越界钳到文本长。
    void reset(std::string text, size_t cursorByte);

// 推栈：text/cursorByte = 本次编辑前的整篇文本与光标偏移。sameBlock 且距上次推栈
// < kMdLiveUndoMergeMs → 合并；否则开新编辑（作废 redo 链）。与栈顶同文本不新增。
    void push(std::string text, size_t cursorByte, bool sameBlock, int64_t nowMs);

// 块切换 / 模式切换前收口：只关合并组，不新增条目、不动 redo 栈。
    void markBoundary();

// 撤销 / 重做：成功写回 (text, cursorByte) 并返回 true；无可用快照返回 false。
// 与当前文本相同的快照（无变化步）直接跳过，不消耗一次撤销。
    bool undo(std::string& text, size_t& cursorByte);
    bool redo(std::string& text, size_t& cursorByte);

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    size_t size() const { return undo_.size(); }
    size_t redoSize() const { return redo_.size(); }
// 生效容量：由最近一次推栈的文档大小决定（> 2MB 降级 50）；未推过栈 = 上限值
    size_t capacity() const { return capacity_; }

private:
    struct Entry {
        std::string text;
        size_t cursorByte = 0;
    };
    std::deque<Entry> undo_;   // 前出（容量满时挤掉最老的一条）
    std::deque<Entry> redo_;
    size_t capacity_ = kMdLiveUndoCap;
    int64_t lastPushMs_ = 0;
    bool groupOpen_ = false;   // 合并组是否还开着（push/markBoundary/undo 改它）
};

// 滚动锚定与光标跟随（越界才拉）：外层 scrollView 是唯一滚动权威，本层只回答
// 此刻要不要拉一次视口、拉到哪（纯算术）。caret 在安全带内不干预。
inline constexpr float kMdLiveFollowTopRatio = 0.15f;
inline constexpr float kMdLiveFollowBottomRatio = 0.75f;
inline constexpr float kMdLiveFollowAnchorRatio = 0.4f;

struct MdLiveFollowDecision {
    // 真值 = caret 越出安全带（判据本身）
    bool pull = false;
    // pull 时 = 目标偏移（钳 [0,maxOffset]），否则 = currentOffset 钳位值
    float offset = 0.0f;
};

// 越界判定 + 目标偏移。viewportH ≤ 0 = 无视口不判；maxOffset 越界按 0 处理。
// 【no-op 抑制是调用侧义务】：本函数不做 offset 是否变化的比较。
MdLiveFollowDecision mdLiveFollowOffset(float caretDocY, float viewportH,
                                        float currentOffset, float maxOffset);

// 单块高度上限：贴合高超过 0.7 × 视口高时封顶——超大单块由 input 内部滚动接管；
// viewportH ≤ 0 不封顶。
inline constexpr float kMdLiveBlockCapRatio = 0.7f;
float mdLiveCappedInputHeight(float fitHeight, float viewportH);

} // namespace module::markdown
