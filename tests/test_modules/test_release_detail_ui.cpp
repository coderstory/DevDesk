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

#include "app_registry.h"          // create_release_tasks_module（Esc 分层用例）
#include "core/module_interface.h"
#include "core/theme.h"
#include "module_release/release_detail_ui.h"
#include "module_release/release_store.h"

#include <eui/dsl.h>
#include <eui/types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
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

REGISTER_TEST(release_detail_auto_revoke_content_change,
              "自动撤回链·内容变更：已完成配置改内容保存后重读对账——doneAt 清零、"
              "记 action=revoke 日志、行视觉回到未完成（跳过勾选框重新出现）"
              "（防：loadDetail 快照对比 / dirty 置位 / applyContentRevocations 对账"
              "改坏后全绿照过）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "revokecontent");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Draft);
    DOCTEST_REQUIRE(ids.taskId != 0);
    DOCTEST_REQUIRE(store
                        .upsertTaskConfig(makeConfig(ids.serviceId,
                                                     rel::ConfigKind::Nacos,
                                                     "nacos: v1",
                                                     /*skipped=*/false, kDoneAt, 0))
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
    // 前置：已完成行不画跳过勾选框（撤回后要用它的重现证明行回到未完成）
    DOCTEST_CHECK(frame.ui.find(row + ".skip.toggle") == nullptr);

    // 打开编辑层 → 改内容保存。保存时 doneAt 原样保留，撤回发生在下一帧重读对账
    DOCTEST_REQUIRE(clickNode(frame.ui, row + ".view.bg"));
    DOCTEST_CHECK(state.editorOpen);
    frame.refresh(store, state, actions);
    DOCTEST_REQUIRE(frame.ui.find("release.detail.config.editor.save") != nullptr);
    state.editorText = "nacos: v2";
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.detail.config.editor.save.bg"));
    DOCTEST_CHECK(!state.editorOpen);

    // 置 dirty 重读触发对账：库中 doneAt 清零 + 落一条 revoke 日志
    frame.refresh(store, state, actions);
    rel::TaskConfig saved;
    DOCTEST_REQUIRE(configByKind(store, ids.serviceId, rel::ConfigKind::Nacos,
                                 &saved));
    DOCTEST_CHECK_EQ(saved.content, std::string("nacos: v2"));
    DOCTEST_CHECK_EQ(saved.doneAt, std::int64_t{0});
    {
        auto logs = store.listLogs(ids.taskId);
        DOCTEST_REQUIRE(logs.isOk());
        bool sawRevoke = false;
        for (const auto& log : logs.value()) {
            if (log.action == "revoke") {
                sawRevoke = true;
                break;
            }
        }
        DOCTEST_CHECK(sawRevoke);
    }
    // 行视觉回到未完成：跳过勾选框重新出现（与既有跳过用例同口径）
    DOCTEST_CHECK(frame.ui.find(row + ".skip.toggle") != nullptr);
    return true;
})

REGISTER_TEST(release_detail_auto_revoke_branch_change,
              "自动撤回链·服务分支变更：已完成「合并分支」步骤后直接改库分支并置 dirty "
              "重读，该步骤 doneAt 被清零并记 action=revoke 日志"
              "（防：applyContentRevocations 分支段回归）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "revokebranch");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Draft);
    DOCTEST_REQUIRE(ids.taskId != 0);
    DOCTEST_REQUIRE(
        store.setStepDone(ids.serviceId, rel::StepKind::MergeBranch, kDoneAt).isOk());

    rel::ReleaseDetailUiState state;
    state.taskId = std::to_string(ids.taskId);
    state.dirty = true;
    rel::ReleaseDetailActions actions;
    Frame frame;
    frame.refresh(store, state, actions);

    // 前置：合并分支步骤确已完成
    {
        auto steps = store.listTaskSteps(ids.serviceId);
        DOCTEST_REQUIRE(steps.isOk());
        bool done = false;
        for (const auto& step : steps.value()) {
            if (step.kind == rel::StepKind::MergeBranch && step.doneAt != 0) {
                done = true;
            }
        }
        DOCTEST_CHECK(done);
    }

    // 直接改库分支（模拟其它层保存分支后由宿主置 dirty 的既有契约路径）
    {
        auto services = store.listTaskServices(ids.envId);
        DOCTEST_REQUIRE(services.isOk());
        DOCTEST_REQUIRE(!services.value().empty());
        rel::TaskService svc = services.value()[0];
        svc.branch = "release/2026.1";
        DOCTEST_REQUIRE(store.updateTaskService(svc).isOk());
    }
    state.dirty = true;
    frame.refresh(store, state, actions);

    {
        auto steps = store.listTaskSteps(ids.serviceId);
        DOCTEST_REQUIRE(steps.isOk());
        bool mergeCleared = false;
        for (const auto& step : steps.value()) {
            if (step.kind == rel::StepKind::MergeBranch && step.doneAt == 0) {
                mergeCleared = true;
            }
        }
        DOCTEST_CHECK(mergeCleared);
    }
    {
        auto logs = store.listLogs(ids.taskId);
        DOCTEST_REQUIRE(logs.isOk());
        bool sawRevoke = false;
        for (const auto& log : logs.value()) {
            if (log.action == "revoke") {
                sawRevoke = true;
                break;
            }
        }
        DOCTEST_CHECK(sawRevoke);
    }
    return true;
})

