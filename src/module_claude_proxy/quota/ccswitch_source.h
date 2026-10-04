// src/module_claude_proxy/quota/ccswitch_source.h
// 从 cc-switch 只读读取 Claude 供应商配置（不复制密钥）。
// 只存 cc-switch 的 provider id 作引用、现读现取：改了 key / base_url 下次查询即生效，
//   且 DevDesk 库里没有第二份密钥副本；密钥只在内存过一手，不落库、不落日志、不进文案。
// 只读硬约束：一律 SQLITE_OPEN_READONLY（kCcSwitchOpenFlags），不 CREATE、不建表、
//   不写 schema、不动 -wal/-shm；不要拿 core/sqlite_db.cpp 的读写 SqliteDb 来读（反面教材）。
// 失败语义：库不存在 / 打不开 / 表结构不认识 / JSON 字段坏 → 不抛异常，返回空列表 +
//   面向用户的「来源不可用」原因；缺字段一律当空值退化，不整体失败。
#pragma once

#include <string>
#include <vector>

namespace module::claude::quota {

// 一个 cc-switch Claude 供应商在 DevDesk 侧需要的全部信息（字段语义对齐 cc-switch bfaaba16）。
struct CcSwitchProviderInfo {
    std::string id;     // cc-switch providers.id（DevDesk 侧只存这个，当引用）
    std::string name;   // 展示名（providers.name）
    // 推理 base_url（ANTHROPIC_BASE_URL，去尾斜杠），用于自动匹配。不是 usage_script.baseUrl——后者是余量端点覆盖，可能是另一个域，拿它匹配会匹配错。
    std::string baseUrl;
    // 推理密钥：ANTHROPIC_AUTH_TOKEN → ANTHROPIC_API_KEY → OPENROUTER_API_KEY → GOOGLE_API_KEY 取第一个非空串（同 first_non_empty）。仅内存。
    std::string apiKey;
    // usage_script.codingPlanProvider（如 "opencode_go" / "minimax" / "volcengine"）；没配 = 空串。
    std::string codingPlanProvider;
    // usage_script 的 accessKeyId / secretAccessKey：火山方舟控制面 OpenAPI 的签名凭据，与推理 key 是两套。仅内存。
    std::string accessKeyId;
    std::string secretAccessKey;
};

// 来源不可用分类；Ok 之外都是「读不到」，区别在文案与排查方向：
enum class CcSwitchSourceStatus {
    Ok,          // 读到了（providers 可能为空 = 没配 Claude 供应商）
    NotFound,    // 库文件不存在
    Unreadable,  // 存在但打不开：非 SQLite / 权限不足 / 查询失败
    Schema,      // 打得开但表结构不认识（缺 id / app_type 列）
};

// 一次读取的全部结果：status == Ok → providers 有效（可空）；status != Ok → providers 必空、
// unavailableReason 是面向用户的中文原因。status != Ok 时不得渲染成「没配供应商」（那是「来源坏了」）。
struct CcSwitchProviderList {
    std::vector<CcSwitchProviderInfo> providers;
    CcSwitchSourceStatus status = CcSwitchSourceStatus::Ok;
    std::string unavailableReason;
};

// cc-switch 库路径：CCSWITCH_DB_PATH（非空时生效，测试隔离用）> 主目录下 .cc-switch/cc-switch.db。
std::string ccSwitchDbPath();

// 读出全部 Claude 侧（app_type = "claude"）供应商。恒返回、不抛异常；行序 = 扫描序（首个命中即取）。
CcSwitchProviderList listCcSwitchProviders();

// 按 base_url 自动匹配：host 完全相等（大小写不敏感）+ path 完全相等（去尾斜杠 / query）。
// host 用标签边界语义，api.deepseek.com 不命中 api.deepseek.com.evil.com（cc-switch 用裸 contains，本模块更严）。
// 命中返回 provider id；未命中 / baseUrl 为空 / 来源不可用 → 空串。
std::string matchCcSwitchProviderByBaseUrl(const std::string& baseUrl);

// 打开 cc-switch 库的 sqlite flags：恒含 SQLITE_OPEN_READONLY，恒不含 READWRITE / CREATE。
// 暴露出来的理由：SQLite 拿不到 O_RDWR 时会静默降级成只读（chmod 0444 的库用 READWRITE
// 照样打开成功），「权限 + 内容没变」区分不出两者，单测只能直接问这个常量。
extern const int kCcSwitchOpenFlags;

}  // namespace module::claude::quota
