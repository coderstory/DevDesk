// src/module_claude_proxy/quota/quota_error_text.cpp
// 文案表实现（从 quota_dialog_config.cpp 逐字搬迁，零改动）。
// 形态：下标表 + 编译期对表，不是 switch。两道防线缺一不可：
//   ① 下标越界 → 运行时兜底 kUnknownHeadline（不返回空串）；
//   ② static_assert(表长 == QuotaError::Count) → 加枚举忘配文案时编译期红。
// ① 保证「不空」，② 保证「不漏」。
#include "quota_error_text.h"

#include <cstddef>
#include <iterator>
#include <string>

namespace module::claude::quota {

namespace {

// QuotaError → 一句话中文。下标 == static_cast<int>(QuotaError)。文案是与用户定的口径，改文案 = 改产品语义。
constexpr const char* kQuotaErrorHeadline[] = {
    /* None          */ "",
    /* Auth          */ "密钥无效，请换一把 API Key",
    /* Entitlement   */ "套餐过期或无权限，换密钥没用，得续费或换账号",
    /* Shape         */ "上游响应结构不认识（供应商可能改了接口）",
    /* Transient     */ "网络失败，稍后重试",
    /* NotConfigured */ "配置不完整，请去补全",
};

// 编译期守卫：枚举个数 == 文案表条目数。基准取枚举内哨兵 QuotaError::Count（手写常量会与枚举一起漂移 → 假守卫，哨兵改不了）。
static_assert(std::size(kQuotaErrorHeadline) ==
                  static_cast<std::size_t>(QuotaError::Count),
              "QuotaError 新增取值时必须在 kQuotaErrorHeadline 里补一条对应文案"
              "（下标顺序 = 枚举顺序）；这条断言就是防「加了错误却没人告诉用户"
              "那是什么」的那道闸");

// 枚举外的值（static_cast 造的 / 内存写坏）→ 显式兜底，绝不返回空串（宁可难看，不编、不空）。
constexpr const char* kUnknownHeadline = "未知错误";

}  // namespace

std::string quotaErrorHeadline(QuotaError kind, const std::string& detail) {
    const std::size_t idx = static_cast<std::size_t>(kind);
    const char* head = (idx < std::size(kQuotaErrorHeadline))
                           ? kQuotaErrorHeadline[idx]
                           : kUnknownHeadline;
    // 空串只允许 QuotaError::None（「没有错」）；其余一律给得出话。
    if (head[0] == '\0') return std::string();
    if (detail.empty()) return std::string(head);
    return std::string(head) + "：" + detail;
}

}  // namespace module::claude::quota
