#pragma once
// layout_guard_kit.h — 布局病守卫断言库（池 need #221，2026-11-07）
//
// 目的：同类布局病出生即拦，不再等老板目检。五类病各一断言，页面 probe 按
// 「composeFrame → 重新 find（clue 125：元素每帧重建）→ 取 Box → 调断言」接入。
//
// 口径来源：
//   ① 字号溢出  = bug187 组C2 / bug215 组C3 断言写法（140% 档内容底 ≤ 卡底-2）
//   ② 双端撑开  = 池 #154/#192「行中间全是空白」：内容合并覆盖宽 / 行宽低于阈值即病
//   ③ 发丝条    = docs/design/2026-11-07-列表统一规范.md L7（R6：1px 功能分隔线豁免，
//                 2px < h < 10px 的全宽条即违例）
//   ④ 浮层锚定  = tooltip 批 ±40/±60 断言（弹层中心相对触发元素中心的偏移上限）
//   ⑤ z 序遮挡  = 池 #168「悬浮层遮关键文本」。L1 近似判据，见 noOcclusion 注释。
//
// 接入约定（新页面接入三步）：
//   1. composeFrame() 后用 ui->find / findIds 取目标元素（不要缓存上一帧指针）；
//   2. 每条断言传 Violation* 收违例详情，返回 false 即病发；
//   3. 已知未修的病登记到 probe 的 known-issue 表（带池号），门禁保持绿、病在池里追。

#include "core/dsl_runtime.h"

#include <string>
#include <vector>

