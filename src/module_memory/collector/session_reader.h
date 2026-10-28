// src/module_memory/collector/session_reader.h
// Claude Code 会话 jsonl 单行解析 + 四道过滤（S3-B）。
// 四道过滤契约（feasibility §4.2，逐条对齐，偏离即契约变更）：
//   ① 角色过滤：只取 user/assistant 正文；工具结果仅在 is_error 时保留 200 字节摘要，
//     thinking/tool_use 块一律丢弃；
//   ② 注入块剔除：正文含 <system-reminder 标记 → 整条丢弃（不能当项目记忆）；
//   ③ 单事件截断：上限 kMaxEventBytes 字节，UTF-8 码点边界安全截断；
//   ④ 段级跳过：整段没有 user 正文时跳过（在 SegmentAssembler，不在本单元）。
// event_seq = 该行在会话文件中的 0 基行号，文件内单调，跨尾读稳定——采集幂等的唯一键。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/result.h"

namespace module::memory::collector {

// 单事件上限（§4.2 实测口径：单事件可达 86.5KB，必须按事件截断）
inline constexpr std::size_t kMaxEventBytes = 4096;
// 错误工具结果保留的摘要上限（§4.2「只在错误/失败时保留摘要」）
inline constexpr std::size_t kToolErrorSummaryBytes = 200;

struct SessionEvent {
    std::string sessionId;
    std::uint64_t eventSeq = 0;  // 行号（0 基）
    bool narrative = false;      // 正文旗标：用户/助手文本为真；工具错误摘要为假
                                 //（段级跳过判据 = 段内无 narrative 事件）
    std::string text;            // 过滤①②③后的正文（UTF-8）
};

// 解析一行会话 jsonl：过滤后无正文 → 空向量（不是错误）；坏 JSON → Error（坏行
// 计数上报，不中断整文件摄取——单行损坏不代表后续行不可用）。
core::Result<std::vector<SessionEvent>> parseSessionLine(
    const std::string& line, std::uint64_t seq);

// UTF-8 码点边界安全截断（不劈开多字节序列）
std::string truncateUtf8(const std::string& text, std::size_t maxBytes);

}  // namespace module::memory::collector
