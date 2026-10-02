// src/module_claude_proxy/quota/provider_minimax_cn.h
// MiniMax 中国站余量 provider（G2）
// （实施计划 docs/superpowers/plans/2026-10-02-quota-implementation-plan.md §G2）
//
// 业务逻辑逐条移植自 cc-switch `src-tauri/src/services/coding_plan.rs`：
//   detect_provider:38-44     host 标签边界探测（api.minimaxi.com / api.minimax.cn）
//   query_minimax:426-509     旧域端点 + Bearer + base_resp 业务错误（HTTP 200 也可能是失败）
//   parse_minimax_tiers:657-712  general 桶 / 剩余百分比反转 / 周桶 status 门控
#pragma once

#include "quota_provider.h"

namespace module::claude::quota {

// 工厂：返回**进程内单例**（provider 无状态；调用方只借不还，见 quota_registry.h）。
// 注册表 quota_registry.cpp 的 minimax_cn 行与测试都从这里取实例。
QuotaProvider* createMinimaxCn();

}  // namespace module::claude::quota
