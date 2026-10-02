// src/module_home/home_quota_card.h
// G7：首页余量卡片的**展示层**（纯绘制 + 纯函数；数据全部来自中立桥
// src/quota_home_bridge.h 的 QuotaHomeRow）
//
// 为什么单独一个 TU 而不是塞进 home_module.cpp：(1) home_module.cpp 已 900+
// 行，再塞 300 行会让「统计网格的几何」和「余量行的几何」互相淹没；
// (2) 色带边界 / 百分比格式 / 倒计时 / 画不画条这些判定必须是**可无头断言的
// 纯函数**，塞进 .cpp 的匿名命名空间就钉不住；(3) 绘制层不碰 DB、不碰 Router。
//
// 本层**零业务判断**：五态判据在桥里（quotaCardState），错误文案在桥里，
// 窗口裁剪在桥里。这里只回答两个问题：这一行画不画条（showsQuotaBar）、
// 画成什么颜色（tintForUsedPercent）。
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

// 色带三档（spec §5.1：≥90 红 / ≥70 橙 / 其余绿）
enum class Tint { Green, Orange, Red };

// **唯一的「画不画条」判据**。False 的四种状态一律不画进度条——
// 这一条就是「假 0% 红线」的代码形态：不是「渲染时注意别画」，而是有一个
// 纯函数把「画」这件事挡在门外，用例可以对它逐态断言。
//
// 语义 = 「这一行有没有**曾经成功查到过**的有效值」。Unconfigured 哪怕留着
// 历史结果也不画：那不是当前配置的有效值（配都沒配）。
bool showsQuotaBar(QuotaCardState state);

// 色带（纯函数，可边界断言）。阈值口径 **≥90 红 / ≥70 橙 / 其余绿**。
Tint tintForUsedPercent(double usedPercent);

// 百分比文案："62%" / "62.4%" / "0%"。≥10 取整，<10 保留 1 位小数——
// 小数字段上「0%」和「0.4%」是天壤之别，整数化会把「几乎没用过」和「完全
// 没用过」画成同一个满格条。
std::string formatQuotaPercent(double usedPercent);

// 余额文案："余额 12.34 CNY"（金额原样，不换算、不四舍五入成整数）。
std::string formatQuotaBalance(double remaining, const std::string& currency);

// 重置倒计时：解析 ISO8601 的 "YYYY-MM-DDTHH:MM:SS" 前缀，算出相对 nowSec 的
// 倒计时文案："3 天后重置" / "5 小时后重置" / "即将重置" / "已过期"。
// **解析失败（空串 / 格式不符）返回空串** —— 调用方据此不画倒计时节点，
// 绝不用「--」冒充（那会被用户读成「没有重置时间」，而真相是「我们没读懂」）。
std::string formatQuotaResetCountdown(const std::string& resetsAtIso,
                                     int64_t nowSec);

// 行高（纯函数；卡片高度由行高求和得出，行数变化 → 卡高自适应）。
float quotaRowHeight(const QuotaHomeRow& row);

// 卡片几何（纯函数）。bodyH = min(所有行高之和 + 行间距, maxBodyHeight)；
// 超出的部分由 scrollView 出滚动条（spec §5.1「高度随 profile 数自适应 +
// scrollView 兜底」）。
//
// `maxBodyHeight` 由调用方按**窗口剩余空间**给（见 home_module 的余量卡段）：
// 首页整页没有外层滚动容器，窗口太矮时卡片会把下方的图表与快捷按钮顶出
// 可视区，所以行内容视口必须封顶到「页面还装得下」的量。
struct QuotaCardMetrics {
    float cardHeight = 0.0f;
    float bodyHeight = 0.0f;
    bool overflows = false;   ///< 内容高于视口（= 会出滚动条）
};

// 卡片固定外壳高：padding×2(32) + 标题行(28) + 标题下间距(10)。
// 行内容高另算。调用方算「窗口还剩多少可用高度」时减掉它。
constexpr float kQuotaCardChromeH = 70.0f;
// 行内容视口下限：可用高度低于它时调用方**整张不画**（画一张只有标题、
// 一行都放不下的卡不如不画，见 home_module 余量卡段的裁决注释）。
constexpr float kQuotaCardMinBodyH = 96.0f;
// 行内容视口上限（内容不满时用它）。
constexpr float kQuotaCardMaxBodyH = 288.0f;
// 空列表时那一行提示文字的高度。
constexpr float kQuotaCardEmptyH = 40.0f;

QuotaCardMetrics computeQuotaCardMetrics(const std::vector<QuotaHomeRow>& rows,
                                         float contentWidth,
                                         float maxBodyHeight = kQuotaCardMaxBodyH);

// ============ 首页高度预算（**纯函数**，G8）============
//
// 为什么把它从 home_module 的 compose lambda 里提出来：那段预算算式原本是
// 写在 lambda 体内的**内联算术**，谁也测不到它——于是「1200×720 下余量卡
// 到底画不画」这件事只能靠人肉心算，而心算与代码漂移过一次（G7 遗留 1 的
// 记载「既有内容 580 / 可用 640」与代码实算对不上）。提成纯函数后：阈值由
// 一条用例钉死，改任一块高度时用例会红。
struct HomeQuotaBudget {
    // 余量卡行内容可用高度。< kQuotaCardMinBodyH 时 drawsQuotaCard == false。
    float quotaBodyHeight = 0.0f;
    // 为腾出余量卡空间，首页图表卡被压缩后的高度（>= kHomeChartMinH）。
    float chartHeight = 0.0f;
    // 首页内容仍然装不下（窗口比设计最小窗还矮）：余量卡整张不画，
    // 且 chartHeight 停在压缩下限。这是**已知未解**的那一段，报告里如实记。
    bool pageStillOverflows = false;
    bool drawsQuotaCard = false;
};

// 首页「今日 token 图表卡」的标称高度（不压缩时）。
constexpr float kHomeChartH = 260.0f;
// 图表卡被压缩后的下限。再矮就不是图表了，宁可不画余量卡也不把图表压成一条缝。
constexpr float kHomeChartMinH = 160.0f;
// 快捷按钮行高。
constexpr float kHomeActionsH = 44.0f;
// 欢迎区高度。
constexpr float kHomeWelcomeH = 72.0f;

// 按窗口可用高度分配「余量卡行内容高」与「图表卡高」。
// 规则（有序，先到先得）：
//   1. 余量卡行内容至少要 kQuotaCardMinBodyH，否则整张不画（画一张只有标题
//      的卡不如不画——见 kQuotaCardMinBodyH 处注释）；
//   2. 图表卡从 kHomeChartH 起往下让位给规则 1，最多压到 kHomeChartMinH；
//   3. 压到下限仍不够 → quotaBodyHeight 仍 < 下限，drawsQuotaCard == false，
//      pageStillOverflows == true。
HomeQuotaBudget computeHomeQuotaBudget(float screenHeight, float statCardHeight,
                                       float pad, float gap);

// 卡片回调（compose 每帧装配；自由函数不捕获 this）
struct Hooks {
    std::function<void()> refreshAll;               // [刷新]
    std::function<void(int64_t profileId)> openConfig;  // 「去配置」
    // 读库失败（不是「没有数据」）——渲染层据此显示「加载失败」而不是空态，
    // 免得把查询失败说成「用户没配」。
    std::string loadError;
};

void drawQuotaCard(eui::Ui& ui, const core::theme::Tokens& t, float contentWidth,
                   const std::vector<QuotaHomeRow>& rows, const Hooks& hooks,
                   float maxBodyHeight = kQuotaCardMaxBodyH);

}  // namespace module::home::quota_card