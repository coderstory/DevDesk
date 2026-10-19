// tests/test_platform/test_notification.cpp
// platform::notify facade 契约（spec §5 + §10.1）：所有发送走 fake backend，
// 默认后端只在裸进程 guard 用例里调用，不触发真实系统通知。
//   * 清洗/截断/去重/限速/授权由 facade 产生；后端只产生 5 种环境状态；
//   * Result::fail 只留给清洗后为空；环境状态一律 ok(Receipt)；
//   * 三态（deduped/rate_limited/truncated）在草案 Result<void> 接口下不可表达
//     ——断言 Receipt 字段的用例即「无实现必红」的口径。
#include "doctest.h"

#include "core/result.h"
#include "platform/notification.h"

#include <chrono>
#include <string>

namespace {

using platform::notify::Authorization;
using platform::notify::Backend;
using platform::notify::Notification;
using platform::notify::Receipt;
using platform::notify::Status;
using Clock = std::chrono::steady_clock;

struct FakeBackend final : Backend {
    int sendCalls = 0;
    int authorizationRequests = 0;
    int initializeCalls = 0;
    Notification lastNotification;
    core::Result<Status> sendResult = core::Result<Status>::ok(Status::Accepted);
    Status sendStatus = Status::Accepted;
    Authorization status = Authorization::Authorized;

    void initialize() override { ++initializeCalls; }

    core::Result<Status> send(const Notification& notification) override {
        ++sendCalls;
        lastNotification = notification;
        if (sendResult.isFail()) return sendResult;
        return core::Result<Status>::ok(sendStatus);
    }

    Authorization authorizationStatus() override { return status; }

    void requestAuthorization() override { ++authorizationRequests; }
};

// 用例 fail-fast 时析构仍会执行：全量复位（后端/去重/限速/时钟），避免悬垂与串味。
struct ResetGuard {
    ~ResetGuard() { platform::notify::resetForTest(); }
};

// 裸进程默认后端的平台差异放在宏外：REGISTER_TEST 是宏，体内直接写 #if 会触发
// -Wembedded-directive（-Werror 下编译红）。
bool bareProcessReceiptIsHonest(const Receipt& receipt) {
#if defined(__APPLE__)
    return receipt.status == Status::NotBundled &&
           receipt.detail.find(".app") != std::string::npos &&
           receipt.detail.find("bundle") != std::string::npos;
#else
    (void)receipt;
    return receipt.status == Status::BackendUnavailable;
#endif
}

// 裸进程的默认后端授权态：Windows 无应用级授权模型（托盘可用即允许）；
// macOS bundle guard 与 POSIX stub 都是「无法查询」= Unknown。
Authorization bareProcessAuthorization() {
#if defined(_WIN32)
    return Authorization::Authorized;
#else
    return Authorization::Unknown;
#endif
}

std::string repeat(const std::string& unit, size_t count) {
    std::string out;
    out.reserve(unit.size() * count);
    for (size_t i = 0; i < count; ++i) out += unit;
    return out;
}

// 假时钟：base + offset，用例内推进 offset。
struct FakeClock {
    Clock::time_point base = Clock::now();
    long offsetMs = 0;

    void install() {
        platform::notify::setClockForTest(
            [this]() { return base + std::chrono::milliseconds(offsetMs); });
    }
    void advance(long ms) { offsetMs += ms; }
};

} // namespace

REGISTER_TEST(notify_send_forwards_fields,
              "notify: 清洗后字段/level/sound/id 原样转发并返回 accepted", {
    ResetGuard guard;
    FakeBackend fake;
    platform::notify::setBackendForTest(&fake);

    Notification notification;
    notification.id = "task-42";
    notification.title = "任务完成";
    notification.body = "今天的备份已写入";
    notification.sound = false;
    notification.level = platform::notify::Level::Success;

    const auto result = platform::notify::send(notification);
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK_EQ(fake.sendCalls, 1);
    DOCTEST_CHECK_EQ(fake.lastNotification.id, std::string("task-42"));
    DOCTEST_CHECK_EQ(fake.lastNotification.title, std::string("任务完成"));
    DOCTEST_CHECK_EQ(fake.lastNotification.body, std::string("今天的备份已写入"));
    DOCTEST_CHECK_EQ(fake.lastNotification.sound, false);
    DOCTEST_CHECK(fake.lastNotification.level == platform::notify::Level::Success);

    const Receipt& receipt = result.value();
    DOCTEST_CHECK(receipt.status == Status::Accepted);
    DOCTEST_CHECK_EQ(receipt.effectiveId, std::string("task-42"));
    DOCTEST_CHECK_FALSE(receipt.titleTruncated);
    DOCTEST_CHECK_FALSE(receipt.bodyTruncated);
    return true;
})

