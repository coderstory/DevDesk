// 气泡对话状态（桌宠设计 §4.5 M2，纯逻辑无 EUI 依赖，供单测）：
//   show(短语, 时长) → 每帧 tick(dt) 递减 → 归零自动隐藏
// 短语表在 pet_skin.h（皮肤可换语料）；触发与绘制在 pet_window.cpp
#pragma once

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

} // namespace module::pet
