// 智能体 UI 私有共享头——弹窗布局常量、状态色映射、操作 chip、页面导航、
// cron/runLimit 文案等跨 TU 复用的 helper。inline 零链接成本。
#pragma once

#include <functional>
#include <string>

#include "core/logging.h"
#include "core/router.h"
#include "module_agent/agent_store.h"
#include "module_agent/trigger_config.h"

namespace module::agent {

// ---- 弹窗布局常量（任务弹窗与详情弹窗共用） ----

inline constexpr float kDialogGap = 12.0f;
inline constexpr float kDialogLabelW = 72.0f;
inline constexpr float kDialogRowH = 40.0f;

// 详情弹窗展示最近 run 数——列表查询侧（agent_module compose）与详情 UI 侧
//（agent_detail_dialog composeTaskDetailDialog）共用同一上限
inline constexpr int kDetailRunListLimit = 20;

// ---- 共享 UI helper 函数 ----

// 列表状态圆点色：只映射既有 theme token，不新造色。completed=success /
// failed=error / limited=warning / running=info；cancelled 与从未运行共用
// tertiary（中性灰 =「无需行动」态，不与四个语义色抢注意力）。点 + 中文文字
// 并存，不只靠色（可访问性双通道）。
inline eui::Color statusDotColor(const std::string& status,
                                  const ::core::theme::Tokens& t) {
    if (status == "completed") return t.colors.success;
    if (status == "failed") return t.colors.error;
    if (status == "limited") return t.colors.warning;
    if (status == "running") return t.colors.info;
    return t.colors.textTertiary;
}

// 行内动作 chip：契约要求 id 落在可见文本上，故手搓两层——<id>.bg 为命中面、文本穿透。
// fontSize 缺省 12（任务列表等既有站点不变）；详情弹窗按 spec §9.2 传 fsS，高度仍 32
inline void actionChip(::eui::Ui& ui, const std::string& id, const std::string& label,
                float width, bool primary, bool enabled,
                const components::theme::ThemeColorTokens& tokens,
                std::function<void()> onClick, float fontSize = 12.0f) {
    constexpr float kChipH = 32.0f;
    const ::core::Color base = primary ? tokens.primary : tokens.surface;
    ::core::Color textColor =
        primary ? ::core::Color{0.94f, 0.97f, 1.0f, 1.0f} : tokens.text;
    if (!enabled) textColor = components::theme::withOpacity(textColor, 0.40f);
    ui.stack(id + ".chip")
        .size(width, kChipH)
        .content([&] {
            ui.rect(id + ".bg")
                .size(width, kChipH)
                .states(base, components::theme::buttonHover(tokens, base),
                        components::theme::buttonPressed(tokens, base))
                .radius(tokens.metrics.radius.overlay)
                .border(components::theme::buttonBorder(tokens, primary))
                .onClick(onClick)
                .build();
            ui.text(id)
                .size(width, kChipH)
                .text(label)
                .fontSize(fontSize)
                .lineHeight(20.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(textColor)
                .hitTestMode(::core::dsl::HitTestMode::None)
                .onClick(onClick)
                .build();
        })
        .build();
}

// 「去 Claude 页」引导的跳转动作：Router 未装配时仅 LOG_WARN（测试 compose 无 router 不崩）
inline void navigateToClaudePage() {
    auto* router = ::core::appRouter();
    if (router == nullptr) {
        LOG_WARN("Agent: 跳转 Claude 页失败——Router 未装配");
        return;
    }
    router->navigateTo(::core::PageId::ClaudeProxy);
}

// trigger_config 的 cron 原文（只读展示）；解析失败显示空串，但非空配置必须留痕
inline std::string cronTextOf(const AgentTaskRow& task) {
    auto cfg = decodeTriggerConfig(task.triggerConfig);
    if (cfg.isOk()) return cfg.value().cron;
    if (!task.triggerConfig.empty()) {
        LOG_WARN("Agent: 任务 " + std::to_string(task.id) +
                 " 的 trigger_config 无法解析（编辑表单按空 cron 显示）：" +
                 cfg.errorMessage());
    }
    return {};
}

// 限次文案（详情摘要用「大白话」而不是 max_runs 术语）
inline std::string runLimitText(int maxRuns) {
    if (maxRuns <= 1) return "只跑一次";
    return "最多 " + std::to_string(maxRuns) + " 次";
}

} // namespace module::agent