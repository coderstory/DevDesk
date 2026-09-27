// src/module_markdown/md_live_edit.h
// Markdown Phase 3 · M2「活动块输入 + 内容贴合高度」的纯函数层
//（设计：docs/superpowers/specs/2026-09-27-markdown-phase3-block-render-design.md
//   §4.1 D2/D3、§4.7 文件改动清单、§5 性能目标 #4；接线硬约束见
//   docs/superpowers/plans/2026-09-27-md-phase3-m0-spike.md §2）。
//
// 职责窄：把「文档字节偏移 → 活动块 / 块局部偏移」与「块文本 → 输入框贴合高度」
// 抽成可单测的纯计算。真实行数测量（InputModel::measureLines）由消费侧注入，
// 本层不 include 任何 EUI-NEO 组件头、不抛异常 —— 照抄 md_block_model /
// md_sync_map 的解耦约定。
//
// 本批（M2）刻意不做、留给后续里程碑的接口（不提前挖坑，故此处不出现）：
//   - 块级 undo 栈（M5）——独立纯函数，加在本文件尾部即可，不改变下列签名的语义。
// M4 已补：跨块 ↑↓ 目标 + 块边界删除（文档尾部），二者仍是纯字节算术。
#pragma once

#include <cstddef>
#include <functional>
#include <string>

#include "md_block_model.h"

