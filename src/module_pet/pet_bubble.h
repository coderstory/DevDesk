// 气泡对话状态（纯逻辑无 EUI 依赖，供单测）：show(短语, 时长) → 每帧 tick(dt)
// 递减 → 归零自动隐藏。短语表在 pet_skin.h；触发与绘制在 pet_window.cpp。
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

// 内容源②Home 逾期任务数文案（纯函数供单测）。桥未注入 / 返回 nullopt（todo 关闭、
// 查询失败）→ 返回空串，调用方回退内容源①；N <= 0 → 正面文案，不弹「0 条」死数字。
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

// 内容源③代理请求事件文案（纯函数供单测）。桥返回 HTTP 状态码：0 = 上游不可达 →
// 「网络失败」，>=400 →「代理出错：{status}」，其余 →「转发完成」；nullopt/未注入 → 空串。
// 读取即消费（take 语义），一个事件最多弹一次。
inline std::string proxyEventBubbleText(
    const std::function<std::optional<int>()>& getProxyEvent) {
    if (!getProxyEvent) {
        return {};
    }
    const std::optional<int> status = getProxyEvent();
    if (!status.has_value()) {
        return {};
    }
    if (*status <= 0) {
        return std::string("代理出错：网络失败");
    }
    if (*status >= 400) {
        return "代理出错：" + std::to_string(*status);
    }
    return std::string("代理转发完成");
}

// 三级内容源选词（③>②>①，纯函数）：返回 {文案, 停留秒}；空文案 = 桥都无数据，调用方回退①。
inline std::pair<std::string, double> petDataBubbleText(
    const std::function<std::optional<int>()>& getProxyEvent,
    const std::function<std::optional<int64_t>()>& getOverdueCount) {
    std::string text = proxyEventBubbleText(getProxyEvent);
    if (!text.empty()) {
        return {std::move(text), 3.0};
    }
    text = overdueBubbleText(getOverdueCount);
    if (!text.empty()) {
        return {std::move(text), 4.0};
    }
    return {};
}

// 气泡调度（纯函数）：输入 dt + 当前状态（气泡在场/是否 Idle）+ roll → 本轮弹哪一级。
// 命中口径：气泡在场或非 Idle 不弹；dt <= 0 不弹；roll < dt/150 → 数据槽位（③>②）；
// roll < dt/60 → ①静态语料；其余不弹。平均间隔：数据槽位 150s、任意一泡 60s。
enum class PetBubbleSource {
    None,    // 本轮不弹
    Data,    // 数据槽位：③代理事件 > ②逾期（文案/秒数由 petDataBubbleText 定）
    Static,  // ①静态语料（皮肤短语表，见 pet_skin.h pickPetPhrase）
};

struct PetBubblePlan {
    PetBubbleSource source = PetBubbleSource::None;
    // 计划停留秒数；Data 无数据时降级静态语料的兜底停留，None = 0。
    double holdSec = 0.0;
};

// 调度节拍（秒）——命名常量 = 注释与代码的单源真相。
inline constexpr double kPetDataSlotSec = 150.0;      // ③②数据槽位
inline constexpr double kPetAnyBubbleSlotSec = 60.0;  // 两级共享槽位上界
inline constexpr double kPetBubbleHoldSec = 3.0;      // 静态语料 / 数据降级停留

inline PetBubblePlan petBubbleSchedule(double dtSec, bool bubbleVisible, bool idle,
                                       double roll) {
    if (bubbleVisible || !idle) {
        return {};
    }
    if (dtSec <= 0.0) {
        return {};
    }
    if (roll < dtSec / kPetDataSlotSec) {
        return {PetBubbleSource::Data, kPetBubbleHoldSec};
    }
    if (roll < dtSec / kPetAnyBubbleSlotSec) {
        return {PetBubbleSource::Static, kPetBubbleHoldSec};
    }
    return {};
}

// 气泡布局：整窗加高一条「气泡让位条」，精灵帧整体下移，气泡画在条内、与帧留
// 6px 间距（不再盖在宠物头顶）。窗口顶 y=0 起是气泡条 0..28，y=34（kBubbleStripH）
// 以下是精灵帧。
// 单位口径：全部是窗口单位 = 点，不乘 Retina contentScale。「气泡不越出屏幕工作区」
// 由 clampPetPosToMonitors 保证（调用方传整窗高，钳制后气泡条上沿不越工作区上缘）。
struct PetRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

inline constexpr float kBubbleHeight = 28.0f;  // 气泡条高（bg 矩形；text 同高）
inline constexpr float kBubbleGap = 6.0f;      // 气泡底沿 ↔ 精灵帧顶间距（4-8px 口径）
inline constexpr float kBubbleStripH = kBubbleHeight + kBubbleGap;  // 34 = 让位条总高

// 气泡矩形（窗口坐标）：气泡条内水平居中（bubbleW 可超窗宽 → x 为负被裁剪）。
inline PetRect petBubbleRect(float windowW, float bubbleW) {
    return {(windowW - bubbleW) * 0.5f, 0.0f, bubbleW, kBubbleHeight};
}

// 精灵帧矩形：整帧让位到气泡条下方（与气泡矩形恒不相交）。
inline PetRect petSpriteRect(float windowW, float windowH) {
    return {0.0f, kBubbleStripH, windowW, windowH - kBubbleStripH};
}

// 桌宠窗口高 = 精灵帧高 + 气泡让位条（点）。
inline int petWindowHeight(int frameH) {
    return frameH + static_cast<int>(kBubbleStripH);
}

} // namespace module::pet
