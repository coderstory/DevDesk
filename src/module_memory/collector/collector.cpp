// src/module_memory/collector/collector.cpp
#include "collector.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <system_error>
#include <utility>

#include "core/logging.h"
#include "core/sqlite_db.h"

namespace module::memory::collector {

namespace {

constexpr const char* kSchema = R"(
CREATE TABLE IF NOT EXISTS collector_watermark (
  session_id TEXT PRIMARY KEY,
  byte_offset INTEGER NOT NULL DEFAULT 0,
  line_count INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS processed_events (
  session_id TEXT NOT NULL,
  event_seq INTEGER NOT NULL,
  processed_at INTEGER NOT NULL,
  PRIMARY KEY (session_id, event_seq)
);
)";

std::int64_t nowSec() { return static_cast<std::int64_t>(std::time(nullptr)); }

}  // namespace

// ── CollectorStore ──

core::Result<CollectorStore> CollectorStore::open(
    const std::filesystem::path& dbFile) {
    auto db = core::data::SqliteDb::open(dbFile);
    if (db.isFail()) {
        return core::Result<CollectorStore>::fail(db.errorCode(),
                                                  db.errorMessage());
    }
    auto applied = db.value().applySchema(kSchema);
    if (applied.isFail()) {
        return core::Result<CollectorStore>::fail(applied.errorCode(),
                                                  applied.errorMessage());
    }
    CollectorStore store;
    store.db_ =
        std::make_shared<core::data::SqliteDb>(std::move(db.value()));
    return core::Result<CollectorStore>::ok(std::move(store));
}

bool CollectorStore::markProcessed(const std::string& sessionId,
                                   std::uint64_t seq) {
    auto stmt = db_->prepare(
        "INSERT OR IGNORE INTO processed_events(session_id, event_seq, "
        "processed_at) VALUES(?, ?, ?)");
    if (stmt.isFail()) return false;
    if (!stmt.value().bind(1, sessionId).isOk()) return false;
    if (!stmt.value().bind(2, static_cast<std::int64_t>(seq)).isOk()) {
        return false;
    }
    if (!stmt.value().bind(3, nowSec()).isOk()) return false;
    auto step = stmt.value().step();
    if (step.isFail()) return false;
    return step.value();  // false = 已存在（幂等去重）
}

bool CollectorStore::isProcessed(const std::string& sessionId,
                                 std::uint64_t seq) const {
    auto stmt = db_->prepare(
        "SELECT 1 FROM processed_events WHERE session_id = ? AND event_seq = ?");
    if (stmt.isFail()) return false;
    if (!stmt.value().bind(1, sessionId).isOk()) return false;
    if (!stmt.value().bind(2, static_cast<std::int64_t>(seq)).isOk()) {
        return false;
    }
    auto step = stmt.value().step();
    return step.isOk() && step.value();
}

core::Result<std::int64_t> CollectorStore::processedCount() const {
    auto stmt = db_->prepare("SELECT COUNT(*) AS v FROM processed_events");
    if (stmt.isFail()) {
        return core::Result<std::int64_t>::fail(stmt.errorCode(),
                                                stmt.errorMessage());
    }
    auto step = stmt.value().step();
    if (!step.isOk() || !step.value()) return core::Result<std::int64_t>::ok(0);
    auto row = stmt.value().currentRow();
    if (row.isFail()) return core::Result<std::int64_t>::ok(0);
    return core::Result<std::int64_t>::ok(row.value().getInt64("v").valueOr(0));
}

core::Result<int> CollectorStore::sessionCount() const {
    auto stmt = db_->prepare(
        "SELECT COUNT(DISTINCT session_id) AS v FROM collector_watermark");
    if (stmt.isFail()) {
        return core::Result<int>::fail(stmt.errorCode(), stmt.errorMessage());
    }
    auto step = stmt.value().step();
    if (!step.isOk() || !step.value()) return core::Result<int>::ok(0);
    auto row = stmt.value().currentRow();
    if (row.isFail()) return core::Result<int>::ok(0);
    return core::Result<int>::ok(
        static_cast<int>(row.value().getInt64("v").valueOr(0)));
}

core::Result<void> CollectorStore::setOffset(const std::string& sessionId,
                                             std::int64_t offset,
                                             std::int64_t lineCount) {
    auto stmt = db_->prepare(
        "INSERT INTO collector_watermark(session_id, byte_offset, line_count) "
        "VALUES(?, ?, ?) ON CONFLICT(session_id) DO UPDATE SET "
        "byte_offset = excluded.byte_offset, line_count = excluded.line_count");
    if (stmt.isFail()) {
        return core::Result<void>::fail(stmt.errorCode(), stmt.errorMessage());
    }
    if (!stmt.value().bind(1, sessionId).isOk() ||
        !stmt.value().bind(2, offset).isOk() ||
        !stmt.value().bind(3, lineCount).isOk()) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "水位绑定失败");
    }
    auto step = stmt.value().step();
    if (step.isFail()) {
        return core::Result<void>::fail(step.errorCode(), step.errorMessage());
    }
    return core::Result<void>::ok();
}

