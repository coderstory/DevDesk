// src/module_claude_proxy/quota/opencode_go_provider.h
// OpenCode Go 余量 provider，移植 cc-switch coding_plan.rs（bfaaba16）。
// 之所以额外有 .h：注册表 findProvider() 返回 const QuotaProvider*，而 fetch() 是
//   非 const 成员，消费方与单测拿到注册表指针后无法发起查询；故真 provider 开一个
//   返回非 const 单例的工厂入口，注册表行 / 消费方 / 单测共用它。
#pragma once

#include "quota_provider.h"

namespace module::claude::quota {

// 返回进程内单例（provider 无状态），调用方只借不还、不得 delete（与 quota_registry.h 契约一致）。
QuotaProvider* createOpenCodeGoProvider();

}  // namespace module::claude::quota
