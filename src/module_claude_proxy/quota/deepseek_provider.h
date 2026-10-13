// src/module_claude_proxy/quota/deepseek_provider.h
// DeepSeek 余额 provider —— Balance 型，只有余额，无窗口 / 无 used / 无 reset。
#pragma once

#include "quota_provider.h"

namespace module::claude::quota {

// 工厂：返回进程内单例（provider 无状态；调用方只借不还）。
// 契约：id()=="deepseek" / displayName()=="DeepSeek" / fields() 为空（沿用 profile 凭据）。
QuotaProvider* createDeepSeekProvider();

}  // namespace module::claude::quota
