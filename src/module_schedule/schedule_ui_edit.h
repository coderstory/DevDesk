// 定时调度编辑弹窗（波 S7b）：表单状态 + spec_json 编解码 + 保存前校验 + 实时预览。
// 分层：本头文件的纯逻辑（EditFormState/buildSpecJson/validateAndParse/previewNextFires/
// buildRuleRecord/formFromRecord）不碰任何 EUI 控件，是无头用例的唯一契约面；
// drawEditDialog（schedule_ui_edit.cpp）只做渲染与事件转发，S7a 的模块类经 EditDialogHost
// 注入保存回调与渠道选项挂载本弹窗。
//
// 接口契约冻结（跨波约定）：规则解析/迭代一律调 core::schedule 的 parseRuleSpec /
// nextFire（交接文档 §3 签名），本文件不实现、不缓存任何时刻算法——预览与
// next_fire_at 重算都经 nextFire 迭代产出，禁止另起炉灶。
#pragma once

#include "core/result.h"
#include "core/schedule_rule.h"
#include "schedule_store.h"

#include <eui/dsl.h>

#include <core/theme.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace module::schedule::ui {

// 渠道多选项（编辑弹窗多选列表项）：id 落 RuleRecord.channelIds（schedule_channel 行 id）；
// key 是 ChannelRegistry 注册键（kChannel* 常量或 DB 渠道的稳定键），label 用于显示。
// 列表项来源 = ChannelRegistry + schedule_channel 表（由宿主模块装配后经 EditDialogHost 注入）。
struct ChannelOption {
    int64_t id = 0;
    std::string key;
    std::string label;
};

// 编辑表单状态（纯值类型，无模块/Ui 引用）。日历字段按本地墙钟存（§4.1），
// 时刻解析/校验一律走 core::schedule::parseRuleSpec，本层只做控件形态映射。
struct EditFormState {
    core::schedule::RuleKind kind = core::schedule::RuleKind::Once;
    std::string name;

    // once：二选一（onceUseLocal 决定用 at_local 还是 at）
    bool onceUseLocal = true;
    std::string onceAtLocal = "2026-10-28 09:00";  // YYYY-MM-DD HH:MM
    int64_t onceAtUnix = 0;

    // interval
    int64_t intervalEverySec = 3600;
    int64_t intervalAnchor = 0;    // 网格相位锚点（Unix 秒）
    int64_t intervalWindow = 0;    // 漂移容差，0 = 关闭

    // calendar（unit ∈ day/week/month/year）
    std::string calendarUnit = "week";
    int calendarInterval = 1;
    std::vector<int> calendarWeekdays;   // 选中周几，0=周日..6=周六
    bool calendarWorkdaysOnly = false;   // 工作日 09:00 控件形态（过滤周六日）
    bool calendarHasMonthDay = true;
    int calendarMonthDay = 1;            // 每月第 N 天（1..31）
    bool calendarHasNthWeekday = false;
    int calendarNth = 1;                 // 每月第 N 个周几（1..5）
    int calendarNthWeekday = 1;          // 0..6
    bool calendarLastWorkday = false;    // 每月最后一个工作日
    int calendarHour = 9;
    int calendarMinute = 0;
    std::string calendarAnchorDate;      // YYYY-MM-DD；unit=week 时其周几必须 ∈ 所选 weekdays

    // cron（5 段「分 时 日 月 周」）
    std::string cronExpr = "0 9 * * *";

    // event
    std::vector<std::string> eventNames;
    int64_t eventCooldownSec = 0;

    // 通用：动作（§4.3）/ 渠道多选 / 错过策略（§4.6）
    std::string actionKind = "remind";   // remind/remind_open/run_task/webhook_only
    std::string actionJson;              // remind_open: {"deeplink":...}；run_task: {"task_key":...}
    std::vector<int64_t> channelIds;
    std::string missedPolicy = "once";   // once/skip/all

    // 保存失败时弹窗内嵌红字文案（validateAndParse 的 errorMessage 回填）
    std::string errorText;
};

// kind → 落库字符串（schedule_rule.kind 列取值）
std::string kindToText(core::schedule::RuleKind kind);
// 落库字符串 → kind；未知串 → fail(ValidationError)
core::Result<core::schedule::RuleKind> kindFromText(const std::string& text);

// 表单 → spec_json（六 kind 各一的字段结构，§4.1）。字符串值经 JSON 转义；
// 只产出结构，不校验语义（校验在 validateAndParse）。
core::Result<std::string> buildSpecJson(const EditFormState& form);

// 保存前校验：本层控件约束（anchor_date 周几匹配、月选择器三选一、动作/策略枚举）
// + core::schedule::parseRuleSpec 全量校验。任一失败 → fail(ValidationError) 带红字文案
//（调用方把 errorMessage 写回 form.errorText，不落库）。
core::Result<core::schedule::RuleSpec> validateAndParse(const EditFormState& form);

// 实时预览接下来 5 次触发时刻：从 afterUnix（严格大于）起连续调 nextFire 迭代 5 次，
// 返回严格递增的 Unix 秒列表（不足 5 次 = 无更多下次时刻，正常截断）。
// 契约：迭代必须经 nextFire 逐次推进，禁止按 form 自算——预览与引擎实际触发
// 同一真源，防「预览 09:00 实际 10:00」类漂移。
core::Result<std::vector<int64_t>> previewNextFires(const EditFormState& form,
                                                    int64_t afterUnix,
                                                    const core::schedule::FireContext& ctx);

// 表单 → RuleRecord（保存载荷）：校验通过后按 §4.4 编辑语义在构建期重算
// next_fire_at（nextFire(spec, ctx.nowUnix, ctx)）。落库事务由宿主模块包
//（createRule/updateRule 同事务），本函数只产出已重算的记录。
core::Result<RuleRecord> buildRuleRecord(const EditFormState& form,
                                         const core::schedule::FireContext& ctx);

// 既有规则 → 表单（编辑回填）：经 core::schedule::parseRuleSpec 解析 spec_json 后
// 逐字段映射；未知 kind / 解析失败 → fail。
core::Result<EditFormState> formFromRecord(const RuleRecord& rec);

// ---- 渲染（schedule_ui_edit.cpp）----

// 宿主模块注入的通道：渠道列表项 + 保存回调（空串 = 成功，非空 = 弹窗内嵌红字）+ 关闭。
struct EditDialogHost {
    std::vector<ChannelOption> channels;
    std::function<std::string(const RuleRecord&)> saveRule;  // 模块侧同事务落库
    std::function<void()> close;
};

// 弹窗自身状态（打开态 + 表单 + 预览缓存）。渲染每帧按当前表单重算预览
//（previewNextFires 纯函数，5 次迭代开销可忽略）：errorText = 校验红字（非法
// spec 不落库），saveError = 宿主落库失败红字（任一字段编辑即清除）。
struct EditDialogState {
    bool open = false;
    EditFormState form;
    std::vector<int64_t> previewCache;  // previewNextFires 结果；空且无红字 = 无下次时刻
    std::string saveError;
};

// 编辑弹窗绘制（关闭态也常驻构建，EUI 内建收起过渡依赖节点存在）。元素 id 以
// "schedule.edit." 为前缀，是无头 UI 用例定位契约。
void drawEditDialog(eui::Ui& ui, const eui::Screen& screen, const core::theme::Tokens& t,
                    EditDialogState& st, const EditDialogHost& host,
                    const core::schedule::FireContext& ctx);

}  // namespace module::schedule::ui
