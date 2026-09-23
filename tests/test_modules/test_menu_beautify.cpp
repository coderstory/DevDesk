// tests/test_modules/test_menu_beautify.cpp
// 菜单美化（2026-09-23）结构测试（headless 断言，真机圆角/主题/图标属视觉点验）：
//   1. 托盘菜单 spec：项文本/顺序/分隔线/图标码点/动作路由（styled 与原生
//      降级共用的唯一数据源）
//   2. 桌宠右键菜单结构 petContextMenuItems：穿透/换肤子组（皮肤有无两态）/
//      重置/隐藏/分隔线/退出的顺序与 path 语义基础
//   3. 选择数学 petMenuActionForPath：path → 语义动作（含换肤子组 {1,j}、
//      下标随子组有无偏移、组头/分隔线/越界 = None）
//   4. 退出确认逻辑保持（审计 [25]）：确认项结构与 petExitConfirmChosen
//   5. styledMenu 组件纯函数：flatten（行序/path 对齐原生 contextMenuCommandPaths
//      语义）、panelHeight、clampPosition（越界钳制/上翻/无头兜底）
//   6. showStyledMenu 参数守卫：空项/无可选行 → false（调用方降级原生）
// REGISTER_TEST 宏体内不能有裸逗号（预处理切参），容器初始化一律走 push_back。
#include "doctest.h"

#include "core/tray_menu_spec.h"
#include "module_pet/pet_window.h"

#include <components/styledmenu.h>

#include <string>
#include <vector>

// ---- 1. 托盘菜单 spec ----

REGISTER_TEST(menuBeautifyTraySpecStructure, "menu: 托盘 spec 项序/分隔线/图标/动作", {
    const auto spec = core::tray::trayMenuSpec();
    DOCTEST_REQUIRE_EQ(spec.size(), static_cast<size_t>(8));
    DOCTEST_CHECK_EQ(spec[0].text, std::string("显示主窗口"));
    DOCTEST_CHECK(spec[0].action == core::tray::TrayMenuAction::ShowMainWindow);
    DOCTEST_CHECK_EQ(spec[1].text, std::string("-"));
    DOCTEST_CHECK_EQ(spec[2].text, std::string("首页"));
    DOCTEST_CHECK(spec[2].action == core::tray::TrayMenuAction::NavigateHome);
    DOCTEST_CHECK_EQ(spec[3].text, std::string("笔记"));
    DOCTEST_CHECK(spec[3].action == core::tray::TrayMenuAction::NavigateMarkdown);
    DOCTEST_CHECK_EQ(spec[4].text, std::string("待办"));
    DOCTEST_CHECK(spec[4].action == core::tray::TrayMenuAction::NavigateTodo);
    DOCTEST_CHECK_EQ(spec[5].text, std::string("Claude"));
    DOCTEST_CHECK(spec[5].action == core::tray::TrayMenuAction::NavigateClaude);
    DOCTEST_CHECK_EQ(spec[6].text, std::string("-"));
    DOCTEST_CHECK_EQ(spec[7].text, std::string("退出 DevDesk"));
    DOCTEST_CHECK(spec[7].action == core::tray::TrayMenuAction::Exit);
    // 可选中项必须有 FA 图标；分隔线无动作
    for (const auto& item : spec) {
        if (item.text == "-") {
            DOCTEST_CHECK(item.action == core::tray::TrayMenuAction::None);
            DOCTEST_CHECK_EQ(item.iconCodepoint, 0);
        } else {
            DOCTEST_CHECK(item.iconCodepoint != 0);
            DOCTEST_CHECK(item.action != core::tray::TrayMenuAction::None);
        }
    }
    return true;
});

// ---- 2. 桌宠右键菜单结构 ----

