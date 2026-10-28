// src/module_memory/proposals/proposal_store.cpp
#include "proposal_store.h"
#include <utility>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "core/logging.h"
#include "core/time.h"
#include "core/sqlite_db.h"
#include "platform/json.h"

namespace module::memory::proposals {

namespace {

using platform::json::JsonObject;
using platform::json::JsonValue;

constexpr const char* kSchema = R"(
CREATE TABLE IF NOT EXISTS memory_proposals (
  id TEXT PRIMARY KEY,
  schema_version INTEGER NOT NULL,
  kind TEXT NOT NULL,
  target_ids TEXT NOT NULL,
  result_text TEXT NOT NULL,
  result_type TEXT NOT NULL,
  result_project_id TEXT NOT NULL,
  evidence TEXT NOT NULL,
  confidence REAL NOT NULL,
  source TEXT NOT NULL,
  source_trust TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL,
  status TEXT NOT NULL,
  decided_at INTEGER,
  applied_at INTEGER,
  base_version INTEGER,
  undo_of TEXT NOT NULL DEFAULT ''
);
CREATE UNIQUE INDEX IF NOT EXISTS ux_proposals_pending_undo
  ON memory_proposals(undo_of) WHERE undo_of <> '' AND status = 'pending';
CREATE TABLE IF NOT EXISTS memory_proposal_audit (
  seq INTEGER PRIMARY KEY AUTOINCREMENT,
  proposal_id TEXT NOT NULL,
  at INTEGER NOT NULL,
  action TEXT NOT NULL,
  detail TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS memory_proposal_idseq (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  seq INTEGER NOT NULL
);
)";

// B4 设计值（待标定）：提案 TTL 30 天
constexpr int kProposalTtlDays = 30;

std::vector<std::string> parseIds(const std::string& json) {
    std::vector<std::string> out;
    const auto v = platform::json::parse(json);
    if (v.isOk() && v.value().isArray()) {
        for (int i = 0; i < v.value().size(); ++i) {
            const JsonValue& e = v.value()[i];
            if (e.isString()) out.push_back(e.asString());
            else if (e.isNumber()) out.push_back(std::to_string(e.asInt()));
        }
    }
    return out;
}

std::string idsToJson(const std::vector<std::string>& ids) {
    JsonValue arr = platform::json::makeArray();
    for (const std::string& id : ids) arr.append(platform::json::makeString(id));
    const auto s = platform::json::stringify(arr);
    return s.isOk() ? s.value() : std::string("[]");
}

std::string evidenceToJson(const std::vector<ProposalEvidence>& ev) {
    JsonValue arr = platform::json::makeArray();
    for (const auto& e : ev) {
        JsonObject obj;
        obj.set("sessionId", platform::json::makeString(e.sessionId));
        obj.set("eventId", platform::json::makeInt(e.eventId));
        obj.set("quoted", platform::json::makeString(e.quoted));
        obj.set("observedAt", platform::json::makeInt(e.observedAt));
        arr.append(JsonValue(obj));
    }
    const auto s = platform::json::stringify(arr);
    return s.isOk() ? s.value() : std::string("[]");
}

std::vector<ProposalEvidence> parseEvidence(const std::string& json) {
    std::vector<ProposalEvidence> out;
    const auto v = platform::json::parse(json);
    if (v.isOk() && v.value().isArray()) {
        for (int i = 0; i < v.value().size(); ++i) {
            if (!v.value()[i].isObject()) continue;
            ProposalEvidence e;
            const auto& obj = v.value()[i];
            e.sessionId = obj.asObject().get("sessionId").asString();
            e.eventId = obj.asObject().get("eventId").asInt();
            e.quoted = obj.asObject().get("quoted").asString();
            e.observedAt = obj.asObject().get("observedAt").asInt();
            out.push_back(std::move(e));
        }
    }
    return out;
}

Proposal rowToProposal(const core::data::SqlRow& row) {
    Proposal p;
    p.id = row.getString("id").valueOr("");
    p.schemaVersion = static_cast<int>(row.getInt64("schema_version").valueOr(1));
    p.kind = row.getString("kind").valueOr("");
    p.targetIds = parseIds(row.getString("target_ids").valueOr("[]"));
    p.resultText = row.getString("result_text").valueOr("");
    p.resultType = row.getString("result_type").valueOr("");
    p.resultProjectId = row.getString("result_project_id").valueOr("");
    p.evidence = parseEvidence(row.getString("evidence").valueOr("[]"));
    p.confidence = row.getDouble("confidence").valueOr(0.0);
    p.source = row.getString("source").valueOr("");
    p.sourceTrust = row.getString("source_trust").valueOr("");
    p.createdAt = row.getInt64("created_at").valueOr(0);
    p.expiresAt = row.getInt64("expires_at").valueOr(0);
    p.status = row.getString("status").valueOr("");
    p.decidedAt = row.getInt64("decided_at").valueOr(-1);
    p.appliedAt = row.getInt64("applied_at").valueOr(-1);
    p.baseVersion = row.getInt64("base_version").valueOr(-1);
    p.undoOf = row.getString("undo_of").valueOr("");
    return p;
}

constexpr const char* kSelectCols =
    "SELECT id, schema_version, kind, target_ids, result_text, result_type, "
    "result_project_id, evidence, confidence, source, source_trust, created_at, "
    "expires_at, status, decided_at, applied_at, base_version, undo_of FROM "
    "memory_proposals ";

}  // namespace

core::Result<ProposalStore> ProposalStore::open(
    const std::filesystem::path& dbFile) {
    auto db = core::data::SqliteDb::open(dbFile);
    if (db.isFail()) {
        return core::Result<ProposalStore>::fail(db.errorCode(),
                                                 db.errorMessage());
    }
    auto applied = db.value().applySchema(kSchema);
    if (applied.isFail()) {
        return core::Result<ProposalStore>::fail(applied.errorCode(),
                                                 applied.errorMessage());
    }
    // 必有种子行（CHECK(id=1)）；seq 只增
    auto seq = db.value().prepare(
        "INSERT INTO memory_proposal_idseq(id, seq) VALUES(1, 0) "
        "ON CONFLICT(id) DO NOTHING");
    if (seq.isOk()) {
        auto step = seq.value().step();
        if (step.isFail()) {
            return core::Result<ProposalStore>::fail(step.errorCode(),
                                                     step.errorMessage());
        }
    }
    ProposalStore store;
    store.db_ = std::make_shared<core::data::SqliteDb>(std::move(db.value()));
    return core::Result<ProposalStore>::ok(std::move(store));
}

std::string ProposalStore::nextId(std::int64_t nowSec) {
    auto stmt = db_->prepare(
        "UPDATE memory_proposal_idseq SET seq = seq + 1 WHERE id = 1");
    std::int64_t seq = 0;
    if (stmt.isOk()) {
        if (stmt.value().step().isOk()) {
            auto q = db_->prepare("SELECT seq FROM memory_proposal_idseq");
            if (q.isOk()) {
                auto s = q.value().step();
                if (s.isOk() && s.value()) {
                    const auto rowResult = q.value().currentRow();
                    if (rowResult.isOk()) {
                        seq = rowResult.value().getInt64("seq").valueOr(0);
                    }
                }
            }
        }
    }
    std::tm tm{};
    core::time::localTime(static_cast<std::time_t>(nowSec), &tm);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "prop_%04d%02d%02d_%06llx",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  static_cast<unsigned long long>(seq));
    return buf;
}

