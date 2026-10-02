// src/module_claude_proxy/quota/quota_credential.h
// 余量查询的**凭据来源缝**（G5）+ 默认实现
// （实施计划 docs/superpowers/plans/2026-10-02-quota-implementation-plan.md §G5）
//
// 为什么是缝（G5 交付要求，G6 依赖）：G6 要把「从 cc-switch 读配置」接进来，
// 那会带一套**与本 profile 无关**的凭据来源。本批只提供
// 「读本 profile 自己的凭据」这一份默认实现；G6 只需再传一个实现进来，
// **不改本批任何代码**。
//
// 缝的形状（两个纯虚函数，全批只有这一处）：
//   resolveAndFetch(profileId, provider, http) → QuotaResult
// 恒返回 QuotaResult（quota_provider.h 不变量 1），失败填 error/errorMessage。
// **不许**抛异常、不许返回 optional —— 四个 provider 与三条失败韧性都按这个读。
#pragma once

#include <cstdint>
#include <string>

#include "quota_provider.h"

namespace module::claude {

class ProfileRepository;

namespace quota {

class QuotaStore;

// 凭据来源缝。G6 的实现（如 cc-switch）经这里接入。
struct QuotaCredentialSource {
    virtual ~QuotaCredentialSource() = default;

    // 解析该 profile 的凭据并**真的发一次查询**。
    // 拿不到凭据（没配密钥 / 存储形态解不开）→ 返回 error = NotConfigured 的
    // QuotaResult（**不是** fail()）：这是「这一种可预料的失败结局」，要带着
    // 中文文案进 last_error，不是异常。
    virtual QuotaResult resolveAndFetch(int64_t profileId,
                                         const QuotaProvider& provider,
                                         const HttpSendFn& http) = 0;
};

// 默认实现：读**本 profile 自己**的凭据（api_key_encrypted 经
// platform::credential 解密，E1 已落地）+ profile_quota_config 的 extra/secrets。
// 够用即可；G6 来了换实现。
class ProfileCredentialSource final : public QuotaCredentialSource {
public:
    ProfileCredentialSource(ProfileRepository& profiles, QuotaStore& store)
        : profiles_(profiles), store_(store) {}

    QuotaResult resolveAndFetch(int64_t profileId, const QuotaProvider& provider,
                                const HttpSendFn& http) override;

private:
    ProfileRepository& profiles_;
    QuotaStore& store_;
};

// 一条可注入的 HTTP 缝的生产实现（包 platform::http::send；测试注入假发送，
// 于是零真实网络 —— 与 quota_provider.h 的 HttpSendFn 约定同款）。
HttpSendFn makeRealHttpSendFn();

}  // namespace quota
}  // namespace module::claude
