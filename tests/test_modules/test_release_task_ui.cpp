// 发布任务 UI（新建弹层 / 任务列表）单测（池 55 重设计后口径）。
//
// 覆盖方式：业务判定（日期约束/同日复用/分页/状态文案）走纯函数；
// 行卡几何 / Tab 筛选 / 空态 / 追加契约走无头 compose + layout 断言（不启动 app、
// 不开窗）；store 路径用独立临时库（test_support::TestEnv）。
// 每条用例的 description 末尾注明它防的具体事故。
#include "doctest.h"
#include "test_support.h"

#include "module_release/release_dialogs.h"
#include "module_release/release_list_ui.h"
#include "module_release/release_logic.h"
#include "module_release/release_store.h"

#include <eui/dsl.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

namespace rel = module::release;

rel::ReleaseTaskCard makeCard(std::int64_t id) {
    rel::ReleaseTaskCard card;
    card.id = id;
    card.planDate = "2026-10-27";  // 周二
    card.dateLabel = "10/27";
    card.status = rel::TaskStatus::Draft;
    card.envNames = {"EM"};
    card.progressDone = 1;
    card.progressTotal = 8;
    return card;
}

bool nearPx(float actual, float expected) {
    return std::fabs(actual - expected) <= 0.01f;
}

eui::Screen testScreen() {
    eui::Screen screen;
    screen.width = 792.0f;   // 1000 窗 − 侧栏 208
    screen.height = 640.0f;
    return screen;
}

void composeCards(eui::Ui& ui, const eui::Screen& screen,
                  const std::vector<rel::ReleaseTaskCard>& cards,
                  const rel::ReleaseListActions& actions) {
    ui.begin("release_task_list_test");
    rel::composeReleaseListPage(ui, screen, cards, actions);
    ui.end();
    ui.layout(screen.width, screen.height);
}

void composeStore(eui::Ui& ui, const eui::Screen& screen, rel::ReleaseStore& store,
                  rel::ReleaseNewTaskDialogState& dialog,
                  const rel::ReleaseListActions& actions) {
    ui.begin("release_task_list_store_test");
    rel::composeReleaseListPage(ui, screen, store, dialog, actions);
    ui.end();
    ui.layout(screen.width, screen.height);
}

