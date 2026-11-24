// 网页终端鉴权纯逻辑（need#490 W2，技术卡 v9 §7 / §4）：绑定令牌状态机、TOTP、
// 会话票据与吊销表、握手限流与登录锁定。不接 HTTP、不碰 UI；时间一律由调用方以
// unix 秒显式传入（可测、无全局时钟）。加密原语走 platform::web_crypto（W1）。
//
// 票据持久化格式（saveToFile / loadFromFile，一行一条，空格分隔，UTF-8）：
//   <idHex 32字符> <expiry unix秒> <createdAt unix秒>\n
// 空表 = 空文件；格式错误行静默跳过（损坏不影响其余条目）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace module::terminal::web {

// ---------- 绑定令牌（技术卡 §7.1，P1-1 claim 幂等） ----------

enum class BindTokenState { Unclaimed, Claimed, Terminal };
enum class BindTokenTerminal { None, Expired, Used, WrongCodes };

inline constexpr int kBindTokenBytes = 32;
inline constexpr int kBindTokenTtlSec = 300;
inline constexpr int kMaxConfirmFailures = 5;

struct BindToken {
    std::string token;                 // base64url(32B)，43 字符
    std::vector<std::uint8_t> secret;  // 20B，TOTP 密钥
    std::int64_t expireAt = 0;         // unix 秒
    BindTokenState state = BindTokenState::Unclaimed;
    BindTokenTerminal terminal = BindTokenTerminal::None;
    int confirmFailures = 0;
};

// 新令牌（新 32B 令牌 + 20B 密钥，expireAt = now + 300）；旧令牌由调用方丢弃即作废
core::Result<BindToken> makeBindToken(std::int64_t nowSec);

struct BindClaim {
    std::string secret;   // base32 展示串
    std::string otpauth;  // otpauth URI
};

// claim：Unclaimed/Claimed 均返回同一 {secret, otpauth}（幂等），首次调用转 Claimed；
// Terminal（过期/已消费/错码作废）返回失败，不泄露原因。
core::Result<BindClaim> claimBindToken(BindToken& t, std::int64_t nowSec);

enum class ConfirmOutcome {
    Ok,            // 码正确 → 令牌消费，state=Terminal/Used
    InvalidCode,   // 码错；累计 5 次 → state=Terminal/WrongCodes
    NotClaimed,    // 非 Claimed（未 claim / 已终态）
};

// confirm 仅 Claimed 态可用；Ok 与 InvalidCode 均可能推进状态
ConfirmOutcome confirmBindToken(BindToken& t, const std::string& code, std::int64_t nowSec);

// ---------- TOTP（技术卡 §7.2） ----------

inline constexpr int kTotpDigits = 6;
inline constexpr int kTotpPeriodSec = 30;
inline constexpr int kTotpSkewSteps = 1;

std::string base32Secret(const std::vector<std::uint8_t>& secret);
// digits 仅 RFC 6238 官方向量（8 位）校验用；产品路径必须留默认 6
std::string totpCode(const std::vector<std::uint8_t>& secret, std::int64_t unixSec,
                     int digits = kTotpDigits);
std::string totpOtpauthUri(const std::vector<std::uint8_t>& secret);
// 接受 T-1/T/T+1；定长比较。code 长度非 digits 直接拒绝
bool verifyTotp(const std::vector<std::uint8_t>& secret, const std::string& code,
                std::int64_t unixSec);

// ---------- 会话票据（技术卡 §6.5 / §7.3） ----------

inline constexpr std::size_t kMaxTickets = 64;
inline constexpr std::int64_t kTicketTtlSec = 7 * 24 * 60 * 60;
inline constexpr std::size_t kTicketIdBytes = 16;

// payload = ver1(1B) ‖ expiry(8B BE) ‖ ticketId(16B)
// token   = base64url(payload ‖ HMAC-SHA256(key, "v1|" ‖ payload))  → 76 字符
struct Ticket {
    std::string idHex;      // 32 字符小写 hex
    std::int64_t expiry = 0;
    std::int64_t createdAt = 0;
};

