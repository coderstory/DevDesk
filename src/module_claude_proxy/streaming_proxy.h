// src/module_claude_proxy/streaming_proxy.h
// SSE 流式 Claude 代理：三线程 + 队列架构——Thread A（cpp-httplib）接收请求并
// chunk by chunk 写客户端，Thread B（libcurl）把上游 chunk callback push 进
// queue（< 100 chunk）后 curl_easy_pause 暂停。
// 职责：StreamingResponse 拥有真实队列 + libcurl 线程 + backpressure；本模块
// 负责 profile 注入、stream 检测、把上游队列桥接到 cpp-httplib chunked provider。
// backpressure：队列 >= 100 → curl_easy_pause(CURLPAUSE_RECV) 暂停上游；
// < 50 → CURLPAUSE_RECV_CONT 恢复，队列深度稳定在 50-100。
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

// SSE 相关常量
namespace sse {
// SSE 事件分隔符：data: <json>\n\n
inline const char* kEventSeparator = "\n\n";
// 流结束标记：data: [DONE]\n\n
inline const char* kDoneMarker = "data: [DONE]\n\n";
// 标准 SSE Content-Type
inline const char* kContentType = "text/event-stream";
} // namespace sse

// 一次流式请求的用量快照（流末经 forwardStream 的 onUsage 交给调用方落库）。
// -1 = 该项没拿到 → 落库写 NULL，与真实的 0 区分；四个 token 字段是累计值口径
//（message_delta 里带的就是累计）。token 四字段由 SseUsageParser 填，firstEventMs 由 forwardStream 填。
struct StreamUsage {
    int64_t inputTokens = -1;
    int64_t outputTokens = -1;
    int64_t cacheReadTokens = -1;
    int64_t cacheWriteTokens = -1;
    int64_t firstEventMs = -1;   // 流开始 → 首个上游 SSE chunk 的毫秒数
};

// SSE 事件重组器 + usage 解析（首页 Token 速率卡片）：网络 chunk 可能从事件中间
// 切断（TCP 分段边界 ≠ 事件边界），重组器按空行切事件、剥 data: 前缀解析 JSON，
// 取累计 output tokens（message_start 嵌套一层 / message_delta 顶层）。其余事件
// 跳过；解析失败跳过不抛（统计尽力而为，绝不影响转发）。
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

// 合成上游断流时的 event: error SSE 帧（原实现只 LOG_WARN，客户端会把截断当
// 正常结束；先写 error 帧再收尾让客户端可感知）。upstreamError 中引号/换行
// 被转义，data 行合法。
std::string buildUpstreamErrorFrame(const std::string& upstreamError);

// 构造转发到 Claude 上游的流式请求并启动 libcurl 线程：
// 1. 克隆 req，相对路径 url 补全为 profile.baseUrl + req.url
// 2. 按上游方言归一请求头与模型名（x-api-key / UA / anthropic-version / 会话头 /
//    剥 [1M] 标记；各网关差异集中在 upstream_dialect.h/.cpp）
// 3. platform::http::sendStreaming 启动 Thread B。
// 返回的 StreamingResponse 由调用方持有：readChunk() 逐个取 chunk（阻塞）、
// cancel() 立即中止上游（客户端断开时调用）。
core::Result<std::unique_ptr<platform::http::StreamingResponse>>
    sendStreamingClaude(const platform::http::Request& req,
                        const Profile& profile);

// 把上游流式响应桥接到客户端 ResponseWriter（启动 chunked provider）：provider
// 每轮拉一个 chunk 写给客户端；客户端断开 → cancel 上游 + 结束；上游出错 → 结束
//（客户端观察到截断）；上游自然结束 → 正常结束。upstream 用 shared_ptr 共享给
// provider 与 releaser，保证响应彻底结束前不被销毁。
// onUsage（可选）：流收尾回调恰好一次（在 releaser，含客户端断开），带 token 四类
// + 首事件毫秒；回调在 cpp-httplib worker 线程执行，落库由调用方自己做（不抛异常）。
using StreamUsageSink = std::function<void(const StreamUsage&)>;

void forwardStream(platform::http::ResponseWriter& out,
                   std::shared_ptr<platform::http::StreamingResponse> upstream,
                   const std::string& contentType,
                   StreamUsageSink onUsage = nullptr);

} // namespace module::claude
