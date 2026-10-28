// 基础配置页（服务目录 / 环境定义）无头用例。
// 跨文件前提：drawReleaseConfigPage 不查库，数据由 state 快照注入；store 只在
// composeReleaseConfigPage 里读，所以本文件可以直接验证渲染与表单语义。
//
// 过滤子串：release_config（ASCII）。
#include "doctest.h"

#include "core/theme.h"
#include "module_release/release_config_ui.h"

#include <eui/dsl.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using module::release::EnvDef;
using module::release::ReleaseConfigActions;
using module::release::ReleaseConfigUiState;
using module::release::ServiceDef;
using module::release::ServiceKind;

// 视觉 token 以浅色档为准；用例结束还原，避免污染同进程其他用例。
struct ThemeGuard {
    bool wasDark = false;
    ThemeGuard() {
        wasDark = core::theme::current().isDark;
        core::theme::setLight();
    }
    ~ThemeGuard() {
        if (wasDark) {
            core::theme::setDark();
        } else {
            core::theme::setLight();
        }
    }
};

struct Frame {
    eui::Ui ui;
    eui::Screen screen{1600.0f, 1000.0f};

    void refresh(ReleaseConfigUiState& state, const ReleaseConfigActions& actions,
                 float width = 1600.0f, float height = 1000.0f) {
        screen.width = width;
        screen.height = height;
        ui.begin("release.config.test");
        module::release::drawReleaseConfigPage(ui, screen, state, actions);
        ui.end();
        ui.layout(screen.width, screen.height);
    }
};

bool clickNode(eui::Ui& ui, const std::string& id) {
    auto* element = ui.find(id);
    if (element == nullptr || !element->onClick) {
        return false;
    }
    element->onClick();
    return true;
}

// id 已统一为 std::int64_t（0 = 新增/无选择）；页面元素 id 用 std::to_string(id) 拼接，
// 因此下面的 find/click 路径同样使用数字 id。测试用固定正数，避开 0 哨兵。
ServiceDef makeService(std::int64_t id, const std::string& name,
                       ServiceKind kind, const std::string& branch, int sort) {
    ServiceDef service;
    service.id = id;
    service.name = name;
    service.kind = kind;
    service.defaultBranch = branch;
    service.sort = sort;
    return service;
}

EnvDef makeEnv(std::int64_t id, const std::string& name,
               std::vector<std::int64_t> serviceIds, int startMinute,
               int endMinute, int sort) {
    EnvDef env;
    env.id = id;
    env.name = name;
    env.serviceIds = std::move(serviceIds);
    env.startMinute = startMinute;
    env.endMinute = endMinute;
    env.sort = sort;
    return env;
}

} // namespace

