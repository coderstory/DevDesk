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
#include "module_release/release_detail_internal.h"  // 状态机直调（撤回/下一步守卫用例）
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
    // 跳过入口在「···」次行（F-13 收纳）：展开后点「跳过此项」
    DOCTEST_REQUIRE(frame.ui.find(row0 + ".more") != nullptr);
    // 已完成行无「···」入口
    DOCTEST_CHECK(frame.ui.find(row2 + ".more") == nullptr);
    DOCTEST_REQUIRE(clickNode(frame.ui, row0 + ".more.bg"));
    frame.refresh(store, state, actions);
    DOCTEST_REQUIRE(clickNode(frame.ui, row0 + ".menu.skip.bg"));
    rel::TaskConfig afterSkip;
    DOCTEST_REQUIRE(configByKind(store, ids.serviceId, rel::ConfigKind::Nacos,
                                 &afterSkip));
    DOCTEST_CHECK(afterSkip.skipped);
    frame.refresh(store, state, actions);
    const auto* stateText = frame.ui.find(row0 + ".state");
    DOCTEST_REQUIRE(stateText != nullptr);
    DOCTEST_CHECK_EQ(stateText->text, std::string("已跳过"));

    // 次行保持展开（openMenuRowKey 跨重读存活）→ 「取消跳过」恢复
    frame.refresh(store, state, actions);
    DOCTEST_REQUIRE(clickNode(frame.ui, row0 + ".menu.skip.bg"));
    rel::TaskConfig afterUnskip;
    DOCTEST_REQUIRE(configByKind(store, ids.serviceId, rel::ConfigKind::Nacos,
                                 &afterUnskip));
    DOCTEST_CHECK(!afterUnskip.skipped);
    frame.refresh(store, state, actions);
    const auto* stateTextAfter = frame.ui.find(row0 + ".state");
    DOCTEST_REQUIRE(stateTextAfter != nullptr);
    // 内容 "x" 仍在：状态字是「已填 n 字」；断言核心是「不再是已完成/已跳过」
    DOCTEST_CHECK(stateTextAfter->text.find("已完成") == std::string::npos);
    DOCTEST_CHECK(stateTextAfter->text.find("已跳过") == std::string::npos);
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
    // 归档态 = 流程轨道形态（全只读）：无编辑域入口、无提交/下一步
    DOCTEST_CHECK(frame.ui.find("release.detail.track") != nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.submit") == nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.next") == nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.prereq.0.input.hit") == nullptr);

    // Voided：同样没有移除/跳过入口
    const Seed voidedIds = seedDetailTask(store, rel::TaskStatus::Voided, "2030-06-11");
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
    // 行视觉回到未完成：状态字回「未完成」（与既有跳过用例同口径）
    const auto* stateText = frame.ui.find(row + ".state");
    DOCTEST_REQUIRE(stateText != nullptr);
    DOCTEST_CHECK(stateText->text.find("已完成") == std::string::npos);
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
    DOCTEST_REQUIRE(clickNode(frame.ui, row + ".more.bg"));
    frame.refresh(store, state, actions);
    DOCTEST_REQUIRE(tdb.handle().exec("PRAGMA query_only = ON").isOk());
    DOCTEST_REQUIRE(clickNode(frame.ui, row + ".menu.skip.bg"));

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

