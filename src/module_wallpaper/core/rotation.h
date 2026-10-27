// src/module_wallpaper/core/rotation.h
// 轮换内核：「下一条播哪条、多久换一条」的唯一真相源。纯逻辑，时间由调用方传入。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "module_wallpaper/core/model.h"

namespace module::wallpaper {

using RotationClock = std::chrono::steady_clock;

// UI 允许的轮换间隔（秒）：5/10/15/30/60/120 分钟。设置与运行时都吸附到这些档位。
inline constexpr int kRotationChoiceSeconds[] = {300, 600, 900, 1800, 3600, 7200};
inline constexpr int kDefaultRotationSeconds = 300;

// 就近吸附到允许档；非法输入（含 0/负数）回默认 300。等距时取较小档（与原型一致）。
int snapRotationIntervalSec(int seconds);

// 线程约束：本类不持锁、不启线程，只允许在 WallpaperService 的 mu_ 内调用。
class RotationController {
public:
    explicit RotationController(std::uint32_t seed = 1);

    // 换清单 = 新一轮：清理索引/洗牌袋/计时并停止。调用方随后 start()。
    void setItems(std::vector<std::string> items);
    // 重扫结果回填：当前条目还在时保留当前与剩余计时（返回 true）；不在时归零等待调用方
    // start() 重新起一轮（返回 false）。不允许打断正在播放的条目。
    bool refreshItems(std::vector<std::string> items);

    void setMode(PlayMode mode);
    void setIntervalSec(int seconds, RotationClock::time_point now);

    // 交出首条并排下一程；空清单返回 nullopt 且不进入运行态。
    std::optional<std::string> start(RotationClock::time_point now);
    void stop();

    // 冻结剩余时间（暂停）与按冻结值续排（恢复）。重复调用幂等。
    void pause(RotationClock::time_point now);
    void resume(RotationClock::time_point now);

    // 到点则推进并返回需要装载的条目；不需要装载（单循环到点/暂停/未运行）返回 nullopt。
    std::optional<std::string> tick(RotationClock::time_point now);
    // 用户「下一个」：任何模式都推进（单循环也换到清单下一条）。
    std::optional<std::string> advanceNow(RotationClock::time_point now);

    // 距下次轮换秒数；不轮换（未运行 / 单循环 / 条目 <= 1）返回 -1。
    int secondsUntilNextRotation(RotationClock::time_point now) const;
    // 0..1；不轮换时为 0。暂停时冻结在暂停那一刻的值。
    float progress(RotationClock::time_point now) const;

    bool isRunning() const { return running_; }
    bool isPaused() const { return paused_; }
    PlayMode mode() const { return mode_; }
    int intervalSec() const { return intervalSec_; }
    std::size_t itemCount() const { return items_.size(); }
    std::optional<std::string> currentPath() const;
    std::size_t currentIndex() const { return currentIndex_; }

private:
    enum class AdvanceReason { Elapsed, UserRequested };

    std::optional<std::string> advance(AdvanceReason reason, RotationClock::time_point now);
    void reschedule(RotationClock::time_point now);
    void refillBag();
    std::chrono::milliseconds remainingLocked(RotationClock::time_point now) const;

    std::vector<std::string> items_;
    std::size_t currentIndex_ = 0;
    PlayMode mode_ = PlayMode::LoopSingle;
    int intervalSec_ = kDefaultRotationSeconds;
    bool running_ = false;
    bool paused_ = false;
    bool hasPausedRemaining_ = false;
    std::chrono::milliseconds pausedRemaining_{0};
    std::optional<RotationClock::time_point> nextFireAt_;
    std::vector<std::size_t> bag_;
    std::mt19937 rng_;
};

} // namespace module::wallpaper