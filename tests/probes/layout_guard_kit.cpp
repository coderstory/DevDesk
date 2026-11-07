// layout_guard_kit.cpp — 布局病守卫断言库实现（接口口径见 layout_guard_kit.h）
#include "probes/layout_guard_kit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace layout_guard {
namespace {

bool isAncestorChain(const std::string& maybeAncestorId,
                     const std::unordered_map<std::string, std::string>& parentOf,
                     const std::string& startId) {
    for (std::string id = startId; !id.empty();) {
        if (id == maybeAncestorId) return true;
        const auto it = parentOf.find(id);
        id = it == parentOf.end() ? std::string() : it->second;
    }
    return false;
}

/// 沿祖先链求 z 上界（Element 无 parent 指针，harness 同款自建映射）。
int chainZUpper(const std::unordered_map<std::string, const core::dsl::Element*>& byId,
                const std::unordered_map<std::string, std::string>& parentOf,
                const std::string& leafId) {
    int upper = 0;
    for (std::string id = leafId; !id.empty();) {
        const auto it = byId.find(id);
        if (it != byId.end()) {
            upper = std::max(upper, it->second->zIndex);
            upper = std::max(upper, it->second->subtreeMaxZIndex);
        }
        const auto p = parentOf.find(id);
        id = p == parentOf.end() ? std::string() : p->second;
    }
    return upper;
}

void fill(Violation* out, const char* rule, const std::string& subject,
          const std::string& detail) {
    if (out != nullptr) {
        out->rule = rule;
        out->subject = subject;
        out->detail = detail;
    }
}

std::string f1(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(v));
    return buf;
}

void walk(const core::dsl::Element& node, const std::string& parentId,
          std::unordered_map<std::string, std::string>& parentOf,
          std::unordered_map<std::string, const core::dsl::Element*>& byId,
          std::vector<const core::dsl::Element*>* leaves,
          std::vector<std::string>* allIds) {
    parentOf[node.id] = parentId;
    if (!node.id.empty()) byId[node.id] = &node;
    if (allIds != nullptr && !node.id.empty()) allIds->push_back(node.id);
    if (node.children.empty()) {
        if (leaves != nullptr) leaves->push_back(&node);
        return;
    }
    for (const auto& child : node.children) {
        walk(*child, node.id, parentOf, byId, leaves, allIds);
    }
}

}  // namespace

Box boxOf(const core::dsl::Element& e) {
    return Box{e.frame.x, e.frame.y,
               e.frame.width, e.frame.height};
}

std::vector<NamedBox> leafBoxes(core::dsl::Ui& ui, const std::string& rootId,
                                bool excludeHitZones) {
    std::vector<NamedBox> out;
    std::unordered_map<std::string, std::string> parentOf;
    std::unordered_map<std::string, const core::dsl::Element*> byId;
    std::vector<const core::dsl::Element*> leaves;
    if (rootId.empty()) {
        for (const auto& root : ui.roots()) {
            walk(*root, "", parentOf, byId, &leaves, nullptr);
        }
    } else {
        core::dsl::Element* root = ui.find(rootId);
        if (root == nullptr) return out;
        walk(*root, "", parentOf, byId, &leaves, nullptr);
    }
    out.reserve(leaves.size());
    for (const core::dsl::Element* leaf : leaves) {
        if (leaf->opacity <= 0.0f) continue;
        if (leaf->frame.width <= 0.0f || leaf->frame.height <= 0.0f) continue;
        if (excludeHitZones &&
            leaf->id.size() >= 4 &&
            leaf->id.compare(leaf->id.size() - 4, 4, ".hit") == 0) {
            continue;
        }
        out.push_back(NamedBox{boxOf(*leaf), leaf->id, leaf->text});
    }
    return out;
}

std::vector<std::string> findIds(core::dsl::Ui& ui, const std::string& idPrefix) {
    std::unordered_map<std::string, std::string> parentOf;
    std::unordered_map<std::string, const core::dsl::Element*> byId;
    std::vector<std::string> ids;
    for (const auto& root : ui.roots()) {
        walk(*root, "", parentOf, byId, nullptr, &ids);
    }
    if (idPrefix.empty()) return ids;
    std::vector<std::string> out;
    for (const std::string& id : ids) {
        if (id.compare(0, idPrefix.size(), idPrefix) == 0) out.push_back(id);
    }
    return out;
}

