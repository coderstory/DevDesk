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
//   - 块级 undo 栈（M5）、跨块 ↑↓ 目标（M4）——二者各自独立纯函数，加在
//     本文件尾部即可，不改变下列签名的语义。
#pragma once

#include <cstddef>
#include <functional>
#include <string>

#include "md_block_model.h"

namespace module::markdown {

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

} // namespace module::markdown
