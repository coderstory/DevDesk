// src/module_claude_proxy/quota/ccswitch_source.h
// H：从 cc-switch **只读**读取供应商配置（不复制密钥）
// （用户需求：「火山 minimax deepseek opencode go 的配置从 cc-switch 里读取」）
//
// ── 为什么是「引用」而不是「复制」────────────────────────────────────────
// 本模块**不把** cc-switch 的密钥搬进 DevDesk 的库。DevDesk 侧只存 cc-switch 的
// provider `id`（引用），要用的时候现读现取。于是：
//   - 用户在 cc-switch 里改了 key / 换了 base_url，DevDesk 下一次查询立刻生效，
//     不需要任何同步 / 迁移 / 失效通知；
//   - DevDesk 的库（devdesk.db + WAL）里**没有**第二份密钥副本，少一个泄漏面。
// 密钥只在内存里过一手：不落 DevDesk 的库、不落日志、不进错误文案。
//
// ── 只读硬约束（本文件存在的另一半理由）────────────────────────────────
// cc-switch 的库是**别的应用**的文件，DevDesk 只借不还：
//   - 一律 SQLITE_OPEN_READONLY 打开（kCcSwitchOpenFlags），绝不 READWRITE、
//     绝不 CREATE（打开一个不存在的路径**不许**把它创建出来）；
//   - 不建表、不写 schema、不 VACUUM、不改文件权限、不动 -wal/-shm/-journal。
// 反面教材是本仓库 core/sqlite_db.cpp:229 的 SqliteDb（READWRITE|CREATE|FULLMUTEX）
// ——**不要**图省事拿它来读 cc-switch 的库。
//
// ── 失败语义 ─────────────────────────────────────────────────────────────
// 库不存在 / 打不开 / 不是 SQLite / 表结构不认识 → **不抛异常**（项目铁律），
// 返回空列表 + 一个面向用户的「来源不可用」原因（CcSwitchProviderList）。
// meta / settings_config 里缺字段、值不是对象、根本不是合法 JSON → 一律**当空值
// 退化**，不整体失败（cc-switch 的 schema 会演进，我们只取认得的字段）。
#pragma once

#include <string>
#include <vector>

namespace module::claude::quota {

// 一个 cc-switch Claude 供应商在 DevDesk 侧需要的全部信息。
// 字段语义逐条对齐 cc-switch（rev bfaaba16）：
//   baseUrl / apiKey  ← src-tauri/src/provider.rs:145 `resolve_usage_credentials`
//                       的 `AppType::Claude` 分支（settings_config.env 形态）
//   其余 usage_script  ← 同文件 :268 `UsageScript` / :444 `ProviderMeta`
struct CcSwitchProviderInfo {
    std::string id;     // cc-switch providers.id（DevDesk 侧**只存这个**，当引用）
    std::string name;   // 展示名（providers.name）
    // 该 profile 的**推理** base_url（settings_config.env.ANTHROPIC_BASE_URL，
    // 去掉尾斜杠后）。用途：G6 按它自动匹配。**不是** usage_script.baseUrl ——
    // 后者是「余量查询专用端点」覆盖，可能压根是另一个域（NewAPI 面板），拿它
    // 做匹配会匹配到错的供应商上。
    std::string baseUrl;
    // 推理密钥（env 里 ANTHROPIC_AUTH_TOKEN → ANTHROPIC_API_KEY →
    // OPENROUTER_API_KEY → GOOGLE_API_KEY 取**第一个非空串**，与
    // resolve_usage_credentials 的 first_non_empty 同序）。**仅内存**。
    std::string apiKey;
    // usage_script.codingPlanProvider（cc-switch 侧的供应商判定，如
    // "opencode_go" / "minimax" / "volcengine" / "zhipu_team"）；没配 = 空串。
    std::string codingPlanProvider;
    // usage_script.accessKeyId / secretAccessKey：火山方舟**控制面 OpenAPI** 的
    // 签名凭据，与上面的推理 key 是两套东西（cc-switch 注释原文如此）。**仅内存**。
    std::string accessKeyId;
    std::string secretAccessKey;
};

// 来源不可用的分类。Ok 之外都是「读不到」，区别只在文案与排查方向：
enum class CcSwitchSourceStatus {
    Ok,          // 读到了（providers 可能为空——那是「cc-switch 里没配 Claude 供应商」）
    NotFound,    // 库文件不存在（没装 cc-switch / 没跑过）
    Unreadable,  // 存在但打不开：非 SQLite 文件、权限不足、查询失败
    Schema,      // 打得开但 providers 表不是我们认识的形状（缺 id / app_type 列）
};

// 一次读取的全部结果。providers 与 unavailableReason 的对应关系：
//   status == Ok        → providers 有效（可能为空），unavailableReason 为空串
//   status != Ok        → providers 必为空，unavailableReason 是面向用户的中文原因
// 调用方**不得**在 status != Ok 时把 providers 当「没有供应商」渲染——那是
// 「来源坏了」，不是「没配」。
struct CcSwitchProviderList {
    std::vector<CcSwitchProviderInfo> providers;
    CcSwitchSourceStatus status = CcSwitchSourceStatus::Ok;
    std::string unavailableReason;
};

// cc-switch 库的路径：环境变量 CCSWITCH_DB_PATH（非空时生效；测试隔离用）
// > 平台用户主目录下的 .cc-switch/cc-switch.db（与 cc-switch 自己的
// dirs::home_dir().join(".cc-switch") 同源）。
std::string ccSwitchDbPath();

// 读出全部 Claude 侧（app_type = "claude"）供应商。恒返回，不抛异常。
// 行序 = providers 表的扫描序（匹配取首个命中，保证确定性）。
CcSwitchProviderList listCcSwitchProviders();

// 按 base_url 自动匹配（G6 的 `auto` 模式；用户在弹窗里手改 = `manual:<id>`，
// 那条路不需要本函数——调用方直接拿 id 去 listCcSwitchProviders() 里找）。
// 匹配 = **host 完全相等（大小写不敏感）+ path 完全相等（去尾斜杠 / 去 query）**。
// host 用标签边界语义：`api.deepseek.com` 不会命中 `api.deepseek.com.evil.com`
// / `notapi.deepseek.com`（cc-switch 自己用的是裸 `contains`，本模块刻意更严）。
// 命中返回 provider id；未命中 / baseUrl 为空 / 来源不可用 → 空串。
std::string matchCcSwitchProviderByBaseUrl(const std::string& baseUrl);

// 打开 cc-switch 库用的 sqlite open flags：**恒含 SQLITE_OPEN_READONLY，
// 恒不含 SQLITE_OPEN_READWRITE / SQLITE_OPEN_CREATE**。
//
// 为什么把它暴露出来（而不是留在 .cpp 里）：SQLite 在拿不到 O_RDWR 时会
// **静默降级**成只读连接——对一个 chmod 0444 的库，SQLITE_OPEN_READWRITE 照样
// 打开成功、照样能读，只是写的时候才报 "attempt to write a readonly database"。
// 也就是说「文件权限 + 内容没变」这一对观察**区分不出** READONLY 与 READWRITE，
// 单测只能直接问这个常量（用例见 test_claude_quota_ccswitch.cpp）。
extern const int kCcSwitchOpenFlags;

}  // namespace module::claude::quota