core::Result<void> ProposalStore::audit(const std::string& proposalId,
                                        std::int64_t nowSec,
                                        const std::string& action,
                                        const std::string& detail) {
    auto stmt = db_->prepare(
        "INSERT INTO memory_proposal_audit(proposal_id, at, action, detail) "
        "VALUES(?, ?, ?, ?)");
    if (stmt.isFail()) {
        return core::Result<void>::fail(stmt.errorCode(), stmt.errorMessage());
    }
    if (!stmt.value().bind(1, proposalId).isOk() ||
        !stmt.value().bind(2, nowSec).isOk() ||
        !stmt.value().bind(3, action).isOk() ||
        !stmt.value().bind(4, detail).isOk()) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "审计写入绑定失败");
    }
    auto step = stmt.value().step();
    if (step.isFail()) {
        return core::Result<void>::fail(step.errorCode(), step.errorMessage());
    }
    return core::Result<void>::ok();
}

core::Result<Proposal> ProposalStore::create(
    const std::string& kind, std::vector<std::string> targetIds,
    const std::string& resultText, const std::string& resultType,
    const std::string& resultProjectId,
    const std::vector<ProposalEvidence>& evidence, double confidence,
    const std::string& source, const std::string& sourceTrust,
    std::int64_t nowSec, std::int64_t baseVersion) {
    Proposal p;
    p.id = nextId(nowSec);
    p.kind = kind;
    p.targetIds = std::move(targetIds);
    p.resultText = resultText;
    p.resultType = resultType;
    p.resultProjectId = resultProjectId;
    p.evidence = evidence;
    p.confidence = confidence;
    p.source = source;
    p.sourceTrust = sourceTrust;
    p.createdAt = nowSec;
    p.expiresAt = nowSec + kProposalTtlDays * 86400;
    p.status = "pending";

    auto stmt = db_->prepare(
        "INSERT INTO memory_proposals(id, schema_version, kind, target_ids, "
        "result_text, result_type, result_project_id, evidence, confidence, "
        "source, source_trust, created_at, expires_at, status, decided_at, "
        "applied_at, base_version, undo_of) VALUES(?,1,?,?,?,?,?,?,?,?,?,?,?, "
        "?, NULL, NULL, ?, '')");
    if (stmt.isFail()) {
        return core::Result<Proposal>::fail(stmt.errorCode(),
                                            stmt.errorMessage());
    }
    p.baseVersion = baseVersion;
    if (!stmt.value().bind(1, p.id).isOk() ||
        !stmt.value().bind(2, p.kind).isOk() ||
        !stmt.value().bind(3, idsToJson(p.targetIds)).isOk() ||
        !stmt.value().bind(4, p.resultText).isOk() ||
        !stmt.value().bind(5, p.resultType).isOk() ||
        !stmt.value().bind(6, p.resultProjectId).isOk() ||
        !stmt.value().bind(7, evidenceToJson(p.evidence)).isOk() ||
        !stmt.value().bind(8, p.confidence).isOk() ||
        !stmt.value().bind(9, p.source).isOk() ||
        !stmt.value().bind(10, p.sourceTrust).isOk() ||
        !stmt.value().bind(11, p.createdAt).isOk() ||
        !stmt.value().bind(12, p.expiresAt).isOk() ||
        !stmt.value().bind(13, p.status).isOk() ||
        !stmt.value().bind(14, p.baseVersion).isOk()) {
        return core::Result<Proposal>::fail(core::ErrorCode::Internal,
                                            "提案写入绑定失败");
    }
    auto step = stmt.value().step();
    if (step.isFail()) {
        return core::Result<Proposal>::fail(step.errorCode(),
                                            step.errorMessage());
    }
    if (auto a = audit(p.id, nowSec, "created",
                       "kind=" + kind + " source=" + source); a.isFail()) {
        LOG_WARN("提案审计写入失败（不阻断建卡）: " + a.errorMessage());
    }
    return core::Result<Proposal>::ok(std::move(p));
}

