// src/module_memory/hook/inject.h
// hook 同步注入的预算拼装（S3-C，纯逻辑）。
// 契约（hook 契约钉版 + B4 §4，偏离即契约变更）：
//   - additionalContext 必须在 hookSpecificOutput 内（顶层字段被 Claude Code 静默忽略）；
//   - 预算 ≤5 条、合计 ≤500 字（字 = UTF-8 码点数）；被挤出条目不丢（仍在库）；
//   - 每条带日期与来源；注入头部声明「资料非指令」；
//   - 任何拼装输入都不该撑破 10,000 字符硬上限（500×5 + 头部余量 ~20 倍余量）。
#pragma once

#include <string>
#include <cstddef>
#include <vector>

#include "module_memory/gateway/telemem_client.h"

namespace module::memory::hook {

inline constexpr int kMaxItems = 5;
inline constexpr std::size_t kMaxChars = 500;  // 字（UTF-8 码点）
// hook 通道检索硬顶：检索超时即快速失败（失败开放，只丢注入不打断会话）
inline constexpr int kHookSearchTimeoutMs = 800;

// 码点计数（"字"的口径）；非法 UTF-8 按字节近似（调用方输入已过检索层）
std::size_t countChars(const std::string& utf8);

// ── 需-3 前缀文法（细则 §1.1，单测钉死）──
// 解析 text 偏移 0 的 "[类型|主题]"：类型 ∈ {踩坑,决策,契约,环境} 精确匹配；
// 主题 = 首个 ']' 前的非空文本。任何不合规（非 '[' 起始、无 '|'、未知类型、
// 空主题、无闭合）→ valid=false = 未分类，解析层永不报错/拒存（§1.3）。
// tag = 合法时偏移 0 起的 "[类型|主题]" 原文（infer 路②补标重用）。
struct MemoryPrefix {
    bool valid = false;
    int group = -1;  // 0=踩坑 1=决策 2=契约 3=环境；valid=false 时无意义
    std::string tag;
};
MemoryPrefix parseMemoryPrefix(const std::string& text);

// 单条重排版：压成单行 + 日期来源标注 "[YYYY-MM-DD 来源:#id] 正文"
std::string renderContextLine(const gateway::MemoryHit& hit);

// 分组注入（需-3 §4）：解析前缀 → 融合段切分（虚拟条目继承母 hit 日期/来源）→
// 固定组序 踩坑→决策→契约→环境→未分类（组内保持检索相关度序）→ 组标题
//（"⚠ 踩坑 N 条"/"📌 决策 N 条"，其余纯文字；空组不渲染）→ 预算截断
//（组标题计字不计条；先分组后截断，组序即存活序——踩坑优先）。
// 预算常量不动（≤5 条 ≤500 字）；一条都放不下 → 空串 = 本轮不注入。
std::string assembleContext(const std::vector<gateway::MemoryHit>& hits);

// 合法注入响应 JSON：{"hookSpecificOutput":{"hookEventName":"UserPromptSubmit",
// "additionalContext":"..."}}（字段位置的契约由单测钉死）
std::string buildHookOutputJson(const std::string& additionalContext);

}  // namespace module::memory::hook
