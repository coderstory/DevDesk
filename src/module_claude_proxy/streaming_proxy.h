// src/module_claude_proxy/streaming_proxy.h
// SSE 流式 Claude 代理（spec §10.4）
//
// 三线程 + 队列架构：
//
//   [Thread A: cpp-httplib]                [Thread B: libcurl]
//      接收 POST /v1/messages                sendStreaming()
//                          \                   /
//                           [queue_ < 100 chunk]
//                          ↓                   ↑
//                      chunk by chunk    chunk callback
//                      write to client    push to queue
//                          ↓                   ↑
//                      end response       curl_easy_perform
//                                         (curl_easy_pause 暂停)
//
// 职责划分：
//   - platform::http::StreamingResponse 拥有真实队列 + libcurl 线程 + backpressure
//   - 本模块负责：profile 注入（base_url / x-api-key）、stream 检测、
//     把上游队列桥接到 cpp-httplib 的 chunked content provider
//
// backpressure 归属（spec §10.4 重要修正）：
//   队列 >= 100 → consumer（cpp-httplib 线程）调 curl_easy_pause(CURLPAUSE_RECV)
//   暂停 libcurl 生产者；队列 < 50 → CURLPAUSE_RECV_CONT 恢复。
//   队列深度稳定在 50-100 之间。
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "claude_service.h"
#include "core/result.h"
#include "platform/http_client.h"
#include "platform/http_server.h"

namespace module::claude {

// 三线程 + 队列流式转发的共享状态（spec §10.4）
//
// 数据路径：Thread B（libcurl write callback）生产 → queue → Thread A
// （cpp-httplib chunked provider）消费。Thread C（UI）只读取 done/error
// 做状态展示，不触碰 queue。
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

    // 读取当前队列深度（线程安全）
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

// SSE 事件重组器 + usage 解析（首页 Token 速率卡片，设计 §3）
//
// 网络 chunk 可能把一条 SSE 事件从中间切断（TCP 分段边界 ≠ 事件边界），
// 重组器累积缓冲、按空行（\n\n 或 \r\n\r\n）切事件，剥 data: 前缀后解析
// JSON，取出累计 output tokens：
//   - message_start：message.usage.output_tokens（嵌套一层）
//   - message_delta：usage.output_tokens（顶层，累计值口径）
// 其余事件（content_block_delta 等）安全跳过；解析失败跳过不抛
// （统计尽力而为，绝不影响转发）。
class SseUsageParser {
public:
    // 喂入一个原始网络 chunk，返回本 chunk 内重组完成的事件里
    // 解析出的全部累计 output token 值（时间序）
    std::vector<int64_t> feed(const std::string& chunk);

private:
    std::string buf_;
};

// 检测请求 body 是否要求流式响应（JSON 中 "stream": true）
// 解析失败时返回 false（走缓冲路径）
bool wantsStream(const std::string& body);

// 判断上游路径是否需要流式处理（POST /v1/messages 等）
bool isStreamablePath(const std::string& path);

// 把 upstream 请求（可能只有相对 path）补全成完整 URL
//   - req.url 已是绝对 URL（http/https 开头）→ 原样返回
//   - 否则返回 baseUrl + "/" + path
std::string buildUpstreamUrl(const std::string& baseUrl,
                             const std::string& pathOrUrl);

// 构造转发到 Claude 上游的流式请求并启动 libcurl 线程（spec §10.4）
//
// 行为：
//   1. 克隆 req，把 url 补全为 profile.baseUrl + req.url（若 req.url 是相对路径）
//   2. 注入 x-api-key = profile.apiKeyPlain（覆盖客户端传来的）
//   3. 调用 platform::http::sendStreaming 启动 Thread B
//
// 返回的 StreamingResponse 由调用方持有：
//   - readChunk() 逐个取 SSE chunk（阻塞）
//   - cancel() 立即中止上游（客户端断开时调用）
core::Result<std::unique_ptr<platform::http::StreamingResponse>>
    sendStreamingClaude(const platform::http::Request& req,
                        const Profile& profile);

// 把上游流式响应桥接到客户端 ResponseWriter（启动 chunked provider）
//
// provider 每轮从 upstream 拉一个 chunk 写给客户端：
//   - 客户端断开（isWritable() == false）→ cancel 上游 + 结束
//   - 上游出错 → 结束（连接关闭，客户端观察到截断）
//   - 上游自然结束 → 正常结束
//
// upstream 用 shared_ptr 共享给 provider 与 releaser，保证在响应彻底
// 结束前不被销毁。
void forwardStream(platform::http::ResponseWriter& out,
                   std::shared_ptr<platform::http::StreamingResponse> upstream,
                   const std::string& contentType);

} // namespace module::claude
