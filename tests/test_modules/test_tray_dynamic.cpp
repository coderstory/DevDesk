// tests/test_tray_dynamic.cpp
// 托盘收缩链已删（2026-09-27），本文件现只守 toast 通道：
//   1. 应用级 toast 通知状态通道（app_sidebar 的 show/clear/visible，
//      失效入口点击的解释反馈）
#include "doctest.h"

#include "app_sidebar.h"
#include "core/tray_menu_spec.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>


// ---- 1. 应用级 toast 通知状态通道 ----

REGISTER_TEST(trayDynamicAppNoticeStateRoundtrip, "menu: tray 应用级 toast 通知状态读写", {
    DOCTEST_CHECK(!app::appNoticeVisible());
    app::showAppNotice("模块已关闭", "「待办」未启用");
    DOCTEST_CHECK(app::appNoticeVisible());
    DOCTEST_CHECK_EQ(app::appNoticeTitle(), std::string("模块已关闭"));
    DOCTEST_CHECK_EQ(app::appNoticeMessage(), std::string("「待办」未启用"));
    // 覆盖式（单实例）：再次 show 替换内容
    app::showAppNotice("第二条", "内容");
    DOCTEST_CHECK_EQ(app::appNoticeTitle(), std::string("第二条"));
    app::clearAppNotice();
    DOCTEST_CHECK(!app::appNoticeVisible());
    DOCTEST_CHECK(app::appNoticeTitle().empty());
    return true;
});
