// src/module_claude_proxy/ccswitch_source.h
// 从 cc-switch 只读读取 Claude 供应商配置；密钥只在内存过一手，不落库、不落日志、不进文案。
// 只读硬约束：恒 SQLITE_OPEN_READONLY，不 CREATE、不写 schema、不动 -wal/-shm、不用读写连接。
// 失败语义：库不存在 / 打不开 / 表结构不认识 / JSON 字段坏 → 不抛异常，返回空列表 + 面向用户的原因；缺字段一律当空值退化。
#pragma once

#include <string>
#include <vector>

namespace module::claude::ccswitch {

// 一个 cc-switch Claude 供应商在 DevDesk 侧需要的全部信息：
struct CcSwitchProviderInfo {
    std::string id;     // cc-switch providers.id（DevDesk 侧只存这个，当引用）
    std::string name;   // 展示名（providers.name）
    // 推理 base_url（ANTHROPIC_BASE_URL，去尾斜杠），用于自动匹配（host+path 精确比对）。
    std::string baseUrl;
    // 推理密钥：ANTHROPIC_AUTH_TOKEN → ANTHROPIC_API_KEY → OPENROUTER_API_KEY → GOOGLE_API_KEY 取第一个非空串（同 first_non_empty）。仅内存。
    std::string apiKey;
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

// 按 base_url 自动匹配：host 完全相等（大小写不敏感、标签边界语义，api.deepseek.com 不命中 api.deepseek.com.evil.com）
// + path 完全相等（去尾斜杠 / query）；命中返回 provider id，未命中 / baseUrl 为空 / 来源不可用 → 空串。
std::string matchCcSwitchProviderByBaseUrl(const std::string& baseUrl);

// 打开 cc-switch 库的 sqlite flags：恒含 SQLITE_OPEN_READONLY，恒不含 READWRITE / CREATE。
// 暴露是因为 SQLite 拿不到 O_RDWR 时会静默降级成只读，「权限 + 内容没变」区分不出，单测只能直接问这个常量。
extern const int kCcSwitchOpenFlags;

}  // namespace module::claude::ccswitch
