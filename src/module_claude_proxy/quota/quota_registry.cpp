// src/module_claude_proxy/quota/quota_registry.cpp
// 显式注册表本体 + 查找 / 探测（G0 抽象层）
// （实施计划 docs/superpowers/plans/2026-10-02-quota-implementation-plan.md §G0）
#include "quota_registry.h"

#include <iterator>

#include "opencode_go_provider.h"  // G1：OpenCode Go 工厂
#include "deepseek_provider.h"    // G3：DeepSeek 工厂
#include "provider_minimax_cn.h"   // G2：MiniMax 中国站工厂
#include "quota_volcano.h"        // G4：火山方舟工厂

namespace module::claude::quota {

// 供应商显式表。**加一个 provider = 在这张表加一行**（G1-G4 各自加自己那一行，
// 插到哨兵之前）+ 在 tests/test_modules/test_claude_quota.cpp 的结构守卫下自动进入覆盖。
//
// 表尾哨兵 `{nullptr, nullptr}` 的 id == nullptr 是「到此为止」的判据（见头文件）。
// G0 交付时表里只有哨兵：零元素数组（`const T arr[] = {}`）不是标准 C++，用哨兵
// 表达空表最省且不引入额外计数常量（计数常量与表行数会成为两个必须同步的真相源，
// 漏同步就是越界或静默丢行）。G1 起每批把真实 provider 的行插到哨兵之前。
//
// constexpr：初始值全是常量表达式（函数指针 + nullptr），编译期落数据段，
// 无运行期构造、无静态初始化顺序问题——这也是「显式表」相对静态自注册的另一半好处。
constexpr QuotaProviderEntry kQuotaProviders[] = {
    // G1：OpenCode Go（$10/月订阅；base_url 两档 /zen/go 与 /zen/go/v1）
    {"opencode_go", &createOpenCodeGoProvider},
    // G3：DeepSeek（Balance 型：只有余额，无窗口）
    {"deepseek", &createDeepSeekProvider},
    // G2：MiniMax 中国站（旧域 api.minimaxi.com；百分比反转 + weekly 门控）
    {"minimax_cn", &createMinimaxCn},
    {"volcano", &volcanoProvider},  // G4 火山方舟（Agent Plan / Coding Plan）
    {nullptr, nullptr},  // ← 表尾哨兵；后续 provider 的行插在本行之前
};

std::size_t quotaProviderTableLength() {
    // 就地 std::size：长度由表本身算出，头文件里那行是边界未知的 extern 数组
    //（对它 sizeof 编译不过，见 quota_registry.h 本函数的说明）
    return std::size(kQuotaProviders);
}

const QuotaProvider* findProvider(const std::string& id,
                                  std::span<const QuotaProviderEntry> table) {
    if (id.empty()) return nullptr;  // 空 id 不是任何一行，也不许命中哨兵后的行
    for (const QuotaProviderEntry& entry : table) {
        if (entry.id == nullptr) break;  // 哨兵 = 表尾
        if (id == entry.id) {
            return entry.create != nullptr ? entry.create() : nullptr;
        }
    }
    return nullptr;
}

const QuotaProvider* findProvider(const std::string& id) {
    return findProvider(id, kQuotaProviders);
}

const QuotaProvider* detectByBaseUrl(const std::string& baseUrl,
                                     std::span<const QuotaProviderEntry> table) {
    if (baseUrl.empty()) return nullptr;  // 空串不匹配任何一行（不猜）
    for (const QuotaProviderEntry& entry : table) {
        if (entry.id == nullptr) break;
        if (entry.create == nullptr) continue;
        const QuotaProvider* provider = entry.create();
        if (provider != nullptr && provider->matchesBaseUrl(baseUrl)) {
            return provider;
        }
    }
    return nullptr;
}

const QuotaProvider* detectByBaseUrl(const std::string& baseUrl) {
    return detectByBaseUrl(baseUrl, kQuotaProviders);
}

}  // namespace module::claude::quota
