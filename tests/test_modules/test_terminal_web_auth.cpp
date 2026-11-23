// tests/test_modules/test_terminal_web_auth.cpp
// 网页终端鉴权纯逻辑单测（need#490 W2，技术卡 v9 §4/§7）：令牌状态机与幂等、
// TOTP（RFC 6238 官方向量 + ±1 步）、票据签发/校验/过期/篡改/吊销/64 上限/重启保留、
// 握手限流与登录锁定按 IP 隔离、失败事件字段。
#include "doctest.h"

#include "module_terminal/web/web_auth.h"

#include <filesystem>
#include <string>
#include <vector>

namespace {

using module::terminal::web::BindToken;
using module::terminal::web::BindTokenState;
using module::terminal::web::BindTokenTerminal;
using module::terminal::web::ConfirmOutcome;
using module::terminal::web::IssuedTicket;
using module::terminal::web::LoginLockout;
using module::terminal::web::SlidingWindowLimiter;
using module::terminal::web::Ticket;
using module::terminal::web::TicketStore;

std::vector<std::uint8_t> asciiBytes(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::vector<std::uint8_t> ticketKey() { return std::vector<std::uint8_t>(32, 0x5a); }

} // namespace

REGISTER_TEST(webAuthTokenLifecycle,
              "web auth: 令牌 300s 过期、claim 幂等、terminal 拒绝（need#490 §7.1）", {
    using namespace module::terminal::web;
    auto made = makeBindToken(1000);
    DOCTEST_REQUIRE(made.isOk());
    BindToken t = made.value();
    DOCTEST_CHECK(t.state == BindTokenState::Unclaimed);
    DOCTEST_CHECK_EQ(t.expireAt, 1000 + kBindTokenTtlSec);
    DOCTEST_CHECK_EQ(t.token.size(), static_cast<std::size_t>(43));  // 32B base64url
    DOCTEST_CHECK_EQ(t.secret.size(), static_cast<std::size_t>(20));

    // 首次 claim 转 claimed，二次同密钥（幂等）
    auto first = claimBindToken(t, 1000);
    DOCTEST_REQUIRE(first.isOk());
    const std::string secret = first.value().secret;
    DOCTEST_CHECK_EQ(t.state, BindTokenState::Claimed);
    DOCTEST_CHECK(secret.find('=') == std::string::npos);  // base32 无填充
    DOCTEST_CHECK(first.value().otpauth.rfind("otpauth://totp/DevDesk:web-terminal?secret=", 0) == 0);

    auto second = claimBindToken(t, 1200);
    DOCTEST_REQUIRE(second.isOk());
    DOCTEST_CHECK_EQ(second.value().secret, secret);
    DOCTEST_CHECK_EQ(second.value().otpauth, first.value().otpauth);

    // 过期即终态：1300 到期，1301 拒绝且不可再 claim
    auto expired = claimBindToken(t, 1000 + kBindTokenTtlSec + 1);
    DOCTEST_CHECK(expired.isFail());
    DOCTEST_CHECK_EQ(t.state, BindTokenState::Terminal);
    DOCTEST_CHECK_EQ(t.terminal, BindTokenTerminal::Expired);
    DOCTEST_CHECK(claimBindToken(t, 1302).isFail());
    return true;
});

REGISTER_TEST(webAuthConfirmConsumes,
              "web auth: confirm 仅 claimed、码对消费、错 5 次作废（need#490 §7.1）", {
    using namespace module::terminal::web;
    auto made = makeBindToken(2000);
    DOCTEST_REQUIRE(made.isOk());
    BindToken t = made.value();

    // 未 claim 不可 confirm
    DOCTEST_CHECK(confirmBindToken(t, totpCode(t.secret, 2000), 2000) == ConfirmOutcome::NotClaimed);
    DOCTEST_REQUIRE(claimBindToken(t, 2000).isOk());

    // ±1 步内的正确码消费；已消费后拒绝
    const std::string code = totpCode(t.secret, 2000 + 30);
    DOCTEST_CHECK(confirmBindToken(t, code, 2000) == ConfirmOutcome::Ok);
    DOCTEST_CHECK_EQ(t.state, BindTokenState::Terminal);
    DOCTEST_CHECK_EQ(t.terminal, BindTokenTerminal::Used);
    DOCTEST_CHECK(confirmBindToken(t, code, 2000) == ConfirmOutcome::NotClaimed);

    // 错码累计 5 次 → 终态（第 5 次仍回 InvalidCode，之后 NotClaimed）
    auto made2 = makeBindToken(3000);
    DOCTEST_REQUIRE(made2.isOk());
    BindToken bad = made2.value();
    DOCTEST_REQUIRE(claimBindToken(bad, 3000).isOk());
    for (int i = 0; i < kMaxConfirmFailures; ++i) {
        DOCTEST_CHECK(confirmBindToken(bad, "000000", 3000) == ConfirmOutcome::InvalidCode);
    }
    DOCTEST_CHECK_EQ(bad.state, BindTokenState::Terminal);
    DOCTEST_CHECK_EQ(bad.terminal, BindTokenTerminal::WrongCodes);
    DOCTEST_CHECK(confirmBindToken(bad, totpCode(bad.secret, 3000), 3000) ==
                  ConfirmOutcome::NotClaimed);

    // claimed 但过期：confirm 拒绝且不消费
    auto made3 = makeBindToken(4000);
    DOCTEST_REQUIRE(made3.isOk());
    BindToken late = made3.value();
    DOCTEST_REQUIRE(claimBindToken(late, 4000).isOk());
    const std::string lateCode = totpCode(late.secret, 4000 + kBindTokenTtlSec + 1);
    DOCTEST_CHECK(confirmBindToken(late, lateCode, 4000 + kBindTokenTtlSec + 1) ==
                  ConfirmOutcome::NotClaimed);
    DOCTEST_CHECK_EQ(late.terminal, BindTokenTerminal::Expired);
    return true;
});

REGISTER_TEST(webAuthTotpRfc6238,
              "web auth: TOTP RFC 6238 SHA1 向量 + 6 位产品码 ±1 步（need#490 §7.2）", {
    using namespace module::terminal::web;
    // RFC 6238 附录 B：secret = ASCII "12345678901234567890"（SHA1 组合）
    const auto secret = asciiBytes("12345678901234567890");
    DOCTEST_CHECK_EQ(base32Secret(secret), "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ");

    const std::int64_t times[] = {59, 1111111109, 1111111111, 1234567890, 2000000000, 20000000000};
    const char* expected[] = {"94287082", "07081804", "14050471", "89005924", "69279037",
                              "65353130"};
    for (int i = 0; i < 6; ++i) {
        DOCTEST_CHECK_EQ(totpCode(secret, times[i], 8), std::string(expected[i]));
    }

    // 产品码（6 位，T=1111111109）：T-1/T/T+1 通过，T±2 拒绝
    const std::int64_t t = 1111111109;
    DOCTEST_CHECK(verifyTotp(secret, totpCode(secret, t), t));
    DOCTEST_CHECK(verifyTotp(secret, totpCode(secret, t - 30), t));
    DOCTEST_CHECK(verifyTotp(secret, totpCode(secret, t + 30), t));
    DOCTEST_CHECK_FALSE(verifyTotp(secret, totpCode(secret, t - 60), t));
    DOCTEST_CHECK_FALSE(verifyTotp(secret, totpCode(secret, t + 60), t));
    DOCTEST_CHECK_FALSE(verifyTotp(secret, "00000", t));  // 长度不符直接拒绝
    return true;
});

REGISTER_TEST(webAuthTicketIssueValidate,
              "web auth: 票据签发/校验/过期/篡改/吊销（need#490 §6.5）", {
    using namespace module::terminal::web;
    const auto key = ticketKey();
    TicketStore store;
    auto issued = store.issue(key, 10000);
    DOCTEST_REQUIRE(issued.isOk());
    const IssuedTicket ticket = issued.value();
    DOCTEST_CHECK_EQ(ticket.token.size(), static_cast<std::size_t>(76));  // 57B base64url
    DOCTEST_CHECK_EQ(ticket.ticket.idHex.size(), static_cast<std::size_t>(32));
    DOCTEST_CHECK_EQ(store.size(), static_cast<std::size_t>(1));

    auto ok = store.validate(ticket.token, key, 10000);
    DOCTEST_REQUIRE(ok.isOk());
    DOCTEST_CHECK_EQ(ok.value().idHex, ticket.ticket.idHex);

    // 7 天绝对过期：到期时刻即失效
    DOCTEST_CHECK(store.validate(ticket.token, key, 10000 + kTicketTtlSec - 1).isOk());
    DOCTEST_CHECK(store.validate(ticket.token, key, 10000 + kTicketTtlSec).isFail());

    // 篡改：改末字符 / 换密钥
    std::string tampered = ticket.token;
    tampered.back() = (tampered.back() == 'A' ? 'B' : 'A');
    DOCTEST_CHECK(store.validate(tampered, key, 10000).isFail());
    DOCTEST_CHECK(store.validate(ticket.token, std::vector<std::uint8_t>(32, 0x01), 10000).isFail());

    // 退出登录删 ID；撤销绑定清表
    DOCTEST_CHECK(store.revoke(ticket.ticket.idHex));
    DOCTEST_CHECK(store.validate(ticket.token, key, 10000).isFail());
    DOCTEST_CHECK_FALSE(store.revoke(ticket.ticket.idHex));

    auto second = store.issue(key, 10000);
    DOCTEST_REQUIRE(second.isOk());
    store.revokeAll();
    DOCTEST_CHECK(store.empty());
    DOCTEST_CHECK(store.validate(second.value().token, key, 10000).isFail());
    return true;
});

REGISTER_TEST(webAuthTicketTableCap,
              "web auth: 票据 64 上限拒新不淘汰、过期清理后放行（need#490 §4）", {
    using namespace module::terminal::web;
    const auto key = ticketKey();
    TicketStore store;
    for (std::size_t i = 0; i < kMaxTickets; ++i) {
        auto issued = store.issue(key, 20000);
        DOCTEST_REQUIRE(issued.isOk());
    }
    DOCTEST_CHECK_EQ(store.size(), kMaxTickets);
    auto overflow = store.issue(key, 20000);
    DOCTEST_CHECK(overflow.isFail());
    DOCTEST_CHECK_EQ(overflow.errorCode(), core::ErrorCode::Conflict);
    DOCTEST_CHECK_EQ(overflow.errorMessage(), std::string("too_many_tickets"));
    DOCTEST_CHECK_EQ(store.size(), kMaxTickets);  // 已有 64 条一条不动

    // 7 天后过期清理：新签发成功
    auto after = store.issue(key, 20000 + kTicketTtlSec);
    DOCTEST_CHECK(after.isOk());
    DOCTEST_CHECK_EQ(store.size(), static_cast<std::size_t>(1));
    return true;
});

REGISTER_TEST(webAuthTicketPersistence,
              "web auth: 票据表重启保留（文件格式往返）（need#490 §6.5）", {
    using namespace module::terminal::web;
    const auto key = ticketKey();
    const std::filesystem::path path = test_temp::tempPath("web_auth_tickets.txt");
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::error_code ec; std::filesystem::remove(p, ec); }
    } cleanup{path};

