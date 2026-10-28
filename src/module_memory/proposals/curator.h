// src/module_memory/proposals/curator.h
// 整理器首版（S4）：只产两类提案，其余（merge/conflict/quarantine）后续批次。
//   ① 新增必审：采集入库前生成 pending 提案卡（kind=add, source=collect），
//     接受才真入 telemem——默认无静默入库通道；
//   ② 遗忘：闲置天数阈值扫描（kind=forget, source=decay），接受 = 移出「自动想起」
//     （标记翻转，不删行）。
// source=collect 是 B4 §1.2 枚举的扩展值（采集新增规则）；B4 五原值不动。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "module_memory/proposals/proposal_store.h"

namespace module::memory::gateway {
class IMemoryRetriever;
}

namespace module::memory::proposals {

// B4 设计值（待标定）：置信度高线 0.8（静默档门槛，本批无静默档仅占位）
inline constexpr double kHighConfidence = 0.8;

struct MemorySummary {
    // 引擎条目 id：telemem 1.10.1 live 定案 = UUID 串，非整型（F3）——遗忘卡
    // targetIds 原样使用本值；形态错位（如 to_string(整型)）会让 forget 卡
    // 执行必挂（setMemoryForgotten 按此 id 定位引擎条目）
    std::string id;
    std::string text;
    std::int64_t lastUsedAt = 0;  // 最近被想起的时间（engine 侧清单提供）
    std::int64_t version = 0;     // 当前版本（baseVersion 前置校验输入）
};

// 引擎侧记忆清单缝（遗忘扫描输入；首版由调用方注入，live 接线在后续批次）
using MemoryListFn = std::function<core::Result<std::vector<MemorySummary>>()>;

class Curator {
public:
    // 新增提案卡（采集入库前必经）：evidence 由采集层给（会话出处）
    static core::Result<Proposal> proposeAdd(
        ProposalStore& store, const std::string& text,
        const std::vector<ProposalEvidence>& evidence, double confidence,
        std::int64_t nowSec);

    // 遗忘扫描：闲置 > 阈值天数 → 每条记忆一张 forget 卡（已接受/已存在的目标
    // 不重复建卡）。返回本轮新建的提案 id 列表。
    static core::Result<std::vector<std::string>> proposeForget(
        ProposalStore& store, const MemoryListFn& memories,
        int idleThresholdDays, std::int64_t nowSec);
};

// 代理执行层（B4 §1.3 回滚表 → 引擎动作的唯一映射点）：
//   add(无 undoOf)        → addDirect(result.text)（infer=false 直存）
//   forget(无 undoOf)     → 打「移出自动想起」标记
//   forget(undoOf 置位)   → 清标记（撤销遗忘）
//   invalidate(undoOf 置位)→ 清失效标记（撤销新增）
// 返回引擎条目 id（add 类；其余空串）。
core::Result<std::string> executeProposal(
    const Proposal& p, class gateway::IMemoryRetriever& engine);

}  // namespace module::memory::proposals