namespace {
// 单值查询垫：SELECT 单列单行 → int64；无行/失败回 0（水位缺省即 0，非错误）
std::int64_t queryInt64(core::data::SqliteDb* db, const std::string& sql,
                        const std::string& sessionId,
                        const std::string& column) {
    auto stmt = db->prepare(sql);
    if (stmt.isFail()) return 0;
    if (!stmt.value().bind(1, sessionId).isOk()) return 0;
    auto step = stmt.value().step();
    if (!step.isOk() || !step.value()) return 0;
    const auto rowResult = stmt.value().currentRow();
    if (rowResult.isFail()) return 0;
    return rowResult.value().getInt64(column).valueOr(0);
}
}  // namespace

std::int64_t CollectorStore::offset(const std::string& sessionId) const {
    return queryInt64(db_.get(),
                      "SELECT byte_offset FROM collector_watermark "
                      "WHERE session_id = ?",
                      sessionId, "byte_offset");
}

std::int64_t CollectorStore::lineCount(const std::string& sessionId) const {
    return queryInt64(db_.get(),
                      "SELECT line_count FROM collector_watermark "
                      "WHERE session_id = ?",
                      sessionId, "line_count");
}

// ── Collector ──

Collector::Collector(std::filesystem::path dbFile,
                     std::filesystem::path projectsRoot)
    : dbFile_(std::move(dbFile)), projectsRoot_(std::move(projectsRoot)) {}

std::filesystem::path Collector::resolveProjectsRoot(const char* rootEnv,
                                                     const char* homeDir) {
    if (rootEnv != nullptr && rootEnv[0] != '\0') return std::filesystem::path(rootEnv);
    if (homeDir == nullptr || homeDir[0] == '\0') return {};
    return std::filesystem::path(homeDir) / ".claude" / "projects";
}

std::filesystem::path Collector::resolveProjectsRootFromEnv() {
    const char* rootEnv = std::getenv("MEMCPP_CC_PROJECTS_ROOT");
    const char* home = std::getenv("USERPROFILE");
    if (home == nullptr || home[0] == '\0') home = std::getenv("HOME");
    return resolveProjectsRoot(rootEnv, home);
}