REGISTER_TEST(release_detail_withdraw_pending_keeps_confirmations,
              "撤回提交: Pending 页头「撤回提交」→ 二次确认 → 库状态回 Draft；"
              "前置勾选与既有日志原样保留，新增一条 revoke/提交 日志"
              "（防：撤回误清理确认数据 / 漏日志 / 状态未回退）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "withdraw");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Pending);
    DOCTEST_REQUIRE(ids.taskId != 0);
    // 种前置勾选 + 单号（撤回后必须原样保留）
    {
        auto tasks = store.listTasks();
        DOCTEST_REQUIRE(tasks.isOk());
        rel::Task task = tasks.value()[0];
        task.pretestNo = "RM-1";
        task.pretestOk = true;
        DOCTEST_REQUIRE(store.updateTask(task).isOk());
    }

    rel::ReleaseDetailUiState state;
    state.taskId = std::to_string(ids.taskId);
    state.dirty = true;
    rel::ReleaseDetailActions actions;
    Frame frame;
    frame.refresh(store, state, actions);
    DOCTEST_REQUIRE(state.displayStatus == rel::TaskStatus::Pending);
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.detail.withdraw.bg"));
    DOCTEST_CHECK(state.confirm == rel::ReleaseDetailConfirm::WithdrawSubmission);
    frame.refresh(store, state, actions);
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.detail.confirm.primary.bg"));

    // 状态回 Draft；确认与单号保留；日志多一条 revoke（step=提交）
    {
        auto tasks = store.listTasks();
        DOCTEST_REQUIRE(tasks.isOk());
        DOCTEST_CHECK(tasks.value()[0].status == rel::TaskStatus::Draft);
        DOCTEST_CHECK(tasks.value()[0].pretestOk);
        DOCTEST_CHECK_EQ(tasks.value()[0].pretestNo, std::string("RM-1"));
    }
    {
        auto logs = store.listLogs(ids.taskId);
        DOCTEST_REQUIRE(logs.isOk());
        bool sawWithdraw = false;
        for (const auto& log : logs.value()) {
            if (log.action == "revoke" && log.step == "提交") sawWithdraw = true;
        }
        DOCTEST_CHECK(sawWithdraw);
    }
    // 回到编辑态：提交按钮与单号输入框重新出现
    frame.refresh(store, state, actions);
    DOCTEST_CHECK(state.displayStatus == rel::TaskStatus::Draft);
    DOCTEST_CHECK(frame.ui.find("release.detail.submit") != nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.prereq.0.input.hit") != nullptr);
    return true;
})

REGISTER_TEST(release_detail_withdraw_rejected_when_running,
              "撤回提交: Running 无页头入口、logic 直调拒绝且不改状态"
              "（防：执行中任务被回退成草稿破坏执行留痕）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "withdrawrun");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Running);
    DOCTEST_REQUIRE(ids.taskId != 0);

    rel::ReleaseDetailUiState state;
    state.taskId = std::to_string(ids.taskId);
    state.dirty = true;
    rel::ReleaseDetailActions actions;
    Frame frame;
    frame.refresh(store, state, actions);
    DOCTEST_CHECK(frame.ui.find("release.detail.withdraw") == nullptr);

    module::release::detail::applyWithdrawSubmission(store, state);
    DOCTEST_CHECK(state.noticeIsError);
    {
        auto tasks = store.listTasks();
        DOCTEST_REQUIRE(tasks.isOk());
        DOCTEST_CHECK(tasks.value()[0].status == rel::TaskStatus::Running);
    }
    return true;
})

REGISTER_TEST(release_detail_publish_mode_blocks_edit_paths,
              "发布态只读: Pending 下移除/填写入口消失，跳过/移除 logic 直调拒绝"
              "（防：issue 06「提交后仍可改配置」残留在写路径）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "publishro");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Pending);
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

    const std::string envBase = "release.detail.env." + std::to_string(ids.envId);
    const std::string row = envBase + ".svc." + std::to_string(ids.serviceId) +
                            ".step.config.0";
    DOCTEST_CHECK(frame.ui.find(envBase + ".svc." +
                                std::to_string(ids.serviceId) + ".remove") == nullptr);
    DOCTEST_CHECK(frame.ui.find(envBase + ".remove") == nullptr);
    DOCTEST_CHECK(frame.ui.find(row + ".view") == nullptr);
    DOCTEST_CHECK(frame.ui.find(row + ".more") == nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.prereq.0.input.hit") == nullptr);

    // 越权写路径：logic 直调被拒
    module::release::detail::toggleConfigSkip(store, state, ids.serviceId,
                                              rel::ConfigKind::Nacos);
    DOCTEST_CHECK(state.noticeIsError);
    module::release::detail::applyRemoveService(store, state);
    DOCTEST_CHECK(state.noticeIsError);
    {
        auto services = store.listTaskServices(ids.envId);
        DOCTEST_REQUIRE(services.isOk());
        DOCTEST_CHECK(!services.value()[0].removed);
    }
    return true;
})

