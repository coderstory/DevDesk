// trigger_config 列的 JSON 编解码唯一真源：写入（saveTaskDialog）与读取（cronTextOf /
// Scheduler::tickNow / Scheduler::drainEventQueue）全经本文件；decode 失败由调用方转成
// 点名 task id 的 WARN + 跳过
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/result.h"

namespace module::agent {

// 事件型任务未配置 cooldown_sec 时的任务级冷却默认值（秒）：冷却窗口内到达的事件
// 直接丢弃不排队（R-5/R-7：不做批量聚合、不做延后）
inline constexpr int kDefaultEventCooldownSec = 300;

// trigger_type == "schedule" 的触发载荷为 cron；"event" 的触发载荷为 events。
// 两者可共存于同一份 JSON（decode 各取所需），但业务上 saveTaskDialog 只按所选
// trigger_type 写其一。cooldown_sec 仅对 event 型有意义（>0 才编码；0 = 未设置，
// 消费方回退 kDefaultEventCooldownSec）
//
// max_runs（可选，2026-10-17 加）是「限次 / 一次性」语义的唯一真源：正整数 = 该任务
// 派发满 N 次后由 Scheduler 自动停用；缺省（nullopt）= 不限次数，即今天「cron/事件
// 匹配即恒循环」的行为。选整数字段而不是 once 布尔：`只跑一次` = 上限 1，将来放开
// 「最多跑 N 次」不用加键、不用迁移（decode 忽略未知键的向后兼容只保旧的、不保新的）。
// 加这个键**零 schema 变更**：它落在既有 trigger_config 列的 JSON 文本里，旧行没有该
// 键 → decode 出 nullopt → 行为与今天逐字节一致，故不需要迁移 / 回填 / 兼容分支。
// 消费方只有 Scheduler（schedule / event 两条派发路径）；manual / resident 不消费该键。
inline constexpr int kMaxRunsUpperBound = 1000;   // 上限寄存器，见 decode 规则

// once_at（可选，2026-10-18 加）是「一次性绝对时间」载荷：Unix 秒，语义为「到点触发
// 一次」。与 cron / events 互斥——三种载荷描述的是三种不同的触发源，同时存在会让
// 「到底按哪个触发」取决于消费方的读取顺序，故 encode / decode 都直接 fail。
//
// 上下限（encode 与 decode 同口径）：
//   下界 1：0 / 负数不是合法时间戳（0 = 1970-01-01 或「未设置」的习惯哨兵，不能当
//           真实时刻；负数更无意义）。刻意**不要求**必须晚于当前时间：解编是纯函数，
//           不引入时钟依赖（否则同一份配置随系统时间红绿不定）；「已过期」由将来
//           的消费方（闹钟调度）判定，配置层只保证值本身是合理时刻。
//   上界 4102444800 = 2100-01-01 00:00:00 UTC。理由：①防手改库塞超大 / 溢出值；
//           ②抓单位写错——毫秒 / 微秒级时间戳（2026 年约 1.7e12 / 1.7e15）都远高于
//           该上界，会被直接拒绝而不是被当成万年后的闹钟静默吞掉；③2100 年对桌面
//           工具的「一次性提醒」足够远，再大只可能是脏数据。
inline constexpr int64_t kOnceAtUpperBound = 4102444800LL;

struct TriggerConfig {
    std::string cron;   // cron 原文；语义校验归 cron.h 的 parseCron
    std::vector<std::string> events;   // 事件名（core::event::kTodoCreated 等）
    int cooldownSec = 0;               // 冷却秒数；0 = 未设置（消费方填默认）
    // 运行次数上限；nullopt = 不限次数（缺省，旧配置串的语义）
    std::optional<int> maxRuns;
    // 一次性触发的绝对时刻（Unix 秒）；nullopt = 未配置。与 cron / events 互斥。
    // 与 maxRuns 相互独立：once_at 本身就是「只触发一次」，maxRuns 是额外的次数闸
    //（once_at + max_runs 合法但冗余；由将来消费方决定以哪个为准，当前只做编解码）
    std::optional<int64_t> onceAt;
};

// 编码：cron / events / once_at 至少其一非空（三者皆空的配置 = 任务静默永不触发，
// encode 侧直接 fail 而不是产出 `{}`）；once_at 与 cron / events 互斥；once_at 须为
// 1..kOnceAtUpperBound 的整数。cooldown_sec > 0 才写入；maxRuns 有值（>0）才写入；
// onceAt 有值才写入——未配置这些可选项的产物必须与加键之前逐字节相同（既有用例已
// 钉住 `{"cron":"…"}` 原文）。经 platform::json 生成，失败返回 fail 而不回落空串
core::Result<std::string> encodeTriggerConfig(const TriggerConfig& cfg);

// 任务级「配置异常」判定：输入任务的 triggerType 与 trigger_config 原始串，返回人类
// 可读的问题描述；空串 = 配置没问题（正常任务零噪音）。判据与 Scheduler 消费侧
// 同源——同一批函数、同一批 triggerType 过滤口径，防「UI 说没事、Scheduler 每跑必跳过」：
//   event 型    → decodeTriggerConfig（Scheduler::processEvent 的唯一校验）
//                 + events 非空（processEvent 逐项比对，空 events 恒不命中）
//   schedule 型 → decodeTriggerConfig + parseCron（Scheduler::tickNow 的两步）
//   once 型     → decodeTriggerConfig + onceAt 有值；配置里出现 cron / events 直接
//                 报「一次性触发不支持 cron/events」（decode 放行 cron-only/events-only
//                 是给其它类型的镜像，对 once 型就是永不触发或语义分裂）
//   manual / resident / daily → 不消费该列 → 恒空
std::string triggerConfigProblem(const std::string& triggerType,
                                 const std::string& triggerConfig);

// 解码规则（schedule 型零回归）：'' / 坏 JSON / 非对象 → fail；对象须至少含
// 「非空字符串 cron」「非空 events 字符串数组」「整数 once_at」之一，三者皆缺 → fail；
// cron 键存在但类型不符/为空 → fail；events 键存在但非数组/含非字符串元素/
// 空数组 → fail；once_at 键存在但非整数字面量 / < 1 / > kOnceAtUpperBound → fail
// （小数、字符串、布尔、null 一律拒绝，不做截断或类型放宽）；once_at 与 cron / events
// 同时出现 → fail（互斥）；cooldown_sec 键存在但非数字 → fail；max_runs 键存在但
// 非数字 / < 1 / > kMaxRunsUpperBound → fail（宁可不认也不静默当「不限次数」——
// 静默会把「只跑一次」悄悄变回永久重复任务）。多余键忽略（向后兼容）。
// task id 由调用方在 WARN 里补
core::Result<TriggerConfig> decodeTriggerConfig(const std::string& json);

} // namespace module::agent