REGISTER_TEST(release_config_service_config_items,
              "服务目录：后端固定 5 项配置 tag；两类前端显示「无配置项」"
              "（防前端被塞配置项 / 后端清单与执行顺序漂移）", {
    ThemeGuard guard;
    ReleaseConfigUiState state;
    state.tab = ReleaseConfigUiState::Tab::Services;
    state.services.push_back(makeService(1, "订单服务", ServiceKind::Backend,
                                         "release/2026", 1));
    state.services.push_back(makeService(2, "运营后台", ServiceKind::PcWeb,
                                         "master", 2));
    state.services.push_back(makeService(3, "司机端", ServiceKind::MobileWeb,
                                         "master", 3));

    ReleaseConfigActions actions;
    Frame frame;
    frame.refresh(state, actions);

    const auto& labels = module::release::configItemLabels(ServiceKind::Backend);
    DOCTEST_REQUIRE(labels.size() == 5);
    DOCTEST_CHECK_EQ(labels[0], std::string("nacos"));
    DOCTEST_CHECK_EQ(labels[1], std::string("数据库"));
    DOCTEST_CHECK_EQ(labels[2], std::string("定时任务"));
    DOCTEST_CHECK_EQ(labels[3], std::string("菜单"));
    DOCTEST_CHECK_EQ(labels[4], std::string("网关"));
    DOCTEST_CHECK(module::release::configItemLabels(ServiceKind::PcWeb).empty());
    DOCTEST_CHECK(module::release::configItemLabels(ServiceKind::MobileWeb).empty());

    for (std::size_t i = 0; i < labels.size(); ++i) {
        const auto* tag = frame.ui.find("release.config.service.1.tag." +
                                        std::to_string(i) + ".text");
        DOCTEST_REQUIRE(tag != nullptr);
        DOCTEST_CHECK_EQ(tag->text, labels[i]);
    }
    const auto* chipBg = frame.ui.find("release.config.service.1.tag.0.bg");
    DOCTEST_REQUIRE(chipBg != nullptr);
    DOCTEST_CHECK(std::fabs(chipBg->radius - 999.0f) < 0.01f);  // chip 圆角 pill
    const auto* chipText = frame.ui.find("release.config.service.1.tag.0.text");
    DOCTEST_REQUIRE(chipText != nullptr);
    DOCTEST_CHECK(std::fabs(chipText->fontSize - 11.0f) < 0.01f);  // chip 字号 11

    const auto* kind = frame.ui.find("release.config.service.1.kind.text");
    DOCTEST_REQUIRE(kind != nullptr);
    DOCTEST_CHECK_EQ(kind->text, std::string("后端"));

    const auto* branch = frame.ui.find("release.config.service.1.branch");
    DOCTEST_REQUIRE(branch != nullptr);
    DOCTEST_CHECK_EQ(branch->text, std::string("release/2026"));

    const auto* pcNoConfig = frame.ui.find("release.config.service.2.noConfig");
    DOCTEST_REQUIRE(pcNoConfig != nullptr);
    DOCTEST_CHECK_EQ(pcNoConfig->text, std::string("无配置项"));
    DOCTEST_CHECK(frame.ui.find("release.config.service.2.tag.0.text") == nullptr);
    const auto* mobileNoConfig = frame.ui.find("release.config.service.3.noConfig");
    DOCTEST_REQUIRE(mobileNoConfig != nullptr);
    DOCTEST_CHECK_EQ(mobileNoConfig->text, std::string("无配置项"));

    DOCTEST_CHECK(frame.ui.find("release.config.card.head.name") != nullptr);
    DOCTEST_CHECK(frame.ui.find("release.config.card.head.config") != nullptr);
    DOCTEST_CHECK(frame.ui.find("release.config.tab.0.bg") != nullptr);
    return true;
})

REGISTER_TEST(release_config_time_window_cross_midnight,
              "时间窗：跨零点显示 HH:MM–HH:MM(次日)，相等按 24 小时窗；"
              "防跨零点被当成同日导致提前一天超时/提醒", {
    DOCTEST_CHECK_EQ(module::release::formatTimeWindow(23 * 60, 2 * 60),
                     std::string("23:00–02:00(次日)"));
    DOCTEST_CHECK_EQ(module::release::formatTimeWindow(9 * 60, 9 * 60),
                     std::string("09:00–09:00(次日)"));
    DOCTEST_CHECK_EQ(module::release::formatTimeWindow(9 * 60, 11 * 60),
                     std::string("09:00–11:00"));
    DOCTEST_CHECK(module::release::isCrossMidnight(22 * 60, 2 * 60));
    DOCTEST_CHECK(module::release::isCrossMidnight(9 * 60, 9 * 60));
    DOCTEST_CHECK(!module::release::isCrossMidnight(9 * 60, 11 * 60));

    int minute = -1;
    DOCTEST_CHECK(module::release::parseTimeText("23:00", &minute));
    DOCTEST_CHECK_EQ(minute, 23 * 60);
    DOCTEST_CHECK(module::release::parseTimeText(" 08:30 ", &minute));
    DOCTEST_CHECK_EQ(minute, 8 * 60 + 30);
    DOCTEST_CHECK(module::release::parseTimeText("9:05", &minute));
    DOCTEST_CHECK_EQ(minute, 9 * 60 + 5);
    minute = 77;
    DOCTEST_CHECK(!module::release::parseTimeText("24:00", &minute));
    DOCTEST_CHECK(!module::release::parseTimeText("12:60", &minute));
    DOCTEST_CHECK(!module::release::parseTimeText("12", &minute));
    DOCTEST_CHECK(!module::release::parseTimeText("ab:cd", &minute));
    DOCTEST_CHECK_EQ(minute, 77);  // 失败不改出参

    ThemeGuard guard;
    ReleaseConfigUiState state;
    state.tab = ReleaseConfigUiState::Tab::Envs;
    state.envs.push_back(makeEnv(10, "EM", {1, 2}, 22 * 60,
                                 2 * 60, 1));
    ReleaseConfigActions actions;
    Frame frame;
    frame.refresh(state, actions);

    const auto* window = frame.ui.find("release.config.env.10.window");
    DOCTEST_REQUIRE(window != nullptr);
    DOCTEST_CHECK_EQ(window->text, std::string("22:00–02:00(次日)"));
    const auto* count = frame.ui.find("release.config.env.10.count");
    DOCTEST_REQUIRE(count != nullptr);
    DOCTEST_CHECK_EQ(count->text, std::string("2 个服务"));
    const auto* header = frame.ui.find("release.config.card.head.window");
    DOCTEST_REQUIRE(header != nullptr);
    DOCTEST_CHECK_EQ(header->text, std::string("时间窗"));
    return true;
})