REGISTER_TEST(release_detail_next_step_track_order_and_window_gate,
              "下一步: 按固定序推进 提测→CAB→公告→合并分支，部署被时间窗挡下并"
              "指名环境（防：推进序漂移 / 时间窗门禁漏指名缺项）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "nextstep");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Pending);
    DOCTEST_REQUIRE(ids.taskId != 0);
    {
        auto tasks = store.listTasks();
        DOCTEST_REQUIRE(tasks.isOk());
        rel::Task task = tasks.value()[0];
        task.pretestNo = "RM-1";
        task.cabNo = "CAB-1";
        task.ticketNacosNo = "MR-1";
        task.ticketDbNo = "DB-1";
        task.announceText = "公告";
        DOCTEST_REQUIRE(store.updateTask(task).isOk());
    }

    rel::ReleaseDetailUiState state;
    state.taskId = std::to_string(ids.taskId);
    state.dirty = true;
    rel::ReleaseDetailActions actions;
    Frame frame;
    frame.refresh(store, state, actions);

    auto taskInDb = [&store]() -> rel::Task {
        auto tasks = store.listTasks();
        // DOCTEST_REQUIRE 在返回 Task 的 lambda 里不可用（其失败路径返回 bool），
        // 失败时回默认值，由后续断言红。
        if (tasks.isFail() || tasks.value().empty()) return rel::Task{};
        return tasks.value()[0];
    };
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.detail.next.bg"));
    DOCTEST_CHECK(taskInDb().pretestOk);
    frame.refresh(store, state, actions);
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.detail.next.bg"));
    DOCTEST_CHECK(taskInDb().cabOk);
    frame.refresh(store, state, actions);
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.detail.next.bg"));
    DOCTEST_CHECK(taskInDb().announceSentAt != 0);
    frame.refresh(store, state, actions);
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.detail.next.bg"));
    // 合并分支不受时间窗限制（既有语义）
    {
        auto steps = store.listTaskSteps(ids.serviceId);
        DOCTEST_REQUIRE(steps.isOk());
        bool mergeDone = false;
        for (const auto& step : steps.value()) {
            if (step.kind == rel::StepKind::MergeBranch && step.doneAt != 0) {
                mergeDone = true;
            }
        }
        DOCTEST_CHECK(mergeDone);
    }
    // 部署：计划日 2030-06-06 未到（nowMinute < 0）→ 时间窗拒绝并指名环境
    frame.refresh(store, state, actions);
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.detail.next.bg"));
    DOCTEST_CHECK(state.noticeIsError);
    DOCTEST_CHECK(state.notice.find("时间窗未开始") != std::string::npos);
    DOCTEST_CHECK(state.notice.find("EM") != std::string::npos);
    return true;
})

REGISTER_TEST(release_detail_next_step_refused_in_draft,
              "轨道发布态专属: Draft 调下一步被拒（防：编辑态绕过提交直接推进）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "nextdraft");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Draft);
    DOCTEST_REQUIRE(ids.taskId != 0);

    rel::ReleaseDetailUiState state;
    state.taskId = std::to_string(ids.taskId);
    state.dirty = true;
    rel::ReleaseDetailActions actions;
    Frame frame;
    frame.refresh(store, state, actions);
    DOCTEST_CHECK(frame.ui.find("release.detail.next") == nullptr);

    module::release::detail::performNextStep(store, state, -1);
    DOCTEST_CHECK(state.noticeIsError);
    DOCTEST_CHECK_EQ(state.displayStatus, rel::TaskStatus::Draft);
    return true;
})

