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

enum class TextOp { Contains, Equals, NonEmpty };

inline bool textOpFromName(const std::string& name, TextOp* out) {
    if (name.empty() || name == "contains") { *out = TextOp::Contains; return true; }
    if (name == "equals") { *out = TextOp::Equals; return true; }
    if (name == "nonempty") { *out = TextOp::NonEmpty; return true; }
    return false;
}

// 非 ASCII 空白的可见字符（字节级近似，对齐探针 HasAnyVisible 的 cp>0x20 口径；
// UTF-8 多字节序列所有字节 >0x20，CJK/框线一律算可见）
inline bool textHasVisibleChar(const std::string& value) {
    for (char c : value) {
        if (static_cast<unsigned char>(c) > 0x20) return true;
    }
    return false;
}

// ---- 几何谓词（喂合成 LayoutRect 可无头单测：相切/窗内外/相交/含于） ----

// frame 含于窗口逻辑矩形（边界相切算在内）
inline bool rectWithinWindow(const core::LayoutRect& r, float windowW, float windowH) {
    return r.x >= 0.0f && r.y >= 0.0f &&
           r.x + r.width <= windowW && r.y + r.height <= windowH;
}

// 滚动容器内子元素的 frame 是内容坐标（EUI 用 render transform 平移：
// runtime_state_bindings.h scrollTransformForElement），命中/几何断言须换算成窗口
// 坐标，否则偏移后按 id 点击会算出内容坐标点（need171：侧栏滚到底后「关于」点击点
// 落在窗高之外）。逐级祖先累减 scrollContentSourceId 所指滚动根的 scrollOffset
//（dump 的 scroll=offset/max 同源，驱动侧唯一实时口径）。
inline core::LayoutRect effectiveFrame(const TreeView& tree,
                                       const core::dsl::Element& el) {
    core::LayoutRect r = el.frame;
    std::string cur = el.id;
    for (int guard = 0; guard < 64; ++guard) {
        const auto p = tree.parentOf.find(cur);
        if (p == tree.parentOf.end()) break;
        cur = p->second;
        const auto parent = tree.byId.find(cur);
        if (parent == tree.byId.end()) continue;
        const std::string& sourceId = parent->second->scrollContentSourceId;
        if (sourceId.empty()) continue;
        const auto src = tree.byId.find(sourceId);
        if (src == tree.byId.end()) continue;
        if (parent->second->scrollHorizontal) {
            r.x -= src->second->scrollOffset;
        } else {
            r.y -= src->second->scrollOffset;
        }
    }
    return r;
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
// nonempty = 「存在任意可见文本」（不比较 text 字段）：终端页「shell 首屏已到达」
// 的跨平台就绪判据——不依赖 %, $, PS 等提示符字形（场景原 cursor 判据在 vterm
// 构造期预填空白 cells 后恒真，测不出「有输出」）。
inline bool textPresent(const TreeView& tree, const std::string& resolvedId,
                        TextOp op, const std::string& text) {
    if (op == TextOp::NonEmpty) {
        if (!resolvedId.empty()) {
            const auto it = tree.byId.find(resolvedId);
            return it != tree.byId.end() && textHasVisibleChar(it->second->text);
        }
        for (const core::dsl::Element* leaf : tree.leaves) {
            if (textHasVisibleChar(leaf->text)) return true;
        }
        return false;
    }
    // 空串 equals = 显式「字段为空」判据（need319 清场断言）；仅定点元素可比——
    // 全页扫描下任何空叶子都会命中，恒真无意义，一律 false。
    // 注意：EUI-NEO input 空值时把 placeholder 写进 .text（input.h:495），故
    // "<input>.text" 上此判据恒红——输入框清场请锁可见副作用（如校验红字）。
    if (text.empty() && (op != TextOp::Equals || resolvedId.empty())) return false;
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
