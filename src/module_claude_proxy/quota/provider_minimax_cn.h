// src/module_claude_proxy/quota/provider_minimax_cn.h
// MiniMax 中国站余量 provider，移植 cc-switch coding_plan.rs。
// 旧域端点 api.minimaxi.com + Bearer；base_resp 业务错误（HTTP 200 也可能是失败）；
// general 桶、剩余百分比反转、周桶 status 门控。
#pragma once

#include "quota_provider.h"

namespace module::claude::quota {

// 工厂：返回进程内单例（provider 无状态；调用方只借不还，见 quota_registry.h）。注册表与测试都从这里取实例。
QuotaProvider* createMinimaxCn();

}  // namespace module::claude::quota
