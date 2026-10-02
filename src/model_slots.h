// src/model_slots.h
// 模型槽位单一真相源（X 宏表）——**中立头**（src 根，与 module_pet_bridge.h 同型）：
// 消费方横跨两个模块（module_claude_proxy 的 provider_config / module_agent 的
// agent_module），放在任一模块内都会让另一方反向依赖兄弟模块（破模块隔离铁律）。
// header-only、零 include：表本身只是宏，不依赖任何类型。
//
// 用法：消费方各自展开本表派生自己需要的那一列（不复制数据、不手抄键名）：
//   provider_config.h  → 成员 / 元数据行 / 计数 / static_assert
//   agent_module.cpp   → 模型槽位下拉的键集与显示名
#pragma once

// ---- 槽位单一真相源（审计 CPX-08）----
// 「数据成员 ↔ models_config JSON 键 ↔ 官方 env 键 ↔ *_NAME 键 ↔ UI 显示名」
// 的唯一声明处。新增一个模型槽位 = 在本表加一行，其余全链自动派生：
//   ModelsConfig 数据成员 / models_config 序列化与反序列化 / 粘贴导入解析 /
//   接管 env 写入 / 托管键全集（managedEnvKeyNames）/ 反向同步映射 /
//   UI 详情面板行 · 表单页行 · 摘要计数 · 导入预览计数
//
// 行参数（X 宏契约）：
//   X(成员名, JSON键, 详情显示名, 表单显示名, 官方env键, *_NAME键, supportsOneM)
//     - 成员名     ModelsConfig 字段名（表行引用它 → 写错即编译期报错）
//     - JSON键     models_config 里该槽位的键
//     - 详情/表单   两套 UI 文案（既有外观两处措辞不同，故各留一列）
//     - env键      发往上游 / settings.json 的模型键
//     - _NAME键    官方 *_NAME 变体；nullptr = 无该变体（def / subagent 不生成）
//     - supportsOneM  该槽位是否支持 1M 上下文标记（批次 F，2026-10-02）：
//                     **尾插第 7 参**（既有 6 参逐字不动，消费方各自多一个形参）。
//                     取值**照抄 cc-switch 参考实现**（用户 2026-10-02 明确指示
//                     「直接照 cc-switch 的业务逻辑抄，不要自己保守裁决」）：
//                     src/components/providers/forms/ClaudeFormFields.tsx 的
//                     modelRoleRows（sonnet/opus/fable/subagent = true，
//                     haiku = false；本表逐项对齐，subagent 亦为 true）。
//                     def 槽位 cc-switch 无对应行（它的 ANTHROPIC_MODEL 是独立
//                     输入框，不在角色表里）——本表按「1M 是主模型能力」取 true，
//                     与 ANTHROPIC_MODEL 的实际语义一致。
//
// 为什么用 X 宏：C++ 无反射，成员名与全部元数据必须由同一处声明生成，才能做到
// 「加一行、全链生效」且「漏改不静默」——表行引用不存在的成员 = 编译期报错；
// 成员没进表 = 紧随其后的 static_assert 报错。
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

// 注：本表原先内联在 module_claude_proxy/provider_config.h（审计 CPX-08 的落点），
// M2 Task 10 迁到 src 根中立头——宏名与 6 参形态逐字不变，老消费方只多一行
// #include。新增槽位改本文件，全链自动跟随。
