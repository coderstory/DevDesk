// src/module_memory/hook/hook_command.cpp
#include "hook_command.h"

#include <iostream>

#include "core/str_util.h"
#include "module_memory/hook/inject.h"
#include "platform/json.h"

namespace module::memory::hook {

namespace {

using platform::json::JsonValue;

constexpr std::size_t kMaxQueryChars = 4000;  // 查询侧截断（同采集单事件上限口径）
constexpr int kSearchLimit = 8;  // 多取再由预算裁到 ≤5 条（裁掉的仍在库）

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
        prompt = prompt.substr(0, kMaxQueryChars);
    }

    auto hits = retriever->search(prompt, kSearchLimit, kHookSearchTimeoutMs);
    if (hits.isFail()) return bail();  // 引擎不在/超时：失败开放
    if (usage) {
        usage("hook_search",
              static_cast<long long>(countChars(prompt)));
    }
    const std::string context = assembleContext(hits.value());
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