core::Result<Proposal> ProposalStore::get(const std::string& id) const {
    auto stmt = db_->prepare(std::string(kSelectCols) + "WHERE id = ?");
    if (stmt.isFail()) {
        return core::Result<Proposal>::fail(stmt.errorCode(),
                                            stmt.errorMessage());
    }
    if (!stmt.value().bind(1, id).isOk()) {
        return core::Result<Proposal>::fail(core::ErrorCode::Internal,
                                            "提案查询绑定失败");
    }
    auto step = stmt.value().step();
    if (step.isFail()) {
        return core::Result<Proposal>::fail(step.errorCode(),
                                            step.errorMessage());
    }
    if (!step.value()) {
        return core::Result<Proposal>::fail(core::ErrorCode::NotFound,
                                            "提案不存在: " + id);
    }
    const auto row = stmt.value().currentRow();
    if (row.isFail()) {
        return core::Result<Proposal>::fail(row.errorCode(),
                                            row.errorMessage());
    }
    return core::Result<Proposal>::ok(rowToProposal(row.value()));
}

core::Result<std::vector<Proposal>> ProposalStore::listPending() const {
    auto stmt = db_->prepare(std::string(kSelectCols) +
                             "WHERE status = 'pending' ORDER BY created_at");
    if (stmt.isFail()) {
        return core::Result<std::vector<Proposal>>::fail(stmt.errorCode(),
                                                         stmt.errorMessage());
    }
    std::vector<Proposal> out;
    for (;;) {
        auto step = stmt.value().step();
        if (step.isFail()) {
            return core::Result<std::vector<Proposal>>::fail(step.errorCode(),
                                                             step.errorMessage());
        }
        if (!step.value()) break;
        const auto row = stmt.value().currentRow();
        if (row.isFail()) {
            return core::Result<std::vector<Proposal>>::fail(row.errorCode(),
                                                             row.errorMessage());
        }
        out.push_back(rowToProposal(row.value()));
    }
    return core::Result<std::vector<Proposal>>::ok(std::move(out));
}