REGISTER_TEST(release_detail_submit_gate_requires_prereq_numbers,
              "提交入口防呆: 前置单号/公告缺失时提交被拒进清单，补齐后放行"
              "（防：F-20 只在校验弹层出现而提交仍放行）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "submitgate");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Draft);
    DOCTEST_REQUIRE(ids.taskId != 0);
    const char* kinds[] = {"nacos: v", "db: v", "cron: v", "menu: v", "gw: v"};
    const rel::ConfigKind kindIds[] = {
        rel::ConfigKind::Nacos, rel::ConfigKind::Db, rel::ConfigKind::Cron,
        rel::ConfigKind::Menu, rel::ConfigKind::Gateway};
    for (int i = 0; i < 5; ++i) {
        DOCTEST_REQUIRE(store
                            .upsertTaskConfig(makeConfig(ids.serviceId, kindIds[i],
                                                         kinds[i]))
                            .isOk());
    }

    rel::ReleaseDetailUiState state;
    state.taskId = std::to_string(ids.taskId);
    state.dirty = true;
    rel::ReleaseDetailActions actions;
    Frame frame;
    frame.refresh(store, state, actions);
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.detail.submit.bg"));
    DOCTEST_CHECK(state.confirm == rel::ReleaseDetailConfirm::ValidationReport);
    bool sawPrereq = false;
    for (const auto& item : state.validationMissing) {
        if (item.find("提测单号未填写") != std::string::npos) sawPrereq = true;
    }
    DOCTEST_CHECK(sawPrereq);
    {
        auto tasks = store.listTasks();
        DOCTEST_REQUIRE(tasks.isOk());
        DOCTEST_CHECK(tasks.value()[0].status == rel::TaskStatus::Draft);
    }

    // 补齐后提交放行 → Pending（数字必须落库：dirty 重读会以库值覆盖 state.task）
    {
        auto tasks = store.listTasks();
        DOCTEST_REQUIRE(tasks.isOk());
        rel::Task task = tasks.value()[0];
        task.pretestNo = "RM-1";
        task.cabNo = "CAB-1";
        task.ticketNacosNo = "MR-1";
        task.ticketDbNo = "DB-1";
        task.announceText = "公告";
        DOCTEST_REQUIRE(store.updateTask(task).isOk());
    }
    state.dirty = true;  // db 改数必须置 dirty：否则 compose 不重读，state.task 还是旧值
    frame.refresh(store, state, actions);
    DOCTEST_REQUIRE(clickNode(frame.ui, "release.detail.submit.bg"));
    DOCTEST_CHECK(state.displayStatus == rel::TaskStatus::Pending);
    {
        auto tasks = store.listTasks();
        DOCTEST_REQUIRE(tasks.isOk());
        DOCTEST_CHECK(tasks.value()[0].status == rel::TaskStatus::Pending);
    }
    return true;
})

