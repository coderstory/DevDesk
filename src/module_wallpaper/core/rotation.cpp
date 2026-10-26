#include "module_wallpaper/core/rotation.h"

#include <algorithm>
#include <cmath>

namespace module::wallpaper {

namespace {
constexpr int kChoiceCount = static_cast<int>(sizeof(kRotationChoiceSeconds) /
                                              sizeof(kRotationChoiceSeconds[0]));
} // namespace

int snapRotationIntervalSec(int seconds) {
    if (seconds <= 0) return kDefaultRotationSeconds;
    // 先找等值档，避免浮点误差参与决策。
    for (int choice : kRotationChoiceSeconds) {
        if (choice == seconds) return choice;
    }
    const double target = static_cast<double>(seconds);
    int best = kRotationChoiceSeconds[0];
    double bestDistance = std::fabs(static_cast<double>(best) - target);
    for (int index = 1; index < kChoiceCount; ++index) {
        const int candidate = kRotationChoiceSeconds[index];
        const double distance = std::fabs(static_cast<double>(candidate) - target);
        if (distance < bestDistance) {  // 严格小于 = 等距取较小档
            best = candidate;
            bestDistance = distance;
        }
    }
    return best;
}

RotationController::RotationController(std::uint32_t seed) : rng_(seed) {}

void RotationController::setItems(std::vector<std::string> items) {
    items_ = std::move(items);
    currentIndex_ = 0;
    bag_.clear();
    running_ = false;
    paused_ = false;
    hasPausedRemaining_ = false;
    pausedRemaining_ = std::chrono::milliseconds{0};
    nextFireAt_.reset();
}

bool RotationController::refreshItems(std::vector<std::string> items) {
    const std::optional<std::string> current = currentPath();
    std::size_t found = items.size();
    if (current.has_value()) {
        for (std::size_t index = 0; index < items.size(); ++index) {
            if (items[index] == *current) {
                found = index;
                break;
            }
        }
    }
    items_ = std::move(items);
    bag_.clear();  // 清单变了，旧的洗牌袋失效
    if (found == items_.size()) {
        // 当前条目已不在清单：归零，由调用方 start() 重新起一轮。
        currentIndex_ = 0;
        running_ = false;
        paused_ = false;
        hasPausedRemaining_ = false;
        pausedRemaining_ = std::chrono::milliseconds{0};
        nextFireAt_.reset();
        return false;
    }
    currentIndex_ = found;
    // 保留当前与剩余计时：running_/paused_/nextFireAt_ 一概不动。
    return true;
}

void RotationController::setMode(PlayMode mode) {
    if (mode_ == mode) return;
    mode_ = mode;
    bag_.clear();  // 模式切换后洗牌袋/顺序语义都变了
}

void RotationController::setIntervalSec(int seconds, RotationClock::time_point now) {
    const int snapped = snapRotationIntervalSec(seconds);
    if (intervalSec_ == snapped) return;
    intervalSec_ = snapped;
    if (running_ && !paused_ && !items_.empty()) {
        reschedule(now);
    } else if (paused_) {
        // 暂停中改间隔 = 以新间隔为完整剩余（原型行为）。
        hasPausedRemaining_ = true;
        pausedRemaining_ = std::chrono::milliseconds{intervalSec_ * 1000};
    }
}

std::optional<std::string> RotationController::start(RotationClock::time_point now) {
    if (items_.empty()) {
        running_ = false;
        return std::nullopt;
    }
    running_ = true;
    paused_ = false;
    hasPausedRemaining_ = false;
    pausedRemaining_ = std::chrono::milliseconds{0};
    if (mode_ == PlayMode::Shuffle) {
        if (bag_.empty()) refillBag();
        currentIndex_ = bag_.front();
        bag_.erase(bag_.begin());
    } else {
        currentIndex_ = 0;
    }
    reschedule(now);
    return currentPath();
}

void RotationController::stop() {
    running_ = false;
    paused_ = false;
    hasPausedRemaining_ = false;
    pausedRemaining_ = std::chrono::milliseconds{0};
    nextFireAt_.reset();
    bag_.clear();
}

void RotationController::pause(RotationClock::time_point now) {
    if (!running_ || paused_) return;
    // 先按「未暂停」语义取剩余，再翻 paused_——remainingLocked 会优先读暂停剩余，
    // 顺序反了会把剩余冻结成 0。
    const std::chrono::milliseconds remaining = remainingLocked(now);
    paused_ = true;
    hasPausedRemaining_ = true;
    pausedRemaining_ = remaining;
    nextFireAt_.reset();
}

void RotationController::resume(RotationClock::time_point now) {
    if (!paused_) return;
    paused_ = false;
    const std::chrono::milliseconds remaining =
        hasPausedRemaining_ ? pausedRemaining_ : std::chrono::milliseconds{intervalSec_ * 1000};
    hasPausedRemaining_ = false;
    pausedRemaining_ = std::chrono::milliseconds{0};
    nextFireAt_ = now + remaining;
}

std::optional<std::string> RotationController::tick(RotationClock::time_point now) {
    if (!running_ || paused_) return std::nullopt;
    if (!nextFireAt_.has_value() || now < *nextFireAt_) return std::nullopt;
    return advance(AdvanceReason::Elapsed, now);
}

std::optional<std::string> RotationController::advanceNow(RotationClock::time_point now) {
    if (items_.empty()) return std::nullopt;
    return advance(AdvanceReason::UserRequested, now);
}

int RotationController::secondsUntilNextRotation(RotationClock::time_point now) const {
    if (!running_ || mode_ == PlayMode::LoopSingle || items_.size() <= 1) return -1;
    const std::chrono::milliseconds remaining = remainingLocked(now);
    if (remaining.count() <= 0) return 0;
    // 向上取整：还剩 0.2 秒时显示 1 而不是 0，避免 UI 长时间停在 0。
    const double seconds = static_cast<double>(remaining.count()) / 1000.0;
    return static_cast<int>(std::ceil(seconds));
}

float RotationController::progress(RotationClock::time_point now) const {
    if (!running_ || mode_ == PlayMode::LoopSingle || items_.size() <= 1 || intervalSec_ <= 0) {
        return 0.0f;
    }
    const std::chrono::milliseconds remaining = remainingLocked(now);
    const double intervalMs = static_cast<double>(intervalSec_) * 1000.0;
    double ratio = 1.0 - static_cast<double>(remaining.count()) / intervalMs;
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    return static_cast<float>(ratio);
}

std::optional<std::string> RotationController::currentPath() const {
    if (currentIndex_ >= items_.size()) return std::nullopt;
    return items_[currentIndex_];
}

std::optional<std::string> RotationController::advance(AdvanceReason reason,
                                                       RotationClock::time_point now) {
    if (items_.empty()) return std::nullopt;
    const std::size_t previousIndex = currentIndex_;
    std::size_t nextIndex = previousIndex;
    switch (mode_) {
        case PlayMode::LoopSingle:
            nextIndex = (reason == AdvanceReason::UserRequested)
                            ? (currentIndex_ + 1) % items_.size()
                            : currentIndex_;
            break;
        case PlayMode::LoopList:
            nextIndex = (currentIndex_ + 1) % items_.size();
            break;
        case PlayMode::Shuffle:
            if (bag_.empty()) refillBag();
            nextIndex = bag_.front();
            bag_.erase(bag_.begin());
            break;
    }
    currentIndex_ = nextIndex;
    // 单循环到点：索引没变 = 只是重新排程，不要求播放端重新装载。
    if (nextIndex == previousIndex && reason == AdvanceReason::Elapsed) {
        reschedule(now);
        return std::nullopt;
    }
    reschedule(now);
    return currentPath();
}

void RotationController::reschedule(RotationClock::time_point now) {
    if (paused_) {
        hasPausedRemaining_ = true;
        pausedRemaining_ = std::chrono::milliseconds{intervalSec_ * 1000};
        nextFireAt_.reset();
        return;
    }
    nextFireAt_ = now + std::chrono::milliseconds{intervalSec_ * 1000};
}

void RotationController::refillBag() {
    bag_.resize(items_.size());
    for (std::size_t index = 0; index < items_.size(); ++index) bag_[index] = index;
    std::shuffle(bag_.begin(), bag_.end(), rng_);
}

std::chrono::milliseconds RotationController::remainingLocked(
    RotationClock::time_point now) const {
    if (paused_) {
        return hasPausedRemaining_ ? pausedRemaining_
                                   : std::chrono::milliseconds{intervalSec_ * 1000};
    }
    if (!nextFireAt_.has_value()) return std::chrono::milliseconds{0};
    if (now >= *nextFireAt_) return std::chrono::milliseconds{0};
    return std::chrono::duration_cast<std::chrono::milliseconds>(*nextFireAt_ - now);
}

} // namespace module::wallpaper