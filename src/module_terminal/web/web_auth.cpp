// 网页终端鉴权纯逻辑实现（need#490 W2）。见 web_auth.h 的契约与持久化格式说明。
#include "module_terminal/web/web_auth.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "core/logging.h"
#include "platform/web_crypto.h"

namespace module::terminal::web {
namespace {

using platform::web_crypto::base64UrlEncode;
using platform::web_crypto::constantTimeEquals;

std::string toHexLower(const std::vector<std::uint8_t>& bytes) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t b : bytes) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0f]);
    }
    return out;
}

bool isHex32(const std::string& text) {
    if (text.size() != 32) return false;
    return std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::vector<std::uint8_t> counterBytes(std::uint64_t counter) {
    std::vector<std::uint8_t> out(8);
    for (int i = 7; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(counter & 0xff);
        counter >>= 8;
    }
    return out;
}

std::vector<std::uint8_t> ticketPayload(std::int64_t expiry,
                                        const std::vector<std::uint8_t>& id) {
    std::vector<std::uint8_t> payload;
    payload.reserve(1 + 8 + id.size());
    payload.push_back(1);  // ver
    const auto expiryBytes = counterBytes(static_cast<std::uint64_t>(expiry));
    payload.insert(payload.end(), expiryBytes.begin(), expiryBytes.end());
    payload.insert(payload.end(), id.begin(), id.end());
    return payload;
}

std::vector<std::uint8_t> hmacV1(const std::vector<std::uint8_t>& key,
                                 const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> message;
    message.reserve(3 + payload.size());
    message.insert(message.end(), {'v', '1', '|'});
    message.insert(message.end(), payload.begin(), payload.end());
    auto mac = platform::web_crypto::hmacSha256(key, message);
    return mac.isOk() ? mac.value() : std::vector<std::uint8_t>{};
}

} // namespace

// ---------- 绑定令牌 ----------

core::Result<BindToken> makeBindToken(std::int64_t nowSec) {
    auto tokenBytes = platform::web_crypto::randomBytes(static_cast<std::size_t>(kBindTokenBytes));
    auto secret = platform::web_crypto::randomBytes(20);
    if (tokenBytes.empty() || secret.empty()) {
        return core::Result<BindToken>::fail(core::ErrorCode::Internal, "rng_failed");
    }
    BindToken t;
    t.token = base64UrlEncode(tokenBytes);
    t.secret = std::move(secret);
    t.expireAt = nowSec + kBindTokenTtlSec;
    return core::Result<BindToken>::ok(std::move(t));
}

core::Result<BindClaim> claimBindToken(BindToken& t, std::int64_t nowSec) {
    if (t.state == BindTokenState::Terminal) {
        return core::Result<BindClaim>::fail(core::ErrorCode::ValidationError, "token_terminal");
    }
    // 过期判定按「未 claim 也过期」口径：到期即终态，不再发密钥
    if (nowSec > t.expireAt) {
        t.state = BindTokenState::Terminal;
        t.terminal = BindTokenTerminal::Expired;
        return core::Result<BindClaim>::fail(core::ErrorCode::ValidationError, "token_expired");
    }
    t.state = BindTokenState::Claimed;
    BindClaim claim;
    claim.secret = base32Secret(t.secret);
    claim.otpauth = totpOtpauthUri(t.secret);
    return core::Result<BindClaim>::ok(std::move(claim));
}

ConfirmOutcome confirmBindToken(BindToken& t, const std::string& code, std::int64_t nowSec) {
    if (t.state != BindTokenState::Claimed) return ConfirmOutcome::NotClaimed;
    if (nowSec > t.expireAt) {
        t.state = BindTokenState::Terminal;
        t.terminal = BindTokenTerminal::Expired;
        return ConfirmOutcome::NotClaimed;
    }
    if (verifyTotp(t.secret, code, nowSec)) {
        t.state = BindTokenState::Terminal;
        t.terminal = BindTokenTerminal::Used;
        return ConfirmOutcome::Ok;
    }
    ++t.confirmFailures;
    if (t.confirmFailures >= kMaxConfirmFailures) {
        t.state = BindTokenState::Terminal;
        t.terminal = BindTokenTerminal::WrongCodes;
    }
    return ConfirmOutcome::InvalidCode;
}

// ---------- TOTP ----------

std::string base32Secret(const std::vector<std::uint8_t>& secret) {
    return platform::web_crypto::base32Encode(secret);
}

