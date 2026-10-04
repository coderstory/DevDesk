// 帧序列播放器 + 四态状态机（纯逻辑无 EUI 依赖，供单测）：Idle→点击→Poked（播一遍）
// →Idle；Idle→代理流式→Typing（循环）→流结束→Idle；Idle→无交互 5min→Sleep。
// Dragged 不是独立动画态：拖拽只是抑制「拖完误触 poke」（UI 层 wasDragged 标志）。
#pragma once

#include "module_pet/pet_skin.h"

namespace module::pet {

enum class PetAnimState { Idle, Poked, Sleep, Typing };

// 入睡阈值：无交互 5 分钟（皮肤无 sleep 帧时不入睡）。
inline constexpr double kSleepAfterSeconds = 5.0 * 60.0;

// 状态 → 帧列表（缺帧状态回落 idle）。
inline const std::vector<std::string>& stateFrames(const PetSkin& skin,
                                                   PetAnimState state) {
    switch (state) {
    case PetAnimState::Poked:
        if (!skin.pokedFrames.empty()) return skin.pokedFrames;
        break;
    case PetAnimState::Sleep:
        if (!skin.sleepFrames.empty()) return skin.sleepFrames;
        break;
    case PetAnimState::Typing:
        if (!skin.typingFrames.empty()) return skin.typingFrames;
        break;
    case PetAnimState::Idle:
        break;
    }
    return skin.idleFrames;
}

struct PetAnimation {
    PetAnimState state = PetAnimState::Idle;
    int frame = 0;    // 当前状态的帧下标
    int ticks = 0;    // 当前状态已播放的帧数（poked 结束判定用）

    // 每帧调用一次（onTimer 周期 = 1/frameRate）：推进帧；poked 播完回 Idle
    void tick(const PetSkin& skin) {
        const std::vector<std::string>& list = stateFrames(skin, state);
        if (list.empty()) return;
        ++ticks;
        frame = (frame + 1) % static_cast<int>(list.size());
        if (state == PetAnimState::Poked && ticks >= static_cast<int>(list.size())) {
            state = PetAnimState::Idle;
            frame = 0;
            ticks = 0;
        }
    }

    // 用户交互：Sleep 唤醒回 Idle；Idle 且皮肤有 poked 帧才戳；Poked/Typing 中忽略。
    void poke(const PetSkin& skin) {
        if (state == PetAnimState::Sleep) {
            state = PetAnimState::Idle;
            frame = 0;
            ticks = 0;
            return;
        }
        if (state != PetAnimState::Idle || skin.pokedFrames.empty()) return;
        state = PetAnimState::Poked;
        frame = 0;
        ticks = 0;
    }

    // 代理流式联动：有活动流 → Typing（打断 Sleep）；流结束 → Idle。
    void setStreaming(bool active, const PetSkin& skin) {
        (void)skin;
        if (active && state != PetAnimState::Typing) {
            state = PetAnimState::Typing;
            frame = 0;
            ticks = 0;
        } else if (!active && state == PetAnimState::Typing) {
            state = PetAnimState::Idle;
            frame = 0;
            ticks = 0;
        }
    }

    // 入睡判定：仅 Idle 可入睡，且皮肤要有 sleep 帧。
    void considerSleep(double idleSeconds, const PetSkin& skin) {
        if (state != PetAnimState::Idle || skin.sleepFrames.empty()) return;
        if (idleSeconds < kSleepAfterSeconds) return;
        state = PetAnimState::Sleep;
        frame = 0;
        ticks = 0;
    }

    std::filesystem::path currentPath(const PetSkin& skin) const {
        const std::vector<std::string>& list = stateFrames(skin, state);
        if (list.empty()) return {};
        return skin.dir / list[static_cast<size_t>(frame) % list.size()];
    }
};

} // namespace module::pet