REGISTER_TEST(notify_backend_failure_maps_unavailable,
              "notify: 后端 Result::fail 映射为 ok(BackendUnavailable) 且保留 detail", {
    ResetGuard guard;
    FakeBackend fake;
    fake.sendResult = core::Result<Status>::fail(core::ErrorCode::Internal,
                                                 "后端明确失败");
    platform::notify::setBackendForTest(&fake);

    Notification notification;
    notification.title = "标题";
    notification.body = "正文";
    const auto result = platform::notify::send(notification);
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().status == Status::BackendUnavailable);
    DOCTEST_CHECK(result.value().detail.find("后端明确失败") != std::string::npos);
    DOCTEST_CHECK_EQ(fake.sendCalls, 1);
    return true;
})

REGISTER_TEST(notify_empty_title_or_body_rejected,
              "notify: 清洗后 title/body 为空 → fail(ValidationError) 且后端零调用", {
    ResetGuard guard;
    FakeBackend fake;
    platform::notify::setBackendForTest(&fake);

    Notification emptyTitle;
    emptyTitle.title = "   ";
    emptyTitle.body = "正文";
    const auto r1 = platform::notify::send(emptyTitle);
    DOCTEST_CHECK(r1.isFail());
    DOCTEST_CHECK_EQ(static_cast<int>(r1.errorCode()),
                     static_cast<int>(core::ErrorCode::ValidationError));
    DOCTEST_CHECK(r1.errorMessage().find("title") != std::string::npos);

    Notification blankLines;
    blankLines.title = "标题";
    blankLines.body = "\n \n";
    const auto r2 = platform::notify::send(blankLines);
    DOCTEST_CHECK(r2.isFail());
    DOCTEST_CHECK(r2.errorMessage().find("body") != std::string::npos);

    DOCTEST_CHECK_EQ(fake.sendCalls, 0);
    return true;
})

REGISTER_TEST(notify_utf8_sanitized,
              "notify: 非法 UTF-8 字节替换为 U+FFFD 后继续发送", {
    ResetGuard guard;
    FakeBackend fake;
    platform::notify::setBackendForTest(&fake);

    Notification notification;
    notification.title = "坏\xFF字节";
    notification.body = "正文";
    const auto result = platform::notify::send(notification);
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().status == Status::Accepted);
    const std::string replacement = "\xEF\xBF\xBD";
    DOCTEST_CHECK(fake.lastNotification.title.find(replacement) != std::string::npos);
    DOCTEST_CHECK_EQ(fake.sendCalls, 1);
    return true;
})

REGISTER_TEST(notify_title_body_truncated,
              "notify: 63/255 unit 截断、省略号、truncated 标志、代理对不切开", {
    ResetGuard guard;
    FakeBackend fake;
    platform::notify::setBackendForTest(&fake);

    // 标题：64 个汉字 = 64 unit → 截成 62 字 + …（共 63 unit）。
    Notification longTitle;
    longTitle.title = repeat("字", 64);
    longTitle.body = "正文";
    auto result = platform::notify::send(longTitle);
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().titleTruncated);
    DOCTEST_CHECK_EQ(fake.lastNotification.title, repeat("字", 62) + "\xE2\x80\xA6");
    DOCTEST_CHECK_FALSE(result.value().bodyTruncated);

    // 正文：300 ASCII = 300 unit → 254 + …。
    Notification longBody;
    longBody.title = "标题";
    longBody.body = repeat("b", 300);
    result = platform::notify::send(longBody);
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().bodyTruncated);
    DOCTEST_CHECK_EQ(fake.lastNotification.body, repeat("b", 254) + "\xE2\x80\xA6");

    // 代理对：62 ASCII + 😀（2 unit）= 64 → 只保留 62 ASCII + …，emoji 整体丢弃。
    Notification surrogate;
    surrogate.title = repeat("a", 62) + "\xF0\x9F\x98\x80";
    surrogate.body = "正文";
    result = platform::notify::send(surrogate);
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().titleTruncated);
    DOCTEST_CHECK_EQ(fake.lastNotification.title, repeat("a", 62) + "\xE2\x80\xA6");
    DOCTEST_CHECK(fake.lastNotification.title.find("\xF0\x9F") == std::string::npos);

    // 恰好 63 unit（61 ASCII + emoji）不截断。本用例已发 3 条打空突发桶，
    // 假时钟前进 1s 补一个令牌，避免第 4 条被自家限速拦在门外。
    FakeClock clock;
    clock.install();
    clock.advance(1000);
    Notification exact;
    exact.title = repeat("a", 61) + "\xF0\x9F\x98\x80";
    exact.body = "正文";
    result = platform::notify::send(exact);
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK_FALSE(result.value().titleTruncated);
    DOCTEST_CHECK(fake.lastNotification.title.find("\xF0\x9F\x98\x80") != std::string::npos);
    return true;
})