core::Result<std::vector<Proposal>> ProposalStore::listRecentDecided(
    int limit) const {
    auto stmt = db_->prepare(std::string(kSelectCols) +
                             "WHERE status != 'pending' "
                             "ORDER BY decided_at DESC, created_at DESC LIMIT " +
                             std::to_string(limit > 0 ? limit : 1));
    if (stmt.isFail()) {
        return core::Result<std::vector<Proposal>>::fail(stmt.errorCode(),
                                                         stmt.errorMessage());
    }
    std::vector<Proposal> out;
    for (;;) {
        auto step = stmt.value().step();
        if (step.isFail()) {
            return core::Result<std::vector<Proposal>>::fail(step.errorCode(),
                                                             step.errorMessage());
        }
        if (!step.value()) break;
        const auto row = stmt.value().currentRow();
        if (row.isFail()) {
            return core::Result<std::vector<Proposal>>::fail(row.errorCode(),
                                                             row.errorMessage());
        }
        out.push_back(rowToProposal(row.value()));
    }
    return core::Result<std::vector<Proposal>>::ok(std::move(out));
}

core::Result<void> ProposalStore::updateResultText(const std::string& id,
                                                   const std::string& text,
                                                   std::int64_t nowSec) {
    auto current = get(id);
    if (current.isFail()) {
        return core::Result<void>::fail(current.errorCode(),
                                        current.errorMessage());
    }
    if (current.value().status != "pending") {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                        "终态提案不可编辑");
    }
    auto stmt = db_->prepare(
        "UPDATE memory_proposals SET result_text=? WHERE id=?");
    if (stmt.isFail()) {
        return core::Result<void>::fail(stmt.errorCode(), stmt.errorMessage());
    }
    if (!stmt.value().bind(1, text).isOk() ||
        !stmt.value().bind(2, id).isOk()) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "编辑绑定失败");
    }
    auto step = stmt.value().step();
    if (step.isFail()) {
        return core::Result<void>::fail(step.errorCode(), step.errorMessage());
    }
    return audit(id, nowSec, "edited", "result_text_updated");
}

