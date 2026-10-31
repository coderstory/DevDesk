// src/module_memory/pipeline/infer_queue.h
// infer 提炼暂存队列（G-1）：过脱敏闸的新采集段暂存，云端提炼（infer=true）
// 逐条消费。硬档暂停 / 节流顺延 / 云端失败均留队不丢（spec 控耗 2/3 与 S3 验收
// 「入队暂存、恢复后自动补跑」）；出队成功即「已提炼水位」（控耗 1 去重口径：
// collector 事件幂等防重复摄取，本表唯一键防崩溃重放重复入队）。
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "core/result.h"
#include "core/sqlite_db.h"

namespace module::memory::pipeline {

class InferQueueStore {
public:
    static core::Result<InferQueueStore> open(
        const std::filesystem::path& dbFile);

    // 入队（INSERT OR IGNORE：同 (session_id, start_seq) 重复 = no-op）
    core::Result<void> enqueue(const std::string& sessionId,
                               std::int64_t startSeq, const std::string& text,
                               std::int64_t nowSec);

    struct Entry {
        std::int64_t id = 0;
        std::string sessionId;
        std::string text;
    };

    // FIFO 取一批（id 升序 = 入队序；提炼串行逐条消费）
    core::Result<std::vector<Entry>> listBatch(int limit) const;

    core::Result<int> depth() const;
    core::Result<void> remove(std::int64_t id);

private:
    InferQueueStore() = default;
    std::shared_ptr<class core::data::SqliteDb> db_;
};

}  // namespace module::memory::pipeline
