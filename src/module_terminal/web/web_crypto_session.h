// 网页终端握手会话表（need#490 W3，技术卡 §6.2/§6.3/§6.4）。
// 一次页面会话 = 一次握手：服务端 X25519 临时密钥对 + 方向密钥 + nonce 计数器。
// 明文只到握手公钥；此后 /api/e 与 SSE 共用本会话的方向密钥。
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/result.h"

namespace module::terminal::web {

inline constexpr std::size_t kMaxSessions = 64;
inline constexpr std::int64_t kSessionIdleSec = 24 * 60 * 60;
inline constexpr std::uint64_t kReplayWindow = 128;
inline constexpr std::int64_t kStreamTicketTtlSec = 60;
inline constexpr std::size_t kMaxStreamTickets = 64;

// 单调计数器 + 128 位滑动窗口：<= highest-128 或命中位图 = 重放。
class ReplayWindow {
public:
    bool checkAndAdvance(std::uint64_t counter);

private:
    bool primed_ = false;
    std::uint64_t highest_ = 0;
    std::uint64_t bitmap_ = 0;
};

enum class StreamTicketState { Unused, InUse, Consumed };

struct StreamTicket {
    std::string hid;
    std::int64_t expiresAt = 0;
    StreamTicketState state = StreamTicketState::Unused;
};

// 会话密钥材料；s2c 计数器在 seal 时递增，c2s 重放窗口在 open 时校验。
class CryptoSession {
public:
    CryptoSession() = default;
    ~CryptoSession();  // 私钥/方向密钥显式擦除（OPENSSL_cleanse）

    std::string hid;
    std::vector<std::uint8_t> serverPublic;
    std::vector<std::uint8_t> salt;
    std::vector<std::uint8_t> privateKey;
    std::int64_t createdAt = 0;
    std::int64_t lastSeen = 0;
    bool ready = false;

    // 票据 id 由 HTTP 线程写、在线数口径由其它线程读：必须走本类 mutex_，
    // 服务主锁不保护本对象（原公开字段跨锁读写 = UB）。值语义拷贝返回。
    void setTicketId(const std::string& id);
    bool hasTicket() const;
    std::string ticketIdCopy() const;

    // 客户端公钥 → 共享密钥 → HKDF 方向密钥。仅握手后调用一次。
    core::Result<void> completeHandshake(const std::vector<std::uint8_t>& clientPublic);

    // hid 必须与握手时一致；失败一律 ValidationError（不区分「错 hid / 篡改 / 错密钥」）。
    core::Result<std::vector<std::uint8_t>> openClientEnvelope(
        const std::vector<std::uint8_t>& envelope);
    core::Result<std::vector<std::uint8_t>> sealServerEnvelope(
        const std::vector<std::uint8_t>& plaintext);

private:
    std::vector<std::uint8_t> c2sKey_;
    std::vector<std::uint8_t> s2cKey_;
    std::uint64_t s2cCounter_ = 0;
    ReplayWindow replay_;
    mutable std::mutex mutex_;
    std::string ticketId_;  // 绑定登录票据 id（非秘密，服务端校验用）；读写全走 mutex_
};

class CryptoSessionTable {
public:
    // 新建握手会话；会话表满（64）→ Conflict（W3 映射 HTTP 409，不淘汰在飞会话）。
    core::Result<std::shared_ptr<CryptoSession>> beginHandshake(std::int64_t nowSec);
    std::shared_ptr<CryptoSession> find(const std::string& hid);
    void erase(const std::string& hid);
    void clear();
    std::size_t size() const;
    // 已登录（持票据）会话数：在线人话口径，握手未登录的会话不计。
    std::size_t authenticatedCount() const;
    // 清理 24h 空闲会话，返回清理条数
    std::size_t prune(std::int64_t nowSec);

    // 流票据 unused→in_use；首次 consume 返回 hid。重复使用 / 过期 = ValidationError。
    core::Result<std::string> issueStreamTicket(const std::string& hid, std::int64_t nowSec);
    core::Result<std::string> consumeStreamTicket(const std::string& ticket,
                                                  std::int64_t nowSec);
    void finishStreamTicket(const std::string& ticket, std::int64_t nowSec);

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<CryptoSession>> sessions_;
    std::map<std::string, StreamTicket> tickets_;
};

} // namespace module::terminal::web