bool contentWithinBottom(const Box& card, const Box& content, float marginPx,
                         Violation* out) {
    const float limit = card.bottom() - marginPx;
    if (content.bottom() <= limit) return true;
    fill(out, "字号溢出", "", "内容底 " + f1(content.bottom()) + " > 卡底-余量 " +
                                  f1(limit) + "（卡 y=" + f1(card.y) + " h=" +
                                  f1(card.h) + "，余量 " + f1(marginPx) + "）");
    if (out != nullptr) out->subject = "content@" + f1(content.x);
    return false;
}

bool rowContentSpread(const Box& row, const std::vector<Box>& leaves,
                      float minRatio, Violation* out) {
    if (leaves.empty() || row.w <= 0.0f) return true;  // 空态交存在性断言
    // 合并 x 区间求有效覆盖宽
    std::vector<std::pair<float, float>> spans;
    spans.reserve(leaves.size());
    for (const Box& b : leaves) {
        const float lo = std::max(b.x, row.x);
        const float hi = std::min(b.right(), row.right());
        if (hi > lo) spans.emplace_back(lo, hi);
    }
    std::sort(spans.begin(), spans.end());
    float covered = 0.0f;
    float curLo = 0.0f, curHi = 0.0f;
    bool open = false;
    for (const auto& s : spans) {
        if (!open) {
            curLo = s.first;
            curHi = s.second;
            open = true;
        } else if (s.first <= curHi) {
            curHi = std::max(curHi, s.second);
        } else {
            covered += curHi - curLo;
            curLo = s.first;
            curHi = s.second;
        }
    }
    if (open) covered += curHi - curLo;
    const float ratio = covered / row.w;
    if (ratio >= minRatio) return true;
    fill(out, "双端撑开", "", "内容有效覆盖 " + f1(covered) + "/" + f1(row.w) +
                                  " = " + f1(ratio * 100.0f) + "% < 阈值 " +
                                  f1(minRatio * 100.0f) + "%（行 y=" + f1(row.y) + "）");
    if (out != nullptr) out->subject = "row@" + f1(row.y);
    return false;
}

bool noHairlineBar(const Box& bar, float referenceWidth, Violation* out,
                   float minBarH, float dividerMaxH) {
    if (referenceWidth <= 0.0f || bar.w < referenceWidth * 0.9f) return true;
    if (bar.h <= dividerMaxH) return true;  // 1px 功能分隔线豁免（R6，规范 L7）
    if (bar.h >= minBarH) return true;
    fill(out, "发丝条", "", "全宽条 " + f1(bar.w) + "x" + f1(bar.h) +
                                " 高度 < 下限 " + f1(minBarH) + " 且 > 豁免 " +
                                f1(dividerMaxH) + "（y=" + f1(bar.y) + "）");
    if (out != nullptr) out->subject = "bar@" + f1(bar.y);
    return false;
}

bool overlayAnchored(const Box& trigger, const Box& overlay, float maxOffsetX,
                     float maxOffsetY, Violation* out) {
    const float dx = overlay.centerX() - trigger.centerX();
    const float dy = overlay.centerY() - trigger.centerY();
    if (std::fabs(dx) <= maxOffsetX && std::fabs(dy) <= maxOffsetY) return true;
    fill(out, "浮层锚定", "", "弹层中心偏移 dx=" + f1(dx) + " / dy=" + f1(dy) +
                                  " 超上限 ±" + f1(maxOffsetX) + "/±" + f1(maxOffsetY));
    if (out != nullptr) out->subject = "overlay@" + f1(overlay.x);
    return false;
}

bool overlayAttached(const Box& trigger, const Box& overlay, float maxGapY,
                     float minCover, Violation* out) {
    const float gapBelow = overlay.y - trigger.bottom();
    const float gapAbove = trigger.y - overlay.bottom();
    const float coverStart = std::max(overlay.x, trigger.x);
    const float coverEnd = std::min(overlay.right(), trigger.right());
    const float cover = std::max(0.0f, coverEnd - coverStart);
    const bool covered = trigger.w <= 0.0f || cover / trigger.w >= minCover;
    const bool attached = (gapBelow >= -4.0f && gapBelow <= maxGapY) ||
                          (gapAbove >= -4.0f && gapAbove <= maxGapY);
    if (attached && covered) return true;
    fill(out, "浮层锚定", "", "附着式弹层 gapBelow=" + f1(gapBelow) + " gapAbove=" +
                                  f1(gapAbove) + "（各容许 [-4," + f1(maxGapY) +
                                  "]）水平覆盖 " +
                                  f1(trigger.w > 0.0f ? cover / trigger.w * 100.0f : 0.0f) +
                                  "%（要求 ≥ " + f1(minCover * 100.0f) + "%）");
    if (out != nullptr) out->subject = "overlay@" + f1(overlay.x);
    return false;
}

