// 系统通知工具 "app.notify"：把一条通知交给 platform::notify facade 并把 Receipt
// 状态映射为回喂 LLM 的 JSON（spec §7.4）。参数校验在这里；清洗/截断/去重/限速/
// 授权全在平台层 facade，工具层不复制任何一份。
//
// 输出契约（isError 全部为 false，除参数错误与降级也失败）：
//   * 受理：{"status":"accepted","deliveredVia":"system","id":"...","truncated":bool}
//   * 去重：{"status":"deduped","deliveredVia":"none","id":"..."}
//   * 限速：{"status":"rate_limited","deliveredVia":"none","retryAfterMs":N}
//   * 环境失败 → 降级应用内提示：
//     {"status":"fallback","deliveredVia":"app_toast","reason":"...","detail":"...","id":"..."}
//   * 参数错误 / 降级也失败：isError=true + {"error":"<中文>"}
// Result::fail 只留给编程不变量破坏（自有字面量序列化失败）。
//
// 线程契约：execute() 在 AgentLoop 工作线程执行；注入的 fallback 也在**同一工作线程**
// 被调用。fallback 实现只能做线程安全入队，不得直接触碰 UI。
#pragma once

#include <functional>
#include <string>

#include "core/tool.h"

namespace module::agent {

class NotifyTool final : public core::ITool {
public:
    // 系统通知不可用（NotBundled/PermissionDenied/NotDetermined/BackendUnavailable）
    // 时的应用内降级回调：收到 "<title>：<body>" 文案。
    // 空 = 不降级（失败时直接向 LLM 回喂错误）。
    using FallbackFn = std::function<void(std::string)>;

    explicit NotifyTool(FallbackFn fallback = {});

    std::string name() const override;
    std::string schemaJson() const override;
    core::Result<core::ToolResult> execute(const core::ToolArgs& args) override;

private:
    FallbackFn fallback_;
};

} // namespace module::agent