REGISTER_TEST(release_config_delete_needs_confirm,
              "删除服务/环境必须二次确认：点删除只开确认框，确认前不得调用 store.delete*；"
              "防误删后被引用的环境引用被级联摘除且不可恢复", {
    ThemeGuard guard;
    ReleaseConfigUiState state;
    state.tab = ReleaseConfigUiState::Tab::Services;
    state.services.push_back(makeService(1, "订单服务", ServiceKind::Backend,
                                         "release/2026", 1));
    state.envs.push_back(makeEnv(10, "EM", {1}, 9 * 60, 18 * 60, 1));

    std::vector<std::int64_t> deleted;
    ReleaseConfigActions actions;
    actions.deleteService = [&deleted](std::int64_t id) {
        deleted.push_back(id);
        return core::Result<void>::ok();
    };
    actions.deleteEnv = [&deleted](std::int64_t id) {
        deleted.push_back(id);
        return core::Result<void>::ok();
    };

    Frame frame;
    frame.refresh(state, actions);
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.config.service.1.delete.bg"));
    DOCTEST_CHECK(state.confirmDelete);
    DOCTEST_CHECK(deleted.empty());  // 点击删除按钮不落库

    frame.refresh(state, actions);
    const auto* panel = frame.ui.find("release.dialog.delete.panel");
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK(std::fabs(panel->opacity - 1.0f) < 0.01f);
    const auto* message = frame.ui.find("release.dialog.delete.message");
    DOCTEST_REQUIRE(message != nullptr);
    DOCTEST_CHECK(message->text.find("1 个环境") != std::string::npos);
    DOCTEST_CHECK(message->text.find("摘除") != std::string::npos);
    DOCTEST_CHECK(deleted.empty());

    // 取消：服务与引用都保留，且不调用删除。
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.dialog.delete.cancel.bg"));
    DOCTEST_CHECK(!state.confirmDelete);
    DOCTEST_CHECK(deleted.empty());
    DOCTEST_CHECK_EQ(state.services.size(), std::size_t{1});
    DOCTEST_CHECK_EQ(state.envs.front().serviceIds.size(), std::size_t{1});

    // 再次打开并确认：只有这时才调用 store.deleteService。
    frame.refresh(state, actions);
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.config.service.1.delete.bg"));
    frame.refresh(state, actions);
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.dialog.delete.confirm.bg"));
    DOCTEST_REQUIRE(deleted.size() == 1);
    DOCTEST_CHECK_EQ(deleted.front(), std::int64_t{1});
    DOCTEST_CHECK(!state.confirmDelete);
    DOCTEST_CHECK(state.dirty);

    // 环境删除走同一确认框（deleteIsService=false）。
    state.confirmDelete = true;
    state.deleteIsService = false;
    state.deleteId = 10;
    frame.refresh(state, actions);
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.dialog.delete.confirm.bg"));
    DOCTEST_REQUIRE(deleted.size() == 2);
    DOCTEST_CHECK_EQ(deleted.back(), std::int64_t{10});
    return true;
})

