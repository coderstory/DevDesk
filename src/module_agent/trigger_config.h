// trigger_config 列的 JSON 编解码唯一真源：写入（saveTaskDialog）与读取（cronTextOf /
// Scheduler::tickNow / Scheduler::drainEventQueue）全经本文件；decode 失败由调用方转成
// 点名 task id 的 WARN + 跳过
#pragma once

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
struct TriggerConfig {
    std::string cron;   // cron 原文；语义校验归 cron.h 的 parseCron
    std::vector<std::string> events;   // 事件名（core::event::kTodoCreated 等）
    int cooldownSec = 0;               // 冷却秒数；0 = 未设置（消费方填默认）
};

// 编码：cron / events 至少其一非空才允许（两者皆空的配置 = 任务静默永不触发，encode
// 侧直接 fail 而不是产出 `{}`）；cooldown_sec > 0 才写入。经 platform::json 生成，
// 失败返回 fail 而不回落空串
core::Result<std::string> encodeTriggerConfig(const TriggerConfig& cfg);

// 解码规则（schedule 型零回归）：'' / 坏 JSON / 非对象 → fail；对象须至少含
// 「非空字符串 cron」或「非空 events 字符串数组」之一，两者皆缺 → fail；
// cron 键存在但类型不符/为空 → fail；events 键存在但非数组/含非字符串元素/
// 空数组 → fail；cooldown_sec 键存在但非数字 → fail。多余键忽略（向后兼容）。
// task id 由调用方在 WARN 里补
core::Result<TriggerConfig> decodeTriggerConfig(const std::string& json);

} // namespace module::agent
