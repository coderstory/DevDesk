// src/module_claude_proxy/quota/opencode_go_provider.h
// G1：OpenCode Go（$10/月订阅，美元三时间窗口）余量 provider
// 移植自 cc-switch src-tauri/src/services/coding_plan.rs（bfaaba16）
//   detect_provider:47-50 / parse_opencode_go_tiers:731-762 / query_opencode_go:764-833
//
// 为什么本 provider 除了 .cpp 还有一个只有一行声明的 .h（G0 头文件里写的是
// 「G1-G4 各自一个 .cpp」）：
//   注册表查询口 `findProvider()` 返回 **const** QuotaProvider*，而契约里
//   `fetch()` 是**非 const** 成员（G0 quota_provider.h:144）——消费方（G5）与
//   单测拿到注册表指针后**无法**发起查询。G0 契约已冻结、不在 G1 改；于是真
//   provider 额外开一个返回非 const 单例的工厂入口，注册表行、消费方、单测
//   共用它。G2-G4 遇到同一处时照此办理（各自的 .h + 工厂）。
#pragma once

#include "quota_provider.h"

namespace module::claude::quota {

// 返回进程内单例（provider 无状态），调用方**只借不还**、不得 delete
//（与 quota_registry.h 的 create 契约一致，见该文件 :24-26）。
QuotaProvider* createOpenCodeGoProvider();

}  // namespace module::claude::quota
