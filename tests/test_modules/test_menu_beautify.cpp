// tests/test_modules/test_menu_beautify.cpp
// 菜单结构测试（headless，真机圆角/主题/图标属视觉点验）：托盘 spec、
// path→动作选择数学、退出二段确认、styledMenu 纯函数、showStyledMenu 降级守卫。
#include "doctest.h"

#include "core/tray_menu_spec.h"

#include <components/styledmenu.h>

#include <set>
#include <string>
#include <vector>

REGISTER_TEST(menuBeautifyTraySpecStructure,
              "menu: 托盘 spec 项序/图标/动作（2026-09-24 精简为打开+退出）", {
    const auto spec = core::tray::trayMenuSpec();
    DOCTEST_REQUIRE_EQ(spec.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(spec[0].text, std::string("显示主窗口"));
    DOCTEST_CHECK(spec[0].action == core::tray::TrayMenuAction::ShowMainWindow);
    DOCTEST_CHECK_EQ(spec[1].text, std::string("退出 DevDesk"));
    DOCTEST_CHECK(spec[1].action == core::tray::TrayMenuAction::Exit);
    for (const auto& item : spec) {
        DOCTEST_CHECK(item.iconCodepoint != 0);
        DOCTEST_CHECK(item.action != core::tray::TrayMenuAction::None);
    }
    return true;
});

REGISTER_TEST(menuBeautifyPetMenuItemsWithSkins, "menu: 桌宠菜单含换肤子组（5 顶层项）", {
    const std::vector<std::string> skins = {"duck", "slime", "robot"};
    const auto items = module::pet::petContextMenuItems(skins);
    DOCTEST_REQUIRE_EQ(items.size(), static_cast<size_t>(5));
    DOCTEST_CHECK_EQ(items[0].text, std::string("换肤"));
    DOCTEST_REQUIRE_EQ(items[0].children.size(), static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(items[0].children[2].text, std::string("robot"));
    DOCTEST_CHECK(items[1].text.find("位置重置") != std::string::npos);
    DOCTEST_CHECK(items[2].text.find("隐藏桌宠") != std::string::npos);
    DOCTEST_CHECK_EQ(items[3].text, std::string("-"));
    DOCTEST_CHECK_EQ(items[4].text, std::string("退出 DevDesk"));
    return true;
});

REGISTER_TEST(menuBeautifyPetMenuItemsWithoutSkins, "menu: 桌宠菜单无皮肤时 4 顶层项", {
    const auto items = module::pet::petContextMenuItems(std::vector<std::string>());
    DOCTEST_REQUIRE_EQ(items.size(), static_cast<size_t>(4));
    DOCTEST_CHECK(items[0].text.find("位置重置") != std::string::npos);
    DOCTEST_CHECK(items[1].text.find("隐藏桌宠") != std::string::npos);
    DOCTEST_CHECK_EQ(items[2].text, std::string("-"));
    DOCTEST_CHECK_EQ(items[3].text, std::string("退出 DevDesk"));
    return true;
});

// path 下标随换肤子组有无整体偏移；分隔线也占一个下标，故退出项在「隐藏」之后隔 2 位
REGISTER_TEST(menuBeautifyPetActionPathsWithSkins, "menu: path→动作（有换肤子组，下标偏移）", {
    int skinIndex = -1;
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{0}, 3, skinIndex) ==
                  module::pet::PetMenuAction::None);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{0, 2}, 3, skinIndex) ==
                  module::pet::PetMenuAction::Skin);
    DOCTEST_CHECK_EQ(skinIndex, 2);
    skinIndex = -1;
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{0, 3}, 3, skinIndex) ==
                  module::pet::PetMenuAction::None);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{1}, 3, skinIndex) ==
                  module::pet::PetMenuAction::ResetPosition);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{2}, 3, skinIndex) ==
                  module::pet::PetMenuAction::Hide);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{3}, 3, skinIndex) ==
                  module::pet::PetMenuAction::None);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{4}, 3, skinIndex) ==
                  module::pet::PetMenuAction::Exit);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>(), 3, skinIndex) ==
                  module::pet::PetMenuAction::None);
    // 只有换肤组接受二层路径，Hide 等顶层项没有子项
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{1, 0}, 3, skinIndex) ==
                  module::pet::PetMenuAction::None);
    return true;
});

