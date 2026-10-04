// src/module_agent/trigger_config.h
// trigger_config 列的 JSON 编解码唯一真源。写入（saveTaskDialog）与两处读取
// （cronTextOf / Scheduler::tickNow）全经本文件；decode 失败由调用方转成「点名
// task id 的 WARN + 跳过」，不再有静默永不触发的路径
#pragma once

#include <string>

#include "core/result.h"

namespace module::agent {

// trigger_type == "schedule" 的触发载荷（本阶段只有 cron）
struct TriggerConfig {
    std::string cron;   // cron 原文；语义校验归 cron.h 的 parseCron
};

// 编码 {"cron":"<expr>"}：经 platform::json 生成（引号/反斜杠由 json 层转义）。
// 失败返回 fail 而不回落空串——空配置会让任务静默永不触发
core::Result<std::string> encodeTriggerConfig(const TriggerConfig& cfg);

// 解码：'' / 坏 JSON / 非对象 / 缺 cron / cron 非字符串或空 → fail（中文说明，task id 由调用方补）
core::Result<TriggerConfig> decodeTriggerConfig(const std::string& json);

} // namespace module::agent
