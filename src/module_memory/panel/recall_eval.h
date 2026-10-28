// src/module_memory/panel/recall_eval.h
// 触发线评测（定案 7）：冻结 15 查询转测引擎检索，产出 Recall@5 存库并驱动
// 主控台红线仪表。零 API 费：入库走 addDirect（infer=false，embedding 落本机
// llama-server），检索走检索接口缝（不 import 引擎）。
// 契约：
//   - doc 合成式与冻结版 eval_retrieval.py 同源（recall_eval_data.h）；
//   - 命中判定 = 前 5 名含任一 gold 等价条目（gold_ids 取值域 = doc 下标）；
//     telemem 融合段落形态下按「gold 正文包含于融合段」判（live 对账定案）；
//   - 评测结束对本次入库条目逐一打「移出自动想起」标记（幂等、尽力而为）——
//     评测数据不进 hook 注入通道，不污染用户记忆流；条目 id 缺失时按正文匹配。
//   - 引擎不在运行态时调用方必须先拦截（检索必败，不做半截评测）。
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/result.h"
#include "core/sqlite_db.h"

namespace module::memory::gateway {
class IMemoryRetriever;
}

namespace module::memory::panel {

struct RecallResult {
    std::int64_t ts = 0;
    int recallPct = -1;  // 0~100；-1 = 无记录
    int hits = 0;        // 15 查询中前 5 命中的条数
    int queries = 0;     // 参评查询数（恒 15；失败截断时如实记录）
    // 清理缺口（不落库，只进当轮快照）：id 未回填（get_memories 清单 ≤200 条/
    // 正文失配）+ 删除失败之和。>0 = 有评测条目残留引擎、可能进 hook 注入通道
    //（审计 F5：必须面板可见，禁止只 LOG_WARN）
    int cleanupFailures = 0;
};

// 评测结果库（memoryDataDir()/recall_eval.db；面板重开取 last()）
class RecallEvalStore {
public:
    static core::Result<RecallEvalStore> open(
        const std::filesystem::path& dbFile);
    core::Result<void> save(const RecallResult& result);
    RecallResult last() const;  // 无行/失败 → recallPct=-1 的空结果

private:
    RecallEvalStore() = default;
    std::shared_ptr<class core::data::SqliteDb> db_;
};

// 同步跑一轮冻结评测（只在后台线程调用——15×检索同步等待，UI 线程禁入）。
// 引擎不可达 / 单查询失败按该查询未命中计（hits 不虚增）；全部查询失败 → nullopt。
core::Result<RecallResult> runFrozenEval(gateway::IMemoryRetriever& engine);

}  // namespace module::memory::panel
