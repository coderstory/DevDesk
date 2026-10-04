// src/module_claude_proxy/quota/quota_error_text.h
// QuotaError → 用户可读中文文案（唯一文案源，与用户定的口径，改文案 = 改产品语义）。
// 拆成独立 TU 的理由：三个消费者（弹窗测试连接 / 首页错误行 / 落库兜底）都只依赖本头，
//   留在弹窗层会让 store 与 home 反向依赖「弹窗配置」头。
#pragma once

#include <string>

#include "quota_provider.h"

namespace module::claude::quota {

// QuotaError → 一句话中文。detail 非空时以「：」接在后面，为空时只给 headline。
// 永不返回空串（唯一例外 QuotaError::None）——空文案会被 UI 画成「什么都没发生」，是静默失败面。
std::string quotaErrorHeadline(QuotaError kind, const std::string& detail);

}  // namespace module::claude::quota