REGISTER_TEST(menuBeautifyPetActionPathsWithoutSkins, "menu: path→动作（无换肤子组，下标回移）", {
    int skinIndex = -1;
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{0}, 0, skinIndex) ==
                  module::pet::PetMenuAction::ResetPosition);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{1}, 0, skinIndex) ==
                  module::pet::PetMenuAction::Hide);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{2}, 0, skinIndex) ==
                  module::pet::PetMenuAction::None);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{3}, 0, skinIndex) ==
                  module::pet::PetMenuAction::Exit);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{9}, 0, skinIndex) ==
                  module::pet::PetMenuAction::None);
    return true;
});

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
    // 可选中数只数可选中的普通行：组头 / 分隔线 / disabled 都不占
    DOCTEST_CHECK_EQ(components::styledMenuSelectableCount(rows), static_cast<size_t>(3));
    return true;
});

static std::vector<components::StyledMenuItem> menuBeautifyFoldableSkinMenu() {
    std::vector<components::StyledMenuItem> out;
    components::StyledMenuItem skinGroup;
    skinGroup.text = "换肤";
    skinGroup.iconCodepoint = 0xF53F;
    skinGroup.collapsedByDefault = true;
    components::StyledMenuItem skinA;
    skinA.text = "robot";
    skinGroup.children.push_back(std::move(skinA));
    components::StyledMenuItem skinB;
    skinB.text = "ghost";
    skinGroup.children.push_back(std::move(skinB));
    out.push_back(std::move(skinGroup));
    return out;
}

