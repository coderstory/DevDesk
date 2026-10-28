// 上线任务 UI（票据 04 新建弹层 / 票据 05 任务列表）单测。
//
// 覆盖方式：业务判定（日期约束/同日复用/分页/状态文案）走纯函数；
// 瀑布流首屏与列宽走无头 compose + layout 的几何断言（不启动 app、不开窗）。
// 每条用例的 description 末尾注明它防的具体事故。
#include "doctest.h"

#include "module_release/release_dialogs.h"
#include "module_release/release_list_ui.h"

#include <eui/dsl.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

module::release::ReleaseTaskCard makeCard(std::int64_t id) {
    module::release::ReleaseTaskCard card;
    card.id = id;
    card.planDate = "2026-10-27";  // 周二
    card.status = module::release::TaskStatus::Draft;
    card.envNames = {"EM"};
    card.progressDone = 1;
    card.progressTotal = 8;
    card.summary = "示例摘要";
    return card;
}

bool nearPx(float actual, float expected) {
    return std::fabs(actual - expected) <= 0.01f;
}

} // namespace

REGISTER_TEST(release_new_task_irregular_day_requires_confirm,
              "release 新建任务：周二/周四直接建，其余日期要求二次确认，非法日期拒绝"
              "（防绕过确认直接落库）", {
                  using namespace module::release;
                  const auto wednesday = releaseDecideNewTaskDate("2026-10-28", {});
                  DOCTEST_CHECK(wednesday.action ==
                                ReleaseNewTaskDateAction::ConfirmIrregular);
                  const auto tuesday = releaseDecideNewTaskDate("2026-10-27", {});
                  DOCTEST_CHECK(tuesday.action == ReleaseNewTaskDateAction::Create);
                  const auto thursday = releaseDecideNewTaskDate("2026-10-29", {});
                  DOCTEST_CHECK(thursday.action == ReleaseNewTaskDateAction::Create);
                  const auto invalid = releaseDecideNewTaskDate("2026-02-30", {});
                  DOCTEST_CHECK(invalid.action == ReleaseNewTaskDateAction::InvalidDate);
                  DOCTEST_CHECK_EQ(releaseWeekdayLabel("2026-10-28"), std::string("周三"));
                  return true;
              })

REGISTER_TEST(release_new_task_same_date_opens_existing,
              "release 新建任务：同日已有非作废任务 → 打开既有不新建；作废任务不占当天名额"
              "（防同一天出现两个任务 / 作废后无法重建）", {
                  using namespace module::release;
                  Task existing;
                  existing.id = 7;
                  existing.planDate = "2026-10-28";
                  existing.status = TaskStatus::Draft;
                  const std::vector<Task> tasks{existing};
                  const auto decision = releaseDecideNewTaskDate("2026-10-28", tasks);
                  DOCTEST_CHECK(decision.action ==
                                ReleaseNewTaskDateAction::OpenExisting);
                  DOCTEST_CHECK_EQ(decision.existingTaskId, std::int64_t{7});

                  Task voided = existing;
                  voided.id = 9;
                  voided.status = TaskStatus::Voided;
                  const std::vector<Task> withVoided{voided};
                  const auto again = releaseDecideNewTaskDate("2026-10-28", withVoided);
                  DOCTEST_CHECK(again.action ==
                                ReleaseNewTaskDateAction::ConfirmIrregular);
                  DOCTEST_CHECK_EQ(again.existingTaskId, std::int64_t{0});
                  return true;
              })