    TicketStore store;
    auto issued = store.issue(key, 30000);
    DOCTEST_REQUIRE(issued.isOk());
    DOCTEST_REQUIRE(store.saveToFile(path));

    // 「重启」：全新 store 从文件恢复，旧票据仍可校验
    TicketStore reloaded = TicketStore::loadFromFile(path);
    DOCTEST_CHECK_EQ(reloaded.size(), static_cast<std::size_t>(1));
    auto still = reloaded.validate(issued.value().token, key, 30000 + 60);
    DOCTEST_CHECK(still.isOk());

    // 撤销清表后落盘，重启读到空表
    reloaded.revokeAll();
    DOCTEST_REQUIRE(reloaded.saveToFile(path));
    DOCTEST_CHECK(TicketStore::loadFromFile(path).empty());
    return true;
});

REGISTER_TEST(webAuthHandshakeRateLimit,
              "web auth: 握手 32 次/60s 滑动窗口按客户端地址计数（need#490 §4）", {
    using namespace module::terminal::web;
    SlidingWindowLimiter limiter(kHandshakeLimit, kRateWindowSec);
    for (int i = 0; i < kHandshakeLimit; ++i) {
        DOCTEST_CHECK(limiter.tryAcquire("192.168.1.10", 50000));
    }
    DOCTEST_CHECK_FALSE(limiter.tryAcquire("192.168.1.10", 50000));  // 第 33 次拒绝
    // A 打满不影响 B
    DOCTEST_CHECK(limiter.tryAcquire("192.168.1.11", 50000));
    // 窗口滑过（61s 后）恢复
    DOCTEST_CHECK(limiter.tryAcquire("192.168.1.10", 50000 + kRateWindowSec + 1));
    return true;
});

