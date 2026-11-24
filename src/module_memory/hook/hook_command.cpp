// src/module_memory/hook/hook_command.cpp
#include "hook_command.h"

#include <iostream>

#include "core/str_util.h"
#include "module_memory/collector/session_reader.h"
#include "module_memory/hook/inject.h"
#include "platform/json.h"

namespace module::memory::hook {

namespace {

using platform::json::JsonValue;

constexpr std::size_t kMaxQueryChars = 4000;  // 查询侧截断（同采集单事件上限口径）
constexpr int kSearchLimit = 8;  // 多取再由预算裁到 ≤5 条（裁掉的仍在库）
// bug415：词面无重合的命中须在更高相似度下限上复检命中才注入。依据=真引擎
// 阈值阶梯实测（evidence/20261123-bug415-e2e）：无关短问「今天天气怎么样」与
// 负例带同分 [0.40,0.42)；近义真提问「改完代码怎么验证」∈[0.48,0.50)；词面
// 重合的命中不走本档（0.4 主门槛原样不动，非一刀切抬阈值）。
constexpr double kHookConfirmThreshold = 0.45;
constexpr int kHookConfirmTimeoutMs = 400;

// 独立门控（不改装配）：任一命中与查询词面重合 → 原样放行；全部不重合 →
// 确认档复检，返回复检结果（空 = 本轮不注入）。复检失败按「宁不注入」处理
//（fail-open 契约是不阻断提问，不是强行注入）。
std::vector<gateway::MemoryHit> gateUnrelatedHits(
    const std::string& query,
    const std::vector<gateway::MemoryHit>& hits,
    gateway::IMemoryRetriever* retriever) {
    if (hits.empty() || retriever == nullptr) return hits;
    for (const auto& hit : hits) {
        if (hasLexicalOverlap(query, hit.text)) return hits;
    }
    auto confirmed = retriever->searchWithThreshold(
        query, kSearchLimit, kHookConfirmTimeoutMs, kHookConfirmThreshold);
    if (confirmed.isFail()) return {};
    return confirmed.value();
}

}  // namespace

HookOutcome runMemoryHook(const std::string& stdinJson,
                          gateway::IMemoryRetriever* retriever,
                          const UsageSink& usage) {
    // fail-open 总闸：任何失败路径直接 {"", 0}——绝不 exit 2
    auto bail = [](HookOutcome o = {}) { return o; };
    if (stdinJson.empty() || retriever == nullptr) return bail();

    const auto parsed = platform::json::parse(stdinJson);
    if (parsed.isFail()) return bail();
    const JsonValue root = parsed.value();
    if (!root.isObject()) return bail();

    // 非 UserPromptSubmit 事件一律不注入（matcher 已过滤，这里双保险）
    const std::string event =
        root.asObject().get("hook_event_name").asString();
    if (!event.empty() && event != "UserPromptSubmit") return bail();

    std::string prompt = root.asObject().get("prompt").asString();
    prompt = core::str::trimAscii(prompt);
    if (prompt.size() < 4) return bail();  // 过短提问没有检索价值，零开销放行
    if (prompt.size() > kMaxQueryChars) {
        // bug255：按 UTF-8 码点边界截断。字节 substr 会劈开多字节字符 → telemem
        // 侧 JSON 解析失败回 400 → 检索 fail → 本 hook 静默零注入（fail-open
        // 掩盖了真因，2026-11-10 实测坐实）。
        prompt = collector::truncateUtf8(prompt, kMaxQueryChars);
    }

    auto hits = retriever->search(prompt, kSearchLimit, kHookSearchTimeoutMs);
    if (hits.isFail()) return bail();  // 引擎不在/超时：失败开放
    const auto gated = gateUnrelatedHits(prompt, hits.value(), retriever);
    if (usage) {
        usage("hook_search",
              static_cast<long long>(countChars(prompt)));
    }
    const std::string context = assembleContext(gated);
    if (context.empty()) return bail();
    const std::string out = buildHookOutputJson(context);
    if (out.empty()) return bail();
    HookOutcome o;
    o.stdoutText = out;
    o.exitCode = 0;
    // 注入记账：条数 = 预算行数；字数 = 预算行字数（不含头部声明，与 ≤500 同口径）
    o.injectedItems = 1;
    for (char c : context) {
        if (c == '\n') ++o.injectedItems;
    }
    o.injectedItems -= 1;  // 首行是头部声明，不是预算行
    {
        const std::size_t headerEnd = context.find('\n');
        o.injectedChars = static_cast<int>(countChars(
            headerEnd == std::string::npos ? std::string()
                                           : context.substr(headerEnd + 1)));
    }
    return o;
}

HookOutcome runMemoryHookFromStdin(gateway::IMemoryRetriever* retriever) {
    std::string input;
    std::string line;
    while (std::getline(std::cin, line)) {
        input += line;
        input += '\n';
    }
    return runMemoryHook(input, retriever);
}

}  // namespace module::memory::hook