namespace layout_guard {

/// 逻辑像素矩形（与 core::dsl::Element::frame 同空间）。
struct Box {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    float centerX() const { return x + w * 0.5f; }
    float centerY() const { return y + h * 0.5f; }
};

/// 一条违例。rule = 病名（与上面五类对应），subject = 元素 id，
/// detail = 人类可读数值详情（直接可进证据日志 / 池报告）。
struct Violation {
    std::string rule;
    std::string subject;
    std::string detail;
};

/// 元素 → Box（取 frame；调用方保证元素是本帧 compose 后重新 find 的）。
Box boxOf(const core::dsl::Element& e);

struct NamedBox {
    Box box;
    std::string id;
    std::string text;  // 元素文本（布局断言通常只认有文本的可见内容叶）
};

/// rootId 子树内所有「可见叶子」的盒（opacity > 0 且 w/h > 0）。
/// excludeHitZones = true（缺省）时跳过 id 以 ".hit" 结尾的命中区——命中区是
/// 透明交互层，会虚增覆盖宽 / 内容底，布局断言不应计入；需要命中区几何时用
/// ui->find 直取。rootId 为空串 = 遍历整棵树的所有根。
std::vector<NamedBox> leafBoxes(core::dsl::Ui& ui, const std::string& rootId,
                                bool excludeHitZones = true);

/// 整棵树内 id 以 idPrefix 开头的元素 id 清单（用于页面级发现，如
/// "release.tasks.card."）。空串 = 全部 id。
std::vector<std::string> findIds(core::dsl::Ui& ui, const std::string& idPrefix);

// ── ① 字号溢出 ─────────────────────────────────────────────────────────────
/// 卡内最底内容行底 ≤ 卡底 - marginPx（非 100% 字号档用，bug187 C2 口径）。
/// 注意：卡内含滚动容器时内容叶子的底可以合法超出卡底（滚动裁剪），调用方
/// 应先滚到底再断言（memory_probe 组 C2 同款），或只对无滚动卡使用。
bool contentWithinBottom(const Box& card, const Box& content, float marginPx,
                         Violation* out = nullptr);

// ── ② 双端撑开 ─────────────────────────────────────────────────────────────
/// 行卡可见内容沿 x 的合并覆盖宽 / 行宽 ≥ minRatio（池 #154/#192「中间全是
/// 空白」：左右各一小块、中间大片留白 → 覆盖率低 → 病发）。
/// leaves 为空 = 无内容可判，跳过（返回 true）——空态由存在性断言负责。
bool rowContentSpread(const Box& row, const std::vector<Box>& leaves,
                      float minRatio, Violation* out = nullptr);

// ── ③ 发丝条 ───────────────────────────────────────────────────────────────
/// 全宽条形元素高度下限：bar.w ≥ 0.9×referenceWidth 视为「全宽」参判；
/// 非 full-width 元素不判（返回 true）。
/// h ≤ dividerMaxH（缺省 2px）= 1px 功能分隔线，R6 豁免口径（规范 L7）；
/// dividerMaxH < h < minBarH（缺省 10px）= 发丝装饰条，违例。
bool noHairlineBar(const Box& bar, float referenceWidth, Violation* out = nullptr,
                   float minBarH = 10.0f, float dividerMaxH = 2.0f);

// ── ④ 浮层锚定 ─────────────────────────────────────────────────────────────
/// 弹层/tooltip 中心相对触发元素中心的偏移上限：|dx| ≤ maxOffsetX 且
/// |dy| ≤ maxOffsetY（tooltip 批 ±40/±60 口径）。
bool overlayAnchored(const Box& trigger, const Box& overlay,
                     float maxOffsetX = 40.0f, float maxOffsetY = 60.0f,
                     Violation* out = nullptr);

/// 附着式弹层（dropdown / datepicker 等「贴着触发元素弹出」族）：弹层顶缘距
/// 触发元素底缘 gapBelow ∈ [-4, maxGapY]（贴下弹）**或**弹层底缘距触发元素
/// 顶缘 gapAbove ∈ [-4, maxGapY]（贴上弹，EUI fork 有 dropdown openUp），
/// 且弹层水平覆盖触发元素 ≥ minCover。
/// 与 overlayAnchored（tooltip 中心距口径）互补：日历/下拉面板天然比触发器
/// 高得多，中心距口径会误报（run1 实测 dy=172 假红）。
bool overlayAttached(const Box& trigger, const Box& overlay,
                     float maxGapY = 40.0f, float minCover = 0.5f,
                     Violation* out = nullptr);

// ── ⑤ z 序遮挡（L1 近似，可单测的纯几何核） ────────────────────────────────
/// 判据：keyText 与 overlay 几何相交 + 非祖先关系 + overlay 可见（opacity 代理，
/// 在 ui 层判定，纯核不收）+ overlay 子树 z 上界 ≥ keyText 上界 → 遮挡病发。
/// 已知局限（L1）：等值 z 且 overlay 在 DOM 序靠前时理论上漏报；等值 z 时弹层
/// 恒后 compose（DOM 序靠后 = 绘制在上），故常态覆盖本仓已知病形。产品侧提供
/// 精确绘制层信息后升级判据，接口不变。
/// related = 两者是祖先 / 同一元素（调用方排除），纯核不做树遍历。
bool occludedPure(const Box& text, int textZUpper, const Box& overlay,
                  int overlayZUpper, bool related);

/// ui 层封装：keyTextId 被 overlayId 遮挡 → 病发。任一元素缺失：overlay 缺失
/// = 无遮挡物（true）；keyText 缺失 = 违例（守卫对象消失要显式暴露，防断言
/// 因改 id 静默失效）。
bool noOcclusion(core::dsl::Ui& ui, const std::string& keyTextId,
                 const std::string& overlayId, Violation* out = nullptr);

/// 页面级一行接入：rootId 子树内所有**有文本的叶子**被任何「zIndex > 0 的可见
/// 非祖先元素」遮挡 → 违例清单（静止态跑；弹层开启态的合法覆盖不要跑本扫描，
/// 否则合法浮层全被误报）。
std::vector<Violation> scanOccluders(core::dsl::Ui& ui, const std::string& rootId);

}  // namespace layout_guard
