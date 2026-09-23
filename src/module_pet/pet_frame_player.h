// 帧序列播放器 + 四态状态机（桌宠设计 §4.4，M2，纯逻辑无 EUI 依赖，供单测）：
//   Idle   --点击-->        Poked（poked 帧播一遍）--> Idle
//   Idle   --代理流式中-->  Typing（typing 帧循环）--流结束--> Idle
//   Idle   --kSleepAfterSeconds 无交互--> Sleep（sleep 帧循环）
//   Sleep  --任意交互-->    Idle（唤醒）
//   Dragged 不是独立动画态：拖拽只是抑制"拖完误触 poke"（UI 层 wasDragged 标志）
// ponytail: 一个 enum + 几个纯函数就是状态机的全部；转移全在 UI 层 onTimer
// 里按序调用（setStreaming → considerSleep → tick），顺序即优先级
#pragma once

#include "module_pet/pet_skin.h"

namespace module::pet {

enum class PetAnimState { Idle, Poked, Sleep, Typing };

// 入睡阈值：无交互 5 分钟（设计 §4.4；皮肤无 sleep 帧时不入睡）
inline constexpr double kSleepAfterSeconds = 5.0 * 60.0;

// 状态 → 帧列表（缺帧状态回落 idle；皮肤可以只有一套帧）
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

    // 用户交互（点击/拖拽）：Sleep 唤醒回 Idle；Idle 且皮肤有 poked 帧才戳；
    // Poked/Typing 中忽略（不重置正在播的反应）
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

    // 代理流式联动（设计 §4.5 内容源③，M2）：有活动流 → Typing（打断
    // Sleep——AI 干活时宠物不睡）；流结束 → Idle。皮肤无 typing 帧时
    // stateFrames 回落 idle 帧照常循环（状态语义仍在，视觉退化为 idle）
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

    // 入睡判定（纯函数，UI 层每帧喂无交互秒数）：仅 Idle 可入睡，且皮肤
    // 要有 sleep 帧（否则睡了看不出，不如不睡）
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
