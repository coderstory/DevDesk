// 系统通知工具 "app.notify"：把一条通知交给 platform::notify（macOS UNUserNotificationCenter /
// Windows 托盘 NIF_INFO）。工具层只做参数校验与错误映射，不做限速/去重/UI。
//
// 线程契约：execute() 在 AgentLoop 工作线程执行；系统通知失败时注入的 fallback 也在
// **同一工作线程**被调用。fallback 实现只能做线程安全入队，不得直接触碰 UI。
#pragma once

#include <functional>
#include <string>

#include "core/tool.h"

namespace module::agent {

class NotifyTool final : public core::ITool {
public:
    // 系统通知失败后的应用内降级回调：收到 "<title>：<body>" 文案。
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
