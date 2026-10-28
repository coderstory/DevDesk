// 任务执行页（票据 06/07/08/10）无头用例：配置编辑层、跳过勾选、移除二次确认、
// 只读整单与返回入口。
//
// 跨文件前提：composeReleaseDetailPage 只在 state.dirty / taskId 变化时读库，
// 数据经 store（临时迁移库上的 ReleaseStore）落库后由页面重读；本文件直接验证
// 渲染与交互语义，每条用例的 description 末尾注明它防的具体事故。
//
// 过滤子串：release_detail（ASCII）。
#include "doctest.h"
#include "test_support.h"

#include "core/theme.h"
#include "module_release/release_detail_ui.h"
#include "module_release/release_store.h"

#include <eui/dsl.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

namespace rel = module::release;

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

    void refresh(rel::ReleaseStore& store, rel::ReleaseDetailUiState& state,
                 const rel::ReleaseDetailActions& actions) {
        ui.begin("release.detail.test");
        rel::composeReleaseDetailPage(ui, screen, store, state, actions);
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

struct Seed {
    std::int64_t taskId = 0;
    std::int64_t envId = 0;
    std::int64_t serviceId = 0;
};

// 建「任务 → 环境快照 → 一个后端服务快照」最小链；任一步失败 taskId 为 0，
// 由调用方断言。配置行由各用例按需经 upsertTaskConfig 补种。
Seed seedDetailTask(rel::ReleaseStore& store, rel::TaskStatus status,
                    const std::string& planDate = "2030-06-06") {
    Seed ids;
    rel::Task task;
    task.planDate = planDate;
    task.status = status;
    auto taskId = store.createTask(task);
    if (taskId.isFail()) return ids;
    ids.taskId = taskId.value();

    rel::TaskEnv env;
    env.taskId = ids.taskId;
    env.envName = "EM";
    env.startMinute = 9 * 60;
    env.endMinute = 18 * 60;

    rel::TaskService svc;
    svc.serviceName = "订单服务";
    svc.kind = rel::ServiceKind::Backend;
    svc.defaultBranch = "main";
    svc.branch = "release/2026";

    auto envId = store.addTaskEnv(env, {svc});
    if (envId.isFail()) return ids;
    ids.envId = envId.value();

    auto services = store.listTaskServices(ids.envId);
    if (services.isFail() || services.value().empty()) return ids;
    ids.serviceId = services.value()[0].id;
    return ids;
}

rel::TaskConfig makeConfig(std::int64_t serviceId, rel::ConfigKind kind,
                           const std::string& content, bool skipped = false,
                           std::int64_t doneAt = 0, int order = 0) {
    rel::TaskConfig cfg;
    cfg.taskServiceId = serviceId;
    cfg.kind = kind;
    cfg.content = content;
    cfg.skipped = skipped;
    cfg.doneAt = doneAt;
    cfg.order = order;
    return cfg;
}

// 按 kind 从库读回配置行（断言「写回 content / skipped」是否真实落库）。
bool configByKind(rel::ReleaseStore& store, std::int64_t serviceId,
                  rel::ConfigKind kind, rel::TaskConfig* out) {
    auto configs = store.listTaskConfigs(serviceId);
    if (configs.isFail()) return false;
    for (const auto& cfg : configs.value()) {
        if (cfg.kind == kind) {
            *out = cfg;
            return true;
        }
    }
    return false;
}

constexpr std::int64_t kDoneAt = 1700000000;

} // namespace

