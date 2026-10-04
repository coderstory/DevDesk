// src/module_claude_proxy/quota/quota_provider.h
// 供应商余量查询的契约层类型定义（无任何真实供应商实现；注册表见 quota_registry.h）。
// 三条语义约定（所有消费方按此读）：
//   1. fetch 恒返回 QuotaResult，永不抛异常、永不返回空指针 / optional；失败全走返回值。
//   2. 错误只走 error / errorMessage（唯一通道）。error != None 时 windows / balanceRemaining /
//      currency 一律不得当作有效值呈现——「从未成功过」必须只渲染错误行，绝不画 0% 条。
//   3. 量纲由 provider 归一：QuotaWindow::usedPercent 是 0-100 的「已用」，上游给「剩余 %」由 provider 反转。
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/result.h"
#include "platform/http_client.h"

namespace module::claude::quota {

// 余量形态：
//   Tiered  = 分周期配额（5 小时 / 周 / 月），数据在 windows
//   Balance = 余额（预付费），数据在 balanceRemaining + currency，无周期无重置
// Count 是哨兵（取值个数），供 quota_home_view.cpp 的「QuotaKind 与 QuotaHomeKind 逐值同义」断言对表；
//   基准必须住在枚举里（手写计数会与枚举一起漂移）。
enum class QuotaKind { Tiered, Balance, Count };

// 查询结果的状态（唯一错误通道）。分类依据是「调用方该做什么」，不是 HTTP 码：
//   None          成功；Auth          凭据无效 / 过期（401）→ 改密钥；
//   Entitlement   凭据有效但无该产品权限（403 / 余额不足）→ 改密钥没用，得换套餐；
//   Shape         响应结构与预期不符 → 该报错，不是「返回空卡」；
//   Transient     网络 / 超时 / 5xx / 限流 → 可重试（保留上次成功值 + 「可能过期」角标）；
//   NotConfigured 没配 / 配置不完整 → 不画 0% 条，引导去配置。
// Count 是哨兵（非真实错误）：恒等于取值个数，供 quota_error_text.cpp 的文案表对表断言，
//   加枚举忘配文案时编译期红。哨兵必须住在枚举里，手写常量会与枚举一起漂移。
enum class QuotaError {
    None,
    Auth,
    Entitlement,
    Shape,
    Transient,
    NotConfigured,
    Count,   // ← 哨兵（表尾）。只用于对表，绝不作为错误值产生或落库。
};

// 单个配额窗口。key 是规范键名，全项目只有三个取值："five_hour" / "weekly" / "monthly"
// （上游叫法由 provider 归一，一律按显式字段名映射，绝不靠 resetsAt 排序猜周期——末尾周桶比 5h 桶更早重置）。
struct QuotaWindow {
    std::string key;            // 规范键：five_hour / weekly / monthly
    double usedPercent = 0.0;   // **已用**百分比，0-100，double——上游给小数时不截断
    std::string resetsAt;       // ISO8601；无重置语义或上游未给 = 空串
};

// 一次查询的完整结果（fetch 的唯一返回类型）。Tiered 读 windows；Balance 读 balanceRemaining + currency。
// 两者不混用：Balance 的 windows 必须为空（不为凑数硬造窗口），Tiered 的 balanceRemaining 保持 0。
struct QuotaResult {
    QuotaKind kind = QuotaKind::Tiered;
    std::vector<QuotaWindow> windows;  // 顺序 = provider 的固定展示序
    double balanceRemaining = 0.0;     // Balance 型专用；币种原样，不换算
    std::string currency;              // 如 "CNY" / "USD"，原样保留
    std::string planName;              // 套餐名（上游给才有，可空）
    QuotaError error = QuotaError::None;  // 唯一错误通道（约定 2）
    std::string errorMessage;             // 面向用户的中文文案；成功时为空
};

// 供应商需要用户填的一个配置字段（G6 弹窗按 fields() 动态渲染的依据）。
//   key 落库 / 查表的稳定键（与 ProviderConfig::fields 一致）；label UI 显示名；
//   secret true = UI 用密文输入框 + 脱敏回显（仅渲染提示，存放位置由存储层定）。
struct ProviderField {
    std::string key;
    std::string label;
    bool secret = false;
};

// 单个 profile 的余量查询配置（fetch 的输入）。
struct ProviderConfig {
    // 注册表 id。空串 = 未配置余量查询（findProvider 返 nullptr，调用方走 NotConfigured，不要拿它去 fetch）。
    std::string providerId;
    // 该 profile 的 base_url（G4 从 host 取 region；G6 显示）。余量接口域名由 provider 自己写死，不从它派生。
    std::string baseUrl;
    // 该 profile 的 API key 明文（仅内存，keychain 读出后填入；沿用 profile 凭据型供应商用它做 Bearer）。永不落日志、落库。
    std::string apiKey;
    // fields() 声明的键 → 用户填的值（如火山的 accessKeyId / secretAccessKey）；fields() 为空 = 不吃额外字段。
    std::map<std::string, std::string> fields;
};

// HTTP 注入缝：provider 调这个可调用对象而非直接 platform::http::send，单测可塞假发送 → 零真实网络。
// 约定：传给 fetch 的 HttpSendFn 必须非空；provider 见空缝应返回 Transient（不许直接调用触发 std::bad_function_call）。
using HttpSendFn = std::function<core::Result<platform::http::Response>(
    const platform::http::Request&)>;

// 一个供应商的查询能力。G1-G4 各实现一个，实例由各自的工厂函数提供。
class QuotaProvider {
public:
    virtual ~QuotaProvider() = default;

    // 注册表 id（稳定标识，落库用；改了等于换供应商）。如 "opencode_go"。
    virtual std::string id() const = 0;
    // 面向用户的中文名，如「OpenCode Go」。
    virtual std::string displayName() const = 0;
    // 需要用户填的字段（顺序 = UI 展示顺序）。空 = 沿用 profile 凭据。
    virtual std::vector<ProviderField> fields() const = 0;

    // base_url 探测（G6 默认选中 / 导入时自动匹配用）。默认不命中：失败方向安全（用户手动挑），
    // 误命中会把别人的 quota 查到当前 profile 上。覆写时用 host 标签边界匹配，不要裸子串包含。
    virtual bool matchesBaseUrl(const std::string& baseUrl) const {
        (void)baseUrl;
        return false;
    }

    // 发一次查询。恒返回 QuotaResult：失败填 error + errorMessage，不把失败信号塞进 windows。
    // 可能阻塞（网络），调用方在后台线程调。const 成员使 findProvider() 的 const 返回可直接用，不必 const_cast。
    virtual QuotaResult fetch(const ProviderConfig& cfg, const HttpSendFn& http) const = 0;
};

}  // namespace module::claude::quota
