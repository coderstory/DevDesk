#include "web_bridge.h"

#include "core/logging.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace module::terminal::web {

namespace {
void markDead(const std::shared_ptr<StreamSubscriber>& sub) {
    std::lock_guard<std::mutex> lock(sub->mutex);
    sub->dead = true;
    sub->cv.notify_all();
}
} // namespace

std::shared_ptr<StreamSubscriber> WebBridge::subscribe(SealFn seal) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (subscribers_.size() >= kMaxSubscribers) return nullptr;
    auto sub = std::make_shared<StreamSubscriber>();
    sub->id = std::to_string(nextSubId_++);
    sub->seal = std::move(seal);
    subscribers_.push_back(sub);
    return sub;
}

void WebBridge::unsubscribe(const std::shared_ptr<StreamSubscriber>& sub) {
    if (!sub) return;
    {
        std::lock_guard<std::mutex> lock(sub->mutex);
        sub->dead = true;
        sub->cv.notify_all();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_.erase(std::remove_if(subscribers_.begin(), subscribers_.end(),
                                     [&](const auto& s) { return s.get() == sub.get(); }),
                       subscribers_.end());
}

std::size_t WebBridge::subscriberCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscribers_.size();
}

void WebBridge::publish(const std::string& type, const std::string& payloadJson, bool control) {
    std::lock_guard<std::mutex> lock(mutex_);
    // seq 必须在锁内递增：两个发布线程并发时会拿到重号（前端按 seq 去重/显隐）。
    std::string event = "{\"seq\":" + std::to_string(++seq_) + ",\"ev\":\"" + type +
                        "\",\"d\":" + (payloadJson.empty() ? "null" : payloadJson) + "}";
    for (auto& sub : subscribers_) {
        std::lock_guard<std::mutex> subLock(sub->mutex);
        if (sub->dead) continue;
        if (control) {
            sub->control.push_back(event);
        } else {
            sub->data.push_back(event);
            if (sub->data.size() > kMaxQueuedFrames) {
                sub->data.pop_front();  // 增量帧合并：丢最旧，保留最新
            }
        }
        sub->cv.notify_all();
    }
}

bool WebBridge::runStream(const std::shared_ptr<StreamSubscriber>& sub, const WriteFn& write,
                          const WritableFn& isWritable) {
    if (!sub) return false;
    while (true) {
        std::string frame;
        bool hasFrame = false;
        {
            std::unique_lock<std::mutex> lock(sub->mutex);
            auto take = [&]() {
                if (!sub->control.empty()) {
                    frame = sub->control.front();
                    sub->control.pop_front();
                } else if (!sub->data.empty()) {
                    frame = sub->data.front();
                    sub->data.pop_front();
                } else {
                    return false;
                }
                hasFrame = true;
                return true;
            };
            if (!take()) {
                sub->cv.wait_for(lock, std::chrono::seconds(kHeartbeatSeconds), [&]() {
                    return sub->dead || !sub->control.empty() || !sub->data.empty();
                });
                if (sub->dead) return false;
                take();
            }
            if (sub->dead) return false;
            // 半开：45s 内没有任何成功写出 = 客户端已失联
            if (std::chrono::steady_clock::now() - sub->lastWrite >
                std::chrono::seconds(kHalfOpenSeconds)) {
                sub->dead = true;
                return false;
            }
        }
        if (isWritable && !isWritable()) {
            markDead(sub);
            return false;
        }
        // 超时无数据 = 心跳（加密空帧）
        const std::string sealed = sub->seal(hasFrame ? frame : std::string());
        if (sealed.empty()) {
            markDead(sub);
            return false;
        }
        const std::string line = "data: " + sealed + "\n\n";
        if (!write || !write(line.data(), line.size())) {
            markDead(sub);
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(sub->mutex);
            sub->lastWrite = std::chrono::steady_clock::now();
        }
    }
}

std::uint64_t WebBridge::lastSeq() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seq_;
}

} // namespace module::terminal::web
