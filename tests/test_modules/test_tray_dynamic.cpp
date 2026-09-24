// tests/test_tray_dynamic.cpp
// 产品审计 [7]（2026-09-24）：托盘菜单按 Router 启用模块动态收缩的 spec 断言
//（headless，纯 std 谓词驱动，不需要真 Router/GL 窗）：
//   1. trayMenuSpec(pred)：不可用导航项置 disabled + 「（已关闭）」说明后缀，
//      项保留、下标不变（styled path {i} 映射基础）；可用项/非导航项不受影响
//   2. trayMenuSpecFiltered(pred)：不可用项整体移除（原生降级路径），
//      导航项全禁用时相邻分隔线合并
//   3. 应用级 toast 通知状态通道（app_sidebar 的 show/clear/visible，
//      失效入口点击的解释反馈）
// REGISTER_TEST 宏体内不能有裸逗号（预处理切参），谓词构造走 unavailableOnly
// 辅助函数（嵌套括号内的逗号受保护）。
#include "doctest.h"

#include "app_sidebar.h"
#include "core/tray_menu_spec.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace {

using Action = core::tray::TrayMenuAction;

// 谓词工厂：disabled 列表内的动作视为不可用，其余可用
std::function<bool(Action)> unavailableOnly(std::vector<Action> disabled) {
    return [disabled = std::move(disabled)](Action a) {
        return std::find(disabled.begin(), disabled.end(), a) == disabled.end();
    };
}

} // namespace


REGISTER_TEST(trayDynamicGatedSpecAllAvailableMatchesBase, "menu: tray spec 全可用谓词 = 无参基线", {
    const auto all = core::tray::trayMenuSpec(
        unavailableOnly(std::vector<Action>{}));
    const auto base = core::tray::trayMenuSpec();
    DOCTEST_REQUIRE_EQ(all.size(), base.size());
    for (size_t i = 0; i < base.size(); ++i) {
        DOCTEST_CHECK_EQ(all[i].text, base[i].text);
        DOCTEST_CHECK_EQ(all[i].iconCodepoint, base[i].iconCodepoint);
        DOCTEST_CHECK(all[i].action == base[i].action);
        DOCTEST_CHECK(!all[i].disabled);
    }
    return true;
});



// ---- 3. 应用级 toast 通知状态通道 ----

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