REGISTER_TEST(menuBeautifyPetMenuItemsWithSkins, "menu: 桌宠菜单含换肤子组（6 顶层项）", {
    std::vector<std::string> skins;
    skins.push_back("duck");
    skins.push_back("slime");
    skins.push_back("robot");
    const auto items = module::pet::petContextMenuItems(false, skins);
    DOCTEST_REQUIRE_EQ(items.size(), static_cast<size_t>(6));
    DOCTEST_CHECK(items[0].text.find("挂机穿透") != std::string::npos);
    DOCTEST_CHECK(items[0].iconCodepoint != 0);
    DOCTEST_CHECK_EQ(items[1].text, std::string("换肤"));
    DOCTEST_REQUIRE_EQ(items[1].children.size(), static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(items[1].children[2].text, std::string("robot"));
    DOCTEST_CHECK(items[2].text.find("位置重置") != std::string::npos);
    DOCTEST_CHECK(items[3].text.find("隐藏桌宠") != std::string::npos);
    DOCTEST_CHECK_EQ(items[4].text, std::string("-"));
    DOCTEST_CHECK_EQ(items[5].text, std::string("退出 DevDesk"));
    return true;
});

REGISTER_TEST(menuBeautifyPetMenuItemsWithoutSkins, "menu: 桌宠菜单无皮肤时 5 顶层项（穿透文案随态）", {
    const auto items = module::pet::petContextMenuItems(true, std::vector<std::string>());
    DOCTEST_REQUIRE_EQ(items.size(), static_cast<size_t>(5));
    DOCTEST_CHECK_EQ(items[0].text, std::string("关闭鼠标穿透"));
    DOCTEST_CHECK(items[1].text.find("位置重置") != std::string::npos);
    DOCTEST_CHECK(items[2].text.find("隐藏桌宠") != std::string::npos);
    DOCTEST_CHECK_EQ(items[3].text, std::string("-"));
    DOCTEST_CHECK_EQ(items[4].text, std::string("退出 DevDesk"));
    return true;
});

// ---- 3. 选择数学（styled 与原生降级共用）----

REGISTER_TEST(menuBeautifyPetActionPathsWithSkins, "menu: path→动作（有换肤子组，下标偏移）", {
    int skinIndex = -1;
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{0}, 3, skinIndex) ==
                  module::pet::PetMenuAction::ToggleThrough);
    // 组头本身不可选
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{1}, 3, skinIndex) ==
                  module::pet::PetMenuAction::None);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{1, 2}, 3, skinIndex) ==
                  module::pet::PetMenuAction::Skin);
    DOCTEST_CHECK_EQ(skinIndex, 2);
    // 子项越界
    skinIndex = -1;
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{1, 3}, 3, skinIndex) ==
                  module::pet::PetMenuAction::None);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{2}, 3, skinIndex) ==
                  module::pet::PetMenuAction::ResetPosition);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{3}, 3, skinIndex) ==
                  module::pet::PetMenuAction::Hide);
    // 分隔线位不可动作
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{4}, 3, skinIndex) ==
                  module::pet::PetMenuAction::None);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{5}, 3, skinIndex) ==
                  module::pet::PetMenuAction::Exit);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>(), 3, skinIndex) ==
                  module::pet::PetMenuAction::None);
    // 非换肤组的二层路径
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{0, 1}, 3, skinIndex) ==
                  module::pet::PetMenuAction::None);
    return true;
});

REGISTER_TEST(menuBeautifyPetActionPathsWithoutSkins, "menu: path→动作（无换肤子组，下标回移）", {
    int skinIndex = -1;
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{0}, 0, skinIndex) ==
                  module::pet::PetMenuAction::ToggleThrough);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{1}, 0, skinIndex) ==
                  module::pet::PetMenuAction::ResetPosition);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{2}, 0, skinIndex) ==
                  module::pet::PetMenuAction::Hide);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{3}, 0, skinIndex) ==
                  module::pet::PetMenuAction::None);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{4}, 0, skinIndex) ==
                  module::pet::PetMenuAction::Exit);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{9}, 0, skinIndex) ==
                  module::pet::PetMenuAction::None);
    return true;
});