core::Result<std::vector<Segment>> Collector::readSessionFile(
    const std::filesystem::path& file) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) {
        return core::Result<std::vector<Segment>>::fail(
            core::ErrorCode::FileReadFailed, "取文件大小失败: " + file.string());
    }
    auto opened = CollectorStore::open(dbFile_);
    if (opened.isFail()) {
        return core::Result<std::vector<Segment>>::fail(opened.errorCode(),
                                                        opened.errorMessage());
    }
    CollectorStore& store = opened.value();

    const std::string sessionId = file.stem().string();  // 会话文件名 = 会话 id
    std::int64_t wmOffset = store.offset(sessionId);
    std::int64_t lineNo = store.lineCount(sessionId);
    if (static_cast<std::int64_t>(size) < wmOffset) {
        // 文件截断/轮转：水位归零重读（事件级唯一键兜底，不会重复入库）
        wmOffset = 0;
        lineNo = 0;
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return core::Result<std::vector<Segment>>::fail(
            core::ErrorCode::FileReadFailed, "打开会话文件失败: " + file.string());
    }
    in.seekg(wmOffset);

    std::vector<Segment> segments;
    Segment cur;
    std::string line;
    std::int64_t consumedFrom = wmOffset;
    while (std::getline(in, line)) {
        // getline 保留 \r（二进制模式）：去尾只影响解析文本。水位必须按流
        // 实际消费字节推进（审计 F8）：\r 计入消费量——否则每个 CRLF 行少计 1 字节，
        // 下轮从残留字节续读、水位逐轮抖动难排查
        const std::int64_t rawLen = static_cast<std::int64_t>(line.size());
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ++lineNo;
        consumedFrom += rawLen + 1;
        if (line.empty()) continue;
        auto events = parseSessionLine(line, static_cast<std::uint64_t>(lineNo - 1));
        if (events.isFail()) {
            LOG_WARN("采集跳过坏行: " + file.filename().string() + ":" +
                     std::to_string(lineNo));
            continue;
        }
        for (auto& ev : events.value()) {
                    if (store.isProcessed(sessionId, ev.eventSeq)) continue;
            // 4KB 段装配：超目标体积即封段
            if (!cur.text.empty() &&
                cur.text.size() + ev.text.size() > kSegmentTargetBytes) {
                if (cur.hasUserText) {  // ④ 无 user 正文整段跳过
                    segments.push_back(std::move(cur));
                }
                cur = Segment{};
            }
            cur.sessionId = sessionId;
            cur.hasUserText = cur.hasUserText || ev.narrative;
            cur.text += ev.text;
            cur.events.push_back(std::move(ev));
        }
    }
    if (!cur.events.empty() && cur.hasUserText) {  // ④
        segments.push_back(std::move(cur));
    }

    // 水位推进到实际读到的位置（未消费整文件时也安全：下轮续读）
    if (consumedFrom != wmOffset) {
        if (auto r = store.setOffset(sessionId, consumedFrom, lineNo); r.isFail()) {
            return core::Result<std::vector<Segment>>::fail(r.errorCode(),
                                                            r.errorMessage());
        }
    }
    return core::Result<std::vector<Segment>>::ok(std::move(segments));
}

core::Result<std::vector<Segment>> Collector::pollOnce() {
    std::error_code ec;
    if (!std::filesystem::exists(projectsRoot_, ec)) {
        return core::Result<std::vector<Segment>>::ok({});
    }
    std::vector<Segment> all;
    std::filesystem::directory_iterator it(projectsRoot_, ec);
    if (ec) {
        return core::Result<std::vector<Segment>>::fail(
            core::ErrorCode::FileReadFailed,
            "扫描根不可读: " + projectsRoot_.string());
    }
    // 目录直接铺 .jsonl 会话文件（Claude Code 布局）；若存在项目子目录则下钻一层
    for (const auto& entry : it) {
        std::error_code ec2;
        if (entry.is_directory(ec2)) {
            for (const auto& sub : std::filesystem::directory_iterator(entry.path(), ec2)) {
                if (ec2) break;
                if (sub.path().extension() == ".jsonl") {
                    auto segs = readSessionFile(sub.path());
                    if (segs.isFail()) return segs;
                    for (auto& s : segs.value()) all.push_back(std::move(s));
                }
            }
            continue;
        }
        if (entry.path().extension() != ".jsonl") continue;
        auto segs = readSessionFile(entry.path());
        if (segs.isFail()) return segs;
        for (auto& s : segs.value()) all.push_back(std::move(s));
    }
    return core::Result<std::vector<Segment>>::ok(std::move(all));
}

core::Result<void> Collector::commit(const std::vector<Segment>& segments) {
    auto opened = CollectorStore::open(dbFile_);
    if (opened.isFail()) {
        return core::Result<void>::fail(opened.errorCode(),
                                        opened.errorMessage());
    }
    CollectorStore& store = opened.value();
    for (const Segment& seg : segments) {
        for (const SessionEvent& ev : seg.events) {
            store.markProcessed(ev.sessionId, ev.eventSeq);
        }
    }
    return core::Result<void>::ok();
}

}  // namespace module::memory::collector