REGISTER_TEST(release_detail_editor_open_and_save_writes_content,
              "配置编辑层：点「查看」开页内遮罩，保存只写回 content"
              "（防：编辑走模块回调跨页丢状态 / 保存改内存不落库 / 内容不回写）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "editor");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Draft);
    DOCTEST_REQUIRE(ids.taskId != 0);
    DOCTEST_REQUIRE(store
                        .upsertTaskConfig(makeConfig(ids.serviceId,
                                                     rel::ConfigKind::Nacos,
                                                     "nacos: v1"))
                        .isOk());

    rel::ReleaseDetailUiState state;
    state.taskId = std::to_string(ids.taskId);
    state.dirty = true;
    rel::ReleaseDetailActions actions;
    Frame frame;
    frame.refresh(store, state, actions);

    const std::string row = "release.detail.env." + std::to_string(ids.envId) +
                            ".svc." + std::to_string(ids.serviceId) +
                            ".step.config.0";
    // 点「查看」→ 页内自绘全屏编辑层打开（不经 actions.viewConfig）
    DOCTEST_REQUIRE(clickNode(frame.ui, row + ".view.bg"));
    DOCTEST_CHECK(state.editorOpen);
    frame.refresh(store, state, actions);
    const auto* editorTitle = frame.ui.find("release.detail.config.editor.title");
    DOCTEST_REQUIRE(editorTitle != nullptr);
    DOCTEST_CHECK(editorTitle->text.find("nacos") != std::string::npos);
    // 可编辑形态：多行输入框 + 保存按钮
    DOCTEST_REQUIRE(frame.ui.find("release.detail.config.editor.input.hit") != nullptr);
    DOCTEST_REQUIRE(frame.ui.find("release.detail.config.editor.save") != nullptr);

    // 编辑缓冲 = 输入框 onChange 绑定的 state.editorText；保存只写 content
    state.editorText = "nacos: v2";
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.detail.config.editor.save.bg"));
    DOCTEST_CHECK(!state.editorOpen);
    rel::TaskConfig saved;
    DOCTEST_REQUIRE(configByKind(store, ids.serviceId, rel::ConfigKind::Nacos,
                                 &saved));
    DOCTEST_CHECK_EQ(saved.content, std::string("nacos: v2"));
    DOCTEST_CHECK(!saved.skipped);
    DOCTEST_CHECK_EQ(saved.doneAt, std::int64_t{0});
    return true;
})

REGISTER_TEST(release_detail_editor_skipped_read_only,
              "配置编辑层：已跳过项打开后只读展示并提示原因、无保存入口"
              "（防：跳过项仍可编辑导致「本次不做」被悄悄改成做 / 保存覆盖跳过标志）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "skipped");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Draft);
    DOCTEST_REQUIRE(ids.taskId != 0);
    DOCTEST_REQUIRE(store
                        .upsertTaskConfig(makeConfig(ids.serviceId,
                                                     rel::ConfigKind::Db, "",
                                                     /*skipped=*/true))
                        .isOk());

    rel::ReleaseDetailUiState state;
    state.taskId = std::to_string(ids.taskId);
    state.dirty = true;
    rel::ReleaseDetailActions actions;
    Frame frame;
    frame.refresh(store, state, actions);

    const std::string row = "release.detail.env." + std::to_string(ids.envId) +
                            ".svc." + std::to_string(ids.serviceId) +
                            ".step.config.1";  // Db → 槽位下标 1
    DOCTEST_REQUIRE(clickNode(frame.ui, row + ".view.bg"));
    DOCTEST_CHECK(state.editorOpen);
    frame.refresh(store, state, actions);

    const auto* hint = frame.ui.find("release.detail.config.editor.hint.text");
    DOCTEST_REQUIRE(hint != nullptr);
    DOCTEST_CHECK_EQ(hint->text, std::string("该项本次已跳过，取消跳过后可编辑"));
    // 只读形态：无输入框、无保存按钮，只有只读值视图
    DOCTEST_CHECK(frame.ui.find("release.detail.config.editor.input.hit") == nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.config.editor.save") == nullptr);
    DOCTEST_REQUIRE(frame.ui.find("release.detail.config.editor.view.text") != nullptr);
    return true;
})

