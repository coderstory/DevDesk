#include "mcp_session_store.h"

#include <cstdint>
#include <random>
#include <utility>

namespace module::mcp {

namespace {

// id 的协议要求是可见 ASCII（0x21..0x7E）；十六进制天然满足。
std::string toHex32(std::uint64_t hi, std::uint64_t lo) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(32, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kHex[hi & 0xF];
        hi >>= 4;
    }
    for (int i = 31; i >= 16; --i) {
        out[static_cast<std::size_t>(i)] = kHex[lo & 0xF];
        lo >>= 4;
    }
    return out;
}

} // namespace

std::string McpSessionStore::defaultSessionIdGenerator() {
    // 非密码学随机：本机信任模型下 session id 只做会话标识，不承担鉴权职责。
    // 两次 std::random_device 派生的 64 位随机数拼成 32 位十六进制；无共享状态，
    // 并发调用天然安全。
    std::random_device rd;
    const std::uint64_t hi = (static_cast<std::uint64_t>(rd()) << 32) |
                             static_cast<std::uint64_t>(rd());
    const std::uint64_t lo = (static_cast<std::uint64_t>(rd()) << 32) |
                             static_cast<std::uint64_t>(rd());
    return toHex32(hi, lo);
}

McpSessionStore::McpSessionStore(IdGenerator generator)
    : generator_(std::move(generator)) {}

std::string McpSessionStore::create() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ids_.size() >= kMaxSessions && !order_.empty()) {
        const std::string oldest = order_.front();
        order_.pop_front();
        ids_.erase(oldest);
    }
    std::string id = generator_();
    // 生成器契约要求唯一；重复/空值只做有限重试，避免测试注入坏生成器时死循环。
    for (int attempt = 0; attempt < 8 && (id.empty() || ids_.count(id) > 0);
         ++attempt) {
        id = generator_();
    }
    if (ids_.count(id) > 0) {
        // 仍重复：先清掉旧条目，保证 order_ 与 ids_ 一一对应（否则淘汰会误删新 id）
        removeLocked(id);
    }
    ids_.emplace(id, true);
    order_.push_back(id);
    return id;
}

bool McpSessionStore::has(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ids_.count(id) > 0;
}

void McpSessionStore::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    removeLocked(id);
}

void McpSessionStore::removeLocked(const std::string& id) {
    if (ids_.erase(id) == 0) return;
    for (auto it = order_.begin(); it != order_.end(); ++it) {
        if (*it == id) {
            order_.erase(it);
            return;
        }
    }
}

void McpSessionStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    ids_.clear();
    order_.clear();
}

std::size_t McpSessionStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ids_.size();
}

} // namespace module::mcp