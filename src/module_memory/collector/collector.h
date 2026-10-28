// src/module_memory/collector/collector.h
// 采集器（S3-B）：Claude Code 会话 jsonl 增量尾读 → 四道过滤 → 4KB 分段。
// 幂等契约（崩溃安全）：水位与已处理事件都落在模块自有 SQLite——
//   - collector_watermark(session_id → byte_offset/line_count)：尾读续点；
//     文件比水位短（截断/轮转）→ 水位归零重读；
//   - processed_events(session_id, event_seq) 唯一键：入库成功的才记账，
//     崩溃重放时 INSERT OR IGNORE 去重，(session_id, event_seq) 至多入库一次。
// 脱敏与入库不在本单元：调用方拿 segments 组合 脱敏闸 → 提案/引擎，成功后 commit()。
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "module_memory/collector/session_reader.h"
#include "core/result.h"
#include "core/sqlite_db.h"

namespace module::memory::collector {

// 轮询间隔常量（首版固定值；面板可调属后续批次）
inline constexpr int kPollIntervalMs = 30000;
// 段目标体积（§4.2 实测口径：清洗后按 4KB 切段）
inline constexpr std::size_t kSegmentTargetBytes = 4096;

struct Segment {
    std::string sessionId;
    bool hasUserText = false;  // ④ 段级跳过判据
    std::string text;          // 已过①②③的正文拼接
    std::vector<SessionEvent> events;  // 段内事件（commit 记账的最小单元）
};

// 水位/去重账本（模块自有 SQLite；单连接仅采集线程触碰）
class CollectorStore {
public:
    static core::Result<CollectorStore> open(const std::filesystem::path& dbFile);

    // 已处理事件记账（幂等键 session_id+event_seq）；重复 = no-op 且返回 false
    bool markProcessed(const std::string& sessionId, std::uint64_t seq);
    bool isProcessed(const std::string& sessionId, std::uint64_t seq) const;

    // 存储概览读数（面板「存储概览」格消费；只读聚合，不改账本）
    core::Result<std::int64_t> processedCount() const;
    core::Result<int> sessionCount() const;

    // 尾读水位
    core::Result<void> setOffset(const std::string& sessionId, std::int64_t offset,
                                 std::int64_t lineCount);
    std::int64_t offset(const std::string& sessionId) const;
    std::int64_t lineCount(const std::string& sessionId) const;

private:
    CollectorStore() = default;
    // SqliteDb 不可拷贝；堆持有保持值语义门面
    std::shared_ptr<class core::data::SqliteDb> db_;
};

class Collector {
public:
    // projectsRoot：扫描根显式注入（测试隔离）；生产用 resolveProjectsRoot()
    Collector(std::filesystem::path dbFile, std::filesystem::path projectsRoot);

    // 扫描根解析（纯函数）：rootEnv 覆盖 → <home>/.claude/projects；home 为空 → 空路径
    static std::filesystem::path resolveProjectsRoot(const char* rootEnv,
                                                     const char* homeDir);
    // 生产装配：读 MEMCPP_CC_PROJECTS_ROOT 与 USERPROFILE/HOME
    static std::filesystem::path resolveProjectsRootFromEnv();

    // 单轮采集：增量尾读全部会话文件 → 出段（④ 无 user 正文的段直接跳过）。
    // 只读内存与 DB 水位，不改账本——成功与否的记账在 commit/discard。
    core::Result<std::vector<Segment>> pollOnce();

    // 段内全部事件记已处理 + 水位推进（入库成功后调用；事件级幂等键兜底）
    core::Result<void> commit(const std::vector<Segment>& segments);

private:
    core::Result<std::vector<Segment>> readSessionFile(
        const std::filesystem::path& file);

    std::filesystem::path dbFile_;
    std::filesystem::path projectsRoot_;
};

}  // namespace module::memory::collector
