// src/model_slots.h
// 模型槽位单一真相源（X 宏表），中立头（src 根）：消费方横跨 module_claude_proxy 的
// provider_config 与 module_agent 的 agent_module，放任一模块内都会让另一方反向依赖兄弟模块。
// header-only、零 include。消费方各自展开本表派生自己需要的那一列，不复制数据不手抄键名。
#pragma once

// 槽位单一真相源
// 「数据成员 ↔ models_config JSON 键 ↔ 官方 env 键 ↔ *_NAME 键 ↔ UI 显示名」的唯一声明处。
// 新增槽位 = 本表加一行，其余全链（数据成员/序列化/导入解析/env 写入/UI 行/计数）自动派生
// 行参数：X(成员名, JSON键, 详情显示名, 表单显示名, 官方env键, *_NAME键, supportsOneM)。
// 成员名 = ModelsConfig 字段名（写错即编译期报错）；_NAME 键 nullptr = 无该变体（def/subagent）；
// supportsOneM 是尾插第 7 参，取值照抄 cc-switch（sonnet/opus/fable/subagent=true，haiku=false）
// 用 X 宏：C++ 无反射，成员名与全部元数据必须同处声明，才能「加一行全链生效」且「漏改不静默」
#define DEVDESK_MODEL_SLOT_LIST(X)                                             \
    X(def,      "default",  "默认",   "默认 (MODEL)", "ANTHROPIC_MODEL",      \
      nullptr, true)                                                          \
    X(sonnet,   "sonnet",   "Sonnet", "Sonnet",                               \
      "ANTHROPIC_DEFAULT_SONNET_MODEL", "ANTHROPIC_DEFAULT_SONNET_MODEL_NAME", \
      true)                                                                   \
    X(opus,     "opus",     "Opus",   "Opus",                                 \
      "ANTHROPIC_DEFAULT_OPUS_MODEL", "ANTHROPIC_DEFAULT_OPUS_MODEL_NAME",     \
      true)                                                                   \
    X(haiku,    "haiku",    "Haiku",  "Haiku",                                \
      "ANTHROPIC_DEFAULT_HAIKU_MODEL", "ANTHROPIC_DEFAULT_HAIKU_MODEL_NAME",   \
      false)                                                                  \
    X(fable,    "fable",    "Fable",  "Fable",                                \
      "ANTHROPIC_DEFAULT_FABLE_MODEL", "ANTHROPIC_DEFAULT_FABLE_MODEL_NAME",   \
      true)                                                                   \
    X(subagent, "subagent", "子代理", "Subagent",                              \
      "CLAUDE_CODE_SUBAGENT_MODEL", nullptr, true)
