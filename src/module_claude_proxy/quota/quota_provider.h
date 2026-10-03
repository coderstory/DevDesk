// src/module_claude_proxy/quota/quota_provider.h
// 供应商余量查询 —— **契约层类型定义**（G0 抽象层）
// （实施计划 docs/superpowers/plans/2026-10-02-quota-implementation-plan.md §G0）
//
// 本文件只有类型与签名，**没有任何真实供应商实现**（opencode_go / minimax_cn /
// deepseek / volcano 是 G1-G4，各自一个 .cpp；注册表行见 quota_registry.h）。
// 之所以独立成层：四个 provider 共用一套结果语义，接口定错了四个一起返工。
//
// 三条贯穿全层的语义约定（G1-G8 全部消费方都按这个读）：
//
//   1. **fetch 恒返回 QuotaResult，永不抛异常、永不返回空指针 / optional。**
//      传输失败、鉴权失败、结构不认识，全部走返回值。
//
//   2. **错误只走 `error` / `errorMessage` 两个字段**（唯一错误通道）。
//      `error == QuotaError::None` 才算成功；`error != None` 时 `windows` /
//      `balanceRemaining` / `currency` 一律**不得**被当作有效值呈现——G7 五态
//      里「从未成功过」必须只渲染错误行，绝不画 0% 条（画了就是「还有 100%
//      可用」的假象）。
//
//   3. **量纲由 provider 负责归一**：`QuotaWindow::usedPercent` 是 **0-100 的
//      「已用」**百分比，不是剩余。上游给「剩余 %」的（G2 五小时窗口）由
//      provider 自己反转，UI 侧不再做第二次换算。
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/result.h"
#include "platform/http_client.h"