REGISTER_TEST(release_task_list_pager_first_page_and_append,
              "release 任务列表：首屏 10 条、触底追加到 20/25、耗尽停止；加载中重复触底不追加"
              "（防重复/漏条与同一页被追加两次）", {
                  using namespace module::release;
                  ReleaseTaskPager pager;
                  pager.reset(25);
                  DOCTEST_CHECK_EQ(pager.visible(), std::size_t{10});
                  DOCTEST_CHECK(pager.loadMore());
                  DOCTEST_CHECK_EQ(pager.visible(), std::size_t{20});
                  DOCTEST_CHECK(pager.loadMore());
                  DOCTEST_CHECK_EQ(pager.visible(), std::size_t{25});
                  DOCTEST_CHECK_FALSE(pager.loadMore());
                  DOCTEST_CHECK_EQ(pager.visible(), std::size_t{25});

                  // 加载门：在飞请求期间重复触底不得再追加一页
                  ReleaseTaskPager gated;
                  gated.reset(25);
                  DOCTEST_CHECK(gated.requestLoad());
                  DOCTEST_CHECK(gated.loading());
                  DOCTEST_CHECK_FALSE(gated.requestLoad());
                  for (int frame = 0; frame < 5; ++frame) {
                      DOCTEST_CHECK_FALSE(gated.advanceLoad());
                  }
                  DOCTEST_CHECK(gated.advanceLoad());  // 第 6 帧完成追加
                  DOCTEST_CHECK_FALSE(gated.loading());
                  DOCTEST_CHECK_EQ(gated.visible(), std::size_t{20});
                  return true;
              })

REGISTER_TEST(release_task_status_chip_text,
              "release 任务卡：五态 chip 文案唯一映射（防状态文案漂移或漏态）", {
                  using namespace module::release;
                  DOCTEST_CHECK_EQ(releaseTaskStatusText(TaskStatus::Draft),
                                   std::string("配置中"));
                  DOCTEST_CHECK_EQ(releaseTaskStatusText(TaskStatus::Pending),
                                   std::string("待执行"));
                  DOCTEST_CHECK_EQ(releaseTaskStatusText(TaskStatus::Running),
                                   std::string("执行中"));
                  DOCTEST_CHECK_EQ(releaseTaskStatusText(TaskStatus::Done),
                                   std::string("已完成"));
                  DOCTEST_CHECK_EQ(releaseTaskStatusText(TaskStatus::Voided),
                                   std::string("已作废"));
                  return true;
              })

REGISTER_TEST(release_task_waterfall_first_page_render,
              "release 任务列表：1000×640 首屏只渲染 10 张、两列 356 宽不横向溢出"
              "（防一次全量渲染与最小窗列宽溢出）", {
                  using namespace module::release;
                  // 列宽公式的验收档：1600 窗内容宽 1328 → 4 列；1000 窗 728 → 2 列
                  DOCTEST_CHECK_EQ(releaseMasonryColumns(1328.0f), 4);
                  DOCTEST_CHECK_EQ(releaseMasonryColumns(728.0f), 2);
                  std::vector<ReleaseTaskCard> cards;
                  for (std::int64_t id = 1; id <= 25; ++id) {
                      cards.push_back(makeCard(id));
                  }
                  eui::Screen screen;
                  screen.width = 792.0f;  // 1000 窗 − 侧栏 208
                  screen.height = 640.0f;
                  eui::Ui ui;
                  ui.begin("release_task_waterfall_test");
                  const ReleaseListActions actions;
                  composeReleaseListPage(ui, screen, cards, actions);
                  ui.end();
                  ui.layout(screen.width, screen.height);

                  const auto* first = ui.find("release.tasks.card.1");
                  const auto* tenth = ui.find("release.tasks.card.10");
                  DOCTEST_REQUIRE(first != nullptr);
                  DOCTEST_REQUIRE(tenth != nullptr);
                  DOCTEST_CHECK(ui.find("release.tasks.card.11") == nullptr);

                  // 1000 窗内容宽 728 → 2 列 × 356、列距 16
                  DOCTEST_CHECK_MESSAGE(nearPx(first->frame.width, 356.0f),
                                        "卡宽实测 " + std::to_string(first->frame.width));
                  const auto* second = ui.find("release.tasks.card.2");
                  DOCTEST_REQUIRE(second != nullptr);
                  DOCTEST_CHECK(nearPx(second->frame.x - first->frame.x, 372.0f));
                  // 无横向溢出：右缘 ≤ 页面右 padding（792 − 32）
                  DOCTEST_CHECK(second->frame.x + second->frame.width <= 760.0f + 0.01f);
                  DOCTEST_CHECK(first->frame.width >= 320.0f);

                  // 卡片自然高度：有环境/摘要行时的估高必须大于两者皆无的卡
                  ReleaseTaskCard bare = cards.front();
                  bare.envNames.clear();
                  bare.summary.clear();
                  DOCTEST_CHECK(releaseTaskCardHeight(cards.front(), 356.0f) >
                                releaseTaskCardHeight(bare, 356.0f));
                  return true;
              })

