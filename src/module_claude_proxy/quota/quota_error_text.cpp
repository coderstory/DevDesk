// src/module_claude_proxy/quota/quota_error_text.cpp
// 文案表实现（从 quota_dialog_config.cpp 逐字搬迁，**零改动**）。
//
// 搬迁理由见 quota_error_text.h 头注释：这张表现在有三个消费者
//（弹窗「测试连接」/ G7 首页错误行 / G5 落库兜底），留在弹窗层会让
// store 与 home 反向依赖「弹窗配置」头。
#include "quota_error_text.h"

namespace module::claude::quota {

std::string quotaErrorHeadline(QuotaError kind, const std::string& detail) {
    // G2 定的口径，本层只做归一入口，**不改文案**。
    std::string head;
    switch (kind) {
        case QuotaError::None:
            return std::string();
        case QuotaError::Auth:
            head = "密钥无效，请换一把 API Key";
            break;
        case QuotaError::Entitlement:
            head = "套餐过期或无权限，换密钥没用，得续费或换账号";
            break;
        case QuotaError::Shape:
            head = "上游响应结构不认识（供应商可能改了接口）";
            break;
        case QuotaError::Transient:
            head = "网络失败，稍后重试";
            break;
        case QuotaError::NotConfigured:
            head = "配置不完整，请去补全";
            break;
    }
    if (detail.empty()) return head;
    return head + "：" + detail;
}

}  // namespace module::claude::quota