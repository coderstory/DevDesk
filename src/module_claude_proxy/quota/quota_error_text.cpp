// src/module_claude_proxy/quota/quota_error_text.cpp
// 文案表实现（从 quota_dialog_config.cpp 逐字搬迁，**零改动**）。
//
// 搬迁理由见 quota_error_text.h 头注释：这张表现在有三个消费者
//（弹窗「测试连接」/ G7 首页错误行 / G5 落库兜底），留在弹窗层会让
// store 与 home 反向依赖「弹窗配置」头。
//
// ⚠ 形态：**下标表 + 编译期对表**，不是 switch（静态审计 m4）。
// 曾经的 switch 有两道各自不够的防线，现在两道都留着：
//   ① switch 的 `default:` → 运行时兜底（新增枚举没配文案也**不返回空串**，
//      显示「未知错误」而不是让首页画出空白错误行）；
//   ② 下文 `static_assert(表长 == QuotaError::Count)` → 编译期兜底，**在加枚举的
//      那一刻就红**，而不是等到某个分支漏了才在真机上看出一行空白。
// ① 只保证「不空」，② 才保证「不漏」——两道缺一不可，故 table 取代 switch。
#include "quota_error_text.h"

#include <cstddef>
#include <iterator>
#include <string>

namespace module::claude::quota {

namespace {

// QuotaError → 一句话中文。**下标 == static_cast<int>(QuotaError)**。
// 文案逐字来自 G2 与用户一起定的口径，一个字都没改（改文案 = 改产品语义）。
constexpr const char* kQuotaErrorHeadline[] = {
    /* None          */ "",
    /* Auth          */ "密钥无效，请换一把 API Key",
    /* Entitlement   */ "套餐过期或无权限，换密钥没用，得续费或换账号",
    /* Shape         */ "上游响应结构不认识（供应商可能改了接口）",
    /* Transient     */ "网络失败，稍后重试",
    /* NotConfigured */ "配置不完整，请去补全",
};

// 🔴 编译期守卫（这条比 default 强，也比它早）：
// **枚举个数 == 文案表条目数**。新增第 7 个 QuotaError 而忘了在这里补一行，
// 这里直接把构建打红 —— 那一刻正是「忘了配文案」被制造出来的时刻。
// 基准取 `QuotaError::Count`（枚举内的哨兵）而不是手写常量：手写常量会与枚举
// 一起漂移（加了枚举忘了改常量 → 断言照样绿 → 假守卫），哨兵改不了。
static_assert(std::size(kQuotaErrorHeadline) ==
                  static_cast<std::size_t>(QuotaError::Count),
              "QuotaError 新增取值时必须在 kQuotaErrorHeadline 里补一条对应文案"
              "（下标顺序 = 枚举顺序）；这条断言就是防「加了错误却没人告诉用户"
              "那是什么」的那道闸");

// 枚举外的值（static_cast 造出来的、或内存被写坏）→ 显式兜底，**绝不返回空串**。
// 与 quota_home_view.cpp「未知键回落为 key 本身而非编一个中文标签」同款取向：
// 宁可难看，不编、不空。
constexpr const char* kUnknownHeadline = "未知错误";

}  // namespace

std::string quotaErrorHeadline(QuotaError kind, const std::string& detail) {
    const std::size_t idx = static_cast<std::size_t>(kind);
    const char* head = (idx < std::size(kQuotaErrorHeadline))
                           ? kQuotaErrorHeadline[idx]
                           : kUnknownHeadline;
    // 空串只允许一种情况：QuotaError::None —— 那是「没有错」，本来就该没文案。
    // 其余一律给得出话（表内命中，或上面的「未知错误」兜底）。
    if (head[0] == '\0') return std::string();
    if (detail.empty()) return std::string(head);
    return std::string(head) + "：" + detail;
}

}  // namespace module::claude::quota
