// src/module_pet/pet_frame_player.h
// 帧序列播放器 + 最小状态机（桌宠设计 §4.4，纯逻辑无 EUI 依赖，供单测）：
//   Idle --点击--> Poked（poked 帧播一遍）--> Idle
//   Dragged 不是独立动画态：拖拽只是抑制"拖完误触 poke"（UI 层 wasDragged 标志）
// ponytail: 一个 enum + tick 就是状态机的全部；Sleep/游走是 M2+/远期
#pragma once

#include "module_pet/pet_skin.h"

namespace module::pet {

enum class PetAnimState { Idle, Poked };

struct PetAnimation {
    PetAnimState state = PetAnimState::Idle;
    int frame = 0;    // 当前状态的帧下标
    int ticks = 0;    // 当前状态已播放的帧数（poked 结束判定用）

    // 每帧调用一次（onTimer 周期 = 1/frameRate）：推进帧；poked 播完回 Idle
    void tick(const PetSkin& skin) {
        const std::vector<std::string>& list = skin.frames(state == PetAnimState::Poked);
        if (list.empty()) return;
        ++ticks;
        frame = (frame + 1) % static_cast<int>(list.size());
        if (state == PetAnimState::Poked && ticks >= static_cast<int>(list.size())) {
            state = PetAnimState::Idle;
            frame = 0;
            ticks = 0;
        }
    }

    // 被戳：Idle 才响应；皮肤没有 poked 帧时维持 Idle（无反应动画）
    void poke(const PetSkin& skin) {
        if (state != PetAnimState::Idle || skin.pokedFrames.empty()) return;
        state = PetAnimState::Poked;
        frame = 0;
        ticks = 0;
    }

    std::filesystem::path currentPath(const PetSkin& skin) const {
        return skin.framePath(state == PetAnimState::Poked,
                              static_cast<size_t>(frame));
    }
};

} // namespace module::pet