core::Result<Proposal> ProposalStore::accept(const std::string& id,
                                             std::int64_t nowSec,
                                             const TargetVersionFn& targetVersion,
                                             const ProposalExecutor& executor) {
    auto current = get(id);
    if (current.isFail()) return current;
    Proposal p = current.value();

    // 幂等：终态重复接受 = 直接返回
    if (p.status != "pending") return core::Result<Proposal>::ok(std::move(p));

    // TTL：过期即作废（不是拒绝；证据若仍成立由整理器重新产出）
    if (p.expiresAt <= nowSec) {
        auto stmt = db_->prepare(
            "UPDATE memory_proposals SET status='expired' WHERE id=?");
        if (stmt.isOk()) {
            if (stmt.value().bind(1, id).isOk()) stmt.value().step();
        }
        if (auto a = audit(id, nowSec, "expired", "ttl"); a.isFail()) {
            return core::Result<Proposal>::fail(a.errorCode(),
                                                a.errorMessage());
        }
        p.status = "expired";
        return core::Result<Proposal>::ok(std::move(p));
    }

    // baseVersion 前置校验：目标已变 → expired（防把新改动盖掉）
    for (const std::string& tid : p.targetIds) {
        const auto v = targetVersion(tid);
        if (v.isFail() || v.value() != p.baseVersion) {
            auto stmt = db_->prepare(
                "UPDATE memory_proposals SET status='expired' WHERE id=?");
            if (stmt.isOk()) {
                if (stmt.value().bind(1, id).isOk()) stmt.value().step();
            }
            if (auto a = audit(id, nowSec, "expired", "target_changed");
                a.isFail()) {
                return core::Result<Proposal>::fail(a.errorCode(),
                                                    a.errorMessage());
            }
            p.status = "expired";
            return core::Result<Proposal>::ok(std::move(p));
        }
    }

    // 撤销类卡缺目标 = 无法定位引擎条目（防御性复检，与 createUndo 同因）：
    // 快速失败给明确文案，不进 executor 循环报误导性的「可重试」
    if (!p.undoOf.empty() && p.targetIds.empty()) {
        if (auto a = audit(id, nowSec, "apply_rejected",
                           "undo_without_target_ids"); a.isFail()) {
            return core::Result<Proposal>::fail(a.errorCode(),
                                                a.errorMessage());
        }
        return core::Result<Proposal>::fail(
            core::ErrorCode::ValidationError,
            "不可撤销：引擎条目 id 缺失（原卡入库时未回填），此卡只能拒绝");
    }

    // 代理执行（失败重试 = 再次接受；保持 pending，失败原因进审计）
    auto applied = executor(p);
    if (applied.isFail()) {
        if (auto a = audit(id, nowSec, "apply_failed",
                           applied.errorMessage()); a.isFail()) {
            return core::Result<Proposal>::fail(a.errorCode(),
                                                a.errorMessage());
        }
        return core::Result<Proposal>::ok(std::move(p));  // 保持 pending
    }

    // add 类回填引擎条目 id（撤销需要知道标记谁；桥接不变量）
    if (p.kind == "add" && !applied.value().empty()) {
        if (!p.targetIds.empty()) p.targetIds.clear();
        // 引擎 id 形态 = telemem UUID 串（2026-10-28 live 对账定案；旧假设整型
        // 与真实形态不符，UUID 被丢弃 → 撤销链 targetIds 恒空必挂）——原样入列
        p.targetIds.push_back(applied.value());
        auto stmt = db_->prepare(
            "UPDATE memory_proposals SET target_ids=? WHERE id=?");
        if (stmt.isOk()) {
            if (stmt.value().bind(1, idsToJson(p.targetIds)).isOk() &&
                stmt.value().bind(2, id).isOk()) {
                stmt.value().step();
            }
        }
        if (auto a = audit(id, nowSec, "engine_id", applied.value());
            a.isFail()) {
            return core::Result<Proposal>::fail(a.errorCode(),
                                                a.errorMessage());
        }
    }

    auto stmt = db_->prepare(
        "UPDATE memory_proposals SET status='accepted', decided_at=?, "
        "applied_at=? WHERE id=?");
    if (stmt.isFail()) {
        return core::Result<Proposal>::fail(stmt.errorCode(),
                                            stmt.errorMessage());
    }
    if (!stmt.value().bind(1, nowSec).isOk() ||
        !stmt.value().bind(2, nowSec).isOk() ||
        !stmt.value().bind(3, id).isOk()) {
        return core::Result<Proposal>::fail(core::ErrorCode::Internal,
                                            "接受更新绑定失败");
    }
    auto step = stmt.value().step();
    if (step.isFail()) {
        return core::Result<Proposal>::fail(step.errorCode(),
                                            step.errorMessage());
    }
    if (auto a = audit(id, nowSec, "accepted", ""); a.isFail()) {
        return core::Result<Proposal>::fail(a.errorCode(), a.errorMessage());
    }
    return get(id);
}

core::Result<Proposal> ProposalStore::reject(const std::string& id,
                                             std::int64_t nowSec) {
    auto current = get(id);
    if (current.isFail()) return current;
    Proposal p = current.value();
    if (p.status != "pending") return core::Result<Proposal>::ok(std::move(p));
    auto stmt = db_->prepare(
        "UPDATE memory_proposals SET status='rejected', decided_at=? "
        "WHERE id=?");
    if (stmt.isFail()) {
        return core::Result<Proposal>::fail(stmt.errorCode(),
                                            stmt.errorMessage());
    }
    if (!stmt.value().bind(1, nowSec).isOk() ||
        !stmt.value().bind(2, id).isOk()) {
        return core::Result<Proposal>::fail(core::ErrorCode::Internal,
                                            "拒绝更新绑定失败");
    }
    auto step = stmt.value().step();
    if (step.isFail()) {
        return core::Result<Proposal>::fail(step.errorCode(),
                                            step.errorMessage());
    }
    if (auto a = audit(id, nowSec, "rejected", ""); a.isFail()) {
        return core::Result<Proposal>::fail(a.errorCode(), a.errorMessage());
    }
    return get(id);
}

