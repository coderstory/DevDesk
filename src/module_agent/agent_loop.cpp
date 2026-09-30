// src/module_agent/agent_loop.cpp
// AgentLoop 主循环（agent M1 Task 4）：消息累积 → 调 LlmEndpoint → 解析
// tool_use → 经 ToolRegistry 执行 → 拼 tool_result 回喂 → 循环 → 落库。
// 状态语义见 spec §5.2（completed/failed/limited/cancelled）；不抛异常，
// 全程 Result 传递。
#include "module_agent/agent_loop.h"

#include <algorithm>
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

// tool_result blocks 数组文本。platform::json 只暴露 makeArray() 空桩 +
// makeStringArray()（字符串专用），**没有对象数组的 push 口径**（json.h:107-109），
// 故这里按「每块 stringify 后用逗号连接」拼出数组文本（每个元素本身是
// stringify(合法 JsonValue)，由构造保证元素合法），再 parse 一次自检：
// 拼装层出错当场 LOG_FATAL 终止，绝不把坏 body 发给上游。
std::string joinBlockArray(const std::vector<std::string>& blockTexts) {
    std::string text = "[";
    for (size_t i = 0; i < blockTexts.size(); ++i) {
        if (i > 0) text += ",";
        text += blockTexts[i];
    }
    text += "]";
    auto check = platform::json::parse(text);
    if (check.isFail() || !check.value().isArray()) {
        LOG_FATAL("AgentLoop: 拼出的 content blocks 数组非法（内部构造错误）：" + text);
        std::abort();
    }
    return text;
}

// 读 content block 的字符串字段；非字符串 / 缺失 → 空串（调用方按空串处理）
std::string strField(const platform::json::JsonObject& obj, const char* key) {
    platform::json::JsonValue v = obj.get(key);
    return v.isString() ? v.asString() : std::string();
}

} // namespace

AgentLoop::AgentLoop(core::LlmEndpoint& llm, core::ToolRegistry& tools,
                     AgentStore& store)
    : llm_(llm), tools_(tools), store_(store) {}

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

    while (!fatal) {
        if (cancelToken.load()) {
            status = "cancelled";
            error = "用户取消";
            break;
        }
        if (iterations >= effectiveMax) {
            status = "limited";
            error = "达到迭代上限 " + std::to_string(iterations);
            break;
        }
        // （Task 5 补：墙钟超时判定位）
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
        if (r.stopReason != "tool_use") {
            if (r.stopReason == "max_tokens" || r.stopReason == "stop_sequence") {
                status = "limited";            // 截断/受限（L1）
                error = "stop_reason=" + r.stopReason;
            }
            break;                             // end_turn（或上面 limited）
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
                // **不中止 run**
                resultTexts.push_back(makeResult(
                    platform::json::makeString("工具 " + name +
                                               " 未找到，可能模块已禁用"),
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
            // content 用工具返回的 contentJson **解析后的值**（LLM 侧拿到结构化
            // 结果而非一坨转义串）；解析失败退回原文串
            auto contentVal = platform::json::parse(result.contentJson);
            platform::json::JsonValue contentValue =
                contentVal.isOk() ? contentVal.value()
                                  : platform::json::makeString(result.contentJson);
            // （Task 5 补：>32KB 截断 + [truncated] 标记）
            resultTexts.push_back(makeResult(contentValue, result.isError));
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