REGISTER_TEST(release_detail_header_primary_action_matrix,
              "页头主动作唯一: Draft=提交+校验次钮+菜单作废 / Pending=下一步+撤回次钮 / "
              "Running 无撤回 / Done 无按钮 / Voided=恢复"
              "（防：主动作漂移 / 撤回入口漏隐藏 / 恢复入口丢失）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "headermatrix");
    rel::ReleaseStore store(tdb.handle());

    Frame frame;
    std::string probeDate;
    auto probe = [&](rel::TaskStatus status) -> void {
        probeDate = status == rel::TaskStatus::Draft ? "2030-06-06"
                      : status == rel::TaskStatus::Pending ? "2030-06-07"
                      : status == rel::TaskStatus::Running ? "2030-06-08"
                      : status == rel::TaskStatus::Done ? "2030-06-09"
                      : "2030-06-10";
        const Seed ids = seedDetailTask(store, status, probeDate);
        // lambda 内不能用 DOCTEST_REQUIRE（宏展开含 return 值）；种子失败时
        // 后续 find 断言自然红。
        if (ids.taskId == 0) return;
        rel::ReleaseDetailUiState state;
        state.taskId = std::to_string(ids.taskId);
        state.dirty = true;
        rel::ReleaseDetailActions actions;
        actions.onBack = [] {};
        frame.refresh(store, state, actions);
    };

    // Draft：提交主按钮 + 校验次按钮 + 菜单（作废）；无下一步/撤回/恢复
    probe(rel::TaskStatus::Draft);
    DOCTEST_CHECK(frame.ui.find("release.detail.submit") != nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.validate") != nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.menu") != nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.next") == nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.withdraw") == nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.restore") == nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.modebadge.publish") == nullptr);

    // Pending：下一步主按钮 + 撤回次按钮 + 菜单；无提交/恢复；发布中徽标出现
    probe(rel::TaskStatus::Pending);
    DOCTEST_CHECK(frame.ui.find("release.detail.next") != nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.withdraw") != nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.menu") != nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.submit") == nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.restore") == nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.modebadge.publish") != nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.track") != nullptr);

    // Running：下一步主按钮；撤回消失
    probe(rel::TaskStatus::Running);
    DOCTEST_CHECK(frame.ui.find("release.detail.next") != nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.withdraw") == nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.modebadge.publish") != nullptr);

    // Done：无主动作；Voided：恢复次按钮
    probe(rel::TaskStatus::Done);
    DOCTEST_CHECK(frame.ui.find("release.detail.submit") == nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.next") == nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.restore") == nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.modebadge.archive") != nullptr);

    probe(rel::TaskStatus::Voided);
    DOCTEST_CHECK(frame.ui.find("release.detail.restore") != nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.modebadge.voided") != nullptr);
    return true;
})

REGISTER_TEST(release_detail_anchor_consumed_once,
              "F-23 锚点: pendingAnchor 消费后清空、发布态轨道渲染"
              "（防：锚点请求残留导致每次重读都跳滚动）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "anchor");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Pending);
    DOCTEST_REQUIRE(ids.taskId != 0);

    rel::ReleaseDetailUiState state;
    state.taskId = std::to_string(ids.taskId);
    state.dirty = true;
    state.pendingAnchor = "publish.current";
    rel::ReleaseDetailActions actions;
    Frame frame;
    frame.refresh(store, state, actions);
    DOCTEST_CHECK(state.pendingAnchor.empty());
    DOCTEST_CHECK(frame.ui.find("release.detail.track") != nullptr);
    // 再次重读不再触发锚点（字段已清）
    state.dirty = true;
    frame.refresh(store, state, actions);
    DOCTEST_CHECK(state.pendingAnchor.empty());
    return true;
})

// ==================== 池 54：分支编辑入口（F-54-1 ~ F-54-3） ====================

namespace {

std::string branchSvcBase(const Seed& ids) {
    return "release.detail.env." + std::to_string(ids.envId) +
           ".svc." + std::to_string(ids.serviceId);
}

// 分支编辑行真实链路助手（同 test_markdown 的 pressEnterOnInput 惯例）：
// onTextInput → onChange → 缓冲；Enter 键 → onEnter → 提交落库。
bool typeIntoBranchInput(eui::Ui& ui, const std::string& inputId,
                         const std::string& text) {
    const auto* hit = ui.find(inputId + ".hit");
    if (hit == nullptr || !hit->onTextInput) return false;
    core::TextInputEvent ev;
    ev.text = text;
    hit->onTextInput(ev);
    return true;
}

bool pressEnterOnBranchInput(eui::Ui& ui, const std::string& inputId) {
    const auto* hit = ui.find(inputId + ".hit");
    if (hit == nullptr || !hit->onKeyEvent) return false;
    eui::KeyEvent ev;
    ev.key = eui::InputKey::Enter;
    ev.action = eui::KeyAction::Press;
    return hit->onKeyEvent(ev);
}

bool branchInDb(rel::ReleaseStore& store, std::int64_t envId, std::string* out) {
    auto services = store.listTaskServices(envId);
    if (services.isFail() || services.value().size() != 1) return false;
    *out = services.value()[0].branch;
    return true;
}

} // namespace

