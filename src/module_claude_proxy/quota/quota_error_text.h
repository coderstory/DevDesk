// src/module_claude_proxy/quota/quota_error_text.h
// QuotaError → 用户可读中文文案（**唯一文案源**，G2 定的口径，逐字照搬不改编）
//
// 为什么要从 quota_dialog_config 里拆出来（G7）：这张表现在有**三个**消费者，
// 而不是拆出来之前的两个（弹窗的「测试连接」文案 / G7 首页余量卡的错误行 /
// G5 落库时给 last_error 兜底）。留在 dialog 层会让 store 与 home 为了拿一句
// 文案去 include 一个「弹窗配置」头 —— 依赖方向是反的，且第三次引用迟早把
// 依赖图画成一个环。拆成独立 TU 后三者都只依赖本头，方向单一。
//
// 文案表本身**一个字都不改**：它是 G2 与用户一起定的口径，改文案等于
// 悄悄改产品语义。本文件只做搬迁 + 加一条「永不返回空串」的保证。
#pragma once

#include <string>

#include "quota_provider.h"

namespace module::claude::quota {

// QuotaError → 一句话中文。detail（供应商原文 / 补充说明）非空时以「：」接在
// 后面；为空时只给 headline。
//
// **永不返回空串**（唯一例外：QuotaError::None —— 那是「没有错」，本来就该
// 没有文案）。理由：空文案会被 UI 画成「什么都没发生」，是静默失败面。
std::string quotaErrorHeadline(QuotaError kind, const std::string& detail);

}  // namespace module::claude::quota