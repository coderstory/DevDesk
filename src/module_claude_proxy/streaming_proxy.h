// src/module_claude_proxy/streaming_proxy.h
// SSE 流式 Claude 代理：Thread B（libcurl）把上游 chunk push 进 queue，
// Thread A（cpp-httplib chunked provider）消费写客户端。StreamingResponse 拥有
// 真实队列 + libcurl 线程 + backpressure：队列 ≥100 → CURLPAUSE_RECV 暂停上游，
// <50 → 恢复，深度稳定在 50-100。本模块另负责 profile 注入、stream 检测、方言归一。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "claude_service.h"
#include "core/result.h"
#include "platform/http_client.h"
#include "platform/http_server.h"

namespace module::claude {

// 三线程流式转发的共享状态：Thread B（libcurl write callback）生产 → queue →
// Thread A（chunked provider）消费；Thread C（UI）只读 done/error，不碰 queue。
struct StreamingContext {
    std::deque<std::string> queue;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> done{false};
    std::atomic<bool> error{false};
    std::string errorMessage;

    // 队列上限（约 1 MB）：>= kMaxQueueDepth 时暂停上游，<= kResumeQueueDepth 恢复
    static constexpr size_t kMaxQueueDepth = 100;
    static constexpr size_t kResumeQueueDepth = 50;

    size_t depth() {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.size();
    }
};

namespace sse {
inline const char* kEventSeparator = "\n\n";
inline const char* kDoneMarker = "data: [DONE]\n\n";
inline const char* kContentType = "text/event-stream";
} // namespace sse

// 一次流式请求的用量快照（流末经 forwardStream 的 onUsage 交给调用方落库）。
// -1 = 该项没拿到 → 落库写 NULL，与真实的 0 区分；token 四字段为累计值口径。
struct StreamUsage {
    int64_t inputTokens = -1;
    int64_t outputTokens = -1;
    int64_t cacheReadTokens = -1;
    int64_t cacheWriteTokens = -1;
    int64_t firstEventMs = -1;   // 流开始 → 首个上游 SSE chunk 的毫秒数
};

// SSE 事件重组器 + usage 解析：网络 chunk 可能从事件中间切断（TCP 分段边界 ≠
// 事件边界），重组器按空行切事件、剥 data: 前缀解析 JSON，取累计 output tokens。
// 其余事件跳过；解析失败跳过不抛（统计尽力而为，绝不影响转发）。
class SseUsageParser {
public:
    // 喂入一个原始网络 chunk，返回其中重组完成事件解析出的累计 token 值（时间序）
    std::vector<int64_t> feed(const std::string& chunk);

    // 截至当前解析到的四类累计 token（-1 = 该字段从未出现）；流末取用
    const StreamUsage& usage() const { return usage_; }

private:
    std::string buf_;
    StreamUsage usage_;
};

// 检测请求 body 是否要求流式响应（JSON 中 "stream": true；解析失败 → false 走缓冲）
bool wantsStream(const std::string& body);

// 判断上游路径是否需要流式处理（POST /v1/messages 等）
bool isStreamablePath(const std::string& path);

// 把 upstream 请求补全成完整 URL（已是绝对 URL → 原样；否则 baseUrl + "/" + path）
std::string buildUpstreamUrl(const std::string& baseUrl,
                             const std::string& pathOrUrl);

// 合成上游断流时的 event: error SSE 帧（让客户端可感知截断而非当正常结束）。
// upstreamError 中引号/换行被转义，data 行合法。
std::string buildUpstreamErrorFrame(const std::string& upstreamError);

// 构造转发到 Claude 上游的流式请求并启动 libcurl 线程：克隆 req、相对路径
// 补全为 profile.baseUrl + url、按上游方言归一请求头与模型名（差异集中在
// upstream_dialect.h/.cpp）。返回的 StreamingResponse 由调用方持有：
// readChunk() 逐个取 chunk（阻塞）、cancel() 立即中止上游（客户端断开时调用）。
core::Result<std::unique_ptr<platform::http::StreamingResponse>>
    sendStreamingClaude(const platform::http::Request& req,
                        const Profile& profile);

// 把上游流式响应桥接到客户端 ResponseWriter（启动 chunked provider）：客户端
// 断开 → cancel 上游；上游出错 → 写 error 帧后结束。upstream 用 shared_ptr
// 共享给 provider 与 releaser，保证响应彻底结束前不被销毁。
// onUsage（可选）：流收尾恰好回调一次（在 releaser，含客户端断开），带 token
// 四类 + 首事件毫秒；在 cpp-httplib worker 线程执行，落库由调用方自己做。
using StreamUsageSink = std::function<void(const StreamUsage&)>;

void forwardStream(platform::http::ResponseWriter& out,
                   std::shared_ptr<platform::http::StreamingResponse> upstream,
                   const std::string& contentType,
                   StreamUsageSink onUsage = nullptr);

} // namespace module::claude
