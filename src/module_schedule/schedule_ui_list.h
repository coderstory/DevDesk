// src/module_schedule/schedule_ui_list.h
// 定时页（池140 瀑布流重设计）：页头（标题/运行历史/新建唯一入口）固定 +
// 「系统定时/用户定时」块面 TAB（wtab）+ 唯一滚动区内瀑布流卡片（列数随宽
// 断点）。卡面零操作钮：用户/归档卡整卡可点开编辑弹窗（启停/删除收进弹窗），
// 系统卡只读不可点。一次性已执行折叠区并入用户 TAB 滚动区尾部 + 运行历史抽屉。
//
// 分层：装配/摘要/时刻文本是纯函数（无 IO 无时钟，「现在」由调用方注入），
// 测试直接驱动；ScheduleListUi 持有 store/providers 引用做 IO 与绘制。
// 系统项只读展示、执行归来源模块（§4.2）：系统卡无开关、无编辑。
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
#include <set>
#include <string>
#include <vector>

namespace module::schedule {

class ScheduleEngine;  // 前向声明（手动重跑入口；引擎生命周期长于列表 UI）

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

// 瀑布流列数断点（池140 §3.1，按内容列宽 W）：<520 → 1、<1080 → 2、
// <1660 → 3、其余 4。纯函数供无头用例锁断点。
int columnCountForWidth(float contentW);

// ---- 列表页 UI（模块持有；compose 由模块 compose 转发）----

class ScheduleListUi {
public:
    // tasks 非空时 run_task 下拉候选取 taskKeys()（v1 生产注册表为空 → 下拉空态）。
    // 引擎先于本类构造（注册只在启动装配段发生），传 &engine->taskRegistry()。
    // engine 非空时接上运行历史抽屉的「重试」按钮（手动重跑失败/错过的 run）。
    ScheduleListUi(ScheduleStore& store, ScheduleProviderRegistry& providers,
                   InternalTaskRegistry* tasks = nullptr,
                   ScheduleEngine* engine = nullptr);

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
    // 分类 TAB（0 = 系统定时、1 = 用户定时）。切换即置滚动回顶与 120ms 淡入
    // 标记，下一帧 compose 消费（setScrollOffset 帧末取件 + opacity 过渡）。
    int activeTab() const { return activeTab_; }
    void setTab(int tab);

    // 运行历史抽屉开合态与数据（无头用例断言面；数据经 store 读取后由
    // assembleHistory 装配；engine 未接入时抽屉隐藏重试按钮）
    bool historyOpen() const { return historyState_.open; }
    const std::vector<ui::RunHistoryItem>& historyItems() const {
        return historyHost_.items;
    }
    // 手动重跑：按 run 反查规则 → 引擎按「现在」再触发一次。
    // 失败标 error_（抽屉保持打开），成功 reload 让新记录出现在历史里。
    void retryHistoryRun(int64_t runId);

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
    ScheduleEngine* engine_ = nullptr;       // 同上；手动重跑入口
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
    // 分类 TAB 状态与一次性标记（compose 消费后清零）：滚动回顶走
    // ui.setScrollOffset 帧末取件；淡入走 opacity 目标值翻转 + 120ms 过渡。
    int activeTab_ = 0;
    bool pendingScrollReset_ = false;
    bool tabFadePending_ = false;
    // 卡 hover 集合（池140）：透明命中面 onHover 写、卡面 tint/按下果冻每帧读。
    // 新卡面零可点控件，命中面 = 整卡唯一 hover 源，单集合即可（旧双集合随
    // 卡内操作钮一起删除）。EUI 兄弟命中逆序遍历：命中面必须建在卡 stack 内
    // **最后**（最上层先命中），否则指针事件被静态内容截走（bug 150 根因口径）。
    std::set<std::string> hoveredCardIds_;
};

} // namespace module::schedule
