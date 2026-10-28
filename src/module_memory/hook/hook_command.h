// src/module_memory/hook/hook_command.h
// `devdesk.exe memory hook` 子命令主体（S3-C）。stdin JSON 事件 → 检索 → stdout 注入 JSON。
// 钉版契约（hook 契约事实 §一/§三，违反即契约变更）：
//   - additionalContext 只能长在 hookSpecificOutput 内；
//   - 任何内部错误（stdin 非法/检索失败/拼装失败）= 空 stdout + exit 0；
//     本单元任何代码路径都不得产生退出码 2（那是 hook 唯一能打断用户提问的路径）；
//   - 失败开放：引擎不在/超时（硬顶 kHookSearchTimeoutMs）只丢注入，不阻断。
#pragma once

#include <functional>
#include <string>

#include "module_memory/gateway/telemem_client.h"

namespace module::memory::hook {

// 消耗记账缝（D 批接线）：query 字数按 embedding 估算入账；空 = 不记账
using UsageSink = std::function<void(const std::string& kind, long long tokens)>;

struct HookOutcome {
    std::string stdoutText;  // 空串 = 不注入（Claude Code 照常放行提问）
    int exitCode = 0;        // 恒 0；存在意义 = 显式钉死"绝不 exit 2"
};

// 纯编排（检索器注入，测试不起真引擎）。任何异常路径都收敛为 {即空即 0}。
HookOutcome runMemoryHook(const std::string& stdinJson,
                          gateway::IMemoryRetriever* retriever,
                          const UsageSink& usage = {});

// stdin 读全部 → runMemoryHook（main.cpp 子命令分派调用）
HookOutcome runMemoryHookFromStdin(gateway::IMemoryRetriever* retriever);

}  // namespace module::memory::hook