REGISTER_TEST(release_detail_branch_save_writes_db,
              "分支保存：缓冲经 updateTaskService 落库、成功清缓冲置 dirty"
              "（防：改分支只进内存不落库 / 缓冲直改 services 让对账空转 / "
              "整行写破坏 defaultBranch 快照）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "branchsave");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Draft);
    DOCTEST_REQUIRE(ids.taskId != 0);

    rel::ReleaseDetailUiState state;
    state.taskId = std::to_string(ids.taskId);
    state.dirty = true;
    rel::ReleaseDetailActions actions;
    Frame frame;
    frame.refresh(store, state, actions);

    rel::detail::beginBranchEdit(state, ids.serviceId, "release/2027");
    rel::detail::applySaveBranch(store, state);
    DOCTEST_CHECK(state.branchEditServiceId == 0);
    DOCTEST_CHECK(state.dirty);  // 置 dirty → 下一帧重读走对账
    DOCTEST_CHECK(state.noticeIsError == false);
    frame.refresh(store, state, actions);

    std::string branch;
    DOCTEST_REQUIRE(branchInDb(store, ids.envId, &branch));
    DOCTEST_CHECK_EQ(branch, std::string("release/2027"));
    auto services = store.listTaskServices(ids.envId);
    DOCTEST_REQUIRE(services.isOk());
    DOCTEST_CHECK_EQ(services.value()[0].defaultBranch, std::string("main"));
    return true;
})

REGISTER_TEST(release_detail_branch_illegal_rejected,
              "分支非法：含空格/控制字符不落库、缓冲保留、错误 notice"
              "（防：破版输入直接进单据 / 非法时缓冲被清丢输入）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "branchbad");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Draft);
    DOCTEST_REQUIRE(ids.taskId != 0);

    rel::ReleaseDetailUiState state;
    state.taskId = std::to_string(ids.taskId);
    state.dirty = true;
    rel::ReleaseDetailActions actions;
    Frame frame;
    frame.refresh(store, state, actions);

    rel::detail::beginBranchEdit(state, ids.serviceId, "bad name");
    rel::detail::applySaveBranch(store, state);
    std::string branch;
    DOCTEST_REQUIRE(branchInDb(store, ids.envId, &branch));
    DOCTEST_CHECK_EQ(branch, std::string("release/2026"));  // 库值未动
    DOCTEST_CHECK(state.branchEditServiceId == ids.serviceId);  // 缓冲保留可改
    DOCTEST_CHECK(state.branchEditText == std::string("bad name"));
    DOCTEST_CHECK(state.noticeIsError);
    DOCTEST_CHECK(state.notice.find("空格") != std::string::npos);

    // 控制字符同样拒
    rel::detail::beginBranchEdit(state, ids.serviceId,
                                 std::string("a\001b"));
    rel::detail::applySaveBranch(store, state);
    DOCTEST_REQUIRE(branchInDb(store, ids.envId, &branch));
    DOCTEST_CHECK_EQ(branch, std::string("release/2026"));
    DOCTEST_CHECK(state.noticeIsError);

    // 超长拒（>200 字节）
    rel::detail::beginBranchEdit(state, ids.serviceId, std::string(201, 'a'));
    rel::detail::applySaveBranch(store, state);
    DOCTEST_REQUIRE(branchInDb(store, ids.envId, &branch));
    DOCTEST_CHECK_EQ(branch, std::string("release/2026"));
    DOCTEST_CHECK(state.noticeIsError);
    return true;
})