REGISTER_TEST(notify_title_control_chars,
              "notify: 标题 CR/LF/TAB/C0/DEL 清洗、连续空格折叠、去首尾", {
    ResetGuard guard;
    FakeBackend fake;
    platform::notify::setBackendForTest(&fake);

    Notification notification;
    notification.title = std::string("a\r\nb\tc\x01 d  e\x7F");
    notification.body = "正文";
    const auto result = platform::notify::send(notification);
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK_EQ(fake.lastNotification.title, std::string("a b c d e"));
    return true;
})

REGISTER_TEST(notify_body_newlines_preserved,
              "notify: 正文 CRLF→LF、3+ 连续换行折 2、去首尾空白", {
    ResetGuard guard;
    FakeBackend fake;
    platform::notify::setBackendForTest(&fake);

    Notification notification;
    notification.title = "标题";
    notification.body = std::string("x\r\ny\r\rz\n\n\n\nw  ");
    const auto result = platform::notify::send(notification);
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK_EQ(fake.lastNotification.body, std::string("x\ny\n\nz\n\nw"));
    return true;
})

REGISTER_TEST(notify_id_derived_when_empty,
              "notify: 空 id 派生 devdesk-<16位十六进制> 且同内容稳定", {
    ResetGuard guard;
    FakeBackend fake;
    platform::notify::setBackendForTest(&fake);

    Notification notification;
    notification.title = "标题";
    notification.body = "正文";
    const auto first = platform::notify::send(notification);
    DOCTEST_REQUIRE(first.isOk());
    const std::string id = first.value().effectiveId;
    DOCTEST_CHECK(id.rfind("devdesk-", 0) == 0);
    DOCTEST_CHECK_EQ(id.size(), static_cast<size_t>(16 + 8));

    const auto second = platform::notify::send(notification);
    DOCTEST_REQUIRE(second.isOk());
    DOCTEST_CHECK_EQ(second.value().effectiveId, id);
    return true;
})

REGISTER_TEST(notify_dedupe_same_content,
              "notify: 2s 内同 id+内容第二次 → deduped 且后端只调一次", {
    ResetGuard guard;
    FakeBackend fake;
    platform::notify::setBackendForTest(&fake);

    Notification notification;
    notification.id = "dup-1";
    notification.title = "标题";
    notification.body = "正文";
    const auto first = platform::notify::send(notification);
    DOCTEST_REQUIRE(first.isOk());
    DOCTEST_CHECK(first.value().status == Status::Accepted);

    const auto second = platform::notify::send(notification);
    DOCTEST_REQUIRE(second.isOk());
    DOCTEST_CHECK(second.value().status == Status::Deduped);
    DOCTEST_CHECK_EQ(fake.sendCalls, 1);
    return true;
})

REGISTER_TEST(notify_dedupe_expires,
              "notify: 假时钟前进 2s 后同内容可再发", {
    ResetGuard guard;
    FakeBackend fake;
    FakeClock clock;
    clock.install();
    platform::notify::setBackendForTest(&fake);

    Notification notification;
    notification.id = "dup-2";
    notification.title = "标题";
    notification.body = "正文";
    auto first = platform::notify::send(notification);
    DOCTEST_REQUIRE(first.isOk());
    DOCTEST_CHECK(first.value().status == Status::Accepted);

    clock.advance(2100);
    const auto second = platform::notify::send(notification);
    DOCTEST_REQUIRE(second.isOk());
    DOCTEST_CHECK(second.value().status == Status::Accepted);
    DOCTEST_CHECK_EQ(fake.sendCalls, 2);
    return true;
})

