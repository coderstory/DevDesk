// 直连 LLM 的配置来源抽象：agent 的直连端点只经本接口取「当前生效 profile」，
// 不依赖 IClaudeProxyService —— 代理开不开与智能体能不能跑无关（解耦契约）。
// 提供方在装配层（main.cpp）从 ClaudeService 读激活 profile + 明文 key 后映射成本结构；
// core 不 include 任何 module_* 头，本头只放纯数据 + 纯虚接口。
#pragma once

#include <string>

#include "core/result.h"

namespace core {

// 直连上游所需的最小 profile 快照（装配层从激活 profile 映射而来）。
// apiKeyPlain 只活在内存里：不得落盘、进日志、进 commit。
struct LlmDirectProfile {
    std::string baseUrl;          // 例如 https://api.anthropic.com（尾斜杠可有可无）
    std::string apiKeyPlain;      // 明文 key（空串 = profile 未配置凭据）
    std::string authField;        // 鉴权头三态：'both' / 'bearer' / 'api_key'（未知/空串按 both）
    std::string modelsConfigJson; // 槽位配置 JSON 原文（空串 = 未配置）
    std::string defaultModel;     // profile.model：default 槽位在 models_config 缺该槽时的回落
};

class LlmProfileSource {
public:
    virtual ~LlmProfileSource() = default;
    // 取当前生效 profile。失败（无激活 profile / 读凭据失败 / 库错）→ fail，
    // 错误文案被端点包装后上屏，故必须自带可操作指引
    virtual Result<LlmDirectProfile> currentProfile() = 0;
};

} // namespace core