REGISTER_TEST(release_detail_skip_toggle_label_and_cancel,
              "跳过勾选：勾上写 skipped 且行显示「已跳过」，再点取消恢复；"
              "已完成行无勾选框（防：跳过只改内存不落库 / 已跳过行看不出状态 / "
              "已完成项被跳过残留 doneAt / 取消跳过不恢复）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "skip");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Draft);
    DOCTEST_REQUIRE(ids.taskId != 0);
    DOCTEST_REQUIRE(store
                        .upsertTaskConfig(makeConfig(ids.serviceId,
                                                     rel::ConfigKind::Nacos, "x",
                                                     /*skipped=*/false, 0, 0))
                        .isOk());
    // 已完成的 cron 行（doneAt != 0）——完成行不允许跳过，也不画勾选框
    DOCTEST_REQUIRE(store
                        .upsertTaskConfig(makeConfig(ids.serviceId,
                                                     rel::ConfigKind::Cron, "c",
                                                     /*skipped=*/false, kDoneAt, 2))
                        .isOk());

    rel::ReleaseDetailUiState state;
    state.taskId = std::to_string(ids.taskId);
    state.dirty = true;
    rel::ReleaseDetailActions actions;
    Frame frame;
    frame.refresh(store, state, actions);

    const std::string row0 = "release.detail.env." + std::to_string(ids.envId) +
                             ".svc." + std::to_string(ids.serviceId) +
                             ".step.config.0";  // nacos
    const std::string row2 = "release.detail.env." + std::to_string(ids.envId) +
                             ".svc." + std::to_string(ids.serviceId) +
                             ".step.config.2";  // cron（已完成）
    DOCTEST_REQUIRE(frame.ui.find(row0 + ".skip.toggle.hit") != nullptr);
    // 已完成行无勾选框
    DOCTEST_CHECK(frame.ui.find(row2 + ".skip.toggle") == nullptr);

    // 勾上 → skipped 落库 + 行显示「已跳过」
    DOCTEST_REQUIRE(clickNode(frame.ui, row0 + ".skip.toggle.hit"));
    rel::TaskConfig afterSkip;
    DOCTEST_REQUIRE(configByKind(store, ids.serviceId, rel::ConfigKind::Nacos,
                                 &afterSkip));
    DOCTEST_CHECK(afterSkip.skipped);
    frame.refresh(store, state, actions);
    const auto* skipLabel = frame.ui.find(row0 + ".skip");
    DOCTEST_REQUIRE(skipLabel != nullptr);
    DOCTEST_CHECK_EQ(skipLabel->text, std::string("已跳过"));

    // 再点一次（跳过态勾选框仍在）→ 取消跳过恢复，行上「已跳过」消失
    DOCTEST_REQUIRE(clickNode(frame.ui, row0 + ".skip.toggle.hit"));
    rel::TaskConfig afterUnskip;
    DOCTEST_REQUIRE(configByKind(store, ids.serviceId, rel::ConfigKind::Nacos,
                                 &afterUnskip));
    DOCTEST_CHECK(!afterUnskip.skipped);
    frame.refresh(store, state, actions);
    DOCTEST_CHECK(frame.ui.find(row0 + ".skip") == nullptr);
    return true;
})