// 过期重建路径回归网（L4）：同键过期后重发 → 窗口内再发仍 Deduped。钉住重插路径——
// order 表每键至多一个条目的不变式若被破坏（残留陈旧键位），这里最先碎。
// 注：「陈旧 order 键位导致容量驱逐提前抹掉活条目」的症状经公共 API 不可构造——
// 令牌桶（3 突发 + 1 条/秒）使 2s 窗口内至多 ~5 个 order 条目，而驱逐需 >64 条，
// 任何被驱逐条目必然早已出窗；该面属结构不变式，由本用例 + 代码内摘除逻辑共同守。
REGISTER_TEST(notify_dedupe_expired_rebuild_still_dedupes,
              "notify: 同键过期重发（重建）后，2s 窗口内再发仍 deduped 且后端不再调", {
    ResetGuard guard;
    FakeBackend fake;
    FakeClock clock;
    clock.install();
    platform::notify::setBackendForTest(&fake);

    Notification notification;
    notification.id = "dup-rebuild";
    notification.title = "标题";
    notification.body = "正文";
    auto first = platform::notify::send(notification);
    DOCTEST_REQUIRE(first.isOk());
    DOCTEST_CHECK(first.value().status == Status::Accepted);

    // 过期 → 重建（erase + 重插路径）
    clock.advance(2100);
    auto rebuild = platform::notify::send(notification);
    DOCTEST_REQUIRE(rebuild.isOk());
    DOCTEST_CHECK(rebuild.value().status == Status::Accepted);

    // 重建后窗口内再发 → 仍去重（后端零调用增量）
    const int sentBefore = fake.sendCalls;
    auto within = platform::notify::send(notification);
    DOCTEST_REQUIRE(within.isOk());
    DOCTEST_CHECK(within.value().status == Status::Deduped);
    DOCTEST_CHECK_EQ(fake.sendCalls, sentBefore);
    return true;
})

REGISTER_TEST(notify_dedupe_different_content,
              "notify: 同 id 不同内容不误去重", {
    ResetGuard guard;
    FakeBackend fake;
    platform::notify::setBackendForTest(&fake);

    Notification notification;
    notification.id = "dup-3";
    notification.title = "标题";
    notification.body = "第一条";
    auto result = platform::notify::send(notification);
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().status == Status::Accepted);

    notification.body = "第二条";
    result = platform::notify::send(notification);
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().status == Status::Accepted);
    DOCTEST_CHECK_EQ(fake.sendCalls, 2);
    return true;
})

REGISTER_TEST(notify_rate_limit_burst,
              "notify: 3 条突发通过，第 4 条 rate_limited 且 retryAfterMs>0", {
    ResetGuard guard;
    FakeBackend fake;
    platform::notify::setBackendForTest(&fake);

    for (int i = 0; i < 3; ++i) {
        Notification notification;
        notification.title = "突发";
        notification.body = "第" + std::to_string(i) + "条";
        auto result = platform::notify::send(notification);
        DOCTEST_REQUIRE(result.isOk());
        DOCTEST_CHECK(result.value().status == Status::Accepted);
    }

    Notification fourth;
    fourth.title = "突发";
    fourth.body = "第4条";
    const auto limited = platform::notify::send(fourth);
    DOCTEST_REQUIRE(limited.isOk());
    DOCTEST_CHECK(limited.value().status == Status::RateLimited);
    DOCTEST_CHECK(limited.value().retryAfterMs > 0);
    DOCTEST_CHECK(limited.value().retryAfterMs <= 1000);
    DOCTEST_CHECK_EQ(fake.sendCalls, 3);
    return true;
})

REGISTER_TEST(notify_rate_limit_refill,
              "notify: 假时钟前进 1s 后令牌恢复一条", {
    ResetGuard guard;
    FakeBackend fake;
    FakeClock clock;
    clock.install();
    platform::notify::setBackendForTest(&fake);

    for (int i = 0; i < 3; ++i) {
        Notification notification;
        notification.title = " refill";
        notification.body = "第" + std::to_string(i) + "条";
        auto result = platform::notify::send(notification);
        DOCTEST_REQUIRE(result.isOk());
        DOCTEST_CHECK(result.value().status == Status::Accepted);
    }
    Notification fourth;
    fourth.title = " refill";
    fourth.body = "第4条";
    auto limited = platform::notify::send(fourth);
    DOCTEST_REQUIRE(limited.isOk());
    DOCTEST_CHECK(limited.value().status == Status::RateLimited);

    clock.advance(1000);
    Notification fifth;
    fifth.title = " refill";
    fifth.body = "第5条";
    const auto refilled = platform::notify::send(fifth);
    DOCTEST_REQUIRE(refilled.isOk());
    DOCTEST_CHECK(refilled.value().status == Status::Accepted);
    DOCTEST_CHECK_EQ(fake.sendCalls, 4);
    return true;
})