std::string totpCode(const std::vector<std::uint8_t>& secret, std::int64_t unixSec,
                     int digits) {
    if (digits < 1 || digits > 9 || unixSec < 0) return {};
    const auto step = static_cast<std::uint64_t>(unixSec / kTotpPeriodSec);
    auto mac = platform::web_crypto::hmacSha1(secret, counterBytes(step));
    if (mac.isFail() || mac.value().size() < 20) return {};
    const auto& h = mac.value();
    const std::size_t offset = h[19] & 0x0f;
    const std::uint32_t binary = ((static_cast<std::uint32_t>(h[offset] & 0x7f) << 24) |
                                  (static_cast<std::uint32_t>(h[offset + 1]) << 16) |
                                  (static_cast<std::uint32_t>(h[offset + 2]) << 8) |
                                  static_cast<std::uint32_t>(h[offset + 3]));
    std::uint32_t modulus = 1;
    for (int i = 0; i < digits; ++i) modulus *= 10;
    std::string out = std::to_string(binary % modulus);
    out.insert(out.begin(), static_cast<std::size_t>(digits) - out.size(), '0');
    return out;
}

std::string totpOtpauthUri(const std::vector<std::uint8_t>& secret) {
    // 标签格式 issuer:account；issuer 同时以参数形式给出（Google Authenticator 口径）
    return "otpauth://totp/DevDesk:web-terminal?secret=" + base32Secret(secret) +
           "&issuer=DevDesk&algorithm=SHA1&digits=" + std::to_string(kTotpDigits) +
           "&period=" + std::to_string(kTotpPeriodSec);
}

bool verifyTotp(const std::vector<std::uint8_t>& secret, const std::string& code,
                std::int64_t unixSec) {
    if (code.size() != static_cast<std::size_t>(kTotpDigits)) return false;
    for (int step = -kTotpSkewSteps; step <= kTotpSkewSteps; ++step) {
        const std::int64_t at = unixSec + static_cast<std::int64_t>(step) * kTotpPeriodSec;
        if (at < 0) continue;
        const std::string candidate = totpCode(secret, at);
        if (!candidate.empty() && constantTimeEquals(candidate, code)) return true;
    }
    return false;
}

// ---------- 会话票据 ----------

core::Result<IssuedTicket> TicketStore::issue(const std::vector<std::uint8_t>& key,
                                              std::int64_t nowSec) {
    prune(nowSec);
    if (tickets_.size() >= kMaxTickets) {
        return core::Result<IssuedTicket>::fail(core::ErrorCode::Conflict, "too_many_tickets");
    }
    auto id = platform::web_crypto::randomBytes(kTicketIdBytes);
    if (id.empty()) {
        return core::Result<IssuedTicket>::fail(core::ErrorCode::Internal, "rng_failed");
    }
    Ticket ticket;
    ticket.idHex = toHexLower(id);
    ticket.createdAt = nowSec;
    ticket.expiry = nowSec + kTicketTtlSec;
    auto payload = ticketPayload(ticket.expiry, id);
    auto tag = hmacV1(key, payload);
    if (tag.empty()) {
        return core::Result<IssuedTicket>::fail(core::ErrorCode::Internal, "hmac_failed");
    }
    payload.insert(payload.end(), tag.begin(), tag.end());
    tickets_.emplace(ticket.idHex, ticket);
    return core::Result<IssuedTicket>::ok(IssuedTicket{base64UrlEncode(payload), ticket});
}

core::Result<Ticket> TicketStore::validate(const std::string& token,
                                           const std::vector<std::uint8_t>& key,
                                           std::int64_t nowSec) const {
    auto raw = platform::web_crypto::base64UrlDecode(token);
    const std::size_t expected = 1 + 8 + kTicketIdBytes + 32;
    if (raw.isFail() || raw.value().size() != expected) {
        return core::Result<Ticket>::fail(core::ErrorCode::ValidationError, "bad_ticket");
    }
    const auto& bytes = raw.value();
    if (bytes[0] != 1) {
        return core::Result<Ticket>::fail(core::ErrorCode::ValidationError, "bad_ticket");
    }
    std::vector<std::uint8_t> payload(bytes.begin(), bytes.begin() + 1 + 8 + kTicketIdBytes);
    std::vector<std::uint8_t> tag(bytes.begin() + static_cast<std::ptrdiff_t>(payload.size()),
                                  bytes.end());
    auto expectedTag = hmacV1(key, payload);
    if (expectedTag.size() != tag.size() ||
        !constantTimeEquals(std::string(expectedTag.begin(), expectedTag.end()),
                            std::string(tag.begin(), tag.end()))) {
        return core::Result<Ticket>::fail(core::ErrorCode::ValidationError, "bad_ticket");
    }
    std::uint64_t expiryRaw = 0;
    for (std::size_t i = 1; i < 9; ++i) {
        expiryRaw = (expiryRaw << 8) | payload[i];
    }
    const std::int64_t expiry = static_cast<std::int64_t>(expiryRaw);
    if (nowSec >= expiry) {
        return core::Result<Ticket>::fail(core::ErrorCode::ValidationError, "expired_ticket");
    }
    std::vector<std::uint8_t> id(payload.begin() + 9, payload.begin() + 9 +
                                                       static_cast<std::ptrdiff_t>(kTicketIdBytes));
    const auto it = tickets_.find(toHexLower(id));
    if (it == tickets_.end()) {
        return core::Result<Ticket>::fail(core::ErrorCode::ValidationError, "revoked_ticket");
    }
    return core::Result<Ticket>::ok(it->second);
}

