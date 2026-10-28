// src/module_schedule/schedule_ui_list.h
// 定时列表页（波 S7a）：系统定时段（锁图标只读）+ 用户定时段（名称/规则摘要/
// 下次时刻/上次结果/启停开关）+ 一次性已执行折叠区（默认折叠）+ 运行历史抽屉入口。
// 抽屉本体 S7b 接，本波只有入口按钮位。
//
// 分层：装配/摘要/时刻文本是纯函数（无 IO 无时钟，「现在」由调用方注入），
// 测试直接驱动；ScheduleListUi 持有 store/providers 引用做 IO 与绘制。
// 系统项只读展示、执行归来源模块（§4.2）：系统行无开关、无编辑。
#pragma once

#include "module_schedule/schedule_provider.h"
#include "module_schedule/schedule_store.h"

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
    ScheduleListUi(ScheduleStore& store, ScheduleProviderRegistry& providers);

    // 重装配：拉全部规则 + 系统视图 + 每条用户规则的最近一次 run。
    // 读库失败降级空列表 + errorText()（整页不因库故障崩溃）。
    void reload(int64_t nowSec);

    // 开关切换落库（compose 的 toggleSwitch onChange 调用点）。
    // 失败 → fail（调用方弹提示），列表状态不变、不回写假开关。
    core::Result<void> setRuleEnabled(int64_t ruleId, bool enabled);

    const ListAssembly& assembly() const { return assembly_; }
    const std::string& errorText() const { return error_; }
    bool archiveExpanded() const { return archiveExpanded_; }
    void setArchiveExpanded(bool expanded) { archiveExpanded_ = expanded; }

    // 测试缝：回默认态（归档折叠 + 清错误文案；生产不调用）
    void resetForTest();

    void compose(eui::Ui& ui, const eui::Screen& screen);

private:
    ScheduleStore& store_;
    ScheduleProviderRegistry& providers_;
    ListAssembly assembly_;
    std::string error_;
    bool archiveExpanded_ = false;  // 归档折叠区默认折叠（§4.4/验收 4）
};

} // namespace module::schedule
