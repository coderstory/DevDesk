// task_draft 实现（契约见 task_draft.h）：
//   buildTaskDraftSystemPrompt —— prompt 组装（时间注入 / 触发类型表 / 工具表 / JSON-only schema / 示例）
//   requestTaskDraft            —— 一次纯文本补全（toolsJson 传空串），取 text 块拼接
//   parseTaskDraft              —— fence 剥离 → JSON 提取 → 校验 → **降级 + warnings**
//
// 降级矩阵（唯一的 fail 只在第一行：连 JSON 对象都没解析出来）：
//   模型返回空串 / 纯文本 / 坏 JSON  → fail「模型返回的不是合法 JSON」
//   goal 空                          → goalFilled=false + warnings，触发照常校验
//   goal 超长                        → UTF-8 边界安全截断 + warnings（不截断的半字会让 UI 渲染乱码）
//   goal 引用未知工具                → warnings 逐个点名（不拦：LLM 可能只是提了一句）
//   triggerType 缺失 / 未知         → 降级 manual + warnings + needsUserInput
//   schedule 且 cron 非法 / 缺失    → 降级 manual + warnings + needsUserInput
//   once 且 onceLocal 缺失 / 非法 / 已过去 → 降级 manual + warnings + needsUserInput
//   event 且 events 缺失 / 空       → 降级 manual + warnings + needsUserInput
//   manual / daily / resident       → 恒合法，triggerFilled=true，triggerConfig 空串
#include "module_agent/task_draft.h"

#include <algorithm>
#include <utility>

#include "core/tool.h"
#include "core/time.h"
#include "module_agent/agent_loop.h"       // currentTimePromptLine
#include "module_agent/cron.h"             // parseCron
#include "module_agent/message_blocks.h"   // decodeMessageBlocks / utf8Truncate
#include "module_agent/task_draft_parse.h" // trimCopy / parseOnceAtLocalText
#include "module_agent/trigger_config.h"   // TriggerConfig / encodeTriggerConfig
#include "platform/json.h"