REGISTER_TEST(menuBeautifyStyledFlattenCollapsedGroup, "menu: styled 折叠组默认只出组头（opt-in）", {
    const auto rows = components::styledMenuFlatten(menuBeautifyFoldableSkinMenu());
    // 折叠态：子行不列出，只剩组头 1 行
    DOCTEST_REQUIRE_EQ(rows.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(rows[0].kind == components::StyledMenuRowKind::GroupHeader);
    DOCTEST_CHECK(rows[0].collapsible);
    DOCTEST_CHECK((rows[0].path == std::vector<int>{0}));
    DOCTEST_CHECK_EQ(rows[0].text, std::string("换肤"));
    return true;
});

REGISTER_TEST(menuBeautifyStyledFlattenExpandedGroup, "menu: styled 展开组出组头+缩进子行", {
    std::set<std::vector<int>> expanded;
    expanded.insert(std::vector<int>{0});
    const auto rows = components::styledMenuFlatten(menuBeautifyFoldableSkinMenu(), expanded);
    DOCTEST_REQUIRE_EQ(rows.size(), static_cast<size_t>(3));
    DOCTEST_CHECK(rows[0].kind == components::StyledMenuRowKind::GroupHeader);
    DOCTEST_CHECK(rows[0].collapsible);
    DOCTEST_CHECK(rows[1].kind == components::StyledMenuRowKind::Item);
    DOCTEST_CHECK((rows[1].path == std::vector<int>{0, 0}));
    DOCTEST_CHECK_EQ(rows[1].depth, 1);
    DOCTEST_CHECK(rows[2].kind == components::StyledMenuRowKind::Item);
    DOCTEST_CHECK((rows[2].path == std::vector<int>{0, 1}));
    return true;
});

REGISTER_TEST(menuBeautifyStyledFullExpandedCount, "menu: styled 全展开预留行数（高度预留基础）", {
    // 忽略 collapsedByDefault 的全展开行数：开窗高度按它预留
    const auto rows = components::styledMenuFlattenAllExpanded(menuBeautifyFoldableSkinMenu());
    DOCTEST_REQUIRE_EQ(rows.size(), static_cast<size_t>(3));
    DOCTEST_CHECK(rows[0].kind == components::StyledMenuRowKind::GroupHeader);
    DOCTEST_CHECK(rows[1].kind == components::StyledMenuRowKind::Item);
    DOCTEST_CHECK(rows[2].kind == components::StyledMenuRowKind::Item);
    return true;
});

REGISTER_TEST(menuBeautifyStyledFoldedPetMenuSelectable, "menu: 折叠态桌宠式菜单仍可选（不降级原生）", {
    std::vector<components::StyledMenuItem> items = menuBeautifyFoldableSkinMenu();
    items.push_back(components::StyledMenuItem("位置重置"));
    items.push_back(components::StyledMenuItem("隐藏桌宠"));
    components::StyledMenuItem sep;
    sep.text = "-";
    items.push_back(std::move(sep));
    items.push_back(components::StyledMenuItem("退出 DevDesk"));
    const auto rows = components::styledMenuFlatten(items);
    DOCTEST_REQUIRE_EQ(rows.size(), static_cast<size_t>(5));
    // 折叠组头不占可选行，但只要还剩顶层可选行就 >0，不触发降级
    DOCTEST_CHECK(components::styledMenuSelectableCount(rows) > static_cast<size_t>(0));
    DOCTEST_CHECK_EQ(components::styledMenuSelectableCount(rows), static_cast<size_t>(3));
    return true;
});

REGISTER_TEST(menuBeautifyPetActionPathFoldIndependent, "menu: path→动作与折叠态无关（组头None/子项Skin）", {
    // path 基于原始 items 向量下标，与运行期折叠/展开态无关：无论 flatten 是否列出子行，
    // 组头 {0} 都映射 None、子项 {0,j} 都映射 Skin。
    int skinIndex = -1;
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{0}, 2, skinIndex) ==
                  module::pet::PetMenuAction::None);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{0, 0}, 2, skinIndex) ==
                  module::pet::PetMenuAction::Skin);
    DOCTEST_CHECK_EQ(skinIndex, 0);
    DOCTEST_CHECK(module::pet::petMenuActionForPath(std::vector<int>{0, 1}, 2, skinIndex) ==
                  module::pet::PetMenuAction::Skin);
    DOCTEST_CHECK_EQ(skinIndex, 1);
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
    auto r = styledMenuClampPosition(100.0f, 100.0f, 220.0f, 200.0f, 0.0f, 0.0f, 1920.0f, 1080.0f);
    DOCTEST_CHECK_EQ(r.x, 100.0f);
    DOCTEST_CHECK_EQ(r.y, 100.0f);
    r = styledMenuClampPosition(1900.0f, 100.0f, 220.0f, 200.0f, 0.0f, 0.0f, 1920.0f, 1080.0f);
    DOCTEST_CHECK_EQ(r.x, 1700.0f);
    // 越下 → 向上翻转（贴光标上沿）；翻转后仍越下界才二次收敛到贴顶，
    // 两次判断都在，别把第二条当冗余删
    r = styledMenuClampPosition(100.0f, 1000.0f, 220.0f, 200.0f, 0.0f, 0.0f, 1920.0f, 1080.0f);
    DOCTEST_CHECK_EQ(r.y, 800.0f);
    r = styledMenuClampPosition(100.0f, 50.0f, 220.0f, 400.0f, 0.0f, 0.0f, 1920.0f, 300.0f);
    DOCTEST_CHECK_EQ(r.y, 0.0f);
    // 副屏可能在左，areaX 为负：钳制是相对 areaX 而非 0
    r = styledMenuClampPosition(-1900.0f, 100.0f, 220.0f, 200.0f, -1920.0f, 0.0f, 1920.0f, 1080.0f);
    DOCTEST_CHECK_EQ(r.x, -1900.0f);
    r = styledMenuClampPosition(-100.0f, 100.0f, 220.0f, 200.0f, -1920.0f, 0.0f, 1920.0f, 1080.0f);
    DOCTEST_CHECK_EQ(r.x, -220.0f);
    // 工作区 w/h<=0（无头/查询失败）时整段钳制跳过，原样返回
    r = styledMenuClampPosition(9999.0f, 9999.0f, 220.0f, 200.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    DOCTEST_CHECK_EQ(r.x, 9999.0f);
    return true;
});

REGISTER_TEST(menuBeautifyStyledShowParamGuard, "menu: styled 空项/无可选行 → false（降级信号）", {
    components::StyledMenuStyle style;
    int selected = -1;
    const auto cb = [&selected](const std::vector<int>&) { selected = 1; };
    DOCTEST_CHECK(!components::showStyledMenu("t", std::vector<components::StyledMenuItem>(),
                                              style, 10.0f, 10.0f, cb));
    std::vector<components::StyledMenuItem> onlySep;
    components::StyledMenuItem sep;
    sep.text = "-";
    onlySep.push_back(std::move(sep));
    DOCTEST_CHECK(!components::showStyledMenu("t", onlySep, style, 10.0f, 10.0f, cb));
    std::vector<components::StyledMenuItem> allDisabled;
    components::StyledMenuItem locked;
    locked.text = "锁";
    locked.disabled = true;
    allDisabled.push_back(std::move(locked));
    DOCTEST_CHECK(!components::showStyledMenu("t", allDisabled, style, 10.0f, 10.0f, cb));
    // 三条守卫路径都不得回调选中
    DOCTEST_CHECK_EQ(selected, -1);
    return true;
});