REGISTER_TEST(release_task_waterfall_scroll_loads_next_page,
              "release 任务列表：触底后先显示「加载中…」占位，再追加到第 20 张"
              "（防触底同步追加导致占位不可见 / 追加后不渲染新卡）", {
                  using namespace module::release;
                  std::vector<ReleaseTaskCard> cards;
                  for (std::int64_t id = 1; id <= 25; ++id) {
                      cards.push_back(makeCard(id));
                  }
                  eui::Screen screen;
                  screen.width = 792.0f;
                  screen.height = 640.0f;
                  eui::Ui ui;
                  const ReleaseListActions actions;

                  auto composeOnce = [&] {
                      ui.begin("release_task_scroll_test");
                      composeReleaseListPage(ui, screen, cards, actions);
                      ui.end();
                  };
                  composeOnce();
                  DOCTEST_CHECK(ui.find("release.tasks.card.11") == nullptr);
                  const auto* scroll = ui.find("release.tasks.scroll");
                  DOCTEST_REQUIRE(scroll != nullptr);
                  DOCTEST_REQUIRE(scroll->onScrollOffsetChanged != nullptr);
                  // 直接派发「滚动到底」：回调只比较 offset+viewport 与内容底，无需真实滚动
                  scroll->onScrollOffsetChanged(1000000.0f);

                  bool sawLoading = false;
                  bool sawTwentieth = false;
                  for (int frame = 0; frame < 10; ++frame) {
                      composeOnce();
                      if (ui.find("release.tasks.loading") != nullptr) sawLoading = true;
                      if (ui.find("release.tasks.card.20") != nullptr) {
                          sawTwentieth = true;
                          break;
                      }
                  }
                  DOCTEST_CHECK_MESSAGE(sawLoading, "触底后从未渲染加载占位");
                  DOCTEST_CHECK_MESSAGE(sawTwentieth, "追加后未渲染第 20 张卡");
                  return true;
              })

REGISTER_TEST(release_task_waterfall_empty_state_and_new_task,
              "release 任务列表：无任务时只显示空态与「新建任务」入口"
              "（防空库还画卡 / 空态没有创建入口）", {
                  using namespace module::release;
                  eui::Screen screen;
                  screen.width = 792.0f;
                  screen.height = 640.0f;
                  eui::Ui ui;
                  ui.begin("release_task_empty_test");
                  bool newTaskRequested = false;
                  ReleaseListActions actions;
                  actions.openNewTask = [&newTaskRequested] { newTaskRequested = true; };
                  composeReleaseListPage(ui, screen, {}, actions);
                  ui.end();
                  ui.layout(screen.width, screen.height);

                  const auto* empty = ui.find("release.tasks.empty");
                  DOCTEST_REQUIRE(empty != nullptr);
                  DOCTEST_CHECK_EQ(empty->text, std::string("暂无数据"));
                  DOCTEST_CHECK(ui.find("release.tasks.card.1") == nullptr);
                  const auto* button = ui.find("release.tasks.empty.new.bg");
                  DOCTEST_REQUIRE(button != nullptr);
                  DOCTEST_REQUIRE(button->onClick != nullptr);
                  button->onClick();
                  DOCTEST_CHECK(newTaskRequested);
                  return true;
              })