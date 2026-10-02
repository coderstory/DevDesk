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
#include <functional>
#include <string>
#include <utility>

#include "ccswitch_source.h"
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
//
// **G6 增补（2026-10-03）：cc-switch 覆盖钩子。** 装配层可传一个
// `CcSwitchListFn`（生产 = `realCcSwitchListFn`，测试注入假列表）。它在
// `QuotaConfigRow::ccswitchProviderId` 非空时，用该 cc-switch 条目的
// accessKeyId / secretAccessKey **覆盖**同名字段后再 fetch——密钥只在内存里
// 过一手，不落 DevDesk 的库（quota_store.h 的「引用而非复制」）。
//
// **为什么是本类的一个钩子而不是第二个 QuotaCredentialSource 实现**：
// ProviderConfig 的装配（profile 密钥 + extra/secrets 叠加 + base_url）在本类
// 里只有一份。再写一个实现就是第二份装配点 = 「改一处漏一处」那类病
// （message_blocks.h 编解码单点、quota_store windows_json 单点同款纪律）。
class ProfileCredentialSource final : public QuotaCredentialSource {
public:
    // cc-switch 读取缝：返回候选列表。**恒返回，不抛异常**（与
    // listCcSwitchProviders 同口径）。测试注入假列表 → 零真实磁盘 IO。
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
    CcSwitchListFn ccSwitch_;   // 空 = 不做 cc-switch 覆盖（测试与旧装配）
};

// 生产用 cc-switch 读取缝的薄包装（`listCcSwitchProviders` 的 std::function 形态）。
quota::CcSwitchProviderList readRealCcSwitchProviders();

// 一条可注入的 HTTP 缝的生产实现（包 platform::http::send；测试注入假发送，
// 于是零真实网络 —— 与 quota_provider.h 的 HttpSendFn 约定同款）。
HttpSendFn makeRealHttpSendFn();

}  // namespace quota
}  // namespace module::claude
