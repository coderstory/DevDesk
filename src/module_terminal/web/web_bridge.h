// 网页终端 SSE 订阅者表（need#490 W3，技术卡 §5.2 / P1-6）。
// 契约：provider 被 cpp-httplib 写循环反复调用，无数据时不得返回 true（热旋，httplib.h:8849）；
// 必须阻塞在条件变量等待数据/关闭，最长 15 秒后发心跳帧。sink 与 writeChunk 只在本次调用内
// 有效（http_server.cpp:63-72），禁止跨调用保存。
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace module::terminal::web {

// SSE 并发上限 = 8：cpp-httplib 默认线程池 max(8, CPU-1)（本机 9），长连接各占
// 一条工作线程；留 1 条给其它 API，超限必须 503 而不是把池占满饿死接口。
// 文档口径 64 与实际实现 8 的偏差由 PM 同步（老板裁决 2026-11-24）。
inline constexpr std::size_t kMaxSubscribers = 8;
inline constexpr std::size_t kMaxQueuedFrames = 8;
inline constexpr int kHeartbeatSeconds = 15;
inline constexpr int kHalfOpenSeconds = 45;

// 明文事件 → base64(密文信封)；返回空串 = 加密失败（按连接死亡处理）
using SealFn = std::function<std::string(const std::string& plaintext)>;
using WriteFn = std::function<bool(const char* data, std::size_t len)>;
using WritableFn = std::function<bool()>;

struct StreamSubscriber {
    std::string id;
    SealFn seal;
    std::mutex mutex;
    std::condition_variable cv;
    // 控制事件（revoke/shutdown）独立优先队列，永不丢弃；数据帧超限丢最旧
    std::deque<std::string> control;
    std::deque<std::string> data;
    bool dead = false;
    std::chrono::steady_clock::time_point lastWrite = std::chrono::steady_clock::now();
};

class WebBridge {
public:
    // 订阅全站上限 64；超限返回 nullptr（路由映射 HTTP 503）。
    std::shared_ptr<StreamSubscriber> subscribe(SealFn seal);
    void unsubscribe(const std::shared_ptr<StreamSubscriber>& sub);
    std::size_t subscriberCount() const;

    // 广播事件：内部注入单调 seq，构造 {"seq":N,"ev":"<type>","d":<payload>}。
    // control=true 走优先队列、不丢；否则数据队列满 8 时丢最旧。
    void publish(const std::string& type, const std::string& payloadJson,
                 bool control = false);

    // 返回 false = 结束响应（客户端断开 / 写失败 / 半开超时 / 加密失败）。
    bool runStream(const std::shared_ptr<StreamSubscriber>& sub, const WriteFn& write,
                   const WritableFn& isWritable);

    std::uint64_t lastSeq() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<StreamSubscriber>> subscribers_;
    std::uint64_t nextSubId_ = 1;
    std::uint64_t seq_ = 0;
};

} // namespace module::terminal::web