REGISTER_TEST(release_detail_branch_trim_and_empty,
              "分支两端空白 trim 后落库；清空合法（存空串）且校验报「分支未填写」"
              "（防：空白分支原样入库 / 清空分支被当成非法拒绝）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "branchtrim");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Draft);
    DOCTEST_REQUIRE(ids.taskId != 0);

    rel::ReleaseDetailUiState state;
    state.taskId = std::to_string(ids.taskId);
    state.dirty = true;
    rel::ReleaseDetailActions actions;
    Frame frame;
    frame.refresh(store, state, actions);

    rel::detail::beginBranchEdit(state, ids.serviceId, "  release/2027\t");
    rel::detail::applySaveBranch(store, state);
    std::string branch;
    DOCTEST_REQUIRE(branchInDb(store, ids.envId, &branch));
    DOCTEST_CHECK_EQ(branch, std::string("release/2027"));

    // 清空合法：trim 后空 = 「分支未填写」可标记态，由提交校验拦提交而非拦保存
    rel::detail::beginBranchEdit(state, ids.serviceId, "   ");
    rel::detail::applySaveBranch(store, state);
    DOCTEST_REQUIRE(branchInDb(store, ids.envId, &branch));
    DOCTEST_CHECK_EQ(branch, std::string(""));
    frame.refresh(store, state, actions);
    auto configs = store.listTaskConfigs(ids.serviceId);
    DOCTEST_REQUIRE(configs.isOk());
    const std::vector<std::string> missing =
        rel::validateTask(state.services, configs.value());
    DOCTEST_REQUIRE(missing.size() == 1);
    DOCTEST_CHECK(missing[0].find("分支未填写") != std::string::npos);
    return true;
})

REGISTER_TEST(release_detail_branch_non_draft_rejected,
              "越权守卫：Pending/Running/Done/Voided 拒绝保存分支且库值不变"
              "（防：发布态只读铁律被逻辑层绕过）", {
    ThemeGuard guard;
    const rel::TaskStatus statuses[] = {rel::TaskStatus::Pending,
                                        rel::TaskStatus::Running,
                                        rel::TaskStatus::Done,
                                        rel::TaskStatus::Voided};
    for (int i = 0; i < 4; ++i) {
        test_support::TestDb tdb("releasedetail",
                                 ("branchguard" + std::to_string(i)).c_str());
        rel::ReleaseStore store(tdb.handle());
        const Seed ids = seedDetailTask(store, statuses[i]);
        DOCTEST_REQUIRE(ids.taskId != 0);

        rel::ReleaseDetailUiState state;
        state.taskId = std::to_string(ids.taskId);
        state.dirty = true;
        rel::ReleaseDetailActions actions;
        Frame frame;
        frame.refresh(store, state, actions);

        rel::detail::beginBranchEdit(state, ids.serviceId, "release/9999");
        rel::detail::applySaveBranch(store, state);
        std::string branch;
        DOCTEST_REQUIRE(branchInDb(store, ids.envId, &branch));
        DOCTEST_CHECK_EQ(branch, std::string("release/2026"));
        DOCTEST_CHECK(state.noticeIsError);
        DOCTEST_CHECK(state.branchEditServiceId == 0);  // 越权缓冲一并清掉
    }
    return true;
})

REGISTER_TEST(release_detail_branch_change_revokes_merge_confirm,
              "分支变更对账：已确认的合并分支在保存重读后自动撤回并记 revoke 日志"
              "（防：改分支后旧确认残留 / 对账因缓冲直改内存而空转）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "branchrevoke");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Draft);
    DOCTEST_REQUIRE(ids.taskId != 0);
    DOCTEST_REQUIRE(
        store.setStepDone(ids.serviceId, rel::StepKind::MergeBranch, kDoneAt)
            .isOk());

    rel::ReleaseDetailUiState state;
    state.taskId = std::to_string(ids.taskId);
    state.dirty = true;
    rel::ReleaseDetailActions actions;
    Frame frame;
    frame.refresh(store, state, actions);

    rel::detail::beginBranchEdit(state, ids.serviceId, "release/2027");
    rel::detail::applySaveBranch(store, state);
    frame.refresh(store, state, actions);  // 重读触发 applyContentRevocations

    auto steps = store.listTaskSteps(ids.serviceId);
    DOCTEST_REQUIRE(steps.isOk());
    bool mergeDone = false;
    for (const auto& step : steps.value()) {
        if (step.kind == rel::StepKind::MergeBranch && step.doneAt != 0) {
            mergeDone = true;
        }
    }
    DOCTEST_CHECK_FALSE(mergeDone);  // 旧确认已自动撤回
    bool revokeLogged = false;
    for (const auto& log : state.logs) {
        if (log.action == "revoke" && log.note == "服务分支已修改") {
            revokeLogged = true;
        }
    }
    DOCTEST_CHECK(revokeLogged);
    DOCTEST_CHECK(state.notice.find("自动撤回") != std::string::npos);
    return true;
})

