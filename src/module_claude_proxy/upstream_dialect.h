// src/module_claude_proxy/upstream_dialect.h
// 上游方言：把不同 Anthropic-compatible 网关的差异——鉴权头形态 / 必需会话头 /
// UA 策略 / 模型名标记——集中到一处。新增网关只改本文件：Kind 加一项、
// forBaseUrl 加 host 分派、applyHeaders/normalizeBody 加分支。
#pragma once

#include <map>
#include <optional>
#include <string>

#include "provider_config.h"   // AuthField（鉴权头三态）

namespace module::claude {

// 代理对外 User-Agent（所有上游统一，不透传客户端 UA）：opencode.ai
//（Cloudflare）按 UA 指纹拦请求，统一用 Claude Code 签名。
inline constexpr const char* kProxyUserAgent = "claude-cli/2.1.0 (external, cli)";

struct UpstreamDialect {
    // 方言种类（决定 applyHeaders / normalizeBody 走哪套差异）
    enum class Kind {
        AnthropicDefault,  // 通用 Anthropic-compatible 网关（bailian 等）
        OpencodeZen,       // opencode.ai/zen（Cloudflare 前置 + 会话头 + 不认 [1M]）
    };

    Kind kind = Kind::AnthropicDefault;

    // 通用方言：x-api-key + Bearer 双发，UA 固定，不碰模型名
    static UpstreamDialect anthropicDefault();
    // opencode.ai zen：额外要求 x-opencode-session，且不认 [1M] 模型标记
    static UpstreamDialect opencodeZen();
    // 按 baseUrl 的 host 选择（opencode.ai 及子域 → opencodeZen；其余 → 通用方言）
    static UpstreamDialect forBaseUrl(const std::string& baseUrl);

    // 是否剥掉模型名尾部 [1M]（本地 1M 标记，上游通常不认识，带着会被判不可用）
    bool stripModelSuffix() const;

    // 就地把请求头调整成该方言期望的形态：清掉客户端所有大小写变体凭据再按
    // authField 注入（空 key 一律不注入）；UA 无条件覆盖；anthropic-version
    // 缺省补默认值；x-opencode-session（仅 OpencodeZen）没带才注入。
    void applyHeaders(std::map<std::string, std::string>& headers,
                      const std::string& apiKey, AuthField authField) const;

    // 就地把请求体归一化成该方言能接受的形态（当前仅 OpencodeZen 剥模型名后缀）。
    // 返回 true 表示 body 被改写；不需要改写时 body 一个字节都不动。
    bool normalizeBody(std::string& body) const;
};

// 模型名剥尾部 [1M]：大小写不敏感、容忍标记前后空白（上游侧配置带空格常见）；
// 命中 → 剥掉后 trim 尾部；未命中 → 原样返回。
std::string stripOneMSuffix(const std::string& model);

// 请求体顶层 "model" 按 stripOneMSuffix 归一。解析失败/无 model/无标记 → false
// 且 body 不变（字节级零扰动）。
bool normalizeModelInBody(std::string& body);

} // namespace module::claude