core::Result<Proposal> ProposalStore::createUndo(const std::string& originalId,
                                                 std::int64_t nowSec,
                                                 const TargetVersionFn& targetVersion) {
    auto original = get(originalId);
    if (original.isFail()) return original;
    // 逆操作 kind 映射（B4 §1.3 回滚表）：add→invalidate（新增打失效，不删行）；
    // forget→forget（undoOf 置位时执行器语义取反 = 清「移出自动想起」标记）
    std::string undoKind;
    if (original.value().kind == "add") {
        undoKind = "invalidate";
    } else if (original.value().kind == "forget") {
        undoKind = "forget";
    } else {
        return core::Result<Proposal>::fail(
            core::ErrorCode::ValidationError,
            "首版仅支持 add/forget 的撤销: " + original.value().kind);
    }
    // add 类 id 回填失败（正文失配/超 get_memories 清单上限）→ targetIds 恒空，
    // 此类撤销卡接受必 ValidationError 且「再点接受重试」永不成立（审计 F6）——
    // 建卡前即如实拒绝，不给用户只能拒绝的死循环卡
    if (undoKind == "invalidate" && original.value().targetIds.empty()) {
        return core::Result<Proposal>::fail(
            core::ErrorCode::ValidationError,
            "不可撤销：原新增卡未回填引擎条目 id（入库时正文失配或超清单上限），"
            "引擎侧无对象可删除");
    }

    // 同一 undoOf 至多一张未决撤销卡：复用并刷新
    auto stmt = db_->prepare(std::string(kSelectCols) +
                             "WHERE undo_of = ? AND status = 'pending'");
    if (stmt.isFail()) {
        return core::Result<Proposal>::fail(stmt.errorCode(),
                                            stmt.errorMessage());
    }
    if (!stmt.value().bind(1, originalId).isOk()) {
        return core::Result<Proposal>::fail(core::ErrorCode::Internal,
                                            "撤销查询绑定失败");
    }
    {
        auto step = stmt.value().step();
        if (step.isFail()) {
            return core::Result<Proposal>::fail(step.errorCode(),
                                                step.errorMessage());
        }
        if (step.value()) {
            const auto row = stmt.value().currentRow();
            if (row.isFail()) {
                return core::Result<Proposal>::fail(row.errorCode(),
                                                    row.errorMessage());
            }
            Proposal existing = rowToProposal(row.value());
            // 刷新 TTL（复用不新建；B4 §1.2 不变量）
            auto upd = db_->prepare(
                "UPDATE memory_proposals SET expires_at=? WHERE id=?");
            if (upd.isOk()) {
                if (upd.value().bind(1, nowSec + kProposalTtlDays * 86400)
                            .isOk() &&
                    upd.value().bind(2, existing.id).isOk()) {
                    upd.value().step();
                }
            }
            if (auto a = audit(existing.id, nowSec, "undo_refreshed",
                               originalId); a.isFail()) {
                return core::Result<Proposal>::fail(a.errorCode(),
                                                    a.errorMessage());
            }
            existing.expiresAt = nowSec + kProposalTtlDays * 86400;
            return core::Result<Proposal>::ok(std::move(existing));
        }
    }

    // 原提案必须已接受（撤销只对已生效的动作有意义）
    if (original.value().status != "accepted") {
        return core::Result<Proposal>::fail(
            core::ErrorCode::ValidationError,
            "仅已接受的提案可撤销，当前: " + original.value().status);
    }

    Proposal undo;
    undo.id = nextId(nowSec);
    undo.kind = undoKind;
    undo.targetIds = original.value().targetIds;
    // B4 §1.2：撤销提案 baseVersion = 生成时 targetIds 各自的现值（失配拒绝建卡）
    if (!undo.targetIds.empty()) {
        const auto v = targetVersion(undo.targetIds[0]);
        if (v.isFail()) {
            return core::Result<Proposal>::fail(v.errorCode(),
                                                "撤销目标版本不可得: " +
                                                    v.errorMessage());
        }
        undo.baseVersion = v.value();
    } else {
        undo.baseVersion = -1;
    }
    undo.undoOf = originalId;
    // 证据透传（2026-10-28 live 定案）：forget 撤销 = 引擎 delete 不可逆后按
    // 原卡证据正文重录入，执行器从 undo 卡 evidence[0].quoted 取原文
    undo.evidence = original.value().evidence;
    undo.createdAt = nowSec;
    undo.expiresAt = nowSec + kProposalTtlDays * 86400;
    undo.status = "pending";
    undo.source = original.value().source;
    undo.sourceTrust = original.value().sourceTrust;

    auto ins = db_->prepare(
        "INSERT INTO memory_proposals(id, schema_version, kind, target_ids, "
        "result_text, result_type, result_project_id, evidence, confidence, "
        "source, source_trust, created_at, expires_at, status, decided_at, "
        "applied_at, base_version, undo_of) VALUES(?,1,?,?,?,?,?,?,?,?,?,?,?, "
        "'pending', NULL, NULL, ?, ?)");
    if (ins.isFail()) {
        return core::Result<Proposal>::fail(ins.errorCode(),
                                            ins.errorMessage());
    }
    if (!ins.value().bind(1, undo.id).isOk() ||
        !ins.value().bind(2, undo.kind).isOk() ||
        !ins.value().bind(3, idsToJson(undo.targetIds)).isOk() ||
        !ins.value().bind(4, original.value().resultText).isOk() ||
        !ins.value().bind(5, original.value().resultType).isOk() ||
        !ins.value().bind(6, original.value().resultProjectId).isOk() ||
        !ins.value().bind(7, evidenceToJson(undo.evidence)).isOk() ||
        !ins.value().bind(8, original.value().confidence).isOk() ||
        !ins.value().bind(9, undo.source).isOk() ||
        !ins.value().bind(10, undo.sourceTrust).isOk() ||
        !ins.value().bind(11, undo.createdAt).isOk() ||
        !ins.value().bind(12, undo.expiresAt).isOk() ||
        !ins.value().bind(13, undo.baseVersion).isOk() ||
        !ins.value().bind(14, undo.undoOf).isOk()) {
        return core::Result<Proposal>::fail(core::ErrorCode::Internal,
                                            "撤销卡写入绑定失败");
    }
    auto step = ins.value().step();
    if (step.isFail()) {
        // 同一 undoOf 未决唯一索引冲突（并发产出）→ 回读既有卡
        return core::Result<Proposal>::fail(step.errorCode(),
                                            step.errorMessage());
    }
    if (auto a = audit(undo.id, nowSec, "created",
                       "undo_of=" + originalId); a.isFail()) {
        return core::Result<Proposal>::fail(a.errorCode(), a.errorMessage());
    }
    return core::Result<Proposal>::ok(std::move(undo));
}