namespace module::agent {

namespace {

namespace pj = platform::json;

// 已知触发类型全集，与 agent_module.cpp 的 kTriggerKeys 同源同值；这里只是草稿层的
// 判据副本（UI 的下拉顺序表不归本层管），新增类型两处一起改
constexpr const char* kTriggerTypes[] = {"manual",   "schedule", "resident",
                                        "event",    "daily",    "once"};

// goal 字节上限：AgentLoop 把 goal 原文回喂给模型，太长会挤爆 max_tokens=4096 的
// 首轮预算；截到字符边界 + warnings（让用户知道被截了），不静默削
constexpr size_t kMaxGoalBytes = 4000;

bool isKnownTriggerType(const std::string& t) {
    for (const char* known : kTriggerTypes) {
        if (t == known) return true;
    }
    return false;
}

void pushUnique(std::vector<std::string>& v, const std::string& s) {
    if (s.empty()) return;
    if (std::find(v.begin(), v.end(), s) != v.end()) return;
    v.push_back(s);
}

// 取字符串字段并 trim；缺键 / 非字符串 → 空串（不报错：模型字段不全属降级范畴）
std::string trimmedStr(const pj::JsonObject& obj, const char* key) {
    pj::JsonValue v = obj.get(key);
    return v.isString() ? trimCopy(v.asString()) : std::string();
}

// 取字符串数组字段；缺键 / 非数组 / 含非字符串元素 → 空 vec（坏形状当没给）
std::vector<std::string> stringArray(const pj::JsonObject& obj, const char* key) {
    std::vector<std::string> out;
    pj::JsonValue v = obj.get(key);
    if (!v.isArray()) return out;
    for (int i = 0; i < v.size(); ++i) {
        pj::JsonValue item = v[i];
        if (!item.isString()) continue;
        const std::string s = trimCopy(item.asString());
        if (!s.empty()) out.push_back(s);
    }
    return out;
}

// 剥 markdown code fence（```json … ``` 或 ``` … ```，可嵌套数层）。
// 只剥**包裹**形态（前缀 ```+ 语言标注 + 换行、就近的 ``` 收尾）；文本里零散出现的
// ``` 不动 —— 那是模型正文，不是围栏。
std::string stripCodeFence(const std::string& raw) {
    std::string s = trimCopy(raw);
    for (int guard = 0; guard < 4; ++guard) {
        if (s.rfind("```", 0) != 0) break;
        const size_t nl = s.find('\n');
        if (nl == std::string::npos) break;
        std::string body = s.substr(nl + 1);
        const size_t close = body.rfind("```");
        if (close == std::string::npos) break;
        body = body.substr(0, close);
        const std::string next = trimCopy(body);
        if (next == s) break;   // 剥不动了就停，防自造死循环
        s = next;
    }
    return s;
}

// 容忍前后废话：取首个 '{' 到末个 '}' 的子串。找不到成对括号 → 空串（交给上层 fail）
std::string extractJsonObject(const std::string& raw) {
    const size_t b = raw.find('{');
    if (b == std::string::npos) return {};
    const size_t e = raw.rfind('}');
    if (e == std::string::npos || e < b) return {};
    return raw.substr(b, e - b + 1);
}

// 扫出 goal 里形如 a.b / md.save / hot.list 的点分标识符 token（只看 [a-z0-9_]，
// 大写与多字节字节天然不匹配 —— 不引 locale、不做 Unicode 类别判定）。
// 用途只是「模型是不是在引用一个不存在的工具名」，宁可漏报也不误报。
std::vector<std::string> dottedTokens(const std::string& text) {
    const auto isWordStart = [](char c) {
        return c == '_' || (c >= 'a' && c <= 'z');
    };
    const auto isWordChar = [&isWordStart](char c) {
        return isWordStart(c) || (c >= '0' && c <= '9');
    };
    std::vector<std::string> found;
    size_t i = 0;
    while (i < text.size()) {
        if (!isWordStart(text[i])) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < text.size() && isWordChar(text[j])) ++j;
        if (j + 1 < text.size() && text[j] == '.' && isWordStart(text[j + 1])) {
            size_t k = j + 1;
            while (k < text.size() && isWordChar(text[k])) ++k;
            found.push_back(text.substr(i, k - i));
            i = k;
        } else {
            i = j;
        }
    }
    return found;
}

std::string joinComma(const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) out += "、";
        out += v[i];
    }
    return out;
}

// 触发载荷编码 + triggerConfigProblem 复核（Scheduler 消费侧的同源判据）。
// 复用原因：decode 能过 ≠ 能触发（schedule 还要 parseCron、event 还要 events 非空）。
// 返回空串 = 合法；非空 = 问题描述。
std::string encodeAndVerify(const TriggerConfig& cfg, const std::string& triggerType,
                            std::string& encoded) {
    auto e = encodeTriggerConfig(cfg);
    if (e.isFail()) return "触发配置编码失败：" + e.errorMessage();
    encoded = e.value();
    return triggerConfigProblem(triggerType, encoded);
}

} // namespace

// 注册表快照 → 工具名列表（task_draft 内部拼 prompt 用；T7b UI 层也用它喂
// parseTaskDraft 的 knownToolNames）。不新增 ToolRegistry::listNames()：
// 那会把「注册表返回 shared_ptr」这一层约定泄漏成第二个列举口，两处以后会漂。
std::vector<std::string> registeredToolNames() {
    std::vector<std::string> names;
    for (const auto& tool : core::ToolRegistry::instance().listTools()) {
        if (tool) names.push_back(tool->name());
    }
    return names;
}

