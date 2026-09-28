// src/module_claude_proxy/call_log_store.h
// CallLogStore：claude_call_logs 表读写 + 落库版本号
// （R10-A1 自 ClaudeService 抽出；职责边界见 R10 spec §2.2，方法搬移不改语义）
//
// 线程约定（现状同款，见 R10 spec §2.2）：
//   - 写 = proxy worker 线程（ProxyRouter 经 ClaudeService 门面调 logCall）
//   - 读 = UI 线程（Claude 页日志面板 / 首页最近调用快照）
//   - 版本号 callLogVersion_ 用 atomic：写方 fetch_add / 读方 acquire load
#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "core/result.h"
#include "core/sqlite_db.h"

namespace module::claude {

class CallLogStore {
public:
    // 单条调用日志（claude_call_logs 一行）
    struct CallLogEntry {
        int64_t id = 0;
        int64_t profileId = 0;
        std::string method;
        std::string path;
        int statusCode = 0;
        int64_t latencyMs = 0;
        int64_t requestBytes = 0;
        int64_t responseBytes = 0;
        std::string error;
        std::time_t startedAt = 0;
    };

    // 日志面板查询条件（产品审计 2026-09-23 [11]）：总设计 §8.3 第 6 条承诺
    // 「完整日志面板（支持过滤、搜索）」，此前只有 recentCalls(20) 静态列表。
    // search 按 path/method 模糊匹配（LIKE，% 与 _ 转义为字面量，ASCII 大小写
    // 不敏感）；statusClass 按状态码分桶（status_code 列可空，NULL 按 0 归
    // 「错误」桶）；limit/offset 翻页。排序与 recentCalls 一致
    // （started_at DESC, id DESC）。
    struct CallLogQuery {
        // statusClass 取值：
        static constexpr int kStatusAll = 0;  // 全部
        static constexpr int kStatus2xx = 2;  // 200..299
        static constexpr int kStatus3xx = 3;  // 300..399
        static constexpr int kStatus4xx = 4;  // 400..499
        static constexpr int kStatus5xx = 5;  // >=500
        static constexpr int kStatusErr = 9;  // <200（0/NULL = 超时、网络错误等无状态码）

        std::string search;    // 空串 = 不做文本过滤
        int statusClass = kStatusAll;
        int limit = 20;        // <=0 按 20
        int offset = 0;        // <0 按 0
    };

    explicit CallLogStore(core::data::SqliteDb& db);

    // 插入一条调用日志；落库成功即推进 callLogVersion()（审计 perf [13]，
    // Claude 页 compose 据此只在有新调用时重查 recentCalls）
    core::Result<void> logCall(const CallLogEntry& entry);

    // 最近 limit 条（按 started_at DESC, id DESC；limit <= 0 时按 20 兜底）
    core::Result<std::vector<CallLogEntry>> recentCalls(int limit = 20);

    // 过滤 + 分页查询（日志弹窗搜索框 / 状态码下拉 / 翻页共用）
    core::Result<std::vector<CallLogEntry>> queryCalls(const CallLogQuery& q);

    // 满足过滤条件的总条数（忽略 limit/offset；UI 算页数、判断有无下一页）
    core::Result<int> countCalls(const CallLogQuery& q);

    // 调用日志写版本号（logCall 成功 +1，proxy worker 线程写；UI 线程读）
    uint64_t callLogVersion() const {
        return callLogVersion_.load(std::memory_order_acquire);
    }

private:
    core::data::SqliteDb& db_;
    std::atomic<uint64_t> callLogVersion_{0};
};

} // namespace module::claude