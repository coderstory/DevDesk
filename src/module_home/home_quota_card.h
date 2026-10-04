// src/module_home/home_quota_card.h
// 首页余量卡片的展示层（纯绘制 + 纯函数；数据全部来自中立桥 quota_home_bridge.h）。
// 单独成 TU 是为了让几何/色带/倒计时等判定可被无头断言。
// 本层零业务判断：五态判据、错误文案、窗口裁剪都在桥里，这里只回答
// 「这一行画不画条」（showsQuotaBar）与「画成什么颜色」（tintForUsedPercent）。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/theme.h"
#include "quota_home_bridge.h"
#include <eui/dsl.h>

namespace module::home::quota_card {

using module::claude::quota::QuotaCardState;
using module::claude::quota::QuotaHomeRow;

// 色带三档：≥90 红 / ≥70 橙 / 其余绿。
enum class Tint { Green, Orange, Red };

// 唯一的「画不画条」判据。False 的四种状态一律不画条——这是「假 0% 红线」的
// 代码形态：把「画」挡在门外，用例可逐态断言。语义 = 这一行曾成功查到过有效值。
bool showsQuotaBar(QuotaCardState state);

Tint tintForUsedPercent(double usedPercent);

// 百分比文案："62%" / "62.4%" / "0%"。≥10 取整，<10 保留 1 位小数——小数字段上
// 「0%」与「0.4%」是满格条与空格条的区别。
std::string formatQuotaPercent(double usedPercent);

// 余额文案："余额 12.34 CNY"（金额原样，不换算、不四舍五入成整数）。
std::string formatQuotaBalance(double remaining, const std::string& currency);

// 重置倒计时：解析 ISO8601 前缀算相对 nowSec 的文案（"3 天后重置" 等）。
// 解析失败返回空串——调用方据此不画倒计时，绝不用「--」冒充「没读懂」。
std::string formatQuotaResetCountdown(const std::string& resetsAtIso,
                                     int64_t nowSec);

// 行高（纯函数；卡片高度由行高求和得出，行数变化 → 卡高自适应）。
float quotaRowHeight(const QuotaHomeRow& row);

// 卡片几何（纯函数）：bodyH = min(所有行高之和 + 行间距, maxBodyHeight)，超出由
// scrollView 出滚动条。maxBodyHeight 由调用方按窗口剩余空间给。
struct QuotaCardMetrics {
    float cardHeight = 0.0f;
    float bodyHeight = 0.0f;
    bool overflows = false;   ///< 内容高于视口（= 会出滚动条）
};

// 卡片固定外壳高：padding×2(32) + 标题行(28) + 标题下间距(10)。
constexpr float kQuotaCardChromeH = 70.0f;
// 行内容视口下限：可用高度低于它时整张不画（只有标题的卡不如不画）。
constexpr float kQuotaCardMinBodyH = 96.0f;
constexpr float kQuotaCardMaxBodyH = 288.0f;
constexpr float kQuotaCardEmptyH = 40.0f;

QuotaCardMetrics computeQuotaCardMetrics(const std::vector<QuotaHomeRow>& rows,
                                         float contentWidth,
                                         float maxBodyHeight = kQuotaCardMaxBodyH);

// 首页高度预算（纯函数）：提出来是为了让「矮窗下余量卡画不画」的阈值可被用例
// 钉死，而不是散在 compose lambda 里靠人肉心算。
struct HomeQuotaBudget {
    // 余量卡行内容可用高度。< kQuotaCardMinBodyH 时 drawsQuotaCard == false。
    float quotaBodyHeight = 0.0f;
    // 为腾出余量卡空间，图表卡被压缩后的高度（>= kHomeChartMinH）。
    float chartHeight = 0.0f;
    // 首页仍装不下：余量卡整张不画，chartHeight 停在压缩下限。
    bool pageStillOverflows = false;
    bool drawsQuotaCard = false;
};

// 首页「今日 token 图表卡」的标称高度（不压缩时）。
constexpr float kHomeChartH = 260.0f;
// 图表卡被压缩后的下限；再矮就不是图表了，宁可不画余量卡也不压成一条缝。
constexpr float kHomeChartMinH = 160.0f;
constexpr float kHomeActionsH = 44.0f;
constexpr float kHomeWelcomeH = 72.0f;

// 按窗口可用高度分配「余量卡行内容高」与「图表卡高」。规则（先到先得）：
// 余量卡行内容至少 kQuotaCardMinBodyH 否则不画；图表卡从 kHomeChartH 往下让位，
// 最多压到 kHomeChartMinH，压到底仍不够则 drawsQuotaCard=false、pageStillOverflows=true。
HomeQuotaBudget computeHomeQuotaBudget(float screenHeight, float statCardHeight,
                                       float pad, float gap);

// 卡片回调（compose 每帧装配）。
struct Hooks {
    std::function<void()> refreshAll;               // [刷新]
    std::function<void(int64_t profileId)> openConfig;  // 「去配置」
    // 读库失败（不是「没有数据」）——渲染层据此显示「加载失败」而不是空态。
    std::string loadError;
};

void drawQuotaCard(eui::Ui& ui, const core::theme::Tokens& t, float contentWidth,
                   const std::vector<QuotaHomeRow>& rows, const Hooks& hooks,
                   float maxBodyHeight = kQuotaCardMaxBodyH);

}  // namespace module::home::quota_card
