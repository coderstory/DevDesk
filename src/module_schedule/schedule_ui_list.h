// src/module_schedule/schedule_ui_list.h
// 定时列表页（波 S7a）：系统定时段（锁图标只读）+ 用户定时段（名称/规则摘要/
// 下次时刻/上次结果/启停开关）+ 一次性已执行折叠区（默认折叠）+ 运行历史抽屉
//（S7b 本体，经页头入口开合）。
//
// 分层：装配/摘要/时刻文本是纯函数（无 IO 无时钟，「现在」由调用方注入），
// 测试直接驱动；ScheduleListUi 持有 store/providers 引用做 IO 与绘制。
// 系统项只读展示、执行归来源模块（§4.2）：系统行无开关、无编辑。
#pragma once

#include "module_schedule/action_runner.h"
#include "module_schedule/schedule_provider.h"
#include "module_schedule/schedule_store.h"
#include "module_schedule/schedule_ui_edit.h"
#include "module_schedule/schedule_ui_history.h"

#include <eui/dsl.h>
#include <eui/types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace module::schedule {

// ---- 纯视图模型 ----

// 列表行（系统项与用户项统一形状；isSystem 决定绘制差异）
struct ListRow {
    bool isSystem = false;
    // 用户项字段
    int64_t ruleId = 0;
    std::string name;
    std::string kind;
    std::string summary;                 // 规则摘要（「每两周 周二 09:00」）
    std::optional<int64_t> nextFireAt;   // Unix 秒；无值 = 无下次时刻
    std::optional<int64_t> lastRunAt;    // 上次运行 planned_at；无记录 = nullopt
    std::string lastRunStatus;           // success/failed/...；空 = 无记录
    bool enabled = true;
    bool archived = false;
    // 系统项字段
    std::string source;
    std::string key;
    std::string sourceRoute;
    bool executable = false;
};

struct ListAssembly {
    std::vector<ListRow> systemRows;    // 系统定时段（锁图标只读）
    std::vector<ListRow> userRows;      // 用户定时段（未归档）
    std::vector<ListRow> archivedRows;  // 一次性已执行折叠区（默认折叠，§4.4）
};

// 装配（纯函数）：用户规则 + 系统视图合并；各段内按 next_fire_at 升序，
// 无 next_fire_at 的排后（升序稳定：用户项按 ruleId、系统项按注册序）。
// archivedAt 非空 → archivedRows；软删行由 store.listRules 侧过滤，不在此处理。
ListAssembly assembleList(const std::vector<RuleRecord>& rules,
                          const std::vector<ScheduleItemView>& systemItems);

// 规则摘要（kind + spec_json → 人话，§4.1 各 kind）。解析失败/未知 kind 回退
// 展示 kind 名——摘要精确性不凌驾列表页可用性，绝不抛。
std::string ruleSummary(const RuleRecord& rule);

// 下次时刻展示文本：今天 HH:MM / 明天 HH:MM / MM-DD HH:MM；无值 → 「—」
std::string nextFireText(const std::optional<int64_t>& nextFireAt, int64_t nowSec);

// 上次结果展示文本：「无记录」/「成功 · 10-28 09:00」/「失败 · 10-28 09:00」
std::string lastRunText(const ListRow& row, int64_t nowSec);

// ---- 列表页 UI（模块持有；compose 由模块 compose 转发）----

class ScheduleListUi {
public:
    // tasks 非空时 run_task 下拉候选取 taskKeys()（v1 生产注册表为空 → 下拉空态）。
    // 引擎先于本类构造（注册只在启动装配段发生），传 &engine->taskRegistry()。
    ScheduleListUi(ScheduleStore& store, ScheduleProviderRegistry& providers,
                   InternalTaskRegistry* tasks = nullptr);

    // 重装配：拉全部规则 + 系统视图 + 每条用户规则的最近一次 run。
    // 读库失败降级空列表 + errorText()（整页不因库故障崩溃）。
    void reload(int64_t nowSec);

