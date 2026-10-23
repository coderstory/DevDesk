// 自然语言 → 结构化任务草稿：纯逻辑层（无 UI、无 DB、无网络自实现）。
//
// 目标：用户用一句话描述想要的任务（"每天早八点帮我盯热榜，有大新闻就通知我"），
// 宿主把这句话转成 AgentTask 的草稿字段 + 校验结果 + 给人看的降级提示。UI 接线
// （T7b）拿这份草稿回填任务弹窗，不直接信任模型输出。
//
// ============================ 关键原则 ============================
// **草稿是增强功能，不是任务本体。** 任何单项校验失败都只**降级 + warnings**，
// 绝不让整个生成失败 —— 哪怕 goal 全空、cron 是乱码，也要给用户一份能改的草稿
// 加一句人话解释，而不是甩一个错误让人重写整句话。
//   因此 `parseTaskDraft` 的 fail **只用于「连一个 JSON 对象都没解析出来」**
//   （模型返回空串 / 纯文本 / 坏 JSON）—— 那时连可编辑的草稿骨架都给不出，
//   返回 ok 会让 UI 拿一份全空草稿装成"生成成功"。
// ==================================================================
//
// 字段可信度用两个布尔位表达（UI 读它们决定要不要弹提示 / 标脏）：
//   goalFilled    —— goal 是否是模型填出的可执行指令（trim 后非空）
//   triggerFilled —— 触发类型**连同载荷**都通过校验并已编码进 triggerConfig。
//                    false = 模型给的触发意图不可信（未知类型 / cron 非法 /
//                    闹钟时间缺失），草稿已降级为手动，请用户自己确认。
// draft.triggerType / draft.triggerConfig 恒是**合法可落库**的组合（降级后是
// manual + 空串），所以草稿不会带出 triggerConfigProblem 非空的"配置异常"标记。
//
// **两个 prompt 里有、草稿里没有的字段**（有意为之，不是漏实现）：
//   - allowDestructive：prompt 要求模型输出，解析层**刻意不采纳** —— 破坏性授权
//     只能由用户在 UI 上亲手开，模型说「要删」不等于授权（与 agent_loop 里
//     allowDestructive=0 遇破坏性工具即终止 run 的口径同向）。
//   - maxIterations：同上，用现有缺省 10，UI 层管这个字段（解析层不搬运配置项）。
#pragma once

#include <ctime>
#include <string>
#include <vector>

#include "core/llm_endpoint.h"
#include "core/result.h"

namespace module::agent {

struct TaskDraft {
    std::string goal;            // 任务指令（LLM 改写后的完整可执行指令）
    std::string triggerType;     // manual|schedule|resident|event|daily|once
    std::string triggerConfig;   // 编码后的 JSON 字符串（由 TriggerConfig encode 而非 LLM 直出）
    std::vector<std::string> warnings;   // 给用户看的降级/提示
};

struct TaskDraftResult {
    TaskDraft draft;
    bool goalFilled = false;     // goal 是否可信填入
    bool triggerFilled = false;  // 触发配置是否通过校验
    std::vector<std::string> needsUserInput;  // 如 "请指定闹钟时间"
};

// 解析 + 校验 + 降级（无网络、无 DB、零副作用）。
//
// knownToolNames 用于校验 goal 里引用的工具名（形如 todo.list 的点分 token）；
// **空表 = 跳过该校验**（单测与无工具环境用）。模型返回非法 JSON → fail；
// 其它一切问题 → 降级 + warnings。
//
// 时钟：once 的「必须晚于现在」判定读一次真时钟（core::time::nowUnix），这是本层
// 唯一的时钟依赖 —— 契约固定为本签名，故不注入；用例用「明显过去 / 明显未来」的
// 绝对时刻构造，不依赖运行时点。
core::Result<TaskDraftResult> parseTaskDraft(const std::string& llmText,
                                              const std::vector<std::string>& knownToolNames);

// 组装 system prompt：角色约束 + 当前本地时间（模型没有时钟，「明天 8 点」靠它换算）
// + 六种触发类型语义 + 可用工具名 + 严格 JSON-only 输出 schema + 常见模式示例。
// toolNames 为空时给出「当前无可用工具」的显式说明（不留空列表让人猜）。
std::string buildTaskDraftSystemPrompt(std::time_t now,
                                       const std::vector<std::string>& toolNames);

// 发一次请求拿模型原文（不含解析）。toolsJson 传空串 = 纯文本补全（不声明 tools，
// 与 agent_loop 的带工具调用形态互斥）。工具名取自 core::ToolRegistry::instance()
//   ——调用方无需注入，避免签名被"当前注册了什么"污染。
core::Result<std::string> requestTaskDraft(core::LlmEndpoint& llm,
                                           const std::string& userText,
                                           std::time_t now,
                                           const std::string& modelSlot);

// 注册表快照 → 工具名列表（T7b UI 层喂 parseTaskDraft 的 knownToolNames 用）。
// 与 buildTaskDraftSystemPrompt 内部取名同源——不新增 ToolRegistry::listNames()：
// 那会把「注册表返回 shared_ptr」这一层约定泄漏成第二个列举口，两处以后会漂。
std::vector<std::string> registeredToolNames();

} // namespace module::agent