std::string buildTaskDraftSystemPrompt(std::time_t now,
                                       const std::vector<std::string>& toolNames) {
    std::string p;
    p +=
        "你是 DevDesk 智能体页的「任务配置助手」。用户会用一句话描述他想要的任务，"
        "你只负责把它转成一份结构化任务草稿。你不执行任务、不调用工具、不回答任务"
        "本身的问题，只输出草稿 JSON。\n\n";
    // 模型没有时钟：相对时间（明天 / 下周一 / 八点）只能靠宿主给的这一行换算，
    // 不注入就让模型自行编一个日期 —— 编出来的闹钟时间会让用户等到错误的时刻
    p += "[当前时间]\n";
    p += currentTimePromptLine(now);
    p +=
        "\n你没有时钟。凡是「明天」「下周一」「八点半」这类相对时间，必须依据上面"
        "这行换算成绝对值；alarm 时刻一律用本地时区的 24 小时制。\n\n";

    p +=
        "[可用触发类型]（triggerType 只能取下面六个值之一）\n"
        "- manual：手动。只在用户亲自点「运行」时执行一次。查询类、临时整理类用这个。\n"
        "- schedule：定时。按 cron 五段表达式循环执行，五段依次是「分 时 日 月 周」"
        "（不含年份；周 0-6，0=周日）。例：「每天 8 点」= 0 8 * * *；「每周一 9 点半」="
        "30 9 * * 1。\n"
        "- daily：每天。不指定具体时刻，当天第一个调度周期跑一次。\n"
        "- once：闹钟。一次性绝对时间到点触发一次。必须给 onceLocal（格式 "
        "YYYY-MM-DD HH:MM，本地时区，且必须晚于当前时间）。用户没说具体时刻就把要"
        "补的信息写进 needsUserInput，**不要自己编一个时间**。\n"
        "- event：事件。应用内事件（如新建/完成待办、保存笔记）发生时触发。\n"
        "- resident：常驻。长期驻留、按需自行推进的任务。\n\n";

    p += "[可用工具]（goal 里只允许引用下面这些工具名）\n";
    if (toolNames.empty()) {
        p += "（当前没有可用工具。goal 请写成不依赖任何工具的自足指令。）\n";
    } else {
        p += joinComma(toolNames) + "\n";
    }
    p +=
        "\n[goal 怎么写]\n"
        "goal 是交给任务执行器的指令，必须是完整、可直接执行的自然语言，并写清：\n"
        "1. 调用什么工具、传什么参数；\n"
        "2. 判断分支（什么条件走哪条路、什么情况不做任何动作）；\n"
        "3. 输出格式（给用户看什么、怎么组织）；\n"
        "4. 约束：不编造数据；不改动或删除用户已有数据（除非用户明确要求且 "
        "allowDestructive 为 true）；不回显密钥等敏感信息。\n"
        "contextNote 只放长期有效的约束（如「只处理 work 前缀的待办」），没有就空串。\n"
        "两者的分工：goal 是这一次要做的步骤，contextNote 是每次都生效的前提。\n\n";

    p +=
        "[输出格式]（唯一硬要求）\n"
        "只输出一个 JSON 对象。不要 markdown 代码块围栏（```），不要任何解释文字、"
        "前后缀或多余空行。字段固定为下面这七个，键名一字不改：\n"
        "{\"goal\":\"...\",\"contextNote\":\"\",\"triggerType\":\"manual\",\"cron\":"
        "\"\",\"onceLocal\":\"YYYY-MM-DD HH:MM\",\"maxIterations\":10,"
        "\"allowDestructive\":false,\"needsUserInput\":[]}\n"
        "- cron：仅 triggerType=schedule 时填五段表达式，其余一律填空串。\n"
        "- onceLocal：仅 triggerType=once 时填，其余一律填空串。\n"
        "- maxIterations：建议迭代上限，10 即可（模型建议值不保证被采纳）。\n"
        "- allowDestructive：默认 false。只有用户**明确要求**删除、覆盖、修改已有数据"
        "时才填 true。\n"
        "- needsUserInput：还需要用户补充的信息（如「请指定闹钟时间（YYYY-MM-DD "
        "HH:MM）」），没有就填空数组。\n\n";

    p +=
        "[示例]（工具名以实际可用工具表为准）\n"
        "用户：每天早上八点帮我看看热榜，有大新闻就通知我\n"
        "输出：{\"goal\":\"调用 hot.list 取热榜；与昨天的条目做对比（取不到昨天数据"
        "就只报当前前十）。有新增的显著条目时调用 app.notify 通知我标题与链接；没有"
        "新增就不通知、不发任何消息。\",\"contextNote\":\"\",\"triggerType\":"
        "\"schedule\",\"cron\":\"0 8 * * *\",\"onceLocal\":\"\",\"maxIterations"
        "\":10,\"allowDestructive\":false,\"needsUserInput\":[]}\n\n"
        "用户：每天帮我新建一个当天的笔记\n"
        "输出：{\"goal\":\"调用 md.save 在笔记目录新建当天笔记，文件名为 "
        "YYYY-MM-DD.md，正文写当天日期和一句待办提示；文件已存在则跳过，不覆盖。\","
        "\"contextNote\":\"\",\"triggerType\":\"daily\",\"cron\":\"\",\"onceLocal"
        "\":\"\",\"maxIterations\":10,\"allowDestructive\":false,\"needsUserInput"
        "\":[]}\n\n"
        "用户：帮我看看有哪些逾期待办\n"
        "输出：{\"goal\":\"调用 todo.list 并传 overdue=true，列出标题与到期日并按"
        "到期日升序；结果为空就明确说没有逾期项。\",\"contextNote\":\"\",\"triggerType"
        "\":\"manual\",\"cron\":\"\",\"onceLocal\":\"\",\"maxIterations\":10,"
        "\"allowDestructive\":false,\"needsUserInput\":[]}\n\n"
        "用户：明早八点提醒我开会\n"
        "输出：{\"goal\":\"到点后调用 app.notify 发一条标题「会议提醒」、正文「该"
        "开会了」的通知。\",\"contextNote\":\"\",\"triggerType\":\"once\",\"cron"
        "\":\"\",\"onceLocal\":\"<按当前时间换算出的明早 08:00>\",\"maxIterations"
        "\":10,\"allowDestructive\":false,\"needsUserInput\":[]}\n\n"
        "用户：等我提醒我交房租（没说时间）\n"
        "输出：{\"goal\":\"到点后调用 app.notify 发一条标题「房租提醒」、正文「该"
        "交房租了」的通知。\",\"contextNote\":\"\",\"triggerType\":\"once\",\"cron"
        "\":\"\",\"onceLocal\":\"\",\"maxIterations\":10,\"allowDestructive\":false,"
        "\"needsUserInput\":[\"请指定闹钟时间（YYYY-MM-DD HH:MM）\"]}\n";
    return p;
}