    // 开关切换落库（compose 的 toggleSwitch onChange 调用点）。
    // 失败 → fail（调用方弹提示），列表状态不变、不回写假开关。
    core::Result<void> setRuleEnabled(int64_t ruleId, bool enabled);

    // 打开运行历史抽屉：经 store 读全部未软删规则（含归档行）的 runs+投递，
    // assembleHistory 装配后置 open。规则清单读失败 → error_ 且不开抽屉；
    // 单规则读失败降级已载入部分并标 error_。
    void openHistory();

    // 新建入口：默认表单（kind=Once / action=remind / 策略=once）开编辑弹窗。
    void openNewRule();
    // 编辑入口：formFromRecord 回填；回填失败标 error_ 不开弹窗。
    void openEditRule(int64_t ruleId);
    // 删除入口：记待删行开确认弹窗（软删默认，勾选「彻底删除」走 purgeRule）。
    void requestDeleteRule(int64_t ruleId);
    // 确认弹窗「删除」落库点：purge=true 物理删（历史 run 保留），否则软删。
    // 失败标 error_ 不关弹窗；成功 reload 并关。
    void confirmPendingDelete();
    void cancelPendingDelete();

    const ListAssembly& assembly() const { return assembly_; }
    const std::string& errorText() const { return error_; }
    bool archiveExpanded() const { return archiveExpanded_; }
    void setArchiveExpanded(bool expanded) { archiveExpanded_ = expanded; }

    // 运行历史抽屉开合态与数据（无头用例断言面；数据经 store 读取后由
    // assembleHistory 装配，retryRun 留空 = 抽屉隐藏重试按钮）
    bool historyOpen() const { return historyState_.open; }
    const std::vector<ui::RunHistoryItem>& historyItems() const {
        return historyHost_.items;
    }

    // 编辑/删除弹窗状态（无头用例断言面）
    bool editOpen() const { return editState_.open; }
    bool deleteConfirmOpen() const { return !pendingDeleteName_.empty(); }

    // 测试缝：回默认态（归档折叠 + 清错误文案 + 收抽屉；生产不调用）
    void resetForTest();
    // 测试缝：直呼 editHost_.saveRule（保存落库点，headless 用例不走绘制层）
    std::string saveRuleForTest(const RuleRecord& rec) {
        return editHost_.saveRule ? editHost_.saveRule(rec) : std::string("no-host");
    }
    // 测试缝：勾选「彻底删除」（headless 用例不点 checkbox）
    void setPendingDeletePurgeForTest(bool purge) { pendingDeletePurge_ = purge; }

    void compose(eui::Ui& ui, const eui::Screen& screen);

private:
    // 弹窗宿主装配：渠道选项（ChannelRegistry 注册键 + DB 行）与 taskKeys 在
    // 每次打开弹窗时刷新（注册只在启动装配段变化，刷新成本可忽略）。
    void refreshEditHost();

    ScheduleStore& store_;
    ScheduleProviderRegistry& providers_;
    InternalTaskRegistry* tasks_ = nullptr;  // 引擎持有，生命周期长于本类
    ListAssembly assembly_;
    std::string error_;
    bool archiveExpanded_ = false;  // 归档折叠区默认折叠（§4.4/验收 4）
    ui::HistoryDrawerState historyState_;  // 抽屉开合态（关闭态也常驻构建）
    ui::HistoryDrawerHost historyHost_;    // 抽屉数据 + close 回调（构造时接好）
    ui::EditDialogState editState_;        // 编辑弹窗（关闭态也常驻构建）
    ui::EditDialogHost editHost_;          // 渠道/任务候选 + saveRule/close（构造时接好）
    int64_t pendingDeleteId_ = 0;          // 非 0 = 删除确认弹窗开着
    std::string pendingDeleteName_;        // 确认弹窗正文用规则名（兼开合标志）
    bool pendingDeletePurge_ = false;      // 「彻底删除」勾选态
};

} // namespace module::schedule
