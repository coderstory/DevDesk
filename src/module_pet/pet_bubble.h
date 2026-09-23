// 气泡对话状态（桌宠设计 §4.5 M2，纯逻辑无 EUI 依赖，供单测）：
//   show(短语, 时长) → 每帧 tick(dt) 递减 → 归零自动隐藏
// 短语表在 pet_skin.h（皮肤可换语料）；触发与绘制在 pet_window.cpp
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace module::pet {

struct PetBubble {
    bool visible = false;
    std::string text;
    double remainingSec = 0.0;

    void show(std::string phrase, double durationSec) {
        text = std::move(phrase);
        remainingSec = durationSec;
        visible = !text.empty();
    }

    void tick(double dtSec) {
        if (!visible) return;
        remainingSec -= dtSec;
        if (remainingSec <= 0.0) {
            visible = false;
            remainingSec = 0.0;
            text.clear();
        }
    }
};

// ---- 气泡内容源②（§4.5，审计 [18]）：Home 逾期任务数文案（纯函数供单测）----
// getOverdueCount 为 main 注入的跨模块只读桥（契约见 pet_window.h
// OverdueCountGetter，pet 不 include module_home）：
//   - 桥未注入（空 std::function）/ 返回 nullopt（todo 模块关闭、查询失败）
//     → 空串 = 数据不可用，调用方回退内容源①静态语料
//     （M2 验收降级条款：不显示死数据）
//   - N > 0  → 「你有 N 条逾期任务」（设计原话款式）
//   - N <= 0 → 正面文案「全部搞定，真棒！」——无逾期不弹「0 条」死数字
inline std::string overdueBubbleText(
    const std::function<std::optional<int64_t>()>& getOverdueCount) {
    if (!getOverdueCount) {
        return {};
    }
    const std::optional<int64_t> n = getOverdueCount();
    if (!n.has_value()) {
        return {};
    }
    if (*n <= 0) {
        return std::string("全部搞定，真棒！");
    }
    return "你有 " + std::to_string(*n) + " 条逾期任务";
}

} // namespace module::pet
