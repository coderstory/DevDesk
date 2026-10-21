// AgentLoop 主循环：消息累积 → LLM → tool_use 执行 → 回喂 → 循环 → 落库。
// 不抛异常，全程 Result 传递。
#include "module_agent/agent_loop.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/logging.h"
#include "core/time.h"
#include "module_agent/agent_store.h"
#include "module_agent/message_blocks.h"
#include "platform/json.h"
#include "platform/utf8.h"

namespace module::agent {

std::string currentTimePromptLine(std::time_t nowSec) {
    std::tm local{};
    core::time::localTime(nowSec, &local);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "当前本地时间：%04d-%02d-%02d %02d:%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min);
    return std::string(buf);
}

namespace {

// max_iterations 硬上限 50（loop 内 clamp，不只靠 UI 钮制）
constexpr int kMaxIterationsHardCap = 50;

// 硬上限：
constexpr int64_t kTokenHardCap = 200000;
constexpr int64_t kWallClockHardCapMs = 30 * 60 * 1000;
constexpr int64_t kRequestTimeoutMs = 60 * 1000;

int64_t defaultNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 单条 tool_result text 硬上限 32KB（防长笔记+多迭代烧爆上下文致上游 400），按字节截断
constexpr size_t kMaxToolResultBytes = 32 * 1024;
const char* kTruncatedMarker = "\n[truncated]";

// platform::json 无对象数组 push 口径：每块 stringify 后逗号连接，parse 自检失败当场 abort
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

std::string strField(const platform::json::JsonObject& obj, const char* key) {
    platform::json::JsonValue v = obj.get(key);
    return v.isString() ? v.asString() : std::string();
}

// 工具 contentJson → tool_result text：字符串原样、其他形状 stringify、解析失败原文。
// 先 UTF-8 归一（非法字节 stringify 放行、parse 拒绝 → abort），再按 kMaxToolResultBytes 截断
std::string toolResultText(const std::string& contentJson) {
    std::string text;
    if (!contentJson.empty()) {
        auto cv = platform::json::parse(contentJson);
        if (cv.isOk() && cv.value().isString()) {
            text = cv.value().asString();
        } else if (cv.isOk()) {
            auto s = platform::json::stringify(cv.value());
            if (s.isFail()) {
                LOG_FATAL("AgentLoop: 序列化 tool_result 文本失败：" + s.errorMessage());
                std::abort();
            }
            text = s.value();
        } else {
            text = contentJson;
        }
    }
    if (!platform::utf8::isValid(text)) {
        LOG_WARN("AgentLoop: 工具文本含非法 UTF-8 字节，已替换为 U+FFFD 后回喂（" +
                 std::to_string(text.size()) + " 字节）");
        text = platform::utf8::sanitize(text);
    }
    if (text.size() > kMaxToolResultBytes) {
        // 切点可能落在多字节字符中间 → 回退到字符边界，否则拼出半个字（上游 400）
        size_t cut = kMaxToolResultBytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        text = text.substr(0, cut) + kTruncatedMarker;
    }
    return text;
}

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

// store 行 → AgentTask 的唯一映射（缺字段 = 参数悄悄退回默认值）；trigger/enabled 不进执行体
AgentTask taskFromRow(const AgentTaskRow& row) {
    AgentTask t;
    t.id = row.id;
    t.name = row.name;
    t.goal = row.goal;
    t.modelSlot = row.modelSlot;
    t.maxIterations = row.maxIterations;
    t.allowDestructive = row.allowDestructive;
    t.contextNote = row.contextNote;
    t.autoNote = (row.autoNote != 0);
    t.systemPrompt = row.systemPrompt;
    return t;
}

AgentLoop::AgentLoop(core::LlmEndpoint& llm, core::ToolRegistry& tools,
                     AgentStore& store, NowMsFn nowMs,
                     FinishFailureFn onFinishFailure)
    : llm_(llm), tools_(tools), store_(store), nowMs_(std::move(nowMs)),
      onFinishFailure_(std::move(onFinishFailure)) {}

Result<int64_t> AgentLoop::run(const AgentTask& task,
                               std::atomic<bool>& cancelToken) {
    // startRunExclusive = 全仓唯一「同 task 不并发」闸点（手动与 Scheduler/cron 都经此）；失败原样上抛
    auto started = store_.startRunExclusive(task.id);
    if (started.isFail()) return started;
    const int64_t runId = started.value();

    std::vector<core::LlmMessage> messages;   // 全量历史（Anthropic 无自动历史）
    int64_t seq = 0;
    int iterations = 0;
    int64_t inTotal = 0, outTotal = 0;
    std::string status = "completed";
    std::optional<std::string> error;
    bool fatal = false;

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
    auto keepAssistant = [&](const core::LlmResponse& r) -> bool {
        return keep("assistant", r.contentJson, std::nullopt);
    };

    // 首条 user 消息 = 目标文本（json::stringify 拿带引号+转义的字面量，不手工拼串）
    auto goalText = platform::json::stringify(platform::json::makeString(task.goal));
    if (goalText.isFail()) {
        // 目标序列化失败就别发请求（空 messages 必被上游拒）
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
        // cancel 检查之后、迭代上限之前——用户取消永远优先（不被"超限"吞成 limited）
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
        // system prompt：用户自定义 > 自动生成（带当前本地时间 + 可选的上下文记忆）。
        // 用户填写了 systemPrompt → 直接使用，不追加自动内容（由用户自己决定要什么）。
        // 未填写 → 自动生成：时间行 + 可选上下文记忆（既往行为不变）。
        std::string systemPrompt;
        if (!task.systemPrompt.empty()) {
            systemPrompt = task.systemPrompt;
        } else {
            systemPrompt = currentTimePromptLine(std::time(nullptr));
            if (task.autoNote && !task.contextNote.empty()) {
                systemPrompt += "\n\n[任务上下文记忆]\n" + task.contextNote;
            }
        }
        // 槽位→真实 model id 由端点实现解析
        auto resp = llm_.send(messages, toolsJson, systemPrompt, task.modelSlot);
        if (resp.isFail()) {
            status = "failed";
            error = resp.errorMessage();
            break;
        }
        const core::LlmResponse& r = resp.value();
        if (r.inputTokens > 0) inTotal += r.inputTokens;
        if (r.outputTokens > 0) outTotal += r.outputTokens;
        ++iterations;
        if (!keepAssistant(r)) break;
        // token 检查点在 keepAssistant 之后：破限轮的用量照实落库，落库完立即判停
        if (inTotal + outTotal > kTokenHardCap) {
            status = "limited";
            error = "token 累计超上限 200K（input " + std::to_string(inTotal) +
                    " + output " + std::to_string(outTotal) + "）";
            break;
        }
        if (r.stopReason != "tool_use") {
            // completed 唯一定义是 end_turn，其余 stop_reason 显式分类，不留「未知即成功」
            if (r.stopReason == "end_turn") {
                status = "completed";
            } else if (r.stopReason == "max_tokens" ||
                       r.stopReason == "stop_sequence") {
                status = "limited";
                error = "stop_reason=" + r.stopReason;
            } else {
                status = "failed";
                error = "stop_reason=" + r.stopReason;
            }
            break;
        }

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
            // input 子树以文本传 ToolArgs（工具实现方自行解析）
            std::string inputText = "{}";
            platform::json::JsonValue input = block.get("input");
            if (!input.isNull()) {
                auto inputStr = platform::json::stringify(input);
                if (inputStr.isOk()) inputText = inputStr.value();
            }

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
                    // 失败理论不可达，仍不静默
                    LOG_FATAL("AgentLoop: 序列化 tool_result 失败：" + text.errorMessage());
                    std::abort();
                }
                return text.value();
            };