struct IssuedTicket {
    std::string token;
    Ticket ticket;
};

// 吊销表：≤64 条，7 天绝对过期，不淘汰有效票据；重启保留（saveToFile / loadFromFile）。
class TicketStore {
public:
    // 先清理过期；仍有 64 条有效 → ErrorCode::Conflict("too_many_tickets")，
    // 已有条目一条不动（W3 映射 HTTP 409）
    core::Result<IssuedTicket> issue(const std::vector<std::uint8_t>& key,
                                     std::int64_t nowSec);

    // 签名 → 版本 → 绝对过期 → 表中存在。失败码统一 ValidationError，
    // 消息区分 bad_ticket / expired_ticket / revoked_ticket（仅日志用，不回显客户端）
    core::Result<Ticket> validate(const std::string& token,
                                  const std::vector<std::uint8_t>& key,
                                  std::int64_t nowSec) const;

    bool revoke(const std::string& ticketIdHex);  // 退出登录
    void revokeAll();                             // 撤销绑定：清整表
    void prune(std::int64_t nowSec);
    std::size_t size() const { return tickets_.size(); }
    bool empty() const { return tickets_.empty(); }

    std::string serialize() const;
    static TicketStore deserialize(std::string_view text);
    bool saveToFile(const std::filesystem::path& path) const;
    static TicketStore loadFromFile(const std::filesystem::path& path);

private:
    std::map<std::string, Ticket> tickets_;
};

// ---------- 限流（技术卡 §4 末条；P1 新裁决：按客户端地址计数） ----------

inline constexpr int kHandshakeClientLimit = 12;  // 单客户端 12 次/60s
inline constexpr int kHandshakeGlobalLimit = 32;  // 全站 32 次/60s
inline constexpr int kHandshakeLimit = kHandshakeClientLimit;  // 兼容旧引用（单客户端口径）
inline constexpr int kLoginFailureLimit = 5;  // 次/60s
inline constexpr int kRateWindowSec = 60;

// 60 秒滑动窗口；A 打满不影响 B（每地址独立 deque）。
// 线程契约：HTTP 线程池会并发调用（握手入口不经服务主锁），故内部互斥；
// 不给「仅单线程调用」的锁死契约——真实并发源就是 httplib 工作线程。
class SlidingWindowLimiter {
public:
    SlidingWindowLimiter(int limit, int windowSec);
    // 记录一次并判定：窗口内第 limit+1 次起返回 false（拒绝也占位，防重试刷穿）
    bool tryAcquire(const std::string& address, std::int64_t nowSec);
    void reset(const std::string& address);

private:
    int limit_;
    int windowSec_;
    mutable std::mutex mutex_;
    std::map<std::string, std::deque<std::int64_t>> hits_;
};

// 登录锁定：5 次失败/60s 按 IP。锁定期间 isLocked=true → 调用方跳过校验、不 recordFailure
class LoginLockout {
public:
    explicit LoginLockout(int maxFailures = kLoginFailureLimit,
                          int windowSec = kRateWindowSec);
    bool isLocked(const std::string& address, std::int64_t nowSec) const;
    void recordFailure(const std::string& address, std::int64_t nowSec);
    void recordSuccess(const std::string& address);
    int failureCount(const std::string& address, std::int64_t nowSec) const;

private:
    int maxFailures_;
    int windowSec_;
    std::map<std::string, std::deque<std::int64_t>> failures_;
};

// 失败事件日志字段 = time/ip/event（技术卡 §4：统一 LOG_WARN，不回显列表）
std::string failureLogLine(const std::string& address, std::string_view event,
                           std::int64_t nowSec);
void logAuthFailure(const std::string& address, std::string_view event, std::int64_t nowSec);

} // namespace module::terminal::web