REGISTER_TEST(release_detail_branch_entry_mode_gated,
              "分支入口双模式：Draft 为表单行输入，发布态隐藏输入且走流程轨道"
              "（防：发布态仍渲染输入框破坏 F-03 只读）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "branchgate");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Draft);
    DOCTEST_REQUIRE(ids.taskId != 0);

    rel::ReleaseDetailUiState state;
    state.taskId = std::to_string(ids.taskId);
    state.dirty = true;
    rel::ReleaseDetailActions actions;
    Frame frame;
    frame.refresh(store, state, actions);

    const std::string svcBase = branchSvcBase(ids);
    DOCTEST_CHECK(frame.ui.find(svcBase + ".branch.input.hit") != nullptr);
    DOCTEST_CHECK(frame.ui.find(svcBase + ".branch.label") != nullptr);

    // 提交转发布态：输入框隐藏，页面切流程轨道（分支展示面随服务卡一并退出，
    // 池 53 IA 既有路由）
    rel::Task next = state.task;
    next.status = rel::TaskStatus::Pending;
    DOCTEST_REQUIRE(store.updateTask(next).isOk());
    state.dirty = true;
    frame.refresh(store, state, actions);
    DOCTEST_CHECK(frame.ui.find(svcBase + ".branch.input.hit") == nullptr);
    DOCTEST_CHECK(frame.ui.find(svcBase + ".branch.label") == nullptr);
    DOCTEST_CHECK(frame.ui.find("release.detail.track") != nullptr);
    return true;
})

REGISTER_TEST(release_detail_branch_real_input_chain,
              "分支真实输入链：onTextInput→onChange 写缓冲，Enter→onEnter 落库"
              "（防：draw 绑定错服务 id / 回调漏接导致打字不进缓冲）", {
    ThemeGuard guard;
    test_support::TestDb tdb("releasedetail", "branchchain");
    rel::ReleaseStore store(tdb.handle());
    const Seed ids = seedDetailTask(store, rel::TaskStatus::Draft);
    DOCTEST_REQUIRE(ids.taskId != 0);

    rel::ReleaseDetailUiState state;
    state.taskId = std::to_string(ids.taskId);
    state.dirty = true;
    rel::ReleaseDetailActions actions;
    Frame frame;
    frame.refresh(store, state, actions);

    const std::string inputId = branchSvcBase(ids) + ".branch.input";
    DOCTEST_REQUIRE(typeIntoBranchInput(frame.ui, inputId, "x"));
    DOCTEST_CHECK(state.branchEditServiceId == ids.serviceId);
    // input 为「光标处插入」语义（components/input.h InputModel::insertAtCursor），
    // 初值聚焦后的光标位不在这里锁——只锁缓冲确实吃到了输入字符。
    DOCTEST_CHECK(state.branchEditText.find("x") != std::string::npos);
    const std::string typed = state.branchEditText;
    DOCTEST_REQUIRE(pressEnterOnBranchInput(frame.ui, inputId));
    DOCTEST_CHECK(state.branchEditServiceId == 0);
    frame.refresh(store, state, actions);

    std::string branch;
    DOCTEST_REQUIRE(branchInDb(store, ids.envId, &branch));
    DOCTEST_CHECK_EQ(branch, typed);  // 落库值 == 提交时缓冲全文（真实链路证据）
    return true;
})
