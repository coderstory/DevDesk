// src/module_memory/proposals/proposal_store.h
// 提案数据层（S4，B4 §1 契约全量落地）。唯一真相 = DevDesk proposals 表；
// telemem 侧无待审状态：卡片代理层在接受之后才调引擎执行（spec §2 卡片记账细节）。
// 契约（B4 §1/§1.2/§1.3，偏离即契约变更）：
//   - 16 字段：id/schemaVersion/kind/targetIds/result/evidence/confidence/source/
//     sourceTrust/createdAt/expiresAt/status/decidedAt/appliedAt/baseVersion/undoOf；
//     result 拆 text/type/projectId 三列存储（字段仍是契约中的一个）。
//   - 状态机：唯一起点 pending；pending → accepted | rejected | expired 三终态写定
//     不改；「已撤销」不是状态，是 undoOf 指回的派生徽章。
//   - 幂等：accept/reject 以 id 为幂等键，终态重复操作 = 直接返回成功。
//   - baseVersion 前置校验：目标记忆当前版本 ≠ baseVersion → 转 expired（证据失效）。
//   - 代理执行失败 → 提案保持 pending，失败原因进审计（面板可见，重试 = 再接受）。
//   - 同一 undoOf 全库至多一张未决撤销提案（partial unique index 钉死）；整理器重复
//     产出复用既有未决卡（刷新），不建新卡。
//   - add 类执行成功后 targetIds 回填引擎条目 id（撤销需要知道标记谁）——这是
//     16 字段之外的桥接不变量，写入审计。
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/result.h"
#include "core/sqlite_db.h"

namespace module::memory::proposals {

struct ProposalEvidence {
    std::string sessionId;
    std::int64_t eventId = 0;
    std::string quoted;
    std::int64_t observedAt = 0;
};

struct Proposal {
    std::string id;
    int schemaVersion = 1;
    std::string kind;  // merge | invalidate | forget | add（undo 类沿用逆操作 kind）
    std::vector<std::int64_t> targetIds;  // add 类执行前为空，成功后回填引擎 id
    std::string resultText;               // 失效/遗忘类为空（不改原正文）
    std::string resultType;
    std::string resultProjectId;
    std::vector<ProposalEvidence> evidence;
    double confidence = 0.0;
    std::string source;  // dedupe|conflict|decay|quarantine|inject|collect
    std::string sourceTrust;  // user_stated|assistant_derived|tool_result|external_doc
    std::int64_t createdAt = 0;
    std::int64_t expiresAt = 0;
    std::string status;  // pending|accepted|rejected|expired
    std::int64_t decidedAt = -1;
    std::int64_t appliedAt = -1;
    std::int64_t baseVersion = -1;  // add 类（无目标）恒 -1
    std::string undoOf;             // 撤销提案指向原提案 id；非撤销为空
};

// 目标记忆当前版本查询缝（baseVersion 前置校验输入）；执行动作缝（引擎落库）
using TargetVersionFn = std::function<core::Result<std::int64_t>(std::int64_t)>;
using ProposalExecutor =
    std::function<core::Result<std::string>(const Proposal&)>;

class ProposalStore {
public:
    static core::Result<ProposalStore> open(
        const std::filesystem::path& dbFile);

    // 建提案（状态恒 pending，TTL 默认 30 天）。id 生成器在 store 内
    //（prop_YYYYMMDD_hexseq）。kind/targetIds 由调用方给全。
    core::Result<Proposal> create(const std::string& kind,
                                  std::vector<std::int64_t> targetIds,
                                  const std::string& resultText,
                                  const std::string& resultType,
                                  const std::string& resultProjectId,
                                  const std::vector<ProposalEvidence>& evidence,
                                  double confidence, const std::string& source,
                                  const std::string& sourceTrust,
                                  std::int64_t nowSec,
                                  std::int64_t baseVersion = -1);

    // 接受（含代理执行）：TTL/baseVersion 校验 → executor → accepted + appliedAt。
    // executor 失败 → 保持 pending，错误信息进审计（返回 ok，appliedAt=-1 表达未落库；
    // 硬失败如库错误才返回 fail）。
    core::Result<Proposal> accept(const std::string& id, std::int64_t nowSec,
                                  const TargetVersionFn& targetVersion,
                                  const ProposalExecutor& executor);

    // 拒绝（留档）
    core::Result<Proposal> reject(const std::string& id, std::int64_t nowSec);

    // 编辑后接受的前半步：改 pending 提案正文（审计留痕；终态 = fail，不留痕）
    core::Result<void> updateResultText(const std::string& id,
                                        const std::string& text,
                                        std::int64_t nowSec);

    // 撤销链：为已接受提案建撤销卡（kind 取逆操作、undoOf 指回）；同一 undoOf 已有
    // 未决卡 → 复用并刷新（返回既有卡）。原提案 status 写定不改。
    core::Result<Proposal> createUndo(const std::string& originalId,
                                      std::int64_t nowSec,
                                      const TargetVersionFn& targetVersion);

    // TTL 到期回收（pending 且 expiresAt < now → expired）；返回回收条数
    core::Result<int> expireSweep(std::int64_t nowSec);

    core::Result<Proposal> get(const std::string& id) const;
    core::Result<std::vector<Proposal>> listPending() const;

    // 最近已决提案（decidedAt 降序；面板「已处理」视图 + 撤销入口取数）
    core::Result<std::vector<Proposal>> listRecentDecided(int limit) const;

    // 「已撤销」派生徽章判定：存在指向 id 的已接受（执行完成）撤销卡
    bool isUndone(const std::string& id) const;

private:
    ProposalStore() = default;
    std::string nextId(std::int64_t nowSec);
    core::Result<void> audit(const std::string& proposalId, std::int64_t nowSec,
                             const std::string& action,
                             const std::string& detail);
    std::shared_ptr<class core::data::SqliteDb> db_;
    std::int64_t idSeq_ = 0;
};

}  // namespace module::memory::proposals