namespace module::claude::quota {

// 余量形态：
//   Tiered  = 分周期配额（5 小时 / 周 / 月），数据在 `windows`
//   Balance = 余额（预付费），数据在 `balanceRemaining` + `currency`，无周期无重置
// `Count` 是**哨兵**（取值个数，不是第三种形态），供 quota_home_view.cpp 的
// 「QuotaKind 与 QuotaHomeKind 逐值同义」static_assert 对表——两份同义枚举
// （quota_home_bridge.h 的中立副本）加值时若只改一处，那条断言立刻红。
// 与 QuotaError::Count 同款理由：基准必须住在枚举里，手写计数会与枚举一起漂移。
enum class QuotaKind { Tiered, Balance, Count };

// 查询结果的状态（**唯一错误通道**，见文件头约定 2）。
// 分类依据是「调用方该做什么」，不是 HTTP 码本身：
//   None          成功（此时 windows / balance 有效）
//   Auth          凭据无效 / 过期（HTTP 401 一类）→ 让用户改密钥
//   Entitlement   凭据有效但该账号 / workspace **没有**这个产品的权限
//                 （HTTP 403、余额不足一类）→ 改密钥没用，得换套餐
//   Shape         响应结构与预期不符（少了根键、窗口不认识、旧扁平形态）
//                 → 该报错，**不是**「返回空卡」
//   Transient     网络 / 超时 / 5xx / 限流 —— 可重试（G8：保留上次成功值 +
//                 「可能过期」角标，不重试风暴）
//   NotConfigured 该 profile 没配余量查询 / 配置不完整（缺字段）
//                 → G7 不画 0% 条，引导用户去配置
//
// `Count` 是**哨兵，不是真实的错误**：它恒等于「取值个数」，供
// quota_error_text.cpp 的 `static_assert(文案表长度 == Count)` 做编译期对表。
// 加枚举值时它自动顶上去，那条断言**立刻**把构建打红，逼人当场给新错误配文案。
// 为什么哨兵必须住在枚举里（静态审计 m4 / 上一批 m1 的同一个坑）：手写的
// 「取值个数」常量与枚举可以一起漂移 —— 加了枚举忘了改常量，断言照样绿，
// 等于没守卫。哨兵在枚举内部，加枚举必然改变它的值，这是唯一无法绕过的基准。
enum class QuotaError {
    None,
    Auth,
    Entitlement,
    Shape,
    Transient,
    NotConfigured,
    Count,   // ← 哨兵（表尾）。只用于对表，绝不作为错误值产生或落库。
};

// 单个配额窗口。key 是**规范键名**，全项目只有三个取值：
//   "five_hour" / "weekly" / "monthly"
// （上游自己的叫法——"rolling" / "session" 等——由 provider 在映射时归一，
//  并且**一律按显式字段名映射**，绝不靠 resetsAt 排序猜周期：周期末尾周桶会比
//  5h 桶更早重置，排序猜必错。）
struct QuotaWindow {
    std::string key;            // 规范键：five_hour / weekly / monthly
    double usedPercent = 0.0;   // **已用**百分比，0-100，double——上游给小数时不截断
    std::string resetsAt;       // ISO8601；无重置语义或上游未给 = 空串
};

// 一次查询的完整结果。**fetch 的唯一返回类型**（见文件头约定 1）。
// Tiered 型读 `windows`；Balance 型读 `balanceRemaining` + `currency`。
// 两者不混用：Balance 的 `windows` 必须为空（不许为凑数硬造一个窗口），
// Tiered 的 `balanceRemaining` 保持 0。
struct QuotaResult {
    QuotaKind kind = QuotaKind::Tiered;
    std::vector<QuotaWindow> windows;  // 顺序 = provider 的固定展示序
    double balanceRemaining = 0.0;     // Balance 型专用；币种原样，**不换算**
    std::string currency;              // 如 "CNY" / "USD"，原样保留
    std::string planName;              // 套餐名（上游给才有，可空）
    QuotaError error = QuotaError::None;  // 唯一错误通道（约定 2）
    std::string errorMessage;             // 面向用户的中文文案；成功时为空
};

// 供应商需要用户填的一个配置字段（G6 弹窗按 fields() **动态渲染**的依据）。
//   key    落库 / 查表的稳定键（英文，如 "accessKeyId"），与 ProviderConfig::fields 的键一致
//   label  UI 显示名（中文）
//   secret true = UI 必须以密文输入框渲染并脱敏回显；**只是渲染提示**，
//          值的存放位置 / 脱敏策略由存储层决定，G0 不表态
struct ProviderField {
    std::string key;
    std::string label;
    bool secret = false;
};

// 单个 profile 的余量查询配置（fetch 的输入）。
// 数据源：G5 从余量配置表读出 + keychain 里的 profile 密钥；G6 弹窗写入。
struct ProviderConfig {
    // 注册表 id（quota_registry 的 QuotaProviderEntry::id）。
    // 空串 = 该 profile 未配置余量查询（findProvider 返 nullptr，调用方走
    // QuotaError::NotConfigured 那条路，**不要**拿它去 fetch）。
    std::string providerId;
    // 该 profile 的 base_url。用途：G4 从 host 段取 region；G6 显示。
    // 余量接口本身用哪个域名由 provider 自己写死（如 G2 走旧域 minimaxi.com），
    // 不从这个字段派生——它只是「这个 profile 本来连哪儿」的事实记录。
    std::string baseUrl;
    // 该 profile 的 API key 明文（**仅内存**，keychain 读出后填入；沿用 profile
    // 凭据型供应商如 G1/G2 直接用它做 Bearer）。永不落日志、永不落库。
    std::string apiKey;
    // fields() 声明的键 → 用户填的值（如 G4 的 accessKeyId / secretAccessKey）。
    // fields() 为空 = 本供应商不吃额外字段（沿用 profile 凭据）。
    std::map<std::string, std::string> fields;
};

// HTTP 注入缝：provider 不直接调 platform::http::send，而是调这个可调用对象，
// 于是单测能塞假发送 → **零真实网络**；生产由 G5 注入真实实现。
// 签名逐字对齐仓库既有注入范式 module_agent/llm_client.h:22-25（HttpFn），
// 底层就是 platform/http_client.h 的自由函数 send(const Request&) 的可注入包装
//（本仓库**没有** HttpClient 类，不要造一个）。
// 约定：传给 fetch 的 HttpSendFn 必须非空；provider 见到空缝应返回
// QuotaError::Transient（**不许**直接调用导致 std::bad_function_call ——
// 项目铁律「不抛 C++ 异常」）。
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

    // base_url 探测（G6 默认选中 / 导入 profile 时自动匹配用）。
    // **默认不命中**：没覆写的供应商永远不会被自动选中——失败方向是安全的
    //（用户手动挑），反过来「误命中」会把别人的 quota 查到当前 profile 上。
    // 覆写时请用 host 的**标签边界**匹配（防 api.foo.com.evil.com 伪造），
    // 不要裸子串包含。
    virtual bool matchesBaseUrl(const std::string& baseUrl) const {
        (void)baseUrl;
        return false;
    }

    // 发一次查询。**恒返回 QuotaResult**（约定 1）：失败填 error + errorMessage，
    // 不要把失败信号塞进 windows（约定 2）。可能阻塞（网络），调用方在后台线程调。
    //
    // **const 成员（评审 m2 更正）**：语义本就是「查询」而非「修改」，四个
    // provider 的 fetch 体内零成员写入。做成 const 后 `findProvider()` 返回的
    // `const QuotaProvider*` 就能直接用——注册表的 const 返回不再与本签名
    // 矛盾，消费方不必 `const_cast`（那个 cast 会把「工厂返回的到底是不是
    // 单例」这个类型错误洗成运行时 UB，而类型系统本来能挡住）。
    virtual QuotaResult fetch(const ProviderConfig& cfg, const HttpSendFn& http) const = 0;
};

}  // namespace module::claude::quota
