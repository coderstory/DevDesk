// src/module_agent/trigger_config.h
// trigger_config 列的 JSON 编解码（M2 静态审计 ARCH-02 收口）。
//
// 背景：agent_tasks.trigger_config 存的是 `{"cron":"<表达式>"}`，但此前没有
// 编解码点——UI 保存时**手拼**字符串（靠「校验通过的表达式只含 [0-9 * , - /
// 空格]」这条推理免转义）、UI 展示（cronTextOf）与调度器 tickNow 各写一份裸
// parse。后果：加第二个键要改三处；且调度器对坏配置是 `continue`（静默不派发）
// 而 UI 照常渲染——用户看到任务存在、永不触发、没有任何提示。
//
// 现状（唯一真源）：写入点（AgentModule::saveTaskEdit）与两处读取点
// （cronTextOf / Scheduler::tickNow）**全部**经本文件的 encode/decode；
// decode 失败由调用方转成「点名 task id 的 LOG_WARN + 跳过」。
#pragma once

#include <string>

#include "core/result.h"

namespace module::agent {

// trigger_type == "schedule" 的触发载荷（本阶段只有 cron 一个键）
struct TriggerConfig {
    std::string cron;   // cron 表达式原文；**语义校验**归 cron.h 的 parseCron
};

// 编码：{"cron":"<expr>"}。经 platform::json 生成——引号 / 反斜杠 / 非 ASCII
// 由 json 层转义，不手拼。序列化失败（理论不可达）返回 Result fail 而**不回落
// 空串**：空配置会让任务静默永不触发，正是本 finding 要消灭的静默面
core::Result<std::string> encodeTriggerConfig(const TriggerConfig& cfg);

// 解码：'' / 坏 JSON / 非对象 / 缺 "cron" / "cron" 非字符串 / cron 为空串
// → Result fail（errorMessage 为中文说明；task id 语境由调用方补）
core::Result<TriggerConfig> decodeTriggerConfig(const std::string& json);

} // namespace module::agent
