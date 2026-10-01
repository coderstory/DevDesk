// src/module_agent/agent_loop.cpp
// AgentLoop 主循环（agent M1 Task 4）：消息累积 → 调 LlmEndpoint → 解析
// tool_use → 经 ToolRegistry 执行 → 拼 tool_result 回喂 → 循环 → 落库。
// 状态语义见 spec §5.2（completed/failed/limited/cancelled）；不抛异常，
// 全程 Result 传递。
#include "module_agent/agent_loop.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/logging.h"
#include "module_agent/agent_store.h"
#include "platform/json.h"

namespace module::agent {
namespace {

// spec §5.2：max_iterations 硬上限 50（clamp 在 loop 内，不只 UI 钮制——L3b）
constexpr int kMaxIterationsHardCap = 50;

// spec §5.2（M2 强制，原 M1 实况标注「刻意留空」由此接棒）：
constexpr int64_t kTokenHardCap = 200000;               // token 累计硬上限
constexpr int64_t kWallClockHardCapMs = 30 * 60 * 1000; // 单 run 墙钟硬上限 30 分钟
constexpr int64_t kRequestTimeoutMs = 60 * 1000;        // 请求超时默认（spec §5.2 表）

int64_t defaultNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// spec §10 L3c：单条 tool_result 的 text 硬上限 32KB，超限截断 + 末尾
// [truncated] 标记（防 md.read 长笔记 + 多迭代烧爆上下文致上游 400）。
// 截断按**字节**算（上游按字节收），故纯 ASCII 恰好 32KB；标记本身不占
// 32KB 配额（超了才有标记，标记是给模型看的额外提示）。
constexpr size_t kMaxToolResultBytes = 32 * 1024;
const char* kTruncatedMarker = "\n[truncated]";

// tool_result blocks 数组文本。platform::json 只暴露 makeArray() 空桩 +
// makeStringArray()（字符串专用），**没有对象数组的 push 口径**（json.h:107-109），
// 故这里按「每块 stringify 后用逗号连接」拼出数组文本（每个元素本身是
// stringify(合法 JsonValue)，由构造保证元素合法），再 parse 一次自检：
// 拼装层出错当场 LOG_FATAL 终止，绝不把坏 body 发给上游。
platform::json::JsonValue parseArrayOrDie(const std::string& text,
                                          const char* what) {
    auto v = platform::json::parse(text);
    if (v.isFail() || !v.value().isArray()) {
        LOG_FATAL(std::string("AgentLoop: ") + what +
                  " 不是合法 blocks 数组（内部构造错误）：" + text);
        std::abort();
    }
    return std::move(v.value());
}

std::string joinBlockArray(const std::vector<std::string>& blockTexts) {
    std::string text = "[";
    for (size_t i = 0; i < blockTexts.size(); ++i) {
        if (i > 0) text += ",";
        text += blockTexts[i];
    }
    text += "]";
    parseArrayOrDie(text, "content blocks");
    return text;
}

// 读 content block 的字符串字段；非字符串 / 缺失 → 空串（调用方按空串处理）
std::string strField(const platform::json::JsonObject& obj, const char* key) {
    platform::json::JsonValue v = obj.get(key);
    return v.isString() ? v.asString() : std::string();
}

// 工具返回的 contentJson → tool_result 的 text 块文本。
// 形状规则（controller 裁定 2026-09-30）：
//   - contentJson 解析成 JSON **字符串** → 用该字符串的值**原样**当文本
//     （工具回 "done" 模型就该看到 done，而不是带引号的 "done"；回 "" 则是
//     真空串，不是 "\"\""——M2 工具里既有回对象的也有回纯文本的）
//   - 其他形状（对象/数组/数字/布尔）→ stringify(值)（对象是 M2 主流形状）
//   - 解析失败（非 JSON 原文，如工具异常消息）→ 原文直接当文本，不再套引号
//   - 空 contentJson → 空串（parse("") 返回 ok(null)，不特判就回喂 "null"）
// 之后再按 kMaxToolResultBytes 截断。
std::string toolResultText(const std::string& contentJson) {
    std::string text;
    if (!contentJson.empty()) {
        auto cv = platform::json::parse(contentJson);
        if (cv.isOk() && cv.value().isString()) {
            text = cv.value().asString();               // 原样，不二次 stringify
        } else if (cv.isOk()) {
            auto s = platform::json::stringify(cv.value());
            if (s.isFail()) {
                LOG_FATAL("AgentLoop: 序列化 tool_result 文本失败：" + s.errorMessage());
                std::abort();
            }
            text = s.value();
        } else {
            text = contentJson;                          // 非 JSON 原文，原样回喂
        }
    }
    if (text.size() > kMaxToolResultBytes) {
        // 按字节截断：切点可能落在多字节 UTF-8 字符中间 → 回退到字符边界，
        // 否则拼出的 JSON 里会出现半个字符（非法 UTF-8，上游 400）
        size_t cut = kMaxToolResultBytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        text = text.substr(0, cut) + kTruncatedMarker;
    }
    return text;
}

// 造一个 tool_result 的 content 数组：[{"type":"text","text":<text>}]
platform::json::JsonValue makeTextContent(const std::string& text) {
    platform::json::JsonObject textBlock;
    textBlock.set("type", platform::json::makeString("text"));
    textBlock.set("text", platform::json::makeString(text));
    auto textBlockText = platform::json::stringify(
        platform::json::JsonValue(std::move(textBlock)));
    if (textBlockText.isFail()) {
        LOG_FATAL("AgentLoop: 序列化 tool_result content block 失败：" +
                  textBlockText.errorMessage());
        std::abort();
    }
    return parseArrayOrDie("[" + textBlockText.value() + "]", "tool_result.content");
}

} // namespace

AgentLoop::AgentLoop(core::LlmEndpoint& llm, core::ToolRegistry& tools,
                     AgentStore& store, NowMsFn nowMs)
    : llm_(llm), tools_(tools), store_(store), nowMs_(std::move(nowMs)) {}

Result<int64_t> AgentLoop::run(const AgentTask& task,
                               std::atomic<bool>& cancelToken) {
    // 起 run 行（started_at 由 store 记）。这一步失败 = 库里连 run 都没有，
    // 无处标状态，直接把错误上抛给调用方。
    auto started = store_.startRun(task.id);
    if (started.isFail()) return started;
    const int64_t runId = started.value();

    std::vector<core::LlmMessage> messages;   // 全量历史（Anthropic 无自动历史）
    int64_t seq = 0;                          // agent_messages.seq 累进
    int iterations = 0;
    int64_t inTotal = 0, outTotal = 0;
    std::string status = "completed";
    std::optional<std::string> error;
    bool fatal = false;                       // 落库失败 = 不可恢复，直接收尾

    // 落库一条消息并追加进累积历史（role=content 原文）。
    // 落库失败不当成功：置 fatal（跳出循环）+ status/error，让 run 以 failed
    // 收尾（不吞错，也不把「消息没写进库」的一次 run 记成 completed）。
    auto keep = [&](const std::string& role, const std::string& content,
                    const std::optional<std::string>& toolCallId) -> bool {
        auto w = store_.appendMessage(runId, seq++, role, content, toolCallId);
        if (w.isFail()) {
            fatal = true;
            status = "failed";
            error = "落库 agent_messages 失败：" + w.errorMessage();
            return false;
        }
        messages.push_back({role, content});
        return true;
    };
    // assistant 响应不进任何前置（它就是本轮新消息），落库 + 追加即可
    auto keepAssistant = [&](const core::LlmResponse& r) -> bool {
        return keep("assistant", r.contentJson, std::nullopt);
    };

    // 首条 user 消息 = 目标文本，包成 content blocks 数组原文
    // （goal 用 json::stringify(makeString()) 拿到带引号+转义的 JSON 字符串字面量，
    //  直接拼进 blocks 数组——引号/换行/反斜杠由 json 层转义，不手工拼串）。
    auto goalText = platform::json::stringify(platform::json::makeString(task.goal));
    if (goalText.isFail()) {
        // 目标都没序列化出来就别发请求（空 messages 上去必被上游拒）
        fatal = true;
        status = "failed";
        error = "序列化任务目标失败：" + goalText.errorMessage();
    } else {
        keep("user",
             "[{\"type\":\"text\",\"text\":" + goalText.value() + "}]",
             std::nullopt);
    }

    const std::string toolsJson = tools_.allSchemasJson();
    const int effectiveMax = std::min(task.maxIterations, kMaxIterationsHardCap);
    // spec §5.2：单 run 墙钟预算 = min(max_iterations × 请求超时(60s), 30 分钟)。
    // startMs 取进循环前的时刻；判定只在循环顶部做（见下），不在请求中途打断。
    const int64_t startMs = nowMs_ ? nowMs_() : defaultNowMs();
    const int64_t wallBudgetMs =
        std::min<int64_t>(static_cast<int64_t>(effectiveMax) * kRequestTimeoutMs,
                          kWallClockHardCapMs);

    while (!fatal) {
        if (cancelToken.load()) {
            status = "cancelled";
            error = "用户取消";
            break;
        }
        // spec §5.2 墙钟上限（M2）：cancel 检查之后、迭代上限之前——用户取消
        // 永远优先（取消不能被"超限"吞成 limited），且超限轮不再发请求。
        if ((nowMs_ ? nowMs_() : defaultNowMs()) - startMs >= wallBudgetMs) {
            status = "limited";
            error = "达到墙钟上限 " + std::to_string(wallBudgetMs / 1000) + "s";
            break;
        }
        if (iterations >= effectiveMax) {
            status = "limited";
            error = "达到迭代上限 " + std::to_string(iterations);
            break;
        }
        // model 原样传 task.modelSlot：槽位→真实 model id 由 LlmClient 解析（H1）
        auto resp = llm_.send(messages, toolsJson, std::string(), task.modelSlot);
        if (resp.isFail()) {
            status = "failed";
            error = resp.errorMessage();
            break;
        }
        const core::LlmResponse& r = resp.value();
        if (r.inputTokens > 0) inTotal += r.inputTokens;
        if (r.outputTokens > 0) outTotal += r.outputTokens;
        ++iterations;
        if (!keepAssistant(r)) break;          // 落库失败 → fatal
        // spec §5.2 token 上限（M2）：检查点在 keepAssistant **之后**——破限轮的
        // assistant 消息与用量照实落库（spec §10 审计），落库完立即判停，不再
        // 执行本轮工具、不再发下一轮。
        if (inTotal + outTotal > kTokenHardCap) {
            status = "limited";
            error = "token 累计超上限 200K（input " + std::to_string(inTotal) +
                    " + output " + std::to_string(outTotal) + "）";
            break;
        }
        if (r.stopReason != "tool_use") {
            // completed 的**唯一**定义是 end_turn（spec §5.2）。其余 stop_reason
            // 逐个显式分类，不留「未知即成功」的开口：refusal / pause_turn
            // （Anthropic）、stop / length（兼容网关）、以及默认空串（上游没给）
            // 都是「没拿到模型承诺的终局回复」→ failed，不冒充 completed。
            if (r.stopReason == "end_turn") {
                status = "completed";
            } else if (r.stopReason == "max_tokens" ||
                       r.stopReason == "stop_sequence") {
                status = "limited";            // 截断/受限（L1）
                error = "stop_reason=" + r.stopReason;
            } else {
                status = "failed";
                error = "stop_reason=" + r.stopReason;
            }
            break;
        }

        // ---- 解析 tool_use blocks → 执行 → tool_result ----
        auto parsed = platform::json::parse(r.contentJson);
        if (parsed.isFail() || !parsed.value().isArray()) {
            status = "failed";
            error = parsed.isFail()
                        ? "解析响应 content 失败：" + parsed.errorMessage()
                        : "stop_reason=tool_use 但 content 不是 blocks 数组";
            break;
        }
        const platform::json::JsonValue blocks = parsed.value();

        std::vector<std::string> resultTexts;      // 每个 tool_result block 的 JSON 原文
        std::vector<std::string> resultIds;        // 对应 tool_use_id（落 tool_call_id）
        bool unauthorized = false;                 // 破坏性未授权 → 终止 run

        for (int i = 0; i < blocks.size(); ++i) {
            platform::json::JsonValue blockValue = blocks[i];
            if (!blockValue.isObject()) continue;
            platform::json::JsonObject block = blockValue.asObject();
            if (strField(block, "type") != "tool_use") continue;

            const std::string id = strField(block, "id");
            const std::string name = strField(block, "name");
            // input 子树以**文本**传 ToolArgs（工具实现方自己用 platform::json 解析）
            std::string inputText = "{}";
            platform::json::JsonValue input = block.get("input");
            if (!input.isNull()) {
                auto inputStr = platform::json::stringify(input);
                if (inputStr.isOk()) inputText = inputStr.value();
            }

            // 造一个 tool_result block 文本
            auto makeResult = [&](const platform::json::JsonValue& content,
                                  bool isError) -> std::string {
                platform::json::JsonObject obj;
                obj.set("type", platform::json::makeString("tool_result"));
                obj.set("tool_use_id", platform::json::makeString(id));
                obj.set("content", content);
                obj.set("is_error", platform::json::makeBool(isError));
                auto text = platform::json::stringify(
                    platform::json::JsonValue(std::move(obj)));
                if (text.isFail()) {
                    // 全字符串 + 一个已解析值的序列化，失败理论不可达；仍不静默
                    LOG_FATAL("AgentLoop: 序列化 tool_result 失败：" + text.errorMessage());
                    std::abort();
                }
                return text.value();
            };

            auto found = tools_.find(name);
            if (found.isFail()) {
                // L3a：模块被禁用 / 工具已注销 → is_error 回喂让 LLM 自纠，
                // **不中止 run**。与正常回喂同一形状（text block 数组），
                // 免得模型收到两种 content 形状（裁定 2026-09-30）
                resultTexts.push_back(makeResult(
                    makeTextContent("工具 " + name + " 未找到，可能模块已禁用"),
                    true));
                resultIds.push_back(id);
                continue;
            }
            // 破坏性授权（spec §5.3）：先拒后不调——未授权时工具不执行，
            // run 直接 failed，不挂起等确认（后台自动化无对话窗）
            if (found.value()->isDestructive() && !task.allowDestructive) {
                unauthorized = true;
                status = "failed";
                error = "破坏性操作 " + name + " 未授权（需 allow_destructive）";
                break;
            }
            core::ToolResult result;
            auto exec = found.value()->execute(core::ToolArgs{inputText});
            if (exec.isFail()) {
                // 工具异常回喂不中止（spec §10 工具异常语义）：is_error + 原因
                result.isError = true;
                result.contentJson = exec.errorMessage();
            } else {
                result = exec.value();
            }
            // content 按 Anthropic 工具协议只能是「字符串」或「content blocks
            // 数组」——工具返回的裸对象/数字塞进去上游直接 400（而所有 M2 工具
            // 返回的都是对象）。故统一包成 text block 数组：
            //   [{"type":"text","text":<工具返回的文本>}]
            // 文本取法 + 32KB 截断见 toolResultText()（裁定 2026-09-30）。
            resultTexts.push_back(
                makeResult(makeTextContent(toolResultText(result.contentJson)),
                           result.isError));
            resultIds.push_back(id);
        }

        if (unauthorized) break;                // 破坏性未授权：run 终止，不回喂
        if (resultTexts.empty()) {
            // stop_reason=tool_use 却没有可回喂的块 = 上游给了自相矛盾的响应；
            // 发空数组只会被上游 400，不如把 run 标失败说清楚
            status = "failed";
            error = "stop_reason=tool_use 但响应里没有 tool_use block";
            break;
        }
        // 多个 tool_result 合一条 user 消息（Anthropic 要求 tool_result 紧跟
        // 对应 assistant 消息，见 spec §4.3）；tool_call_id 只在**单块**时记
        // （多块时一个 id 代表不了全部，记首个会误导审计，故留 NULL）
        const std::optional<std::string> toolCallId =
            resultIds.size() == 1 ? std::optional<std::string>(resultIds.front())
                                  : std::nullopt;
        keep("user", joinBlockArray(resultTexts), toolCallId);
    }

    auto finished = store_.finishRun(
        runId, status, iterations,
        inTotal > 0 ? std::optional<int64_t>(inTotal) : std::nullopt,
        outTotal > 0 ? std::optional<int64_t>(outTotal) : std::nullopt,
        error);
    if (finished.isFail()) {
        // 收尾写不回：run 会停在 'running'，下次启动被 sweepStaleRuns 标
        // 「悬挂 run」。此处只能留日志——调用方拿到的仍是 runId，无处再报。
        LOG_ERROR("AgentLoop: 收尾 finishRun 失败（run " + std::to_string(runId) +
                  " 将留在 running）：" + finished.errorMessage());
    }
    return Result<int64_t>::ok(runId);
}

} // namespace module::agent
