#include "web_crypto_session.h"

#include "platform/web_crypto.h"

#include <utility>
#include <openssl/crypto.h>

namespace module::terminal::web {

CryptoSession::~CryptoSession() {
    // 会话密钥即用即清：普通清空可能被编译器优化省略，走 OpenSSL 显式擦除。
    auto wipe = [](std::vector<std::uint8_t>& v) {
        if (!v.empty()) OPENSSL_cleanse(v.data(), v.size());
    };
    wipe(privateKey);
    wipe(c2sKey_);
    wipe(s2cKey_);
}

namespace {

namespace wc = platform::web_crypto;

// 1024 位窗口用 uint64[16] = 128 字节；这里只需 128 位，用两个 uint64 足够。
std::string deriveInfo(const char* direction, const std::string& hid,
                       const std::vector<std::uint8_t>& clientPublic,
                       const std::vector<std::uint8_t>& serverPublic) {
    return std::string("ddweb/v1/") + direction + "|" + hid + "|" +
           wc::base64UrlEncode(clientPublic) + "|" + wc::base64UrlEncode(serverPublic);
}

} // namespace

bool ReplayWindow::checkAndAdvance(std::uint64_t counter) {
    if (!primed_) {
        primed_ = true;
        highest_ = counter;
        bitmap_ = 1;
        return true;
    }
    if (counter > highest_) {
        const std::uint64_t shift = counter - highest_;
        bitmap_ = (shift >= 64) ? 1 : ((bitmap_ << shift) | 1);
        highest_ = counter;
        return true;
    }
    const std::uint64_t delta = highest_ - counter;
    if (delta >= 64) return false;  // 超出位图 = 过旧
    const std::uint64_t bit = std::uint64_t{1} << delta;
    if (bitmap_ & bit) return false;  // 已见 = 重放
    bitmap_ |= bit;
    return true;
}

core::Result<void> CryptoSession::completeHandshake(
    const std::vector<std::uint8_t>& clientPublic) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ready) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                        "handshake already complete");
    }
    if (clientPublic.size() != wc::kKeySize) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError, "bad client key");
    }
    auto shared = wc::x25519SharedSecret(privateKey, clientPublic);
    if (!shared.isOk()) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError, "bad client key");
    }
    auto prk = wc::hkdfExtract(salt, shared.value());
    if (!prk.isOk()) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError, "hkdf failed");
    }
    auto c2s = wc::hkdfExpand(prk.value(), deriveInfo("c2s", hid, clientPublic, serverPublic),
                              wc::kKeySize);
    auto s2c = wc::hkdfExpand(prk.value(), deriveInfo("s2c", hid, clientPublic, serverPublic),
                              wc::kKeySize);
    if (!c2s.isOk() || !s2c.isOk()) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError, "hkdf failed");
    }
    c2sKey_ = std::move(c2s.value());
    s2cKey_ = std::move(s2c.value());
    ready = true;
    return core::Result<void>::ok();
}

core::Result<std::vector<std::uint8_t>> CryptoSession::openClientEnvelope(
    const std::vector<std::uint8_t>& envelope) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready) {
        return core::Result<std::vector<std::uint8_t>>::fail(
            core::ErrorCode::ValidationError, "no handshake");
    }
    std::vector<std::uint8_t> nonce;
    auto opened = wc::openEnvelope(c2sKey_, envelope, {}, &nonce);
    if (!opened.isOk()) {
        return core::Result<std::vector<std::uint8_t>>::fail(
            core::ErrorCode::ValidationError, "bad envelope");
    }
    // nonce = 方向 4B（大端）‖ 计数器 8B（大端）
    if (nonce.size() != wc::kNonceSize) {
        return core::Result<std::vector<std::uint8_t>>::fail(
            core::ErrorCode::ValidationError, "bad nonce");
    }
    std::uint32_t direction = 0;
    std::uint64_t counter = 0;
    for (int i = 0; i < 4; ++i) direction = (direction << 8) | nonce[i];
    for (int i = 4; i < 12; ++i) counter = (counter << 8) | nonce[i];
    if (direction != wc::kDirectionC2S || !replay_.checkAndAdvance(counter)) {
        return core::Result<std::vector<std::uint8_t>>::fail(
            core::ErrorCode::ValidationError, "replay");
    }
    return opened;
}

