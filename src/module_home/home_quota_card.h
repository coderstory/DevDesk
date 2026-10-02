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