// src/module_claude_proxy/upstream_dialect.h
// 上游方言（provider dialect）：把不同 Anthropic-compatible 网关的差异
// ——鉴权头形态 / 必需会话头 / User-Agent 策略 / 模型名标记——集中到一处，
// 转发代码（proxy_router / streaming_proxy）不再散落 "某个网关才这样" 的 if。
//
// 新增一个网关时只改本文件：Kind 加一项、forBaseUrl 加 host 分派、
// applyHeaders/normalizeBody 加分支，两个转发路径零改动。
#pragma once

#include <map>
#include <string>

#include "provider_config.h"   // AuthField（鉴权头三态）

namespace module::claude {

// 代理对外 User-Agent（所有上游统一使用，不透传客户端 UA）
//
// 为什么不透传：opencode.ai（Cloudflare 前置）按 UA 指纹拦请求，实测
// `Python-urllib/3.14` → 403 error 1010 browser_signature_banned，而
// `claude-cli/...` → 200。客户端是什么工具不影响上游正确性（鉴权是 key），
// 统一用 Claude Code 自己的签名即可，顺带把本地代理伪装成官方客户端。
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
    // 按 baseUrl 的 host 选择（opencode.ai 及其子域 → opencodeZen，其余默认）
    static UpstreamDialect forBaseUrl(const std::string& baseUrl);

    // 该方言是否要求剥掉模型名尾部的 [1M]（Claude Code 的本地 1M 上下文标记，
    // 上游通常不认识这个后缀，模型名带它会被判 "Model is unavailable"）
    bool stripModelSuffix() const;

    // 就地把请求头调整成该方言期望的形态：
    //   - 鉴权：清掉客户端送来的**所有大小写变体**凭据，再按 authField 注入：
    //     both → x-api-key + Authorization: Bearer；bearer → 只 Authorization；
    //     api_key → 只 x-api-key；空 key 一律不注入
    //   - User-Agent：无条件覆盖为 kProxyUserAgent（见其注释）
    //   - anthropic-version：客户端没带时补默认值
    //   - x-opencode-session（仅 OpencodeZen）：客户端没带时注入代理实例级 id；
    //     客户端已带则不覆盖（尊重其会话亲和意图）
    void applyHeaders(std::map<std::string, std::string>& headers,
                      const std::string& apiKey, AuthField authField) const;

    // 就地把请求体归一化成该方言能接受的形态（当前仅 OpencodeZen 剥模型名后缀）。
    // 返回 true 表示 body 被改写；不需要改写时 body 一个字节都不动。
    bool normalizeBody(std::string& body) const;
};

// 模型名剥尾部 [1M]：大小写不敏感（[1m] 同样命中），容忍标记前后空白
// （形如 "deepseek-v4-pro [1M] "，上游侧配置里带空格很常见）。
// 命中 → 返回剥掉标记再 trim 尾部空白后的名字；未命中 → 原样返回入参。
std::string stripOneMSuffix(const std::string& model);

// 把请求体顶层 "model" 字符串按 stripOneMSuffix 归一，就地把该值替换掉。
// 解析失败 / 没有 model / model 不是字符串 / 没有可剥的标记 → 返回 false
// 且 body 不变（字节级零扰动，不做无谓的重序列化）。
bool normalizeModelInBody(std::string& body);

} // namespace module::claude
