// 智能体 UI 私有共享头——弹窗布局常量、状态色映射、操作 chip、页面导航、
// cron/runLimit 文案等跨 TU 复用的 helper。inline 零链接成本。
#pragma once

#include <functional>
#include <string>

#include "core/logging.h"
#include "core/router.h"
#include "core/theme.h"
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
// fontSize 缺省 12（任务列表等既有站点不变）；详情弹窗按 spec §9.2 传 fsS，高度仍 32。
// chipH 缺省 32（池162 §3.3：消息卡头行 chips 传 24——同卡控件同几何 48×24）
inline void actionChip(::eui::Ui& ui, const std::string& id, const std::string& label,
                float width, bool primary, bool enabled,
                const components::theme::ThemeColorTokens& tokens,
                std::function<void()> onClick, float fontSize = 12.0f,
                float chipH = 32.0f) {
    const ::core::Color base = primary ? tokens.primary : tokens.surface;
    ::core::Color textColor =
        primary ? ::core::Color{0.94f, 0.97f, 1.0f, 1.0f} : tokens.text;
    if (!enabled) textColor = components::theme::withOpacity(textColor, 0.40f);
    ui.stack(id + ".chip")
        .size(width, chipH)
        .content([&] {
            ui.rect(id + ".bg")
                .size(width, chipH)
                .states(base, components::theme::buttonHover(tokens, base),
                        components::theme::buttonPressed(tokens, base))
                .radius(tokens.metrics.radius.overlay)
                .border(components::theme::buttonBorder(tokens, primary))
                .onClick(onClick)
                .build();
            ui.text(id)
                .size(width, chipH)
                .text(label)
                .fontSize(fontSize)
                .lineHeight(chipH)
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

// 「可用工具」入口钮宽度（池170 v2 §关键决策 3，bug #6）：固定 132 在工具数位数
// 增长（9→12→108）时把图标/文本/计数压叠，改流式自适应纯函数：
//   2×padX + 图标盒 + 6 + 「可用工具」文字 + 8 + 计数 badge（primarySoftBg pill）
// badge 宽 = 左右内边距 8×2 + 位数×字号；文字/位数逐字按 fontSize 保守估
//（CJK 全宽口径，同 kTaskWarnW 注释约定），实渲染更窄 → badge 只会留白不会越界。
// 缩放跟随 core::theme::scaled；scale=1 时 N=3/19/108 → 136/150/164（设计稿验证值）。
// 按钮右缘由页头布局锚定（title 吃剩余宽），位数增长只向左扩。
inline float toolsEntryButtonWidth(int toolCount) {
    const int digits =
        static_cast<int>(std::to_string(toolCount < 0 ? 0 : toolCount).size());
    const float font = core::theme::scaled(14.0f);
    const float iconW = core::theme::scaled(16.0f);  // fa-plug @14 图标盒
    const float textW = 4.0f * font;                 // 「可用工具」4 个 CJK
    const float badgeW =
        2.0f * core::theme::scaled(8.0f) + digits * font;  // pill 内边距 + 逐位
    const float padX = core::theme::scaled(10.0f);
    return 2.0f * padX + iconW + 6.0f + textW + 8.0f + badgeW;
}

} // namespace module::agent