REGISTER_TEST(release_config_env_form_minutes_and_services,
              "环境表单：多选服务 + HH:MM 输入保存为分钟数；非法时间被拦截不落库；"
              "防时间窗以字符串落库导致跨零点比较错误", {
    ThemeGuard guard;
    ReleaseConfigUiState state;
    state.services.push_back(makeService(11, "服务A", ServiceKind::Backend,
                                         "master", 1));
    state.services.push_back(makeService(12, "服务B", ServiceKind::PcWeb,
                                         "master", 2));
    state.form = ReleaseConfigUiState::Form::Env;
    state.envName = "EM";
    state.envServiceIds = {11};
    state.envStartText = "22:00";
    state.envEndText = "02:00";

    std::vector<EnvDef> saved;
    ReleaseConfigActions actions;
    actions.upsertEnv = [&saved](const EnvDef& env) {
        saved.push_back(env);
        return core::Result<void>::ok();
    };

    Frame frame;
    frame.refresh(state, actions);
    const auto* checked = frame.ui.find(
        "release.dialog.env.services.check.11");
    DOCTEST_REQUIRE(checked != nullptr);
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.dialog.env.services.check.12.hit"));
    DOCTEST_CHECK_EQ(state.envServiceIds.size(), std::size_t{2});
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.dialog.env.services.check.11.hit"));
    DOCTEST_CHECK_EQ(state.envServiceIds.size(), std::size_t{1});
    DOCTEST_CHECK_EQ(state.envServiceIds.front(), std::int64_t{12});

    frame.refresh(state, actions);
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.dialog.env.confirm.bg"));
    DOCTEST_REQUIRE(saved.size() == 1);
    DOCTEST_CHECK_EQ(saved.front().name, std::string("EM"));
    DOCTEST_CHECK_EQ(saved.front().startMinute, 22 * 60);
    DOCTEST_CHECK_EQ(saved.front().endMinute, 2 * 60);
    DOCTEST_REQUIRE(saved.front().serviceIds.size() == 1);
    DOCTEST_CHECK_EQ(saved.front().serviceIds.front(), std::int64_t{12});
    DOCTEST_CHECK(state.form == ReleaseConfigUiState::Form::None);

    // 非法时间：保留弹窗、给出错误、不写库。
    state.form = ReleaseConfigUiState::Form::Env;
    state.envName = "EU";
    state.envStartText = "25:00";
    state.envEndText = "02:00";
    frame.refresh(state, actions);
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.dialog.env.confirm.bg"));
    DOCTEST_CHECK(saved.size() == 1);
    DOCTEST_CHECK(!state.envError.empty());
    DOCTEST_CHECK(state.form == ReleaseConfigUiState::Form::Env);
    return true;
})

REGISTER_TEST(release_config_service_form_duplicate_name,
              "服务表单：同名服务保存被拒（重名由服务目录唯一约束兜底）；"
              "防重名服务导致任务快照与提醒指向不明", {
    ThemeGuard guard;
    ReleaseConfigUiState state;
    state.services.push_back(makeService(1, "订单服务", ServiceKind::Backend,
                                         "master", 1));
    state.form = ReleaseConfigUiState::Form::Service;
    state.envName.clear();
    state.serviceName = "订单服务";
    state.serviceKind = ServiceKind::PcWeb;
    state.serviceBranch = "master";

    int upserts = 0;
    ReleaseConfigActions actions;
    actions.upsertService = [&upserts](const ServiceDef&) {
        ++upserts;
        return core::Result<void>::ok();
    };

    Frame frame;
    frame.refresh(state, actions);
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.dialog.service.confirm.bg"));
    DOCTEST_CHECK_EQ(upserts, 0);
    DOCTEST_CHECK(!state.serviceError.empty());
    DOCTEST_CHECK(state.form == ReleaseConfigUiState::Form::Service);

    // 改名后放行，并把类型/默认分支原样带出。
    state.serviceName = "运营后台";
    frame.refresh(state, actions);
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.dialog.service.confirm.bg"));
    DOCTEST_CHECK_EQ(upserts, 1);
    DOCTEST_CHECK(state.form == ReleaseConfigUiState::Form::None);
    return true;
})