bool occludedPure(const Box& text, int textZUpper, const Box& overlay,
                  int overlayZUpper, bool related) {
    if (related) return false;
    const bool intersect = text.x < overlay.right() && overlay.x < text.right() &&
                           text.y < overlay.bottom() && overlay.y < text.bottom();
    if (!intersect) return false;
    return overlayZUpper >= textZUpper;  // 等值 z = 弹层后 compose 绘制在上
}

bool noOcclusion(core::dsl::Ui& ui, const std::string& keyTextId,
                 const std::string& overlayId, Violation* out) {
    core::dsl::Element* text = ui.find(keyTextId);
    core::dsl::Element* overlay = ui.find(overlayId);
    if (overlay == nullptr) return true;  // 无遮挡物
    if (text == nullptr) {
        fill(out, "z序遮挡", keyTextId, "守卫目标元素缺失（id 变更？断言勿静默失效）");
        return false;
    }
    std::unordered_map<std::string, std::string> parentOf;
    std::unordered_map<std::string, const core::dsl::Element*> byId;
    for (const auto& root : ui.roots()) {
        walk(*root, "", parentOf, byId, nullptr, nullptr);
    }
    const bool related = isAncestorChain(text->id, parentOf, overlay->id) ||
                         isAncestorChain(overlay->id, parentOf, text->id);
    const Box textBox = boxOf(*text);
    const Box overlayBox = boxOf(*overlay);
    if (overlay->opacity <= 0.5f) return true;  // 隐没态不判
    if (!occludedPure(textBox, chainZUpper(byId, parentOf, text->id),
                      overlayBox, chainZUpper(byId, parentOf, overlay->id),
                      related)) {
        return true;
    }
    fill(out, "z序遮挡", keyTextId, "被 '" + overlayId + "' 覆盖（overlay z 上界 " +
                                        std::to_string(chainZUpper(byId, parentOf, overlay->id)) +
                                        " ≥ 文本上界 " +
                                        std::to_string(chainZUpper(byId, parentOf, text->id)) +
                                        "，opacity " + f1(overlay->opacity) + "）");
    return false;
}

std::vector<Violation> scanOccluders(core::dsl::Ui& ui, const std::string& rootId) {
    std::vector<Violation> out;
    std::unordered_map<std::string, std::string> parentOf;
    std::unordered_map<std::string, const core::dsl::Element*> byId;
    std::vector<const core::dsl::Element*> leaves;
    const auto scanRoot = [&](const core::dsl::Element& root) {
        std::unordered_map<std::string, std::string> p2 = parentOf;
        std::unordered_map<std::string, const core::dsl::Element*> b2 = byId;
        std::vector<const core::dsl::Element*> l2;
        walk(root, "", p2, b2, &l2, nullptr);
        parentOf = std::move(p2);
        byId = std::move(b2);
        leaves.insert(leaves.end(), l2.begin(), l2.end());
    };
    if (rootId.empty()) {
        for (const auto& root : ui.roots()) scanRoot(*root);
    } else {
        core::dsl::Element* root = ui.find(rootId);
        if (root == nullptr) return out;
        scanRoot(*root);
    }
    // 遮挡候选：zIndex > 0 的可见元素（弹层链根）；候选全集 = 整棵树
    std::vector<const core::dsl::Element*> candidates;
    std::vector<std::string> allIds = findIds(ui, "");
    for (const std::string& id : allIds) {
        const auto it = byId.find(id);
        if (it == byId.end()) continue;
        const core::dsl::Element* e = it->second;
        if (e->zIndex > 0 && e->opacity > 0.5f) candidates.push_back(e);
    }
    for (const core::dsl::Element* text : leaves) {
        if (text->text.empty()) continue;  // 只守关键文本
        for (const core::dsl::Element* cand : candidates) {
            const bool related = isAncestorChain(text->id, parentOf, cand->id) ||
                                 isAncestorChain(cand->id, parentOf, text->id);
            if (occludedPure(boxOf(*text), chainZUpper(byId, parentOf, text->id),
                             boxOf(*cand), chainZUpper(byId, parentOf, cand->id),
                             related)) {
                Violation v;
                fill(&v, "z序遮挡", text->id,
                     "文本被 '" + cand->id + "' 覆盖（zIndex " +
                         std::to_string(cand->zIndex) + "）");
                out.push_back(v);
            }
        }
    }
    return out;
}

}  // namespace layout_guard