REGISTER_TEST(release_detail_editor_save_failure_keeps_draft,
              "编辑层保存失败：只读库注入写失败——notice 回显错误、编辑层保持打开、"
              "草稿保留且未落库（防：保存失败静默关层丢草稿）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "savefail");
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
    DOCTEST_REQUIRE(clickNode(frame.ui, row + ".view.bg"));
    frame.refresh(store, state, actions);
    state.editorText = "nacos: v2";

    // 加载已完成，只封后续写（连接级只读，读仍可用）
    DOCTEST_REQUIRE(tdb.handle().exec("PRAGMA query_only = ON").isOk());
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.detail.config.editor.save.bg"));

    // 修复后行为：失败只回显 notice，编辑层保持打开、草稿保留
    DOCTEST_CHECK(state.editorOpen);
    DOCTEST_CHECK_EQ(state.editorText, std::string("nacos: v2"));
    DOCTEST_CHECK(state.noticeIsError);
    DOCTEST_CHECK(state.notice.find("保存配置失败") != std::string::npos);

    frame.refresh(store, state, actions);
    const auto* noticeText = frame.ui.find("release.detail.notice.text");
    DOCTEST_REQUIRE(noticeText != nullptr);
    DOCTEST_CHECK(noticeText->text.find("保存配置失败") != std::string::npos);
    // 编辑层仍在：输入框 + 保存按钮都在，字数统计反映草稿
    DOCTEST_CHECK(frame.ui.find("release.detail.config.editor.input.hit") != nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.config.editor.save") != nullptr);
    const auto* count = frame.ui.find("release.detail.config.editor.count");
    DOCTEST_REQUIRE(count != nullptr);
    DOCTEST_CHECK_EQ(count->text, std::string("共 9 字"));
    // 草稿未落库：库里 content 仍是旧值
    rel::TaskConfig saved;
    DOCTEST_REQUIRE(configByKind(store, ids.serviceId, rel::ConfigKind::Nacos,
                                 &saved));
    DOCTEST_CHECK_EQ(saved.content, std::string("nacos: v1"));
    return true;
})

REGISTER_TEST(release_detail_skip_failure_notice,
              "跳过失败：只读库注入写失败——notice 回显错误、库中 skipped 不变"
              "（防：跳过写失败静默、界面却表现已跳过）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "skipfail");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Draft);
    DOCTEST_REQUIRE(ids.taskId != 0);
    DOCTEST_REQUIRE(store
                        .upsertTaskConfig(makeConfig(ids.serviceId,
                                                     rel::ConfigKind::Nacos, "x"))
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
    DOCTEST_REQUIRE(frame.ui.find(row + ".skip.toggle.hit") != nullptr);
    DOCTEST_REQUIRE(tdb.handle().exec("PRAGMA query_only = ON").isOk());
    DOCTEST_REQUIRE(clickNode(frame.ui, row + ".skip.toggle.hit"));

    DOCTEST_CHECK(state.noticeIsError);
    DOCTEST_CHECK(state.notice.find("保存跳过状态失败") != std::string::npos);

    frame.refresh(store, state, actions);
    const auto* noticeText = frame.ui.find("release.detail.notice.text");
    DOCTEST_REQUIRE(noticeText != nullptr);
    DOCTEST_CHECK(noticeText->text.find("保存跳过状态失败") != std::string::npos);
    rel::TaskConfig after;
    DOCTEST_REQUIRE(configByKind(store, ids.serviceId, rel::ConfigKind::Nacos,
                                 &after));
    DOCTEST_CHECK(!after.skipped);
    return true;
})

