// src/module_memory/pipeline/infer_queue.h
// infer 提炼暂存队列（G-1 + F-03 波 2 腿 A）：过脱敏闸的新采集段暂存，云端
// 提炼（infer=true）逐条消费。硬档暂停 / 节流顺延 / 云端失败均留队不丢（spec
// 控耗 2/3 与 S3 验收「入队暂存、恢复后自动补跑」）；出队成功即「已提炼水位」
//（控耗 1 去重口径：collector 事件幂等防重复摄取，本表唯一键防崩溃重放重复入队）。
// 腿 A 队列修复（池 #333「只进不出」根因）：条目带 attempts/last_error 失败
// 计数与原因——云端失败重试有上限（infer_batch 判耗尽），耗尽兜底/低置信兜底
// 与成功一样必出队；队列不再因队首永久失败无限滞留。
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/result.h"
#include "core/sqlite_db.h"

namespace module::memory::pipeline {

// 入队缺省置信 = collect 侧未评估值（collect_round.cpp:36 的镜像常量——那边
// 在 .cpp 匿名域不可见）。OQ-5 低置信兜底阈值 0.3 沿用此既有值（严格小于
// 才触发，collect 缺省恰好不触发）
inline constexpr double kQueueDefaultConfidence = 0.3;

class InferQueueStore {
public:
    static core::Result<InferQueueStore> open(
        const std::filesystem::path& dbFile);

    // 入队（INSERT OR IGNORE：同 (session_id, start_seq) 重复 = no-op）。
    // confidence 缺省走 kQueueDefaultConfidence——collect_round 入队点零改动
    //（腿 A 禁动 collect_round）；低于 0.3 的入队源由测试/未来调用方显式传
    core::Result<void> enqueue(const std::string& sessionId,
                               std::int64_t startSeq, const std::string& text,
                               std::int64_t nowSec,
                               double confidence = kQueueDefaultConfidence);

    struct Entry {
        std::int64_t id = 0;
        std::string sessionId;
        std::int64_t startSeq = 0;
        std::string text;
        int attempts = 0;       // 云端失败累计（重试耗尽判定输入；出队后不再读）
        std::string lastError;  // 最近一次失败原因（耗尽兜底卡文案来源）
        double confidence = kQueueDefaultConfidence;
    };

    // FIFO 取一批（id 升序 = 入队序；提炼串行逐条消费）
    core::Result<std::vector<Entry>> listBatch(int limit) const;

    core::Result<int> depth() const;

    core::Result<void> remove(std::int64_t id);

    // 失败记账：attempts + 1 并留最近原因（截 200 字符防膨胀）；返回该条目
    // 累计 attempts（重试耗尽判定输入）
    core::Result<int> recordFailure(std::int64_t id, const std::string& error);

private:
    InferQueueStore() = default;
    std::shared_ptr<class core::data::SqliteDb> db_;
};

}  // namespace module::memory::pipeline
