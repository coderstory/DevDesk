// cron 引擎已上移到 core::cron（core 层调度子系统与 module_agent 共用同一实现）。
// 本头是兼容转发：既有 module_agent 调用点（module::agent::parseCron 等）零改动。
// 新代码直接 #include "core/cron.h"。不要在本头再添加任何实现。
#pragma once
#include "core/cron.h"

namespace module::agent {
using core::cron::CronExpr;
using core::cron::parseCron;
using core::cron::cronMatches;
using core::cron::nextCronMatch;
using core::cron::previousCronMatch;
} // namespace module::agent
