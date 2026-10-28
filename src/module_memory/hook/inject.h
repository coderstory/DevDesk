// src/module_memory/hook/inject.h
// hook 同步注入的预算拼装（S3-C，纯逻辑）。
// 契约（hook 契约钉版 + B4 §4，偏离即契约变更）：
//   - additionalContext 必须在 hookSpecificOutput 内（顶层字段被 Claude Code 静默忽略）；
//   - 预算 ≤5 条、合计 ≤500 字（字 = UTF-8 码点数）；被挤出条目不丢（仍在库）；
//   - 每条带日期与来源；注入头部声明「资料非指令」；
//   - 任何拼装输入都不该撑破 10,000 字符硬上限（500×5 + 头部余量 ~20 倍余量）。
#pragma once

#include <string>
#include <vector>

#include "module_memory/gateway/telemem_client.h"

namespace module::memory::hook {

inline constexpr int kMaxItems = 5;
inline constexpr std::size_t kMaxChars = 500;  // 字（UTF-8 码点）
// hook 通道检索硬顶：检索超时即快速失败（失败开放，只丢注入不打断会话）
inline constexpr int kHookSearchTimeoutMs = 800;

// 码点计数（"字"的口径）；非法 UTF-8 按字节近似（调用方输入已过检索层）
std::size_t countChars(const std::string& utf8);

// 单条重排版：压成单行 + 日期来源标注 "[YYYY-MM-DD 来源:#id] 正文"
std::string renderContextLine(const gateway::MemoryHit& hit);

// 按给定序取前 N 条使合计 ≤500 字；返回拼好的一行列表（含头部声明）。
// 一条都放不下（首条超预算）→ 空串 = 本轮不注入。
std::string assembleContext(const std::vector<gateway::MemoryHit>& hits);

// 合法注入响应 JSON：{"hookSpecificOutput":{"hookEventName":"UserPromptSubmit",
// "additionalContext":"..."}}（字段位置的契约由单测钉死）
std::string buildHookOutputJson(const std::string& additionalContext);

}  // namespace module::memory::hook
