// src/module_claude_proxy/quota/deepseek_provider.h
// DeepSeek 余额 provider（G3）—— **Balance 型**
// （实施计划 docs/superpowers/plans/2026-10-02-quota-implementation-plan.md §G3）
//
// 移植自 cc-switch（bfaaba16）src-tauri/src/services/balance.rs 的 DeepSeek 分支：
//   detect_provider :26-43 的 deepseek 项 / query_deepseek :70-146
// DeepSeek **只有余额**：无时间窗口、无 used、无 reset（cc-switch 未实现套餐 / 速率
// 限制），故 QuotaResult.windows 恒空（契约 quota_provider.h:66-68）。
#pragma once

#include "quota_provider.h"

namespace module::claude::quota {

// 工厂：返回**进程内单例**（provider 无状态；调用方只借不还）。
// 契约：id()=="deepseek" / displayName()=="DeepSeek" / fields() 为空——cc-switch
// 的 get_balance 只有 (base_url, api_key) 两个入参，本 provider 沿用 profile 凭据。
QuotaProvider* createDeepSeekProvider();

}  // namespace module::claude::quota
