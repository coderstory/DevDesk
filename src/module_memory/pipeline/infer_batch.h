// src/module_memory/pipeline/infer_batch.h
// infer 提炼批处理（G-1）：暂存队列 → 闸判定 → 云端提炼（infer=true）→ 记账出队。
// 闸的两级都在本地判定、先于任何云端出机（spec 控耗 2/3）：
//   - 硬档暂停（控耗 3）：月 token 账面 ≥ 硬阈值 → 暂停，队列留队；判定复用
//     TokenUsageMeter::phase（与面板消耗卡同一判定函数，恰等越线语义单点），
//     账面源 = UsageLedger::monthTokens（usage_ledger.db，与面板同库同源）；
//   - 每小时节流（控耗 2）：本小时尝试数（llm_attempt 行 COUNT）≥ 上限 → 顺延
//     下一窗口，不报错不丢数据；失败也计数（attempts 口径）约束重试风暴。
// 直存链（addDirect）与检索链不设闸——硬档只拦提炼，检索不受影响。
#pragma once

#include <cstdint>
#include <functional>

#include "module_memory/gateway/telemem_client.h"
#include "module_memory/pipeline/collect_round.h"
#include "module_memory/pipeline/infer_queue.h"
#include "module_memory/proposals/usage_ledger.h"

namespace module::memory::pipeline {

enum class InferGate { Proceed, Paused, Throttled };

// 闸判定（纯函数，单测钉恰等越线）：阈值 ≤0 = 关闭该档（同 usage_alert 外部
// 防护口径——TokenUsageMeter::phase 自身无 ≤0 防护，必须在此先挡）
InferGate inferGateDecision(long long monthTokens, long long hardBudgetTokens,
                            long long hourAttempts, long long hourlyLimit);

struct InferLimits {
    long long hardBudgetTokens = 0;  // 月硬档阈值（panel_settings 同键同源）
    long long hourlyLimit = 0;       // 每小时云端提炼调用上限（≤0 = 关闭节流）
};

struct InferStepReport {
    bool gateBlocked = false;  // 硬档暂停或节流顺延（本步无出机）
    bool paused = false;       // true = 硬档暂停（区别于节流顺延）
    bool attempted = false;    // 本步发生云端尝试
    bool succeeded = false;    // 尝试成功并出队
};

// 单步提炼（同步；宿主 = collectLoopMain 采集轮后置，每轮 1 步）。至多取队首
// 1 条：闸过 → attempt（成败都记 llm_attempt）→ 成功记账（llm_input + llm_output
// 估算）出队 / 失败留队。单步化理由：宿主 30s 节拍下小时上限自然达满，且引擎
// 停止时 join 最坏只等一次在途调用，不放大 UI 阻塞。硬档/节流为常态态不刷动态
// 流（面板既有档位文案承载）；失败不静默（note 显式原因，spec S3 验收）。
core::Result<InferStepReport> runInferStep(
    InferQueueStore& queue, gateway::IMemoryRetriever& engine,
    proposals::UsageLedger& ledger, const InferLimits& limits,
    const std::function<void(const RoundNote&)>& onNote);

}  // namespace module::memory::pipeline
