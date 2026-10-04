// src/module_claude_proxy/quota/quota_registry.cpp
// 显式注册表本体 + 查找 / 探测。
#include "quota_registry.h"

#include <iterator>

#include "opencode_go_provider.h"  // G1：OpenCode Go 工厂
#include "deepseek_provider.h"    // G3：DeepSeek 工厂
#include "provider_minimax_cn.h"   // G2：MiniMax 中国站工厂
#include "quota_volcano.h"        // G4：火山方舟工厂

namespace module::claude::quota {

// 供应商显式表。加一个 provider = 在这张表加一行（插到哨兵之前）。
// 表尾哨兵 id == nullptr 是「到此为止」的判据；用哨兵而非计数常量，避免两个必须同步的真相源。
// constexpr：编译期落数据段，无运行期构造、无静态初始化顺序问题。
constexpr QuotaProviderEntry kQuotaProviders[] = {
    {"opencode_go", &createOpenCodeGoProvider},   // OpenCode Go（base_url 两档 /zen/go 与 /zen/go/v1）
    {"deepseek", &createDeepSeekProvider},        // DeepSeek（Balance 型）
    {"minimax_cn", &createMinimaxCn},             // MiniMax 中国站（旧域 + 百分比反转 + weekly 门控）
    {"volcano", &volcanoProvider},  // 火山方舟（Agent Plan / Coding Plan）
    {nullptr, nullptr},  // ← 表尾哨兵；后续 provider 的行插在本行之前
};

std::size_t quotaProviderTableLength() {
    // 就地 std::size：长度由表本身算出（头文件那行是边界未知的 extern 数组，sizeof 编译不过）
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