rel::Task makeStoreTask(const std::string& planDate, rel::TaskStatus status) {
    rel::Task task;
    task.planDate = planDate;
    task.status = status;
    return task;
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
              "release 任务卡：五态 chip 文案唯一映射（池 53 词表：草稿/待执行/执行中/"
              "已完成/已作废；防「配置中」旧词回流或状态文案漂移）", {
                  using namespace module::release;
                  DOCTEST_CHECK_EQ(releaseTaskStatusText(TaskStatus::Draft),
                                   std::string("草稿"));
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

REGISTER_TEST(release_list_group_of,
              "release 列表 Tab 分组：发布中=Pending+Running、准备中=Draft、"
              "已归档=Done+Voided（防 Tab 状态分组与池 53 模式徽标映射漂移）", {
                  using namespace module::release;
                  DOCTEST_CHECK(releaseListGroupOf(TaskStatus::Draft) ==
                                ReleaseListTab::Preparing);
                  DOCTEST_CHECK(releaseListGroupOf(TaskStatus::Pending) ==
                                ReleaseListTab::Publishing);
                  DOCTEST_CHECK(releaseListGroupOf(TaskStatus::Running) ==
                                ReleaseListTab::Publishing);
                  DOCTEST_CHECK(releaseListGroupOf(TaskStatus::Done) ==
                                ReleaseListTab::Archived);
                  DOCTEST_CHECK(releaseListGroupOf(TaskStatus::Voided) ==
                                ReleaseListTab::Archived);
                  DOCTEST_CHECK_EQ(std::string(releaseListTabLabel(0)), std::string("全部"));
                  DOCTEST_CHECK_EQ(std::string(releaseListTabLabel(1)),
                                   std::string("发布中"));
                  DOCTEST_CHECK_EQ(std::string(releaseListTabLabel(2)),
                                   std::string("准备中"));
                  DOCTEST_CHECK_EQ(std::string(releaseListTabLabel(3)),
                                   std::string("已归档"));
                  return true;
              })

REGISTER_TEST(release_task_list_row_card_geometry,
              "release 任务列表：单列 64px 定高行卡、内容列 768 宽不横向溢出、首屏只渲染"
              " 10 行（防瀑布流几何残留 / 行高漂移 / 最小窗溢出）", {
                  std::vector<rel::ReleaseTaskCard> cards;
                  for (std::int64_t id = 1; id <= 25; ++id) {
                      cards.push_back(makeCard(id));
                  }
                  const eui::Screen screen = testScreen();
                  eui::Ui ui;
                  const rel::ReleaseListActions actions;
                  composeCards(ui, screen, cards, actions);

                  const auto* first = ui.find("release.tasks.card.1");
                  const auto* tenth = ui.find("release.tasks.card.10");
                  DOCTEST_REQUIRE(first != nullptr);
                  DOCTEST_REQUIRE(tenth != nullptr);
                  DOCTEST_CHECK(ui.find("release.tasks.card.11") == nullptr);

                  // 单列定高：行宽 = 内容列 792−12×2 = 768、行高 64、纵向步进 64
                  DOCTEST_CHECK_MESSAGE(nearPx(first->frame.height, 64.0f),
                                        "行高实测 " + std::to_string(first->frame.height));
                  DOCTEST_CHECK_MESSAGE(nearPx(first->frame.width, 768.0f),
                                        "行宽实测 " + std::to_string(first->frame.width));
                  const auto* second = ui.find("release.tasks.card.2");
                  DOCTEST_REQUIRE(second != nullptr);
                  DOCTEST_CHECK(nearPx(second->frame.y - first->frame.y, 64.0f));
                  // 无横向溢出：右缘 ≤ 内容列右界（12 + 768）
                  DOCTEST_CHECK(first->frame.x >= 12.0f - 0.01f);
                  DOCTEST_CHECK(first->frame.x + first->frame.width <= 780.0f + 0.01f);
                  return true;
              })

REGISTER_TEST(release_task_list_append_contract_single_column,
              "release 任务列表：触底后先显示「加载中…」占位，再追加到第 20 行"
              "（防触底同步追加导致占位不可见 / 追加后不渲染新行）", {
                  std::vector<rel::ReleaseTaskCard> cards;
                  for (std::int64_t id = 1; id <= 25; ++id) {
                      cards.push_back(makeCard(id));
                  }
                  const eui::Screen screen = testScreen();
                  eui::Ui ui;
                  const rel::ReleaseListActions actions;

                  auto composeOnce = [&] {
                      composeCards(ui, screen, cards, actions);
                  };
                  composeOnce();
                  DOCTEST_CHECK(ui.find("release.tasks.card.11") == nullptr);
                  const auto* scroll = ui.find("release.tasks.scroll");
                  DOCTEST_REQUIRE(scroll != nullptr);
                  DOCTEST_REQUIRE(scroll->onScrollOffsetChanged != nullptr);
                  // 直接派发「滚动到底」：回调只比较 offset+viewport 与内容底
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
                  DOCTEST_CHECK_MESSAGE(sawTwentieth, "追加后未渲染第 20 行");
                  const auto* row20 = ui.find("release.tasks.card.20");
                  DOCTEST_REQUIRE(row20 != nullptr);
                  DOCTEST_CHECK(nearPx(row20->frame.height, 64.0f));
                  return true;
              })

REGISTER_TEST(release_task_list_empty_state_and_new_task,
              "release 任务列表：无任务时显示「还没有上线任务」空态与「新建任务」次按钮"
              "（防空库还画行 / 空态没有创建入口 / 空态误用绿填充主按钮）", {
                  const eui::Screen screen = testScreen();
                  eui::Ui ui;
                  ui.begin("release_task_empty_test");
                  bool newTaskRequested = false;
                  rel::ReleaseListActions actions;
                  actions.openNewTask = [&newTaskRequested] { newTaskRequested = true; };
                  rel::composeReleaseListPage(ui, screen, {}, actions);
                  ui.end();
                  ui.layout(screen.width, screen.height);

                  const auto* empty = ui.find("release.tasks.empty");
                  DOCTEST_REQUIRE(empty != nullptr);
                  DOCTEST_CHECK_EQ(empty->text, std::string("还没有上线任务"));
                  DOCTEST_CHECK(ui.find("release.tasks.card.1") == nullptr);
                  const auto* button = ui.find("release.tasks.empty.new.bg");
                  DOCTEST_REQUIRE(button != nullptr);
                  DOCTEST_REQUIRE(button->onClick != nullptr);
                  button->onClick();
                  DOCTEST_CHECK(newTaskRequested);
                  return true;
              })

REGISTER_TEST(release_task_list_skeleton_then_rows,
              "release 任务列表：首屏先画骨架行 ×3、下一帧泵装填出数据行"
              "（防首屏同步装填骨架永不可见 / 骨架帧之后不出数据）", {
                  test_support::TestEnv tdb("releaselist", "skeleton",
                                            /*registerAppDb=*/true);
                  rel::ReleaseStore store(tdb.handle());
                  DOCTEST_REQUIRE(store.createTask(makeStoreTask("2026-10-27",
                                                                 rel::TaskStatus::Draft))
                                      .isOk());
                  const eui::Screen screen = testScreen();
                  eui::Ui ui;
                  rel::ReleaseNewTaskDialogState dialog;
                  const rel::ReleaseListActions actions;

                  auto composeOnce = [&] {
                      composeStore(ui, screen, store, dialog, actions);
                  };
                  composeOnce();
                  DOCTEST_CHECK_MESSAGE(ui.find("release.tasks.skeleton") != nullptr,
                                        "首帧未渲染骨架行");
                  DOCTEST_CHECK(ui.find("release.tasks.card.1") == nullptr);
                  composeOnce();
                  DOCTEST_CHECK(ui.find("release.tasks.skeleton") == nullptr);
                  DOCTEST_CHECK_MESSAGE(ui.find("release.tasks.card.1") != nullptr,
                                        "骨架帧之后未装填出数据行");
                  return true;
              })

REGISTER_TEST(release_task_list_tab_filter_and_counts,
              "release 任务列表：Tab=状态分组筛选（发布中=Pending+Running 等）+ 右端计数"
              "句式联动（防筛选漏组 / 计数与库值口径不一致 / 切 Tab 不重锚）", {
                  test_support::TestEnv tdb("releaselist", "tabfilter",
                                            /*registerAppDb=*/true);
                  rel::ReleaseStore store(tdb.handle());
                  DOCTEST_REQUIRE(
                      store.createTask(makeStoreTask("2026-10-20", rel::TaskStatus::Draft))
                          .isOk());
                  DOCTEST_REQUIRE(
                      store.createTask(makeStoreTask("2026-10-21", rel::TaskStatus::Draft))
                          .isOk());
                  DOCTEST_REQUIRE(store
                                      .createTask(makeStoreTask("2026-10-22",
                                                                rel::TaskStatus::Pending))
                                      .isOk());
                  DOCTEST_REQUIRE(store
                                      .createTask(makeStoreTask("2026-10-23",
                                                                rel::TaskStatus::Pending))
                                      .isOk());
                  DOCTEST_REQUIRE(store
                                      .createTask(makeStoreTask("2026-10-24",
                                                                rel::TaskStatus::Running))
                                      .isOk());
                  DOCTEST_REQUIRE(store
                                      .createTask(makeStoreTask("2026-10-25",
                                                                rel::TaskStatus::Done))
                                      .isOk());
                  DOCTEST_REQUIRE(store
                                      .createTask(makeStoreTask("2026-10-26",
                                                                rel::TaskStatus::Voided))
                                      .isOk());
                  const eui::Screen screen = testScreen();
                  eui::Ui ui;
                  rel::ReleaseNewTaskDialogState dialog;
                  const rel::ReleaseListActions actions;

                  auto composeOnce = [&] {
                      composeStore(ui, screen, store, dialog, actions);
                  };
                  composeOnce();
                  composeOnce();
                  DOCTEST_CHECK(ui.find("release.tasks.card.1") != nullptr);
                  const auto* count = ui.find("release.tasks.tabcount");
                  DOCTEST_REQUIRE(count != nullptr);
                  DOCTEST_CHECK_EQ(count->text, std::string("共 7 个任务"));

                  auto clickTab = [&](int tab) {
                      // 注：lambda 内禁用 doctest 断言宏（展开含 return，与推导
                      // 返回类型冲突）；节点缺失由后续行为断言兜底。
                      auto* node = ui.find("release.tasks.tab." + std::to_string(tab));
                      if (node == nullptr || node->onClick == nullptr) return;
                      node->onClick();
                      composeOnce();
                  };
                  // 发布中：Pending ×2 + Running ×1
                  clickTab(1);
                  DOCTEST_CHECK(ui.find("release.tasks.card.3") != nullptr);
                  DOCTEST_CHECK(ui.find("release.tasks.card.5") != nullptr);
                  DOCTEST_CHECK(ui.find("release.tasks.card.1") == nullptr);
                  DOCTEST_CHECK(ui.find("release.tasks.card.6") == nullptr);
                  const auto* count1 = ui.find("release.tasks.tabcount");
                  DOCTEST_REQUIRE(count1 != nullptr);
                  DOCTEST_CHECK_EQ(count1->text, std::string("发布中 · 3 个"));
                  // 准备中：Draft ×2
                  clickTab(2);
                  DOCTEST_CHECK(ui.find("release.tasks.card.1") != nullptr);
                  DOCTEST_CHECK(ui.find("release.tasks.card.2") != nullptr);
                  DOCTEST_CHECK(ui.find("release.tasks.card.3") == nullptr);
                  const auto* count2 = ui.find("release.tasks.tabcount");
                  DOCTEST_REQUIRE(count2 != nullptr);
                  DOCTEST_CHECK_EQ(count2->text, std::string("准备中 · 2 个"));
                  // 已归档：Done + Voided
                  clickTab(3);
                  DOCTEST_CHECK(ui.find("release.tasks.card.6") != nullptr);
                  DOCTEST_CHECK(ui.find("release.tasks.card.7") != nullptr);
                  DOCTEST_CHECK(ui.find("release.tasks.card.1") == nullptr);
                  const auto* count3 = ui.find("release.tasks.tabcount");
                  DOCTEST_REQUIRE(count3 != nullptr);
                  DOCTEST_CHECK_EQ(count3->text, std::string("已归档 · 2 个"));
                  return true;
              })

REGISTER_TEST(release_task_list_filter_empty_and_back,
              "release 任务列表：筛选空态句式随 Tab + 「查看全部」切回全部"
              "（防筛选空态冒充全部空态 / 无路可回）", {
                  test_support::TestEnv tdb("releaselist", "filterempty",
                                            /*registerAppDb=*/true);
                  rel::ReleaseStore store(tdb.handle());
                  DOCTEST_REQUIRE(
                      store.createTask(makeStoreTask("2026-10-20", rel::TaskStatus::Draft))
                          .isOk());
                  const eui::Screen screen = testScreen();
                  eui::Ui ui;
                  rel::ReleaseNewTaskDialogState dialog;
                  const rel::ReleaseListActions actions;

                  auto composeOnce = [&] {
                      composeStore(ui, screen, store, dialog, actions);
                  };
                  composeOnce();
                  composeOnce();

                  auto* tab = ui.find("release.tasks.tab.1");
                  DOCTEST_REQUIRE(tab != nullptr);
                  DOCTEST_REQUIRE(tab->onClick != nullptr);
                  tab->onClick();
                  composeOnce();
                  const auto* emptyText = ui.find("release.tasks.filterempty");
                  DOCTEST_REQUIRE(emptyText != nullptr);
                  DOCTEST_CHECK_EQ(emptyText->text, std::string("暂无发布中的任务"));
                  auto* back = ui.find("release.tasks.filterempty.all");
                  DOCTEST_REQUIRE(back != nullptr);
                  DOCTEST_REQUIRE(back->onClick != nullptr);
                  back->onClick();
                  composeOnce();
                  DOCTEST_CHECK_MESSAGE(ui.find("release.tasks.card.1") != nullptr,
                                        "查看全部后未回到全部列表");
                  return true;
              })

REGISTER_TEST(release_task_list_filter_empty_page_fills_next_page,
              "release 任务列表：筛选空态若匹配行整页落在未加载区 → 自动补页装出行卡"
              "（防「发布中·1 个 + 暂无发布中的任务」矛盾呈现钉死：空态提前 return "
              "短路矮窗自动补页与触底追加，池 55 BUG-1）", {
                  test_support::TestEnv tdb("releaselist", "filteremptyfill",
                                            /*registerAppDb=*/true);
                  rel::ReleaseStore store(tdb.handle());
                  // listTasksPage 为 plan_date DESC：10 条 Draft（11-01..11-10）占满
                  // 首页，发布中行日期最旧（10-01）→ 整行落在未加载的第 2 页。
                  for (int day = 1; day <= 10; ++day) {
                      const std::string date = day < 10
                          ? "2026-11-0" + std::to_string(day)
                          : "2026-11-" + std::to_string(day);
                      DOCTEST_REQUIRE(
                          store.createTask(makeStoreTask(date, rel::TaskStatus::Draft))
                              .isOk());
                  }
                  DOCTEST_REQUIRE(
                      store.createTask(makeStoreTask("2026-10-01",
                                                    rel::TaskStatus::Pending))
                          .isOk());
                  const eui::Screen screen = testScreen();
                  eui::Ui ui;
                  rel::ReleaseNewTaskDialogState dialog;
                  const rel::ReleaseListActions actions;

                  auto composeOnce = [&] {
                      composeStore(ui, screen, store, dialog, actions);
                  };
                  composeOnce();
                  composeOnce();
                  DOCTEST_CHECK(ui.find("release.tasks.card.1") != nullptr);
                  DOCTEST_CHECK(ui.find("release.tasks.card.11") == nullptr);

                  auto* tab = ui.find("release.tasks.tab.1");
                  DOCTEST_REQUIRE(tab != nullptr);
                  DOCTEST_REQUIRE(tab->onClick != nullptr);
                  tab->onClick();
                  // 补页帧泵 6 帧 + 装帧；修复前空态提前 return，泵再久也装不出第 2 页
                  for (int frame = 0; frame < 12; ++frame) composeOnce();
                  DOCTEST_CHECK_MESSAGE(ui.find("release.tasks.card.11") != nullptr,
                                        "发布中行在第 2 页：筛选空态未触发自动补页");
                  DOCTEST_CHECK_MESSAGE(ui.find("release.tasks.filterempty") == nullptr,
                                        "补页装出行卡后不得仍呈现筛选空态");
                  const auto* count = ui.find("release.tasks.tabcount");
                  DOCTEST_REQUIRE(count != nullptr);
                  DOCTEST_CHECK_EQ(count->text, std::string("发布中 · 1 个"));
                  return true;
              })

REGISTER_TEST(release_task_list_voided_running_visual,
              "release 任务列表：Running 行左缘绿竖条、Voided 行内容 0.7 灰化、"
              "Draft 行无进度无指引（防状态矩阵视觉回归）", {
                  rel::ReleaseTaskCard running = makeCard(1);
                  running.status = rel::TaskStatus::Running;
                  running.nextStepLabel = "DB 脚本";
                  rel::ReleaseTaskCard voided = makeCard(2);
                  voided.status = rel::TaskStatus::Voided;
                  rel::ReleaseTaskCard draft = makeCard(3);
                  draft.status = rel::TaskStatus::Draft;
                  std::vector<rel::ReleaseTaskCard> cards{running, voided, draft};

                  const eui::Screen screen = testScreen();
                  eui::Ui ui;
                  const rel::ReleaseListActions actions;
                  composeCards(ui, screen, cards, actions);

                  DOCTEST_CHECK(ui.find("release.tasks.card.1.bar") != nullptr);
                  DOCTEST_CHECK(ui.find("release.tasks.card.2.bar") == nullptr);
                  const auto* next = ui.find("release.tasks.card.1.next");
                  DOCTEST_REQUIRE(next != nullptr);
                  DOCTEST_CHECK_EQ(next->text, std::string("下一步 · DB 脚本"));
                  // Voided：内容节点 0.7（背景保持白）
                  const auto* voidedDate = ui.find("release.tasks.card.2.date");
                  DOCTEST_REQUIRE(voidedDate != nullptr);
                  DOCTEST_CHECK(nearPx(voidedDate->opacity, 0.7f));
                  // Draft：无指引无进度
                  DOCTEST_CHECK(ui.find("release.tasks.card.3.next") == nullptr);
                  DOCTEST_CHECK(ui.find("release.tasks.card.3.prog") == nullptr);
                  DOCTEST_CHECK(ui.find("release.tasks.card.3.prog.count") == nullptr);
                  return true;
              })

REGISTER_TEST(release_task_cards_assembly_next_step_label,
              "release 装配层：Pending/Running 卡装配「下一步」步名（与详情页主动作"
              "同源）、Draft/Done/Voided 为空；跨年日期短标加宽（防列表指引与详情页"
              "不同源 / 非发布态误装配）", {
                  test_support::TestEnv tdb("releaselist", "assembly",
                                            /*registerAppDb=*/true);
                  rel::ReleaseStore store(tdb.handle());

                  // Pending（未来计划日：时间窗未开）+ 前置全齐 + nacos 工单已批
                  // → 首个未完成且已解锁 = 合并分支（合并分支不受时间窗门禁）
                  rel::Task pending = makeStoreTask("2026-10-27", rel::TaskStatus::Pending);
                  pending.pretestOk = true;
                  pending.cabOk = true;
                  pending.ticketNacosNo = "N-1";
                  pending.ticketNacosOk = true;
                  pending.announceSentAt = 1;
                  const auto pendingId = store.createTask(pending);
                  DOCTEST_REQUIRE(pendingId.isOk());
                  rel::TaskEnv env;
                  env.taskId = pendingId.value();
                  env.envName = "生产环境";
                  env.startMinute = 1439;
                  env.endMinute = 30;
                  rel::TaskService svc;
                  svc.serviceName = "order-service";
                  svc.kind = rel::ServiceKind::Backend;
                  const auto envId = store.addTaskEnv(env, {svc});
                  DOCTEST_REQUIRE(envId.isOk());
                  auto services = store.listTaskServices(envId.value());
                  DOCTEST_REQUIRE(services.isOk());
                  DOCTEST_REQUIRE_EQ(services.value().size(), std::size_t{1});
                  rel::TaskConfig cfg;
                  cfg.taskServiceId = services.value()[0].id;
                  cfg.kind = rel::ConfigKind::Nacos;
                  cfg.content = "nacos-content";
                  DOCTEST_REQUIRE(store.upsertTaskConfig(cfg).isOk());

                  // 全锁定 → 空串：合并分支已完成，其余步骤/配置时间窗未开
                  rel::Task locked = makeStoreTask("2030-01-15", rel::TaskStatus::Pending);
                  locked.pretestOk = true;
                  locked.cabOk = true;
                  locked.ticketDbNo = "D-1";
                  locked.ticketDbOk = true;
                  locked.announceSentAt = 1;
                  const auto lockedId = store.createTask(locked);
                  DOCTEST_REQUIRE(lockedId.isOk());
                  rel::TaskEnv lockedEnv;
                  lockedEnv.taskId = lockedId.value();
                  lockedEnv.envName = "生产环境";
                  lockedEnv.startMinute = 540;
                  lockedEnv.endMinute = 600;
                  rel::TaskService lockedSvc;
                  lockedSvc.serviceName = "svc";
                  const auto lockedEnvId = store.addTaskEnv(lockedEnv, {lockedSvc});
                  DOCTEST_REQUIRE(lockedEnvId.isOk());
                  auto lockedServices = store.listTaskServices(lockedEnvId.value());
                  DOCTEST_REQUIRE(lockedServices.isOk());
                  DOCTEST_REQUIRE(
                      store
                          .setStepDone(lockedServices.value()[0].id,
                                       rel::StepKind::MergeBranch, 1700000000)
                          .isOk());

                  DOCTEST_REQUIRE(
                      store.createTask(makeStoreTask("2026-10-20", rel::TaskStatus::Draft))
                          .isOk());
                  DOCTEST_REQUIRE(
                      store.createTask(makeStoreTask("2026-10-19", rel::TaskStatus::Done))
                          .isOk());

                  auto cards = rel::releaseLoadTaskCards(store, 0, 10);
                  DOCTEST_REQUIRE(cards.isOk());
                  std::string pendingLabel = "?";
                  std::string lockedLabel = "?";
                  std::string draftLabel = "?";
                  std::string doneLabel = "?";
                  std::string pendingDate;
                  bool pendingDateWide = true;
                  for (const auto& card : cards.value()) {
                      if (card.id == pendingId.value()) {
                          pendingLabel = card.nextStepLabel;
                          pendingDate = card.dateLabel;
                          pendingDateWide = card.dateWide;
                      } else if (card.id == lockedId.value()) {
                          lockedLabel = card.nextStepLabel;
                      } else if (card.status == rel::TaskStatus::Draft) {
                          draftLabel = card.nextStepLabel;
                      } else if (card.status == rel::TaskStatus::Done) {
                          doneLabel = card.nextStepLabel;
                      }
                  }
                  DOCTEST_CHECK_EQ(pendingLabel, std::string("合并分支"));
                  DOCTEST_CHECK_EQ(lockedLabel, std::string(""));
                  DOCTEST_CHECK_EQ(draftLabel, std::string(""));
                  DOCTEST_CHECK_EQ(doneLabel, std::string(""));
                  DOCTEST_CHECK_EQ(pendingDate, std::string("10/27"));
                  DOCTEST_CHECK_FALSE(pendingDateWide);
                  return true;
              })
