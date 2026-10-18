// tests/test_platform/test_notification.cpp
// platform::notify 抽象层契约：
//   1) 自由函数按字段原样转发给注入的后端，返回值透传；
//   2) 空 title / 空 body 在进入后端**之前**拦掉（后端零调用）；
//   3) 测试缝可注入 / 恢复默认后端，不悬垂；
//   4) 裸测试进程里的默认后端不崩溃（macOS 走 .app bundle guard，其它平台走 stub）。
//
// 本文件**不得**触发真实系统通知：所有发送都走 fake 或裸进程 guard。
#include "doctest.h"

#include "core/result.h"
#include "platform/notification.h"

#include <string>

namespace {

using platform::notify::Authorization;
using platform::notify::Backend;
using platform::notify::Notification;

struct FakeBackend final : Backend {
    int sendCalls = 0;
    int authorizationRequests = 0;
    Notification lastNotification;
    core::Result<void> sendResult = core::Result<void>::ok();
    Authorization status = Authorization::NotDetermined;

    core::Result<void> send(const Notification& notification) override {
        ++sendCalls;
        lastNotification = notification;
        return sendResult;
    }

    Authorization authorizationStatus() override { return status; }

    void requestAuthorization() override { ++authorizationRequests; }
};

// 用例 fail-fast 时析构仍会执行：把槽位拉回默认后端，避免栈上 fake 悬垂。
struct BackendResetGuard {
    ~BackendResetGuard() { platform::notify::resetBackendForTest(); }
};

// 裸进程默认后端的平台差异放在宏外：REGISTER_TEST 是宏，体内直接写 #if 会触发
// -Wembedded-directive（-Werror 下编译红）。macOS 必须点名 .app / bundle；
// 其它平台走 stub，只要求诚实失败。
bool bareProcessFailureMentionsBundle(const core::Result<void>& result) {
#if defined(__APPLE__)
    return result.errorMessage().find(".app") != std::string::npos &&
           result.errorMessage().find("bundle") != std::string::npos;
#else
    (void)result;
    return true;
#endif
}

} // namespace

REGISTER_TEST(notification_send_forwards_fields,
              "notification: send 原样转发字段并透传成功", {
    BackendResetGuard guard;
    FakeBackend fake;
    platform::notify::setBackendForTest(&fake);

    Notification notification;
    notification.id = "task-42";
    notification.title = "任务完成";
    notification.body = "今天的备份已写入";
    notification.sound = false;

    const auto result = platform::notify::send(notification);
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK_EQ(fake.sendCalls, 1);
    DOCTEST_CHECK_EQ(fake.lastNotification.id, std::string("task-42"));
    DOCTEST_CHECK_EQ(fake.lastNotification.title, std::string("任务完成"));
    DOCTEST_CHECK_EQ(fake.lastNotification.body, std::string("今天的备份已写入"));
    DOCTEST_CHECK_EQ(fake.lastNotification.sound, false);
    return true;
})

REGISTER_TEST(notification_send_passthrough_failure,
              "notification: send 失败码与消息原样透传", {
    BackendResetGuard guard;
    FakeBackend fake;
    fake.sendResult = core::Result<void>::fail(core::ErrorCode::HttpError,
                                               "后端明确失败");
    platform::notify::setBackendForTest(&fake);

    const auto result = platform::notify::send(Notification{"", "标题", "正文", true});
    DOCTEST_REQUIRE(result.isFail());
    DOCTEST_CHECK_EQ(static_cast<int>(result.errorCode()),
                     static_cast<int>(core::ErrorCode::HttpError));
    DOCTEST_CHECK_EQ(result.errorMessage(), std::string("后端明确失败"));
    DOCTEST_CHECK_EQ(fake.sendCalls, 1);
    return true;
})

REGISTER_TEST(notification_authorization_forwards,
              "notification: authorizationStatus 透传、requestAuthorization 调用一次", {
    BackendResetGuard guard;
    FakeBackend fake;
    fake.status = Authorization::Denied;
    platform::notify::setBackendForTest(&fake);

    DOCTEST_CHECK(platform::notify::authorizationStatus() == Authorization::Denied);
    DOCTEST_CHECK_EQ(fake.authorizationRequests, 0);

    platform::notify::requestAuthorization();
    DOCTEST_CHECK_EQ(fake.authorizationRequests, 1);
    return true;
})

REGISTER_TEST(notification_empty_title_or_body_rejected,
              "notification: 空 title/body 失败且不调用后端", {
    BackendResetGuard guard;
    FakeBackend fake;
    platform::notify::setBackendForTest(&fake);

    const auto emptyTitle = platform::notify::send(Notification{"", "", "正文", true});
    DOCTEST_CHECK(emptyTitle.isFail());
    DOCTEST_CHECK_EQ(static_cast<int>(emptyTitle.errorCode()),
                     static_cast<int>(core::ErrorCode::ValidationError));
    DOCTEST_CHECK(emptyTitle.errorMessage().find("title") != std::string::npos);

    const auto emptyBody = platform::notify::send(Notification{"", "标题", "", true});
    DOCTEST_CHECK(emptyBody.isFail());
    DOCTEST_CHECK_EQ(static_cast<int>(emptyBody.errorCode()),
                     static_cast<int>(core::ErrorCode::ValidationError));
    DOCTEST_CHECK(emptyBody.errorMessage().find("body") != std::string::npos);

    DOCTEST_CHECK_EQ(fake.sendCalls, 0);
    return true;
})

REGISTER_TEST(notification_default_backend_in_bare_process,
              "notification: 裸进程默认后端失败但不崩溃", {
    // devdesk_tests 不是 .app bundle：macOS 必须走 bundle guard（错误信息点名
    // .app / bundle），Windows/其它 POSIX 必须走 stub。三条路径都不许抛异常。
    platform::notify::resetBackendForTest();

    const auto result = platform::notify::send(Notification{"", "标题", "正文", true});
    DOCTEST_CHECK(result.isFail());
    DOCTEST_CHECK(bareProcessFailureMentionsBundle(result));

    DOCTEST_CHECK(platform::notify::authorizationStatus() ==
                  Authorization::NotDetermined);
    platform::notify::requestAuthorization();   // 只验证不崩（guard / stub 空实现）
    return true;
})

REGISTER_TEST(notification_backend_seam_restores_default,
              "notification: 测试缝注入与恢复默认后端不悬垂", {
    BackendResetGuard guard;
    DOCTEST_REQUIRE(&platform::notify::backend() == &platform::notify::defaultBackend());

    FakeBackend first;
    Backend* previous = platform::notify::setBackendForTest(&first);
    DOCTEST_CHECK(previous == &platform::notify::defaultBackend());
    DOCTEST_CHECK(&platform::notify::backend() == &first);

    // 传 nullptr = 恢复默认，并把旧后端（栈上 fake）交还调用方。
    Backend* replaced = platform::notify::setBackendForTest(nullptr);
    DOCTEST_CHECK(replaced == &first);
    DOCTEST_CHECK(&platform::notify::backend() == &platform::notify::defaultBackend());

    // 再注入后走 reset 恢复；恢复后的默认后端仍可安全调用（不会访问已销毁的 fake）。
    platform::notify::setBackendForTest(&first);
    platform::notify::resetBackendForTest();
    DOCTEST_CHECK(&platform::notify::backend() == &platform::notify::defaultBackend());
    (void)platform::notify::send(Notification{"", "标题", "正文", true});
    return true;
})
