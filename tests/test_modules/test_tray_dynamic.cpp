// tests/test_tray_dynamic.cpp
// 应用级 toast 通知通道：show / clear / visible 的单实例覆盖语义。
#include "doctest.h"

#include "app_sidebar.h"
#include "core/tray_menu_spec.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

REGISTER_TEST(trayDynamicAppNoticeStateRoundtrip, "menu: tray 应用级 toast 通知状态读写", {
    DOCTEST_CHECK(!app::appNoticeVisible());
    app::showAppNotice("模块已关闭", "「待办」未启用");
    DOCTEST_CHECK(app::appNoticeVisible());
    DOCTEST_CHECK_EQ(app::appNoticeTitle(), std::string("模块已关闭"));
    DOCTEST_CHECK_EQ(app::appNoticeMessage(), std::string("「待办」未启用"));
    app::showAppNotice("第二条", "内容");
    DOCTEST_CHECK_EQ(app::appNoticeTitle(), std::string("第二条"));
    app::clearAppNotice();
    DOCTEST_CHECK(!app::appNoticeVisible());
    DOCTEST_CHECK(app::appNoticeTitle().empty());
    return true;
});
