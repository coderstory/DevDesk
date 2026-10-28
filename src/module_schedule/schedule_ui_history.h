// 运行历史抽屉（波 S7b）：计划 vs 实际时刻、状态、耗时、错误、各渠道投递与重试次数。
// 分层同 schedule_ui_edit.h：本头的纯装配逻辑（summarizeDeliveries/assembleHistory/
// loadHistory）不碰 EUI 控件，是无头用例契约面；drawHistoryDrawer（schedule_ui_history.cpp）
// 只做渲染。数据一律经 ScheduleStore 读 schedule_run + schedule_delivery。
//
// 口径：渠道失败只记 schedule_delivery、不改变 run 的 success——抽屉必须同时展示
// run 状态与各渠道投递明细，「run 成功、投递失败」必须可分辨（§4.5/验收 7）。
#pragma once

#include "core/result.h"
#include "schedule_store.h"

#include <eui/dsl.h>

#include <core/theme.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace module::schedule::ui {

// 单渠道投递汇总（schedule_delivery 聚合）：attempts = 该渠道尝试行数，
// retries = attempts - 1（0 尝试 = 0）；每次尝试一行（§4.5/§4.9 重试契约）。
struct ChannelDeliverySummary {
    int64_t channelId = 0;
    int attempts = 0;
    int retries = 0;
    int successCount = 0;                       // status == "success" 的行数
    std::string lastStatus;                     // 最高 attempt 行的状态
    std::optional<int64_t> nextRetryAt;         // 最高 attempt 行的下次重试时刻
};

// 一条运行记录 + 其全部投递明细（计划/实际、状态、耗时、错误 + 渠道汇总）
struct RunHistoryItem {
    RunRecord run;
    std::vector<DeliveryRecord> deliveries;     // attempt 升序
    std::vector<ChannelDeliverySummary> channelSummaries;  // channelId 升序
};

// schedule_delivery 行集 → 渠道汇总（按 channelId 升序，行集 attempt 升序由
// ScheduleStore::listDeliveriesByRun 保证）。
std::vector<ChannelDeliverySummary> summarizeDeliveries(
    const std::vector<DeliveryRecord>& deliveries);

// 装配运行历史：runs（任意顺序）按 planned_at 倒序稳定排序，deliveries 按 run_id
// 归组；limit 截断（≤0 = 空）。同规则同 planned_at 只有一条 run（唯一索引），
// 排序后顺序即列表展示顺序。
std::vector<RunHistoryItem> assembleHistory(const std::vector<RunRecord>& runs,
                                            const std::vector<DeliveryRecord>& deliveries,
                                            int limit);

// 经 ScheduleStore 读 schedule_run + schedule_delivery 装配（规则维度历史；
// 系统项 origin_key 维度的入口由宿主模块决定查询后转 assembleHistory）。
core::Result<std::vector<RunHistoryItem>> loadHistory(ScheduleStore& store,
                                                      int64_t ruleId, int limit);

// ---- 渲染（schedule_ui_history.cpp）----

// 宿主注入：已装配的历史数据（模块侧调 loadHistory 或系统项查询装配）+ 手动重试
// 回调（重跑 run；由引擎/宿主实现，抽屉只转发）+ 关闭。
struct HistoryDrawerHost {
    std::vector<RunHistoryItem> items;
    std::function<void(int64_t runId)> retryRun;  // 可为空 = 隐藏重试按钮
    std::function<void()> close;
};

// 抽屉自身状态（打开态 + 当前选中的 run id，0 = 未选中，显示总览列表）
struct HistoryDrawerState {
    bool open = false;
    int64_t selectedRunId = 0;
};

// 运行历史抽屉绘制（关闭态也常驻构建）。元素 id 以 "schedule.history." 为前缀。
void drawHistoryDrawer(eui::Ui& ui, const eui::Screen& screen, const core::theme::Tokens& t,
                       HistoryDrawerState& st, const HistoryDrawerHost& host);

}  // namespace module::schedule::ui