REGISTER_TEST(notify_authorization_denied,
              "notify: 授权 Denied → ok(PermissionDenied) 且后端零调用", {
    ResetGuard guard;
    FakeBackend fake;
    fake.status = Authorization::Denied;
    platform::notify::setBackendForTest(&fake);

    Notification notification;
    notification.title = "标题";
    notification.body = "正文";
    const auto result = platform::notify::send(notification);
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().status == Status::PermissionDenied);
    DOCTEST_CHECK_EQ(fake.sendCalls, 0);
    DOCTEST_CHECK_EQ(fake.authorizationRequests, 0);   // 已拒绝不再弹窗
    return true;
})

REGISTER_TEST(notify_authorization_not_determined,
              "notify: 授权 NotDetermined → ok(NotDetermined)、异步请求授权、后端零调用", {
    ResetGuard guard;
    FakeBackend fake;
    fake.status = Authorization::NotDetermined;
    platform::notify::setBackendForTest(&fake);

    Notification notification;
    notification.title = "标题";
    notification.body = "正文";
    const auto result = platform::notify::send(notification);
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().status == Status::NotDetermined);
    DOCTEST_CHECK_EQ(fake.authorizationRequests, 1);
    DOCTEST_CHECK_EQ(fake.sendCalls, 0);
    return true;
})

REGISTER_TEST(notify_authorization_unknown_proceeds,
              "notify: 授权 Unknown（无法查询）仍尝试后端", {
    ResetGuard guard;
    FakeBackend fake;
    fake.status = Authorization::Unknown;
    platform::notify::setBackendForTest(&fake);

    Notification notification;
    notification.title = "标题";
    notification.body = "正文";
    const auto result = platform::notify::send(notification);
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().status == Status::Accepted);
    DOCTEST_CHECK_EQ(fake.sendCalls, 1);
    return true;
})

REGISTER_TEST(notify_default_backend_in_bare_process,
              "notify: 裸进程默认后端 ok 回执且状态诚实（macOS NotBundled / 其它 BackendUnavailable）", {
    // devdesk_tests 不是 .app bundle：macOS 必须走 bundle guard（NotBundled，detail
    // 点名 .app / bundle），其它平台走 stub（BackendUnavailable）。都不许抛异常。
    platform::notify::resetForTest();

    Notification notification;
    notification.title = "标题";
    notification.body = "正文";
    const auto result = platform::notify::send(notification);
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(bareProcessReceiptIsHonest(result.value()));

    DOCTEST_CHECK(platform::notify::authorizationStatus() == bareProcessAuthorization());
    platform::notify::requestAuthorization();   // 只验证不崩（guard / stub 空实现）
    return true;
})
REGISTER_TEST(notify_backend_seam_restores_default,
              "notify: 测试缝注入/恢复/resetForTest 不悬垂", {
    ResetGuard guard;
    platform::notify::resetBackendForTest();
    DOCTEST_REQUIRE(&platform::notify::backend() == &platform::notify::defaultBackend());

    FakeBackend first;
    Backend* previous = platform::notify::setBackendForTest(&first);
    DOCTEST_CHECK(previous == &platform::notify::defaultBackend());
    DOCTEST_CHECK(&platform::notify::backend() == &first);

    // 传 nullptr = 恢复默认，并把旧后端（栈上 fake）交还调用方。
    Backend* replaced = platform::notify::setBackendForTest(nullptr);
    DOCTEST_CHECK(replaced == &first);
    DOCTEST_CHECK(&platform::notify::backend() == &platform::notify::defaultBackend());

    // 再注入后走 resetForTest 恢复；恢复后的默认后端仍可安全调用（不会访问已销毁的 fake）。
    platform::notify::setBackendForTest(&first);
    platform::notify::resetForTest();
    DOCTEST_CHECK(&platform::notify::backend() == &platform::notify::defaultBackend());
    Notification notification;
    notification.title = "标题";
    notification.body = "正文";
    const auto result = platform::notify::send(notification);
    DOCTEST_REQUIRE(result.isOk());
    return true;
})

REGISTER_TEST(notify_initialize_idempotent,
              "notify: 连续 initialize 只穿透后端一次；resetForTest 复位幂等标记", {
    ResetGuard guard;
    FakeBackend fake;
    platform::notify::setBackendForTest(&fake);

    platform::notify::initialize();
    platform::notify::initialize();
    DOCTEST_CHECK_EQ(fake.initializeCalls, 1);

    platform::notify::resetForTest();
    platform::notify::setBackendForTest(&fake);   // resetForTest 把槽位还原回默认后端，需重装
    platform::notify::initialize();
    DOCTEST_CHECK_EQ(fake.initializeCalls, 2);
    return true;
})