core::Result<std::vector<std::uint8_t>> CryptoSession::sealServerEnvelope(
    const std::vector<std::uint8_t>& plaintext) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready) {
        return core::Result<std::vector<std::uint8_t>>::fail(
            core::ErrorCode::ValidationError, "no handshake");
    }
    const auto nonce = wc::makeNonce(wc::kDirectionS2C, s2cCounter_++);
    return wc::sealEnvelope(s2cKey_, nonce, plaintext);
}

core::Result<std::shared_ptr<CryptoSession>> CryptoSessionTable::beginHandshake(
    std::int64_t nowSec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sessions_.size() >= kMaxSessions) {
        return core::Result<std::shared_ptr<CryptoSession>>::fail(
            core::ErrorCode::Conflict, "too_many_sessions");
    }
    auto keyPair = wc::x25519Generate();
    auto salt = wc::randomBytes(32);
    auto hidBytes = wc::randomBytes(16);  // base64url = 22 字符
    if (!keyPair.isOk() || salt.empty() || hidBytes.empty()) {
        return core::Result<std::shared_ptr<CryptoSession>>::fail(
            core::ErrorCode::Internal, "rng failed");
    }
    auto session = std::make_shared<CryptoSession>();
    session->hid = wc::base64UrlEncode(hidBytes);
    session->serverPublic = keyPair.value().publicKey;
    session->privateKey = keyPair.value().privateKey;
    session->salt = std::move(salt);
    session->createdAt = nowSec;
    session->lastSeen = nowSec;
    sessions_[session->hid] = session;
    return core::Result<std::shared_ptr<CryptoSession>>::ok(session);
}

std::shared_ptr<CryptoSession> CryptoSessionTable::find(const std::string& hid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(hid);
    return it == sessions_.end() ? nullptr : it->second;
}

void CryptoSessionTable::erase(const std::string& hid) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(hid);
}

void CryptoSessionTable::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.clear();
    tickets_.clear();
}

std::size_t CryptoSessionTable::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

std::size_t CryptoSessionTable::authenticatedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t n = 0;
    for (const auto& kv : sessions_) {
        if (!kv.second->ticketId.empty()) ++n;
    }
    return n;
}

std::size_t CryptoSessionTable::prune(std::int64_t nowSec) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t removed = 0;
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (nowSec - it->second->lastSeen > kSessionIdleSec) {
            it = sessions_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    for (auto it = tickets_.begin(); it != tickets_.end();) {
        if (nowSec > it->second.expiresAt) {
            it = tickets_.erase(it);
        } else {
            ++it;
        }
    }
    return removed;
}

core::Result<std::string> CryptoSessionTable::issueStreamTicket(const std::string& hid,
                                                                std::int64_t nowSec) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 注意：不能在这里调 prune()（它会再取同一把非递归锁）。只清过期票据。
    for (auto it = tickets_.begin(); it != tickets_.end();) {
        if (nowSec > it->second.expiresAt) {
            it = tickets_.erase(it);
        } else {
            ++it;
        }
    }
    if (tickets_.size() >= kMaxStreamTickets) {
        return core::Result<std::string>::fail(core::ErrorCode::Conflict, "too_many_tickets");
    }
    auto raw = wc::randomBytes(16);
    if (raw.empty()) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal, "rng failed");
    }
    std::string ticket = wc::base64UrlEncode(raw);
    tickets_[ticket] = StreamTicket{hid, nowSec + kStreamTicketTtlSec, StreamTicketState::Unused};
    return core::Result<std::string>::ok(std::move(ticket));
}

core::Result<std::string> CryptoSessionTable::consumeStreamTicket(const std::string& ticket,
                                                                  std::int64_t nowSec) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tickets_.find(ticket);
    if (it == tickets_.end() || nowSec > it->second.expiresAt ||
        it->second.state != StreamTicketState::Unused) {
        return core::Result<std::string>::fail(core::ErrorCode::ValidationError, "bad_ticket");
    }
    it->second.state = StreamTicketState::InUse;
    return core::Result<std::string>::ok(it->second.hid);
}

void CryptoSessionTable::finishStreamTicket(const std::string& ticket, std::int64_t nowSec) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tickets_.find(ticket);
    if (it == tickets_.end()) return;
    if (nowSec > it->second.expiresAt) {
        tickets_.erase(it);
    } else {
        it->second.state = StreamTicketState::Consumed;
    }
}

} // namespace module::terminal::web