core::Result<int> ProposalStore::expireSweep(std::int64_t nowSec) {
    auto stmt = db_->prepare(
        "UPDATE memory_proposals SET status='expired' WHERE "
        "status='pending' AND expires_at < ?");
    if (stmt.isFail()) {
        return core::Result<int>::fail(stmt.errorCode(), stmt.errorMessage());
    }
    if (!stmt.value().bind(1, nowSec).isOk()) {
        return core::Result<int>::fail(core::ErrorCode::Internal,
                                       "过期回收绑定失败");
    }
    auto step = stmt.value().step();
    if (step.isFail()) {
        return core::Result<int>::fail(step.errorCode(), step.errorMessage());
    }
    auto cnt = db_->prepare(
        "SELECT COUNT(*) AS v FROM memory_proposals WHERE status='expired'");
    int n = 0;
    if (cnt.isOk()) {
        auto cs = cnt.value().step();
        if (cs.isOk() && cs.value()) {
            const auto rowResult = cnt.value().currentRow();
            if (rowResult.isOk()) {
                n = static_cast<int>(
                    rowResult.value().getInt64("v").valueOr(0));
            }
        }
    }
    return core::Result<int>::ok(n);
}

bool ProposalStore::isUndone(const std::string& id) const {
    auto stmt = db_->prepare(std::string(kSelectCols) +
                             "WHERE undo_of = ? AND status = 'accepted'");
    if (stmt.isFail()) return false;
    if (!stmt.value().bind(1, id).isOk()) return false;
    auto step = stmt.value().step();
    return step.isOk() && step.value();
}

}  // namespace module::memory::proposals
