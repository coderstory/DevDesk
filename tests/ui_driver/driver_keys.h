#pragma once
// driver_keys.h — 键名表：语义键名 → GLFW 键码 + 修饰位（债-18 UI 驱动入口底层件）。
// 仅依赖 GLFW 常量头（GLFW_INCLUDE_NONE 下不含平台 GL 头）；被 driver_schema
// （加载期校验键名）与 driver_steps（执行期查表）共用——这个共用关系是它必须
// 待在零 probe_harness 依赖底层头的理由（方案卡 §3 分层纪律）。
// GLFW 键码 → core::InputKey 一层在 probe_harness::detail::glfwKeyToInputKey，
// 本头不重复（既有双份消除属方案 C 档 2，本期不动）。
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>


#include <cstddef>
#include <string>
#include <vector>

namespace ui_drive {

// "Shortcut+" 前缀的平台判定不在表内：表只记 shortcut 标志，执行层按
// test_support::shortcutUsesControl/Super 折算成 GLFW_MOD_CONTROL / GLFW_MOD_SUPER
// （EUI-NEO KeyModifiers::shortcut() 当唯一判据，键名双平台一致、无平台分叉表）。
struct KeySpec {
    int key = 0;            // GLFW 键码；查表 miss 时 known=false
    int modifiers = 0;      // GLFW_MOD_* 位组合（Shortcut 前缀不含在此）
    bool shortcut = false;  // "Shortcut+" 前缀
    bool known = false;
};

// 解析组合键："Ctrl+S" → GLFW_KEY_S + MOD_CONTROL；"Shift+Alt+Enter" 多级前缀；
// "Shortcut+S" → shortcut=true（修饰位执行期折算）。裸键名无前缀。
inline KeySpec lookupKeyName(const std::string& combo);

// 相近候选：前缀优先、其次双向子串，≤5 个（AC-2 口径，与 id 候选同机制）。
inline std::vector<std::string> nearCandidates(const std::string& value,
                                               const std::vector<std::string>& vocabulary);

namespace detail {

inline int lookupSingleKey(const std::string& name) {
    if (name.size() == 1) {
        const char c = name[0];
        if (c >= 'A' && c <= 'Z') return GLFW_KEY_A + (c - 'A');
        if (c >= '0' && c <= '9') return GLFW_KEY_0 + (c - '0');
        return 0;
    }
    struct FixedEntry { const char* name; int key; };
    static const FixedEntry kFixed[] = {
        {"Enter", GLFW_KEY_ENTER},     {"Escape", GLFW_KEY_ESCAPE},
        {"Tab", GLFW_KEY_TAB},         {"Backspace", GLFW_KEY_BACKSPACE},
        {"Space", GLFW_KEY_SPACE},     {"Delete", GLFW_KEY_DELETE},
        {"Insert", GLFW_KEY_INSERT},   {"Home", GLFW_KEY_HOME},
        {"End", GLFW_KEY_END},         {"PageUp", GLFW_KEY_PAGE_UP},
        {"PageDown", GLFW_KEY_PAGE_DOWN}, {"Left", GLFW_KEY_LEFT},
        {"Right", GLFW_KEY_RIGHT},     {"Up", GLFW_KEY_UP},
        {"Down", GLFW_KEY_DOWN},
    };
    for (const auto& e : kFixed) {
        if (name == e.name) return e.key;
    }
    if (name.size() >= 2 && name[0] == 'F') {
        // F1..F24
        int num = 0;
        for (std::size_t i = 1; i < name.size(); ++i) {
            if (name[i] < '0' || name[i] > '9') return 0;
            num = num * 10 + (name[i] - '0');
        }
        if (num >= 1 && num <= 24) return GLFW_KEY_F1 + (num - 1);
    }
    return 0;
}

inline int modifierPrefixBit(const std::string& prefix) {
    if (prefix == "Ctrl") return GLFW_MOD_CONTROL;
    if (prefix == "Shift") return GLFW_MOD_SHIFT;
    if (prefix == "Alt") return GLFW_MOD_ALT;
    if (prefix == "Cmd" || prefix == "Win") return GLFW_MOD_SUPER;
    return 0;  // "Shortcut" 由 shortcut 标志表达，不折算成位
}

} // namespace detail

inline KeySpec lookupKeyName(const std::string& combo) {
    KeySpec spec;
    if (combo.empty()) return spec;
    // 从右往左按 '+' 切：最后一段 = 键名，其余前缀 = 修饰。键名段本身不含 '+'。
    std::vector<std::string> parts;
    std::string current;
    for (const char c : combo) {
        if (c == '+') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (current.empty()) return spec;  // 以 '+' 结尾（如 "Ctrl+"）→ 键名段为空，miss
    parts.push_back(current);

    spec.key = detail::lookupSingleKey(parts.back());
    if (spec.key == 0) return spec;  // 键名段查表 miss
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        const std::string& prefix = parts[i];
        if (prefix == "Shortcut") {
            spec.shortcut = true;
            continue;
        }
        const int bit = detail::modifierPrefixBit(prefix);
        if (bit == 0) return spec;  // 未知修饰前缀 → 整体 miss（防静默错拼）
        spec.modifiers |= bit;
    }
    spec.known = true;
    return spec;
}

inline std::vector<std::string> keyNameVocabulary() {
    std::vector<std::string> vocab = {
        "Enter", "Escape", "Tab", "Backspace", "Space", "Delete", "Insert",
        "Home", "End", "PageUp", "PageDown", "Left", "Right", "Up", "Down",
        "Ctrl+", "Shift+", "Alt+", "Cmd+", "Win+", "Shortcut+",
    };
    for (char c = 'A'; c <= 'Z'; ++c) vocab.push_back(std::string(1, c));
    for (char c = '0'; c <= '9'; ++c) vocab.push_back(std::string(1, c));
    for (int f = 1; f <= 24; ++f) vocab.push_back("F" + std::to_string(f));
    return vocab;
}

inline std::vector<std::string> nearCandidates(const std::string& value,
                                               const std::vector<std::string>& vocabulary) {
    std::vector<std::string> prefixHits;
    std::vector<std::string> substringHits;
    for (const auto& item : vocabulary) {
        if (item == value) continue;
        if (item.rfind(value, 0) == 0 || value.rfind(item, 0) == 0) {
            prefixHits.push_back(item);
        } else if (item.find(value) != std::string::npos ||
                   value.find(item) != std::string::npos) {
            substringHits.push_back(item);
        }
        if (prefixHits.size() >= 5) break;
    }
    for (const auto& item : substringHits) {
        if (prefixHits.size() >= 5) break;
        prefixHits.push_back(item);
    }
    return prefixHits;
}

} // namespace ui_drive
