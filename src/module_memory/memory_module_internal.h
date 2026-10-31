// memory_module_internal.h
// memory_module.cpp 三拆后的共享面（2026-10-31）：匿名命名空间里的度量常量与
// 绘制小工具，提为 module::memory::detail 供 panels 文件共用。类成员函数不走这里
// ——MemoryModule 的声明在 memory_module.h，本身就是跨 TU 契约。
#pragma once

// 偏差处（预算环→双横条、来源枚举对齐数据层）在控件旁注明口径。
#include "memory_module.h"
#include <utility>

#include <components/input.h>
#include <components/scrollview.h>
#include <eui/dsl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "core/component_theme.h"
#include "core/llm_profile_source.h"
#include "core/logging.h"
#include "core/service_locator.h"
#include "core/theme.h"
#include "engine/telemem_config.h"
#include "engine/telemem_engine.h"
#include "engine/token_usage.h"
#include "module_memory/proposals/usage_ledger.h"
#include "ui_kit/button.h"
#include "ui_kit/dialog_form.h"
#include "ui_kit/metrics.h"
#include "ui_kit/slider.h"
#include "ui_kit/wtab.h"
#include "ui_kit/wswitch.h"

namespace module::memory {

using engine::TelememEngine;
using proposals::Proposal;

inline constexpr float kTitleH = 32.0f;
inline constexpr float kTabH = 36.0f;
inline constexpr float kCardGap = 16.0f;

inline constexpr int kViewConsole = 0;
inline constexpr int kViewReview = 1;
inline constexpr int kViewConfig = 2;

// ── 文案/取值映射 ──

inline const char* engineStateText(TelememEngine::State s) {
    switch (s) {
        case TelememEngine::State::Idle: return "未启动";
        case TelememEngine::State::Preparing: return "准备中";
        case TelememEngine::State::Running: return "运行中";
        case TelememEngine::State::Stopped: return "已停止";
        case TelememEngine::State::Failed: return "故障";
    }
    return "?";
}

inline eui::Color engineStateColor(const core::theme::Tokens& t,
                            TelememEngine::State s) {
    switch (s) {
        case TelememEngine::State::Running: return t.colors.success;
        case TelememEngine::State::Preparing: return t.colors.warning;
        case TelememEngine::State::Failed: return t.colors.errorText;
        default: return t.colors.textTertiary;
    }
}

inline std::string kindLabel(const std::string& kind) {
    if (kind == "add") return "新增";
    if (kind == "forget") return "遗忘";
    if (kind == "invalidate") return "失效";
    if (kind == "merge") return "合并";
    return kind;
}

inline eui::Color kindColor(const core::theme::Tokens& t, const std::string& kind) {
    if (kind == "add") return t.colors.success;
    if (kind == "forget") return t.colors.textTertiary;
    if (kind == "invalidate") return t.colors.warning;
    if (kind == "merge") return t.colors.info;
    return t.colors.textSecondary;
}

inline std::string confidenceTier(double c) {
    if (c >= 0.8) return "高";
    if (c >= 0.5) return "中";
    return "低";
}

inline std::string srcLabel(const std::string& source) {
    if (source == "collect") return "Claude 对话";
    if (source == "decay") return "遗忘扫描";
    if (source == "dedupe" || source == "conflict") return "整理";
    return source;
}

// 来源 chip 过滤键（数据层 source 枚举对齐口径）
inline bool srcMatches(int filter, const std::string& source) {
    switch (filter) {
        case 0: return true;
        case 1: return source == "collect";
        case 2: return source == "dedupe" || source == "conflict";
        case 3: return source == "decay";
        default: return true;
    }
}

inline bool kindMatches(int filter, const std::string& kind) {
    switch (filter) {
        case 0: return true;
        case 1: return kind == "merge";
        case 2: return kind == "invalidate";
        case 3: return kind == "forget";
        case 4: return kind == "add";
        default: return true;
    }
}

// 批量接受仅对「遗忘 / 新增」开放（设计稿契约；合并/失效须逐张处理）
inline bool batchableKind(const std::string& kind) {
    return kind == "forget" || kind == "add";
}

inline std::string formatInt(long long n) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", n);
    std::string s = buf;
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
        s.insert(static_cast<std::size_t>(i), ",");
    }
    return s;
}

inline std::string formatBytes(std::int64_t bytes) {
    if (bytes < 0) return "统计中…";
    char buf[32];
    if (bytes >= 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f KB",
                      static_cast<double>(bytes) / 1024.0);
    }
    return buf;
}

inline std::string formatMilliYuan(long long milli) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld.%03lld 元", milli / 1000,
                  milli % 1000);
    return buf;
}

// Primary 按钮在滚动画布内的文字补丁：EUI 渲染端对「stack 内 states 矩形 +
// 白字」的组合会把文字盖回底色下（定位实验见 2026-10-28 探针； Secondary /
// Danger 不触发）。文字改为画布层后置兄弟节点，保证浮于底色之上；渲染端
// 修复后应删掉本包装恢复直调 ui_kit::button。
inline void primaryButton(eui::Ui& ui, const std::string& id, const std::string& label,
                   float x, float y, float w, float h,
                   const core::theme::Tokens& t, std::function<void()> onClick) {
    ui_kit::button(ui, id, "", ui_kit::ButtonLevel::Primary, x, y, w, h, t,
                   {std::move(onClick)});
    ui.text(id + ".label")
        .position(x, y)
        .size(w, h)
        .text(label)
        .fontSize(core::theme::scaled(ui_kit::kButtonFontSize))
        .lineHeight(h)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.onPrimary)
        .build();
}

} // namespace module::memory
