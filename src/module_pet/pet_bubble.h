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

// ---- 气泡内容源③（M2 §4.5，产品审计 [18]）：代理请求事件文案（纯函数供单测）----
// getProxyEvent 为 main 注入的 app 级只读桥（契约见 pet_window.h
// ProxyEventGetter，pet 不 include module_claude_proxy；读取即消费，一个事件
// 最多弹一次）：
//   - 桥未注入（空 std::function）/ 返回 nullopt（无事件 / claude_proxy 关闭 /
//     事件过期）→ 空串 = 无数据，调用方按 ②→① 回退（不显示死数据条款）
//   - 0（上游不可达，网络层失败无响应码）→「代理出错：网络失败」
//     （原始 curl 报错文本不入气泡——太长且对用户无意义，日志面板已有）
//   - >=400 →「代理出错：{status}」（429 限流 / 500 / 503 带码直读）
//   - 其余（2xx/3xx）→「代理转发完成」
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

// 三级内容源选词（M2 §4.5 ③>②>① 优先级，纯函数供单测断言回退序）：
// 返回 {文案, 停留秒}；空文案 = ③② 桥都无数据，调用方回退内容源①静态语料。
// 停留时长沿用既定口径：代理事件是短句 3s；逾期文案含数字 4s 才读完。
// 注意：getProxyEvent 为 take 语义——有事件必出非空文案即弹，不误吞
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

} // namespace module::pet