bool TicketStore::revoke(const std::string& ticketIdHex) {
    return tickets_.erase(ticketIdHex) > 0;
}

void TicketStore::revokeAll() { tickets_.clear(); }

void TicketStore::prune(std::int64_t nowSec) {
    for (auto it = tickets_.begin(); it != tickets_.end();) {
        if (nowSec >= it->second.expiry) {
            it = tickets_.erase(it);
        } else {
            ++it;
        }
    }
}

std::string TicketStore::serialize() const {
    std::string out;
    for (const auto& [id, ticket] : tickets_) {
        out += id + ' ' + std::to_string(ticket.expiry) + ' ' +
               std::to_string(ticket.createdAt) + '\n';
    }
    return out;
}

TicketStore TicketStore::deserialize(std::string_view text) {
    TicketStore store;
    std::istringstream stream{std::string(text)};
    std::string line;
    while (std::getline(stream, line)) {
        std::istringstream row(line);
        std::string id;
        Ticket ticket;
        if (!(row >> id >> ticket.expiry >> ticket.createdAt)) continue;
        if (!isHex32(id)) continue;
        ticket.idHex = id;
        store.tickets_.emplace(id, ticket);
    }
    return store;
}

bool TicketStore::saveToFile(const std::filesystem::path& path) const {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    const std::string text = serialize();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(file);
}

TicketStore TicketStore::loadFromFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return TicketStore{};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return deserialize(buffer.str());
}

// ---------- 限流 ----------

SlidingWindowLimiter::SlidingWindowLimiter(int limit, int windowSec)
    : limit_(limit), windowSec_(windowSec) {}

bool SlidingWindowLimiter::tryAcquire(const std::string& address, std::int64_t nowSec) {
    auto& hits = hits_[address];
    const std::int64_t cutoff = nowSec - windowSec_;
    while (!hits.empty() && hits.front() < cutoff) hits.pop_front();
    hits.push_back(nowSec);
    return static_cast<int>(hits.size()) <= limit_;
}

void SlidingWindowLimiter::reset(const std::string& address) { hits_.erase(address); }

LoginLockout::LoginLockout(int maxFailures, int windowSec)
    : maxFailures_(maxFailures), windowSec_(windowSec) {}

bool LoginLockout::isLocked(const std::string& address, std::int64_t nowSec) const {
    return failureCount(address, nowSec) >= maxFailures_;
}

void LoginLockout::recordFailure(const std::string& address, std::int64_t nowSec) {
    failures_[address].push_back(nowSec);
}

void LoginLockout::recordSuccess(const std::string& address) { failures_.erase(address); }

int LoginLockout::failureCount(const std::string& address, std::int64_t nowSec) const {
    const auto it = failures_.find(address);
    if (it == failures_.end()) return 0;
    const std::int64_t cutoff = nowSec - windowSec_;
    return static_cast<int>(
        std::count_if(it->second.begin(), it->second.end(),
                      [cutoff](std::int64_t at) { return at >= cutoff; }));
}

// ---------- 失败事件日志 ----------

std::string failureLogLine(const std::string& address, std::string_view event,
                           std::int64_t nowSec) {
    return "time=" + std::to_string(nowSec) + " ip=" + address + " event=" +
           std::string(event);
}

void logAuthFailure(const std::string& address, std::string_view event, std::int64_t nowSec) {
    LOG_WARN(failureLogLine(address, event, nowSec));
}

} // namespace module::terminal::web