            auto found = tools_.find(name);
            if (found.isFail()) {
                // 未找到（模块禁用/已注销）→ is_error 回喂让 LLM 自纠，不中止 run
                resultTexts.push_back(makeResult(
                    makeTextContent("工具 " + name + " 未找到，可能模块已禁用"),
                    true));
                resultIds.push_back(id);
                continue;
            }
            // 破坏性未授权：先拒后不调——工具不执行、run 直接 failed，不挂起等确认
            if (found.value()->isDestructive() && !task.allowDestructive) {
                unauthorized = true;
                status = "failed";
                error = "破坏性操作 " + name + " 未授权（需 allow_destructive）";
                break;
            }
            core::ToolResult result;
            auto exec = found.value()->execute(core::ToolArgs{inputText});
            if (exec.isFail()) {
                // 工具异常回喂不中止：is_error + 原因
                result.isError = true;
                result.contentJson = exec.errorMessage();
            } else {
                result = exec.value();
            }
            // Anthropic 工具协议要求 content 为字符串或 blocks 数组（裸对象会 400）
            resultTexts.push_back(
                makeResult(makeTextContent(toolResultText(result.contentJson)),
                           result.isError));
            resultIds.push_back(id);
        }

        if (unauthorized) break;
        if (resultTexts.empty()) {
            // tool_use 却无可回喂块 = 上游响应自相矛盾；发空数组只会 400，标失败说清楚
            status = "failed";
            error = "stop_reason=tool_use 但响应里没有 tool_use block";
            break;
        }
        // 多个 tool_result 合一条 user 消息（须紧跟 assistant）；多块时 tool_call_id 留 NULL，记首个会误导
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
        // 收尾写不回 → run 永停 'running'，UI 轮询判停永不满足；必须回报调用方记孤儿
        LOG_ERROR("AgentLoop: 收尾 finishRun 失败（run " + std::to_string(runId) +
                  " 将留在 running）：" + finished.errorMessage());
        if (onFinishFailure_) {
            onFinishFailure_(runId, finished.errorMessage());
        }
    } else if (status == "completed" && task.autoNote) {
        // 自动 append 最终回复到 context note：取内存 messages 最后一条 assistant 的 text 块，
        // 截断 500 字（1500 字节），加时间戳前缀；append 失败只 WARN 不改变 run 终态
        const core::LlmMessage* lastAssistant = nullptr;
        for (const auto& m : messages) {
            if (m.role == "assistant") lastAssistant = &m;
        }
        if (lastAssistant != nullptr) {
            auto blocks = decodeMessageBlocks(lastAssistant->contentJson);
            std::string finalText;
            for (const auto& b : blocks) {
                if (b.type != MessageBlock::Type::Text) continue;
                if (!finalText.empty()) finalText += "\n";
                finalText += b.text;
            }
            if (!finalText.empty()) {
                finalText = utf8Truncate(finalText, 1500);  // 500 汉字 ≈ 1500 UTF-8 字节
                std::time_t now = std::time(nullptr);
                std::tm local{};
                core::time::localTime(now, &local);
                char ts[20];
                std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d",
                              local.tm_year + 1900, local.tm_mon + 1,
                              local.tm_mday, local.tm_hour, local.tm_min);
                std::string note = std::string("[") + ts + "] " + finalText;
                auto appended = store_.appendContextNote(task.id, note);
                if (appended.isFail()) {
                    LOG_WARN("AgentLoop: 自动 append context note 失败（run " +
                             std::to_string(runId) + "）：" + appended.errorMessage());
                }
            }
        }
    }
    return Result<int64_t>::ok(runId);
}

} // namespace module::agent