REGISTER_TEST(release_detail_remove_confirm_then_applies,
              "移除：点移除只弹二次确认、确认前不调 store；确认后服务/环境不再渲染"
              "（防：误点即删不可恢复 / 确认前就落库 / 取消仍删）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "remove");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Draft);
    DOCTEST_REQUIRE(ids.taskId != 0);

    rel::ReleaseDetailUiState state;
    state.taskId = std::to_string(ids.taskId);
    state.dirty = true;
    rel::ReleaseDetailActions actions;
    Frame frame;
    frame.refresh(store, state, actions);

    const std::string envBase =
        "release.detail.env." + std::to_string(ids.envId);
    const std::string svcRemove =
        envBase + ".svc." + std::to_string(ids.serviceId) + ".remove";
    // 点移除 → 只开二次确认，不落库
    DOCTEST_REQUIRE(clickNode(frame.ui, svcRemove + ".bg"));
    DOCTEST_CHECK(state.confirm == rel::ReleaseDetailConfirm::RemoveService);
    DOCTEST_CHECK_EQ(state.removeServiceId, ids.serviceId);
    {
        auto services = store.listTaskServices(ids.envId);
        DOCTEST_REQUIRE(services.isOk());
        DOCTEST_REQUIRE_EQ(services.value().size(), std::size_t{1});
        DOCTEST_CHECK(!services.value()[0].removed);
    }
    // 取消：不落库、确认框关闭
    frame.refresh(store, state, actions);
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.detail.confirm.secondary.bg"));
    DOCTEST_CHECK(state.confirm == rel::ReleaseDetailConfirm::None);
    {
        auto services = store.listTaskServices(ids.envId);
        DOCTEST_REQUIRE(services.isOk());
        DOCTEST_REQUIRE_EQ(services.value().size(), std::size_t{1});
        DOCTEST_CHECK(!services.value()[0].removed);
    }

    // 再次打开并确认 → 落库 removed，服务不再渲染
    frame.refresh(store, state, actions);
    DOCTEST_REQUIRE(clickNode(frame.ui, svcRemove + ".bg"));
    frame.refresh(store, state, actions);
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.detail.confirm.primary.bg"));
    DOCTEST_CHECK(state.confirm == rel::ReleaseDetailConfirm::None);
    {
        auto services = store.listTaskServices(ids.envId);
        DOCTEST_REQUIRE(services.isOk());
        DOCTEST_REQUIRE_EQ(services.value().size(), std::size_t{1});
        DOCTEST_CHECK(services.value()[0].removed);
    }
    frame.refresh(store, state, actions);
    DOCTEST_CHECK(frame.ui.find(envBase + ".svc." + std::to_string(ids.serviceId) +
                                ".bg") == nullptr);

    // 环境移除走同一确认口径：确认前不落库，确认后环境不再渲染（空态提示出现）
    DOCTEST_REQUIRE(clickNode(frame.ui, envBase + ".remove.bg"));
    DOCTEST_CHECK(state.confirm == rel::ReleaseDetailConfirm::RemoveEnv);
    DOCTEST_CHECK_EQ(state.removeEnvId, ids.envId);
    {
        auto envs = store.listTaskEnvs(ids.taskId);
        DOCTEST_REQUIRE(envs.isOk());
        DOCTEST_REQUIRE_EQ(envs.value().size(), std::size_t{1});
        DOCTEST_CHECK(!envs.value()[0].removed);
    }
    frame.refresh(store, state, actions);
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.detail.confirm.primary.bg"));
    {
        auto envs = store.listTaskEnvs(ids.taskId);
        DOCTEST_REQUIRE(envs.isOk());
        DOCTEST_REQUIRE_EQ(envs.value().size(), std::size_t{1});
        DOCTEST_CHECK(envs.value()[0].removed);
    }
    frame.refresh(store, state, actions);
    DOCTEST_CHECK(frame.ui.find(envBase + ".bg") == nullptr);
    const auto* emptyText = frame.ui.find("release.detail.env.empty");
    DOCTEST_REQUIRE(emptyText != nullptr);
    DOCTEST_CHECK_EQ(emptyText->text, std::string("任务还没有环境"));
    return true;
})