REGISTER_TEST(release_detail_remove_failure_notice,
              "移除失败：确认后写库失败——notice 回显错误、库中服务未移除"
              "（防：移除写失败静默、界面却消失）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "removefail");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Draft);
    DOCTEST_REQUIRE(ids.taskId != 0);

    rel::ReleaseDetailUiState state;
    state.taskId = std::to_string(ids.taskId);
    state.dirty = true;
    rel::ReleaseDetailActions actions;
    Frame frame;
    frame.refresh(store, state, actions);

    const std::string svcRemove = "release.detail.env." + std::to_string(ids.envId) +
                                  ".svc." + std::to_string(ids.serviceId) +
                                  ".remove";
    DOCTEST_REQUIRE(clickNode(frame.ui, svcRemove + ".bg"));
    DOCTEST_CHECK(state.confirm == rel::ReleaseDetailConfirm::RemoveService);
    frame.refresh(store, state, actions);
    DOCTEST_REQUIRE(tdb.handle().exec("PRAGMA query_only = ON").isOk());
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.detail.confirm.primary.bg"));

    DOCTEST_CHECK(state.noticeIsError);
    DOCTEST_CHECK(state.notice.find("移除服务失败") != std::string::npos);

    frame.refresh(store, state, actions);
    const auto* noticeText = frame.ui.find("release.detail.notice.text");
    DOCTEST_REQUIRE(noticeText != nullptr);
    DOCTEST_CHECK(noticeText->text.find("移除服务失败") != std::string::npos);
    {
        auto services = store.listTaskServices(ids.envId);
        DOCTEST_REQUIRE(services.isOk());
        DOCTEST_REQUIRE_EQ(services.value().size(), std::size_t{1});
        DOCTEST_CHECK(!services.value()[0].removed);
    }
    return true;
})

REGISTER_TEST(release_detail_module_esc_layering,
              "Esc 分层：编辑层开着时 Esc 只关编辑层、确认框开着时只关确认框、都关着时"
              "返回列表（openTaskId_ 被清）（防：Esc 直通导致带层返回列表）", {
    ThemeGuard guard;
    // 注册进程级 appDatabase：模块 compose 经 core::appDatabase() 建 store
    test_support::TestEnv tdb("releasedetail", "esc", /*registerAppDb=*/true);
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Draft);
    DOCTEST_REQUIRE(ids.taskId != 0);
    DOCTEST_REQUIRE(store
                        .upsertTaskConfig(makeConfig(ids.serviceId,
                                                     rel::ConfigKind::Nacos, "x"))
                        .isOk());

    std::unique_ptr<::core::Module> module(create_release_tasks_module());
    DOCTEST_REQUIRE(module != nullptr);

    eui::KeyEvent esc;
    esc.key = eui::InputKey::Escape;
    esc.action = eui::KeyAction::Press;

    eui::Screen screen{1600.0f, 1000.0f};
    auto compose = [&](eui::Ui& ui) {
        ui.begin("release.detail.esc.test");
        module->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);
    };

    // 列表页：Esc 不消费
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("release.tasks.root") != nullptr);
        DOCTEST_CHECK_FALSE(module->onKeyEvent(esc));
    }

    // 点卡片进详情
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_REQUIRE(clickNode(
            ui, "release.tasks.card." + std::to_string(ids.taskId) + ".bg"));
    }

    const std::string row = "release.detail.env." + std::to_string(ids.envId) +
                            ".svc." + std::to_string(ids.serviceId) +
                            ".step.config.0";
    // 编辑层开着：Esc 只关编辑层，仍在详情页
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("release.detail.root") != nullptr);
        DOCTEST_REQUIRE(clickNode(ui, row + ".view.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("release.detail.config.editor.title") != nullptr);
        DOCTEST_CHECK(module->onKeyEvent(esc));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("release.detail.config.editor.title") == nullptr);
        DOCTEST_CHECK(ui.find("release.detail.root") != nullptr);
    }

    // 确认框开着：Esc 只关确认框，仍在详情页
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_REQUIRE(clickNode(
            ui, "release.detail.env." + std::to_string(ids.envId) + ".svc." +
                    std::to_string(ids.serviceId) + ".remove.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("release.detail.confirm.primary.bg") != nullptr);
        DOCTEST_CHECK(module->onKeyEvent(esc));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("release.detail.confirm.primary.bg") == nullptr);
        DOCTEST_CHECK(ui.find("release.detail.root") != nullptr);
    }

    // 都关着：Esc 返回列表（openTaskId_ 被清）
    DOCTEST_CHECK(module->onKeyEvent(esc));
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("release.detail.root") == nullptr);
        DOCTEST_CHECK(ui.find("release.tasks.root") != nullptr);
    }
    return true;
})

REGISTER_TEST(release_detail_editor_char_count,
              "配置编辑层：字数统计「共 N 字」随编辑缓冲变化（防：字数只渲染一次 / "
              "绑定错误缓冲导致 N 不跟随）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "count");
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
    DOCTEST_REQUIRE(clickNode(frame.ui, row + ".view.bg"));
    frame.refresh(store, state, actions);

    const auto* count = frame.ui.find("release.detail.config.editor.count");
    DOCTEST_REQUIRE(count != nullptr);
    DOCTEST_CHECK_EQ(count->text, std::string("共 9 字"));  // "nacos: v1"

    state.editorText = "配置内容已更新 v2";  // 10 字符
    frame.refresh(store, state, actions);
    const auto* countAfter = frame.ui.find("release.detail.config.editor.count");
    DOCTEST_REQUIRE(countAfter != nullptr);
    DOCTEST_CHECK_EQ(countAfter->text, std::string("共 10 字"));
    return true;
})
