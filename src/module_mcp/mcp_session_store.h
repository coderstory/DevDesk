// MCP 会话表：握手成功后的 session id 只存在内存，服务停止/重启即全部失效。
// 线程安全（HTTP worker 并发 initialize/DELETE）。
#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace module::mcp {

class McpSessionStore {
public:
    // 注入缝：生产用 defaultSessionIdGenerator（32 位十六进制），测试用确定性生成器。
    // 生成器契约：返回非空且当前表中不存在的可见 ASCII 串。
    using IdGenerator = std::function<std::string()>;

    static std::string defaultSessionIdGenerator();

    explicit McpSessionStore(IdGenerator generator = defaultSessionIdGenerator);

    // 超出 64 个先淘汰最旧（先进先出），返回新 id
    std::string create();
    bool has(const std::string& id) const;
    void remove(const std::string& id);
    // 服务停止/重启清空全部会话（spec §5.2：旧 id 一律 404）
    void clear();
    std::size_t size() const;

private:
    static constexpr std::size_t kMaxSessions = 64;

    void removeLocked(const std::string& id);

    IdGenerator generator_;
    mutable std::mutex mutex_;
    // 与 ids_ 一一对应；队头 = 最旧
    std::deque<std::string> order_;
    std::unordered_map<std::string, bool> ids_;
};

} // namespace module::mcp