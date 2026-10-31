#pragma once
// driver_checks.h — 判定纯函数（债-18 UI 驱动入口底层件，零 probe_harness 依赖）：
// wait 条件评估 / 断言谓词 / 命中回读四态判定。
// 签名一律收基础数据（TreeView / LayoutRect / 字符串）：树遍历（walkElements，
// 住 probe_harness detail）由编排层做完传入——本头因此不需要 probe_harness 的
// 任何设施，可被 devdesk_tests 无头单测直接 include（方案卡 §3 分层纪律）。
#include "core/dsl.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace ui_drive {

// 元素树视图：编排层每帧 walkElements 产出后传值/传引用进来。
struct TreeView {
    std::unordered_map<std::string, const core::dsl::Element*> byId;
    std::unordered_map<std::string, std::string> parentOf;
    std::vector<const core::dsl::Element*> leaves;
};

enum class TextOp { Contains, Equals };

inline bool textOpFromName(const std::string& name, TextOp* out) {
    if (name.empty() || name == "contains") { *out = TextOp::Contains; return true; }
    if (name == "equals") { *out = TextOp::Equals; return true; }
    return false;
}

// ---- 几何谓词（喂合成 LayoutRect 可无头单测：相切/窗内外/相交/含于） ----

// frame 含于窗口逻辑矩形（边界相切算在内）
inline bool rectWithinWindow(const core::LayoutRect& r, float windowW, float windowH) {
    return r.x >= 0.0f && r.y >= 0.0f &&
           r.x + r.width <= windowW && r.y + r.height <= windowH;
}

// 两矩形相交（边相切 = 不相交；复用 probe_harness::detail::framesIntersect 同一口径）
inline bool rectIntersectsRect(const core::LayoutRect& a, const core::LayoutRect& b) {
    return a.x < b.x + b.width && b.x < a.x + a.width &&
           a.y < b.y + b.height && b.y < a.y + a.height;
}

// ---- wait 条件 / 断言谓词 ----

inline bool elementExists(const TreeView& tree, const std::string& resolvedId) {
    return !resolvedId.empty() && tree.byId.count(resolvedId) != 0;
}

// 文本匹配：resolvedId 非空 = 该元素 text；空 = 全页叶子扫描（任一叶子命中即真）。
inline bool textPresent(const TreeView& tree, const std::string& resolvedId,
                        TextOp op, const std::string& text) {
    if (text.empty()) return false;
    auto matches = [op, &text](const std::string& value) {
        if (op == TextOp::Equals) return value == text;
        return value.find(text) != std::string::npos;
    };
    if (!resolvedId.empty()) {
        const auto it = tree.byId.find(resolvedId);
        return it != tree.byId.end() && matches(it->second->text);
    }
    for (const core::dsl::Element* leaf : tree.leaves) {
        if (matches(leaf->text)) return true;
    }
    return false;
}

// ---- 命中回读四态（F-05）----
// 命中 = 目标自身；命中 ∈ 目标子树（parentOf 链判定——对齐 Playwright
// 「target or descendant」，组件内部 hit 区（如 button 的 .bg）都是这种）；
// 命中 = 其他 id（截胡）；空命中（该点无任何可交互元素接住——多为 clip 外或
// disabled，诊断与「被 Y 截胡」明确区分）。
enum class HitVerdict { Target, Descendant, Hijacked, Empty };

inline HitVerdict classifyHit(const TreeView& tree,
                              const std::string& targetResolvedId,
                              const std::string& hitId) {
    if (hitId.empty()) return HitVerdict::Empty;
    if (hitId == targetResolvedId) return HitVerdict::Target;
    for (std::string id = hitId;;) {
        const auto it = tree.parentOf.find(id);
        if (it == tree.parentOf.end() || it->second.empty()) break;
        id = it->second;
        if (id == targetResolvedId) return HitVerdict::Descendant;
    }
    return HitVerdict::Hijacked;
}

} // namespace ui_drive
