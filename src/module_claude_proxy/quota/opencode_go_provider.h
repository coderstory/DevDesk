// src/module_claude_proxy/quota/opencode_go_provider.h
// OpenCode Go 余量 provider。另开 .h：注册表 findProvider() 只给 const 指针而 fetch() 非 const，
// 消费方与单测共用这里返回非 const 单例的工厂入口。
#pragma once

#include "quota_provider.h"

namespace module::claude::quota {

// 返回进程内单例（provider 无状态），调用方只借不还、不得 delete（与 quota_registry.h 契约一致）。
QuotaProvider* createOpenCodeGoProvider();

}  // namespace module::claude::quota