// ---- 4. 退出确认（审计 [25]）结构保持 ----

REGISTER_TEST(menuBeautifyPetExitConfirmKept, "menu: 退出二段确认结构与仅确认退语义保持", {
    const auto confirm = module::pet::petExitConfirmItems();
    DOCTEST_REQUIRE_EQ(confirm.size(), static_cast<size_t>(4));
    DOCTEST_CHECK(confirm[0].disabled);
    DOCTEST_CHECK_EQ(confirm[1].text, std::string("-"));
    DOCTEST_CHECK_EQ(confirm[2].text, std::string("确认退出"));
    DOCTEST_CHECK_EQ(confirm[3].text, std::string("取消"));
    DOCTEST_CHECK(module::pet::petExitConfirmChosen(std::vector<int>{2}));
    DOCTEST_CHECK(!module::pet::petExitConfirmChosen(std::vector<int>{3}));
    DOCTEST_CHECK(!module::pet::petExitConfirmChosen(std::vector<int>()));
    return true;
});

// ---- 5. styledMenu 组件纯函数 ----

static std::vector<components::StyledMenuItem> menuBeautifySampleMenu() {
    std::vector<components::StyledMenuItem> out;
    components::StyledMenuItem through;
    through.text = "穿透";
    through.iconCodepoint = 0xF245;
    out.push_back(std::move(through));
    components::StyledMenuItem skinGroup;
    skinGroup.text = "换肤";
    skinGroup.iconCodepoint = 0xF53F;
    components::StyledMenuItem skinA;
    skinA.text = "duck";
    skinGroup.children.push_back(std::move(skinA));
    out.push_back(std::move(skinGroup));
    components::StyledMenuItem sep;
    sep.text = "-";
    out.push_back(std::move(sep));
    components::StyledMenuItem locked;
    locked.text = "锁定项";
    locked.disabled = true;
    out.push_back(std::move(locked));
    components::StyledMenuItem current;
    current.text = "当前项";
    current.checked = true;
    out.push_back(std::move(current));
    return out;
}

REGISTER_TEST(menuBeautifyStyledFlattenPaths, "menu: styled flatten 行序/path/可选中数", {
    const auto rows = components::styledMenuFlatten(menuBeautifySampleMenu());
    // 换肤子组展平为组头 + 缩进子行：5 个顶层项 → 6 行
    DOCTEST_REQUIRE_EQ(rows.size(), static_cast<size_t>(6));
    DOCTEST_CHECK(rows[0].kind == components::StyledMenuRowKind::Item);
    DOCTEST_CHECK((rows[0].path == std::vector<int>{0}));
    DOCTEST_CHECK_EQ(rows[0].iconCodepoint, 0xF245);
    DOCTEST_CHECK(rows[1].kind == components::StyledMenuRowKind::GroupHeader);
    DOCTEST_CHECK((rows[1].path == std::vector<int>{1}));
    DOCTEST_CHECK(rows[2].kind == components::StyledMenuRowKind::Item);
    DOCTEST_CHECK((rows[2].path == std::vector<int>{1, 0}));
    DOCTEST_CHECK_EQ(rows[2].depth, 1);
    DOCTEST_CHECK(rows[3].kind == components::StyledMenuRowKind::Separator);
    DOCTEST_CHECK(rows[4].disabled);
    DOCTEST_CHECK(rows[5].checked);
    // 可选中数：穿透/duck/当前项（组头/分隔线/disabled 不占）
    DOCTEST_CHECK_EQ(components::styledMenuSelectableCount(rows), static_cast<size_t>(3));
    return true;
});

