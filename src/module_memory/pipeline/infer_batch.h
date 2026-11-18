// src/module_memory/pipeline/infer_batch.h
// infer 提炼批处理（G-1 + F-03 波 2 腿 A）：暂存队列 → 闸判定 → 云端提炼
//（infer=true）→ 终稿分流（OQ-2 裁决：修饰成功 → 终稿卡自动入库
// auto_accepted，撤销链保留；重试耗尽 / add 失败 / 低置信 → 兜底 pending 卡
// 待人审）。
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
#include <string>

#include "module_memory/gateway/telemem_client.h"
#include "module_memory/pipeline/collect_round.h"
#include "module_memory/pipeline/infer_queue.h"
#include "module_memory/proposals/proposal_store.h"
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
    bool succeeded = false;    // 尝试成功并出队（含零产物兜底形态）
    bool autoAccepted = false;  // OQ-2：终稿卡已自动入库（auto_accepted，
                                // 撤销链保留）
    bool fallbackPending = false;  // OQ-2：兜底转人审（重试耗尽/add 失败/
                                   // 低置信 → pending 终稿卡）
    std::string fallbackReason;    // 兜底原因（note 与测试断言口径）
    bool productMissing = false;   // bug239：add 回包成功但引擎清单零新增
                                   //（现走兜底人审，保留显式上浮口径）
};

// 单步提炼（同步；宿主 = collectLoopMain 采集轮后置，每轮 1 步）。至多取队首
// 1 条：闸过 → 低置信前置分流（不出机直接兜底出队）→ attempt（成败都记
// llm_attempt）→ 成功：记账（llm_input + llm_output 估算）出队 + 终稿卡
// auto_accepted（store 落 source=final_draft）/ 失败：attempts 记账，未耗尽
// 留队下轮补跑，耗尽出队落兜底 pending 卡。单步化理由：宿主 30s 节拍下小时
// 上限自然达满，且引擎停止时 join 最坏只等一次在途调用，不放大 UI 阻塞。
// 硬档/节流为常态态不刷动态流（面板既有档位文案承载）；失败不静默（note 显式
// 原因，spec S3 验收）。
core::Result<InferStepReport> runInferStep(
    InferQueueStore& queue, gateway::IMemoryRetriever& engine,
    proposals::ProposalStore& store, proposals::UsageLedger& ledger,
    const InferLimits& limits,
    const std::function<void(const RoundNote&)>& onNote);

}  // namespace module::memory::pipeline