REGISTER_TEST(webAuthLoginLockout,
              "web auth: 登录锁定 5 次/60s 按 IP、成功清零、失败事件字段（need#490 §4）", {
    using namespace module::terminal::web;
    LoginLockout lockout;
    for (int i = 0; i < kLoginFailureLimit - 1; ++i) {
        lockout.recordFailure("10.0.0.5", 60000);
    }
    DOCTEST_CHECK_FALSE(lockout.isLocked("10.0.0.5", 60000));
    lockout.recordFailure("10.0.0.5", 60000);
    DOCTEST_CHECK(lockout.isLocked("10.0.0.5", 60000));
    // 其他 IP 不受影响
    DOCTEST_CHECK_FALSE(lockout.isLocked("10.0.0.6", 60000));
    // 窗口滑过后解锁
    DOCTEST_CHECK(lockout.isLocked("10.0.0.5", 60000 + kRateWindowSec - 1));
    DOCTEST_CHECK_FALSE(lockout.isLocked("10.0.0.5", 60000 + kRateWindowSec + 1));
    // 成功登录清零
    lockout.recordFailure("10.0.0.7", 70000);
    lockout.recordSuccess("10.0.0.7");
    DOCTEST_CHECK_EQ(lockout.failureCount("10.0.0.7", 70000), 0);

    const std::string line = failureLogLine("10.0.0.5", "bad_ticket", 60000);
    DOCTEST_CHECK(line.find("time=60000") != std::string::npos);
    DOCTEST_CHECK(line.find("ip=10.0.0.5") != std::string::npos);
    DOCTEST_CHECK(line.find("event=bad_ticket") != std::string::npos);
    return true;
});