REGISTER_TEST(menuBeautifyStyledPanelHeight, "menu: styled 面板高度行数和+分隔线+inset", {
    const auto rows = components::styledMenuFlatten(menuBeautifySampleMenu());
    components::StyledMenuStyle style;
    style.rowHeight = 30.0f;
    style.separatorHeight = 8.0f;
    style.inset = 5.0f;
    // 5 个普通行 + 1 分隔线 + 上下 inset
    const float expected = 30.0f * 5 + 8.0f + 10.0f;
    DOCTEST_CHECK_EQ(components::styledMenuPanelHeight(rows, style), expected);
    return true;
});

REGISTER_TEST(menuBeautifyStyledClampPosition, "menu: styled 位置钳制（贴右/上翻/贴顶/副屏/无头）", {
    using components::styledMenuClampPosition;
    // 界内原样
    auto r = styledMenuClampPosition(100.0f, 100.0f, 220.0f, 200.0f, 0.0f, 0.0f, 1920.0f, 1080.0f);
    DOCTEST_CHECK_EQ(r.x, 100.0f);
    DOCTEST_CHECK_EQ(r.y, 100.0f);
    // 越右 → 贴右缘
    r = styledMenuClampPosition(1900.0f, 100.0f, 220.0f, 200.0f, 0.0f, 0.0f, 1920.0f, 1080.0f);
    DOCTEST_CHECK_EQ(r.x, 1700.0f);
    // 越下 → 向上翻转（贴光标上沿展开）
    r = styledMenuClampPosition(100.0f, 1000.0f, 220.0f, 200.0f, 0.0f, 0.0f, 1920.0f, 1080.0f);
    DOCTEST_CHECK_EQ(r.y, 800.0f);
    // 翻转后仍越上界 → 贴顶
    r = styledMenuClampPosition(100.0f, 50.0f, 220.0f, 400.0f, 0.0f, 0.0f, 1920.0f, 300.0f);
    DOCTEST_CHECK_EQ(r.y, 0.0f);
    // 副屏负坐标工作区（左屏 x=-1920）：界内原样 + 越界贴副屏右缘（=0-220）
    r = styledMenuClampPosition(-1900.0f, 100.0f, 220.0f, 200.0f, -1920.0f, 0.0f, 1920.0f, 1080.0f);
    DOCTEST_CHECK_EQ(r.x, -1900.0f);
    r = styledMenuClampPosition(-100.0f, 100.0f, 220.0f, 200.0f, -1920.0f, 0.0f, 1920.0f, 1080.0f);
    DOCTEST_CHECK_EQ(r.x, -220.0f);
    // 无工作区信息（无头兜底）→ 原样
    r = styledMenuClampPosition(9999.0f, 9999.0f, 220.0f, 200.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    DOCTEST_CHECK_EQ(r.x, 9999.0f);
    return true;
});

// ---- 6. showStyledMenu 参数守卫（无头不触真开窗）----

REGISTER_TEST(menuBeautifyStyledShowParamGuard, "menu: styled 空项/无可选行 → false（降级信号）", {
    components::StyledMenuStyle style;
    int selected = -1;
    const auto cb = [&selected](const std::vector<int>&) { selected = 1; };
    // 空项 → false（调用方应降级原生）
    DOCTEST_CHECK(!components::showStyledMenu("t", std::vector<components::StyledMenuItem>(),
                                              style, 10.0f, 10.0f, cb));
    // 只有分隔线 → false
    std::vector<components::StyledMenuItem> onlySep;
    components::StyledMenuItem sep;
    sep.text = "-";
    onlySep.push_back(std::move(sep));
    DOCTEST_CHECK(!components::showStyledMenu("t", onlySep, style, 10.0f, 10.0f, cb));
    // 全 disabled → false
    std::vector<components::StyledMenuItem> allDisabled;
    components::StyledMenuItem locked;
    locked.text = "锁";
    locked.disabled = true;
    allDisabled.push_back(std::move(locked));
    DOCTEST_CHECK(!components::showStyledMenu("t", allDisabled, style, 10.0f, 10.0f, cb));
    DOCTEST_CHECK_EQ(selected, -1);
    return true;
});