core::Result<std::string> requestTaskDraft(core::LlmEndpoint& llm,
                                           const std::string& userText,
                                           std::time_t now,
                                           const std::string& modelSlot) {
    // user 消息 = 单个 text block；字面量走 json::stringify 拿转义，不手拼串
    auto textLiteral = pj::stringify(pj::makeString(userText));
    if (textLiteral.isFail()) {
        return core::Result<std::string>::fail(
            core::ErrorCode::Internal,
            "序列化用户描述失败：" + textLiteral.errorMessage());
    }
    std::vector<core::LlmMessage> messages;
    messages.push_back(
        {"user", "[{\"type\":\"text\",\"text\":" + textLiteral.value() + "}]"});

    // toolsJson 传空串 = 不声明 tools 的纯文本补全（DirectLlmClient 据此省掉
    // tools 键）；本层要的是「结构化草稿」而不是工具调用
    const std::string systemPrompt =
        buildTaskDraftSystemPrompt(now, registeredToolNames());
    auto resp = llm.send(messages, "", systemPrompt, modelSlot);
    if (resp.isFail()) {
        return core::Result<std::string>::fail(resp.errorCode(),
                                              resp.errorMessage());
    }
    // Text 块按序拼接；Plain 块（contentJson 非标准 blocks 数组时的原文回落）
    // 一并计入 —— 某些网关会把纯文本回灌成非数组形态，只认 Text 会误判成空回复
    std::string text;
    for (const auto& block : decodeMessageBlocks(resp.value().contentJson)) {
        if (block.type == MessageBlock::Type::Text ||
            block.type == MessageBlock::Type::Plain) {
            text += block.text;
        }
    }
    if (trimCopy(text).empty()) {
        return core::Result<std::string>::fail(
            core::ErrorCode::ValidationError, "模型未返回内容");
    }
    return core::Result<std::string>::ok(text);
}