namespace module::markdown {

// 活动 input 的行高系数：与 EUI-NEO components/input.h 的 textLineHeight =
// fontSize × 1.2 同源（上游真值 input.h:128）。改本值必须与上游同步——高度
// 比内容矮一像素，活动 input 就会长出内部滚动并吃掉外层滚轮（M0 约束 C4 / spec D2）。
inline constexpr float kInputLineHeightFactor = 1.2f;

// 活动块下标：块身份锚 = 文档字节偏移（spec D3，不用 stableId —— 拆块/并块时
// stableId 会变，偏移天然跟随光标）。语义 = MdBlockModel::blockIndexAt：
//   - 空文档（无块）→ 0（消费侧按「块序列为空」决定不建 input）
//   - cursorByte ≥ 文档尾 → 末块（光标停在文末时仍能编辑最后一段）
//   - 块边界字节归【后一块】：块序列连续覆盖 [0,size)，b.end == 后一块 start
//     （分块器把块间空行并入下一块头部，见 md_block_model.h）
size_t mdLiveActiveBlockIndex(const MdBlockModel& model, size_t cursorByte);

// 文档字节偏移 → 块局部偏移（钳到 [0, 块长]）。input 只持块文本，本函数是
// 「文档偏移 ↔ input 光标」的唯一换算点（切块反向写入 / 拼回整篇都靠它）。
size_t mdLiveBlockLocalOffset(const MdBlock& block, size_t cursorByte);

// 行数测量注入：块文本 + 文本区宽 → 可视行数（≥1）。真实场景 =
// InputModel::measureLines(blockText, fontFamily, fontSize, textWidth).size()；
// 单测用假函数（N 行 / 超宽折行都能直接构造）。
using MdLiveLineCountFn =
    std::function<int(const std::string& blockText, float textWidth)>;

// IME 冻结谓词（spec §3.4 / 风险 R3）：组字非空期间【冻结块推导 + 拒绝一切
// 切块入口】——中文拼音在飞的预编辑文本只活在 input 的 display 状态里（不写
// 文档），此刻换块换文本会把组字撕裂（丢字/串块）。语义 = 「组字文本非空」，
// 单独抽成函数是为了让判定口径可单测（消费侧喂 InputState::compositionText）。
// 消费侧两条纪律（缺一不可）：
//   ① 冻结期间不重算活动块锚（不换文本、不换块）；
//   ② 失焦时必须清 compositionText（否则焦点已走、组字永不结束 → 本谓词
//      恒真 → 切块被永久拒绝 + 幽灵预编辑留在输入框里）。
bool mdLiveCompositionFrozen(const std::string& compositionText);

// 内容贴合高度（spec D2 / §4.2）：
//     h = inset×2 + 行数 × fontSize × 1.2 + 1
// 行高系数 1.2 与 input 内部 lineHeight = fontSize×1.2 同源（components/input.h）；
// 末尾 +1px 余量保证 viewportHeight(= h − inset×2) ≥ contentHeight(= 行数×行高)
// ⇒ 活动 input 的 maxVerticalScroll == 0：不吃外层滚轮、不出现内部滚动条
//（M0 约束 C4 / V3a：任何"偏矮"都会让 input 注册 onScroll 并吞掉滚轮）。
// textWidth 必须是 input 内部排版用的同一宽度（= 声明宽 − inset×2；本模块不给
// 活动 input 开滚动条，故无滚动条槽位扣减）。
float mdLiveInputHeight(const std::string& blockText, float textWidth,
                        float fontSize, float inset,
                        const MdLiveLineCountFn& lineCount);

// ---- M4：块边界键（spec §3.4 / §4.7）----
// 核心决策（spec §3.4 / 已定决策 D-C）：块边界【不做块结构操作】，只做普通文本
// 编辑 —— 块身份始终跟随光标（锚 = 文档字节偏移，D3）。所以本层只回答两件事：
//   「↑↓ 该跳到哪个字节」与「边界退格/删除该删哪个字节范围」，不拥有任何状态。

// 「不消费」哨兵：交回 input 内置（块内移动 / 什么也不做）。
inline constexpr size_t kMdLiveNoConsume = static_cast<size_t>(-1);

// 跨块 ↑↓ 目标（spec §3.4）：direction < 0 = ↑、> 0 = ↓；返回目标【文档字节
// 偏移】或 kMdLiveNoConsume。
//   ↑：光标在活动块【首行】→ 目标 = 上一块末行（行首 + 列尽量保持）
//   ↓：光标在活动块【末行】→ 目标 = 下一块首行（行首 + 列尽量保持）
// 不消费：空块序列 / 空文档 / direction == 0 / 首块 ↑ / 末块 ↓ / 光标不在边界行。
// 术语按【源码行】（软折行不参与判定 —— 真实折行位置只有 input 的内部 layout
// 知道，本层不 include 组件头，故折行内的 ↑↓ 也按源码行处理；块首行内的 ↑ 一律
// 跨块，这是 spec §3.4 的原文口径）。
// 列 = 光标在当前行的行内字节偏移，落到目标行时钳到该行【内容】长（行终结符
// 不计 —— "\r\n" 的 '\r' 不算内容，光标不会落到 '\r' 之后）。块下标与
// blockIndexAt 同口径：块边界字节归后一块；块首恒为行首（分块器按行切）。
size_t mdLiveVerticalBlockTarget(const std::string& text,
                                 const std::vector<MdBlock>& blocks,
                                 size_t cursorByte, int direction);

// 块边界删除（spec §3.4）的待执行编辑：删 [eraseStart, eraseEnd) 后光标落
// cursor（== eraseStart，即两个块粘连的接缝字节）。
struct MdLiveBoundaryEdit {
    size_t eraseStart = 0;
    size_t eraseEnd = 0;
    size_t cursor = 0;
};

// 块首退格 / 块尾删除（direction < 0 = Backspace、> 0 = Delete）：都要求光标
// 正好落在【块边界字节】上（docByte == 某块 start —— 也就是"后一块块首 == 前
// 一块块尾"的同一字节，spec §3.4 的「文档偏移 == 块起点 / 块终点」）：
//   Backspace：k ≥ 1 → 删光标【前】一个行终结符（'\n'，CRLF 成对删 '\r\n'）
//   Delete   ：光标处就是行终结符 → 删它（CRLF 成对删）；否则不消费
// 返回 false = 不消费（交回内置）：
//   空块序列 / direction == 0 / 光标不在块边界 / 首块 Backspace / 文末 Delete
//   （cursorByte ≥ size）/ Delete 处不是行终结符（= 普通块内字符删除，内置口径
//   与「只做普通文本编辑」一致）。有选区时调用侧不得进来（内置负责删选区）。
bool mdLiveBoundaryEdit(const std::string& text,
                        const std::vector<MdBlock>& blocks, size_t cursorByte,
                        int direction, MdLiveBoundaryEdit& out);

} // namespace module::markdown
