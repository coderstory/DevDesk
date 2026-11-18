// src/ui_kit/text_metrics.h
// presentation 能力层：统一估宽入口（need:296 D2 三套归一）。
//
// 归并三处各自为政的 measure 缓存：agent bug248 版（带字重）、terminal bug248 版
//（恒 400、缺字重维度）、各页直调 core::TextPrimitive::measureTextWidth 无缓存版。
// measureTextWidth 每帧对同批字符串完整 shaping，逐帧重复测量是纯浪费——键空间
// 小且有界（文案来自固定 UI 文案 + 有限列表项），容量封顶整体清空防膨胀。
// 缓存键含字重/字族/字号/字符串：字重入键防 400/500/600 档互串（bug267 同款
// 防御——terminal 版只入族/字号，字重维度是其缺口，归一时补齐）。
#pragma once

#include <core/render/text.h>

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

namespace ui_kit {

// 容量上限：超限整体清空（单帧键数有限，整体清空比 LRU 简单且足够）。
inline constexpr std::size_t kTextWidthCacheCapacity = 128;

// 缓存估宽：与直调 measureTextWidth 逐字节同结果（缓存对调用方行为透明）。
inline float cachedTextWidth(const std::string& text, float fontSize,
                             float weight, const std::string& family = {}) {
    struct Key {
        float size;
        float weight;
        std::string family;
        std::string text;
        bool operator==(const Key& o) const {
            return size == o.size && weight == o.weight &&
                   family == o.family && text == o.text;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            std::size_t h = std::hash<std::string>{}(k.text) ^
                            (std::hash<std::string>{}(k.family) *
                             0x9e3779b9ULL);
            return (h ^ (std::hash<float>{}(k.size) * 0xc2b2ae3dULL)) ^
                   (std::hash<float>{}(k.weight) * 0x85ebca6bULL);
        }
    };
    static std::unordered_map<Key, float, KeyHash> cache;
    if (cache.size() >= kTextWidthCacheCapacity) cache.clear();
    Key key{fontSize, weight, family, text};
    const auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    const float w =
        core::TextPrimitive::measureTextWidth(text, family, fontSize, weight);
    cache.emplace(std::move(key), w);
    return w;
}

} // namespace ui_kit