core::Result<TaskDraftResult> parseTaskDraft(
    const std::string& llmText, const std::vector<std::string>& knownToolNames) {
    const std::string body = extractJsonObject(stripCodeFence(llmText));
    if (body.empty()) {
        return core::Result<TaskDraftResult>::fail(
            core::ErrorCode::ValidationError, "模型返回的不是合法 JSON");
    }
    auto parsed = pj::parse(body);
    if (parsed.isFail()) {
        return core::Result<TaskDraftResult>::fail(
            core::ErrorCode::ValidationError,
            "模型返回的不是合法 JSON：" + parsed.errorMessage());
    }
    if (!parsed.value().isObject()) {
        return core::Result<TaskDraftResult>::fail(
            core::ErrorCode::ValidationError,
            "模型返回的不是合法 JSON（顶层不是对象）");
    }
    const pj::JsonObject obj = parsed.value().asObject();

    TaskDraftResult out;
    TaskDraft& d = out.draft;

    // ---- goal ----
    std::string goal = trimmedStr(obj, "goal");
    if (goal.empty()) {
        d.warnings.push_back("模型没有给出可执行的任务目标，请手动补充");
    } else {
        if (goal.size() > kMaxGoalBytes) {
            goal = utf8Truncate(goal, kMaxGoalBytes);
            d.warnings.push_back(
                "任务目标过长，已截断到 " + std::to_string(kMaxGoalBytes) +
                " 字节，请检查结尾是否被截掉");
        }
        // 未知工具只提示不拦：模型可能只是在散文里提到一个词。
        // 工具表为空 = 本层拿不到注册表（无工具环境 / 未注入）→ 整体跳过，
        // 否则每一个点分 token 都会被误报成「未知工具」，warnings 变成噪音。
        if (!knownToolNames.empty()) {
            for (const auto& token : dottedTokens(goal)) {
                if (std::find(knownToolNames.begin(), knownToolNames.end(), token) !=
                    knownToolNames.end()) {
                    continue;
                }
                pushUnique(d.warnings, "任务目标引用了未知工具 " + token +
                                           "，请确认工具名是否正确");
            }
        }
        d.goal = goal;
        out.goalFilled = true;
    }
    d.contextNote = trimmedStr(obj, "contextNote");

    // ---- 模型自报「还缺什么」 ----
    for (const auto& ask : stringArray(obj, "needsUserInput")) {
        pushUnique(out.needsUserInput, ask);
    }

    // ---- 触发类型 + 载荷 ----
    // 降级统一落 manual + 空串：草稿恒是「可落库」的组合，不会带出配置异常标记
    auto degradeToManual = [&out](const std::string& why,
                                  const std::string& ask) {
        out.draft.triggerType = "manual";
        out.draft.triggerConfig.clear();
        out.triggerFilled = false;
        pushUnique(out.draft.warnings, why + "，已按「手动」处理");
        if (!ask.empty()) pushUnique(out.needsUserInput, ask);
    };
    auto accept = [&out](const std::string& type, const std::string& cfg) {
        out.draft.triggerType = type;
        out.draft.triggerConfig = cfg;
        out.triggerFilled = true;
    };

    const std::string triggerType = trimmedStr(obj, "triggerType");
    if (triggerType.empty()) {
        degradeToManual("模型没有指定触发方式", "请确认触发方式（手动 / 定时 / 每天 / 闹钟）");
    } else if (!isKnownTriggerType(triggerType)) {
        degradeToManual("模型给出的触发类型「" + triggerType + "」不在可选范围",
                        "请确认触发方式（手动 / 定时 / 每天 / 闹钟）");
    } else if (triggerType == "manual" || triggerType == "daily" ||
               triggerType == "resident") {
        // 这三型不消费 trigger_config（triggerConfigProblem 恒空），空串即合法
        accept(triggerType, std::string());
    } else if (triggerType == "schedule") {
        const std::string cron = trimmedStr(obj, "cron");
        auto expr = parseCron(cron);
        if (cron.empty()) {
            degradeToManual("模型选了定时触发但没有给出 cron 表达式",
                            "请填写 cron 表达式（分 时 日 月 周，如 0 8 * * *）");
        } else if (expr.isFail()) {
            degradeToManual("模型给出的 cron 表达式非法：" + expr.errorMessage(),
                            "请填写 cron 表达式（分 时 日 月 周，如 0 8 * * *）");
        } else {
            TriggerConfig cfg;
            cfg.cron = cron;
            std::string encoded;
            const std::string problem = encodeAndVerify(cfg, "schedule", encoded);
            if (!problem.empty()) {
                degradeToManual("定时触发配置无效：" + problem,
                                "请填写 cron 表达式（分 时 日 月 周，如 0 8 * * *）");
            } else {
                accept("schedule", encoded);
            }
        }
    } else if (triggerType == "once") {
        const std::string onceLocal = trimmedStr(obj, "onceLocal");
        const std::string ask = "请指定闹钟时间（YYYY-MM-DD HH:MM）";
        if (onceLocal.empty()) {
            degradeToManual("模型选了闹钟触发但没有给出具体时刻", ask);
        } else {
            auto parsedOnce = parseOnceAtLocalText(onceLocal);
            if (parsedOnce.isFail()) {
                degradeToManual("模型给出的闹钟时间无法解析：" + parsedOnce.errorMessage(),
                                ask);
            } else if (parsedOnce.value() <=
                       static_cast<int64_t>(core::time::nowUnix())) {
                // 存一个已过去的时刻 = 建了个永不触发的任务（Scheduler 错过不补）
                degradeToManual("模型给出的闹钟时间必须晚于当前时间", ask);
            } else {
                TriggerConfig cfg;
                cfg.onceAt = parsedOnce.value();
                std::string encoded;
                const std::string problem = encodeAndVerify(cfg, "once", encoded);
                if (!problem.empty()) {
                    degradeToManual("闹钟触发配置无效：" + problem, ask);
                } else {
                    accept("once", encoded);
                }
            }
        }
    } else {   // event
        const std::vector<std::string> events = stringArray(obj, "events");
        if (events.empty()) {
            degradeToManual("模型选了事件触发但没有给出要监听的事件",
                            "请选择要监听的事件（新建待办 / 完成待办 / 保存笔记）");
        } else {
            TriggerConfig cfg;
            cfg.events = events;
            std::string encoded;
            const std::string problem = encodeAndVerify(cfg, "event", encoded);
            if (!problem.empty()) {
                degradeToManual("事件触发配置无效：" + problem,
                                "请选择要监听的事件（新建待办 / 完成待办 / 保存笔记）");
            } else {
                accept("event", encoded);
            }
        }
    }

    return core::Result<TaskDraftResult>::ok(std::move(out));
}

} // namespace module::agent