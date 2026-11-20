// tests/test_platform/test_file_trash.cpp
// platform::file_trash 覆盖：
//   ① 签名守卫（static_assert）：三平台实现必须同签名，漂了即编不过。
//   ② 非 Win/macOS（POSIX 兜底）平台：必经证实降级，恒返回 TrashUnavailable——
//      不直删是产品前提（误删不可回收），本用例锁住「兜底不偷偷直删」。
//   ③ Win/macOS：只测「不存在路径」这一条无副作用路径（不触碰真实废纸篓）；
//      真机移废纸篓验证由主会话另派的 probe 承担。
#include "doctest.h"

#include "platform/file_trash.h"

#include <type_traits>
#include <utility>

namespace {

// 与头文件内的守卫同口径再钉一次：用例 TU 也独立捕获签名漂移
static_assert(
    std::is_same_v<decltype(&platform::file_trash::moveToTrash),
                   platform::file_trash::TrashOutcome (*)(const std::string&)>,
    "file_trash: moveToTrash 签名漂移");

}  // namespace

#if !defined(_WIN32) && !defined(__APPLE__)

REGISTER_TEST(file_trash_posix_reports_unavailable,
              "file_trash: 非 Win/macOS 平台诚实返回 TrashUnavailable，不直删", {
    // 连存在的路径也不能例外：兜底实现不看文件系统，一律降级
    const auto outcome = platform::file_trash::moveToTrash("/tmp/devdesk_file_trash_probe");
    DOCTEST_CHECK_FALSE(outcome.ok());
    DOCTEST_CHECK(outcome.failure ==
                  platform::file_trash::TrashFailure::TrashUnavailable);
    return true;
})

#else

REGISTER_TEST(file_trash_missing_path_reports_failure,
              "file_trash: 不存在路径返回失败档位（不触碰废纸篓，无副作用）", {
    // 用确定的绝对不存在路径：调用只做存在性判定，绝不写系统废纸篓
    const auto outcome = platform::file_trash::moveToTrash(
        "/devdesk-nonexistent-dir-9e31/devdesk_file_trash_absent.mp4");
    DOCTEST_CHECK_FALSE(outcome.ok());
    DOCTEST_CHECK(outcome.failure != platform::file_trash::TrashFailure::None);
    return true;
})

#endif

REGISTER_TEST(file_trash_outcome_ok_only_on_none,
              "file_trash: TrashOutcome::ok() 仅 None 为真（五档语义互斥）", {
    using platform::file_trash::TrashFailure;
    using platform::file_trash::TrashOutcome;
    DOCTEST_CHECK(TrashOutcome{TrashFailure::None}.ok());
    const TrashFailure failures[] = {TrashFailure::NotFound,
                                     TrashFailure::PermissionDenied,
                                     TrashFailure::TrashUnavailable,
                                     TrashFailure::Busy};
    for (const auto failure : failures) {
        const TrashOutcome outcome{failure};
        DOCTEST_CHECK_FALSE(outcome.ok());
        DOCTEST_CHECK(outcome.failure == failure);
    }
    return true;
})
