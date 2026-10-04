// src/module_claude_proxy/quota/quota_credential.h
// 余量查询的凭据来源缝 + 默认实现（读本 profile 自己的凭据）。
// 缝的形状：resolveAndFetch(profileId, provider, http) → QuotaResult。
// 恒返回 QuotaResult（失败填 error/errorMessage），不许抛异常、不许返回 optional。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "ccswitch_source.h"
#include "quota_provider.h"

namespace module::claude {

class ProfileRepository;

namespace quota {

class QuotaStore;

// 凭据来源缝（如 cc-switch 的实现经这里接入）。
struct QuotaCredentialSource {
    virtual ~QuotaCredentialSource() = default;

    // 解析该 profile 的凭据并发一次查询。拿不到凭据（没配密钥 / 解不开）→ 返回 error = NotConfigured
    // 的 QuotaResult（不是 fail()）：这是可预料的失败结局，带中文文案进 last_error。
    virtual QuotaResult resolveAndFetch(int64_t profileId,
                                         const QuotaProvider& provider,
                                         const HttpSendFn& http) = 0;
};

// 默认实现：读本 profile 自己的凭据（api_key_encrypted 解密 + extra/secrets）。
// cc-switch 覆盖钩子：装配层可传 CcSwitchListFn（生产 = readRealCcSwitchProviders，测试注入假列表）。
// ccswitchProviderId 非空时用该条目的 accessKeyId / secretAccessKey 覆盖同名字段后再 fetch——密钥只在内存过一手，不落库。
class ProfileCredentialSource final : public QuotaCredentialSource {
public:
    // cc-switch 读取缝：返回候选列表。恒返回、不抛异常（同 listCcSwitchProviders）。测试注入假列表 → 零真实磁盘 IO。
    using CcSwitchListFn = std::function<quota::CcSwitchProviderList()>;

    ProfileCredentialSource(ProfileRepository& profiles, QuotaStore& store)
        : profiles_(profiles), store_(store) {}
    ProfileCredentialSource(ProfileRepository& profiles, QuotaStore& store,
                            CcSwitchListFn ccSwitch)
        : profiles_(profiles), store_(store), ccSwitch_(std::move(ccSwitch)) {}

    QuotaResult resolveAndFetch(int64_t profileId, const QuotaProvider& provider,
                                const HttpSendFn& http) override;

private:
    ProfileRepository& profiles_;
    QuotaStore& store_;
    CcSwitchListFn ccSwitch_;   // 空 = 不做 cc-switch 覆盖
};

// 生产用 cc-switch 读取缝的薄包装（`listCcSwitchProviders` 的 std::function 形态）。
quota::CcSwitchProviderList readRealCcSwitchProviders();

// HTTP 缝的生产实现（包 platform::http::send；测试注入假发送 → 零真实网络）。
HttpSendFn makeRealHttpSendFn();

}  // namespace quota
}  // namespace module::claude