REGISTER_TEST(release_detail_done_voided_read_only,
              "只读整单：Done/Voided 下移除/跳过入口消失，配置查看变只读并提示"
              "（防：终态任务仍可改配置/移除快照破坏历史留痕）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "readonly");
    rel::ReleaseStore store(tdb.handle());

    // Done：无移除按钮、无跳过勾选框；配置查看变只读
    const Seed doneIds = seedDetailTask(store, rel::TaskStatus::Done);
    DOCTEST_REQUIRE(doneIds.taskId != 0);
    DOCTEST_REQUIRE(store
                        .upsertTaskConfig(makeConfig(doneIds.serviceId,
                                                     rel::ConfigKind::Nacos, "x"))
                        .isOk());

    rel::ReleaseDetailUiState doneState;
    doneState.taskId = std::to_string(doneIds.taskId);
    doneState.dirty = true;
    rel::ReleaseDetailActions actions;
    Frame frame;
    frame.refresh(store, doneState, actions);

    const std::string envBase =
        "release.detail.env." + std::to_string(doneIds.envId);
    const std::string row0 = envBase + ".svc." + std::to_string(doneIds.serviceId) +
                             ".step.config.0";
    DOCTEST_CHECK(frame.ui.find(envBase + ".svc." +
                                std::to_string(doneIds.serviceId) + ".remove") ==
                  nullptr);
    DOCTEST_CHECK(frame.ui.find(envBase + ".remove") == nullptr);
    DOCTEST_CHECK(frame.ui.find(row0 + ".skip.toggle") == nullptr);

    DOCTEST_REQUIRE(clickNode(frame.ui, row0 + ".view.bg"));
    DOCTEST_CHECK(doneState.editorOpen);
    frame.refresh(store, doneState, actions);
    const auto* doneHint = frame.ui.find("release.detail.config.editor.hint.text");
    DOCTEST_REQUIRE(doneHint != nullptr);
    DOCTEST_CHECK_EQ(doneHint->text, std::string("任务已只读，配置内容仅可查看"));
    DOCTEST_CHECK(frame.ui.find("release.detail.config.editor.save") == nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.config.editor.input.hit") == nullptr);
    DOCTEST_REQUIRE(frame.ui.find("release.detail.config.editor.view.text") != nullptr);

    // Voided：同样没有移除/跳过入口
    const Seed voidedIds = seedDetailTask(store, rel::TaskStatus::Voided);
    DOCTEST_REQUIRE(voidedIds.taskId != 0);
    rel::ReleaseDetailUiState voidedState;
    voidedState.taskId = std::to_string(voidedIds.taskId);
    voidedState.dirty = true;
    frame.refresh(store, voidedState, actions);
    const std::string voidedEnvBase =
        "release.detail.env." + std::to_string(voidedIds.envId);
    DOCTEST_CHECK(frame.ui.find(voidedEnvBase + ".svc." +
                                std::to_string(voidedIds.serviceId) + ".remove") ==
                  nullptr);
    DOCTEST_CHECK(frame.ui.find(voidedEnvBase + ".remove") == nullptr);
    DOCTEST_CHECK(frame.ui.find(voidedEnvBase + ".svc." +
                                std::to_string(voidedIds.serviceId) +
                                ".step.config.0.skip.toggle") == nullptr);
    return true;
})

REGISTER_TEST(release_detail_back_button_only_with_callback,
              "返回：注入 onBack 显示按钮并触发回调；空回调不显示"
              "（防：无返回入口的嵌入场景仍占位 / 返回按钮点了无反应）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "back");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Draft);
    DOCTEST_REQUIRE(ids.taskId != 0);

    rel::ReleaseDetailUiState state;
    state.taskId = std::to_string(ids.taskId);
    state.dirty = true;
    rel::ReleaseDetailActions actions;  // onBack 为空
    Frame frame;
    frame.refresh(store, state, actions);
    DOCTEST_CHECK(frame.ui.find("release.detail.back") == nullptr);

    // 注入回调后按钮出现，点击触发一次
    int backCount = 0;
    rel::ReleaseDetailActions withBack;
    withBack.onBack = [&backCount] { ++backCount; };
    frame.refresh(store, state, withBack);
    DOCTEST_REQUIRE(frame.ui.find("release.detail.back") != nullptr);
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.detail.back.bg"));
    DOCTEST_CHECK_EQ(backCount, 1);
    return true;
})
