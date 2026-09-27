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

// ---- 气泡调度（PAB-05）：本轮「弹哪一级内容源 / 停多久」的纯函数 ----
// 抽取自 pet_window.cpp 的 onTimer 闭包（内联在闭包里时不可单测）：输入 dt +
// 当前状态（气泡在场 / 是否 Idle）+ 一个 roll 值 → 计划；不依赖 EUI、不读全局、
// 不碰随机源（roll 由调用方给，固定输入必得同一结果）。
// 命中口径（与抽取前逐字一致）：
//   - 气泡在场 或 非 Idle → 不弹（上一泡没走不让位；Poked/Sleep/Typing 不刷屏）
//   - dt <= 0（首帧 / 时钟未推进）→ 不弹
//   - roll < dt/150 → 数据槽位：③代理事件 > ②逾期（取词见 petDataBubbleText；
//     其桥为 take 语义，调用方只能在命中本槽位时才读，未命中不得消费事件）
//   - roll < dt/60  → ①静态语料（dt/150 ⊂ dt/60：一个 roll 即表达两级优先）
//   - 其余 → 不弹
// 平均间隔（每 tick 命中概率 = dt/节拍 → 期望等待 = 节拍）：
//   数据槽位 150s（2.5min）；任意一泡 60s（两级合计）；静态语料单独 ~100s——
//   两级共享一个 roll，静态的净概率是 dt/60 − dt/150 = dt/100。
// 旧注释写「①静态语料——平均 ~60s 一句」，把"任意一泡"的节拍安在了静态单列头
// 上，与代码行为不符：PAB-05 数字对齐以代码行为为准，只改注释、不改行为。
enum class PetBubbleSource {
    None,    // 本轮不弹
    Data,    // 数据槽位：③代理事件 > ②逾期（文案/秒数由 petDataBubbleText 定）
    Static,  // ①静态语料（皮肤短语表，见 pet_skin.h pickPetPhrase）
};

struct PetBubblePlan {
    PetBubbleSource source = PetBubbleSource::None;
    // 计划停留秒数。Static = 静态语料停留；Data = 桥无数据时降级静态语料的
    // 兜底停留（桥有数据时被 petDataBubbleText 的秒数覆盖）；None = 0
    double holdSec = 0.0;
};

// 调度节拍（秒）——命名常量 = 注释与代码的单源真相（PAB-05 数字对齐：
// 上一版注释的 60s/100s 口径差就出在裸字面量上）
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

} // namespace module::pet
