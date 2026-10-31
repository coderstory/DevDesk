// 上线模块两个 Router 页面：任务列表走 release_list_ui 入口，基础配置走
// release_config_ui 入口（不再自绘占位页）。
//
// 契约：
//   - 数据访问唯一出口 = 进程级 appDatabase() 共享连接；ReleaseStore 只持有
//     SqliteDb 引用，不是新连接（新开连接会与 schema / 事务路径分叉）。
//   - db 未注册时退回骨架空态（任务页用 release_list_ui 的兼容重载，配置页用
//     本文件空态），保证并行开发期页面仍可渲染、既有冒烟用例的节点契约不破。
//   - 任务页两态：openTaskId_ 为空 = 列表页；有值 = 执行页（composeReleaseDetailPage）。
//     进入（卡片 / 新建成功 / 提醒「去处理」）与返回（页头「← 返回」/ Esc）都只改
//     openTaskId_ 这一份状态；detailState_.taskId 只是每帧由它派生的副本。
//     弹层打开时 Esc 先关弹层。
//   - 提醒引擎 ReleaseReminder 由本模块持有、必须比 ReleaseStore 活得短：
//     经 core::ticker 的全局帧泵按秒推进（intervalSec=1，与当前页面无关；
//     引擎内部按秒幂等、冷却去重），onStop 先注销订阅再 shutdown/释放。提醒窗
//     的「去处理」只置 openTaskId_ + 导航；Router 缺失时仅置 openTaskId_
//     （下一次 compose 仍会进入该任务详情）。
#include "release_module.h"

#include "core/database.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/router.h"
#include "core/theme.h"
#include "core/ticker.h"

#include "release_config_ui.h"
#include "release_detail_ui.h"
#include "release_dialogs.h"
#include "release_list_ui.h"
#include "release_reminder.h"
#include "release_store.h"

#include <eui/dsl.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace module::release {

namespace {

// 空态页：标题 + 居中占位文案。
// 现存唯一调用方：ReleaseConfigModule::compose 在 db 未注册时的兜底
//（任务页走 release_list_ui 的骨架兼容重载，不经过本函数）。
void composeEmptyPage(eui::Ui& ui, const eui::Screen& screen,
                      const char* pageKey, const char* title) {
    const auto& t = ::core::theme::current();
    const float pad = t.sizes.spacingLarge * 2.0f;
    ui.stack(pageKey)
        .size(screen.width, screen.height)
        .content([&] {
            ui.text(std::string(pageKey) + ".title")
                .position(pad, pad)
                .size(screen.width - pad * 2.0f, 32.0f)
                .text(title)
                .fontSize(t.sizes.fontSizeTitle)
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            ui.text(std::string(pageKey) + ".empty")
                .position(pad, pad + 48.0f)
                .size(screen.width - pad * 2.0f, 24.0f)
                .text("暂无数据")
                .fontSize(t.sizes.fontSizeBody)
                .color(t.colors.textTertiary)
                .build();
        })
        .build();
}

// 共享连接建 store：db 未注册返回 nullptr（调用方画空态）。只取已有连接，
// 绝不在此新开 SqliteDb —— 进程内单连接是与 schema 迁移/事务路径的公共前提。
std::unique_ptr<ReleaseStore> makeReleaseStore() {
    auto* db = ::core::appDatabase();
    if (db == nullptr) return nullptr;
    return std::make_unique<ReleaseStore>(*db);
}

class ReleaseTasksModule final : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::ReleaseTasks; }
    std::string displayName() const override { return "任务"; }
    std::string shortName() const override { return "任务"; }
    std::string iconGlyph() const override { return eui::utf8(0xF0AE); }

    // 提醒引擎挂到全局 ticker：模块启用即订阅（1 秒节流），与当前停留页面无关。
    // 首次 pump 在启动首帧立即推进，演示模式的提醒窗因此无需额外的启动 tick。
    void onStart() override {
        if (!store_) store_ = makeReleaseStore();
        ensureReminder();
        if (reminder_) {
            tickerHandle_ = ::core::ticker::subscribe(
                {.name = "release.reminder",
                 .fn =
                     [](::core::ticker::Now now, void* user) {
                         static_cast<ReleaseTasksModule*>(user)->reminder_->tick(
                             now);
                     },
                 .user = this,
                 .firstAt = 0,
                 .intervalSec = 1,
                 .expireAt = 0});
        }
    }

    void onStop() override {
        // 先摘全局 ticker 订阅（此后不再被 pump 到），再断窗回调并释放。
        // 之后若模块重启，onStart 会重新建并重新订阅。
        ::core::ticker::unsubscribe(tickerHandle_);
        tickerHandle_ = 0;
        if (reminder_) {
            reminder_->shutdown();
            reminder_.reset();
        }
    }

    // Esc 与页头「← 返回」同义（弹层打开时先关弹层，与其它模块的 Esc 语义一致）。
    // 焦点元素（输入框等）已消费的 Esc 不会到这里；配置编辑层的输入框聚焦时 Esc
    // 由输入框 onCancel 关层（同语义），未聚焦时走这里的 editorOpen 分支。
    bool onKeyEvent(const eui::KeyEvent& key) override {
        if (!key.isDown() || key.key != eui::InputKey::Escape) return false;
        if (!openTaskId_) return false;
        if (detailState_.menuOpen) {
            detailState_.menuOpen = false;
            return true;
        }
        if (detailState_.editorOpen) {
            detailState_.editorOpen = false;
            return true;
        }
        if (detailState_.confirm != ReleaseDetailConfirm::None) {
            // 与页面内「取消」按钮同语义：只关弹层，不改校验标红状态。
            detailState_.confirm = ReleaseDetailConfirm::None;
            return true;
        }
        openTaskId_.reset();
        return true;
    }

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        // onStart 未跑（无头测试 / 惰性装配）时补建；db 未注册则画骨架兼容空态
        if (!store_) store_ = makeReleaseStore();
        if (!store_) {
            composeReleaseListPage(ui, screen);
            return;
        }
        ensureReminder();

        if (openTaskId_) {
            // taskId 每帧写入，页面内按 loadedTaskId 检测切换，切换时自行重读。
            detailState_.taskId = std::to_string(*openTaskId_);
            ReleaseDetailActions actions;
            // 空态「去基础配置」出口（任务无环境时的就近导航，F-16）。
            actions.onOpenConfig = [] {
                auto* router = ::core::appRouter();
                if (router != nullptr) {
                    router->navigateTo(::core::PageId::ReleaseConfig);
                }
            };
            // 配置查看 / 编辑由详情页页内自绘全屏编辑层承担，因此不再注入
            // viewConfig（该回调保留为可选扩展口，详情页不调用）。
            // 页头「← 返回」的唯一实现：清空 openTaskId_ 即回列表（openTaskId_
            // 是本模块进/出详情的唯一状态源）。置 dirty 使下次进入任意任务详情
            // 都重读库，吃到详情页期间的写操作。
            // 返回后列表滚动位置回到顶部：列表元素在详情页期间不参与 compose，
            // runtime 会在结构切换后回收其 scrollState；已加载分页与卡片由
            // ui.state 缓存保留，不会重新从首屏分页。
            actions.onBack = [this] {
                openTaskId_.reset();
                detailState_.dirty = true;
            };
            composeReleaseDetailPage(ui, screen, *store_, detailState_, actions);
            return;
        }

        ReleaseListActions actions;
        actions.openTask = [this](std::int64_t taskId) {
            // 点卡片 / 新建成功都走这里：列表 → 执行页。
            openTaskId_ = taskId;
            // 执行页只在 dirty 或 taskId 变化时读库；同任务重入也要重读。
            detailState_.dirty = true;
        };
        composeReleaseListPage(ui, screen, *store_, newTaskState_, actions);
    }

private:
    // 首次装配后建一次；store 缺失时（db 未注册）不建，因为引擎构造要求有效引用。
    void ensureReminder() {
        if (!store_ || reminder_) return;
        reminder_ = std::make_unique<ReleaseReminder>(
            *store_, [this](std::int64_t taskId, const std::string& scope) {
                // 「去处理」= 打开对应任务的执行页。scope（"task" / 环境名）只用于
                // 定位提醒条目，执行页按任务 id 展示，无需按 scope 分流。
                (void)scope;
                openTaskId_ = taskId;
                // F-23：Pending/Running 落发布态轨道当前步（滚动+高亮）；
                // Draft 打开详情即编辑态，无需锚点。
                auto tasks = store_->listTasks();
                if (tasks.isOk()) {
                    for (const auto& task : tasks.value()) {
                        if (task.id != taskId) continue;
                        if (task.status == TaskStatus::Pending ||
                            task.status == TaskStatus::Running) {
                            detailState_.pendingAnchor = "publish.current";
                        }
                        break;
                    }
                }
                detailState_.dirty = true;
                auto* router = ::core::appRouter();
                if (router == nullptr) {
                    // Router 未装配（无头测试 / 惰性装配）：只置 openTaskId_，
                    // 下一次 compose 即进入该任务详情，不做导航。
                    LOG_WARN("Release：Router 未装配，提醒「去处理」仅在任务页内切详情");
                    return;
                }
                router->navigateTo(::core::PageId::ReleaseTasks);
            });
    }

    // store 必须先于页面元素树存在且活得更久（模块成员）。
    std::unique_ptr<ReleaseStore> store_;
    // 提醒引擎引用 store，析构顺序：reminder_ 先于 store_（成员逆序销毁）。
    std::unique_ptr<ReleaseReminder> reminder_;
    // 全局 ticker 句柄（onStart 订阅、onStop 注销；0 = 未订阅）。
    ::core::ticker::Handle tickerHandle_ = 0;
    // 新建任务弹层状态跨帧复用（弹层输入 / 选中环境不能每帧重置）。
    ReleaseNewTaskDialogState newTaskState_;
    // 执行页会话态跨帧复用（滚动 / 弹层 / 校验结果不能每帧重置）。
    ReleaseDetailUiState detailState_;
    // 空 = 列表页；有值 = 执行页。进（卡片 / 新建成功 / 提醒去处理）与出
    //（页头返回 / Esc）的唯一状态源；detailState_.taskId 是每帧派生的副本。
    std::optional<std::int64_t> openTaskId_;
};

class ReleaseConfigModule final : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::ReleaseConfig; }
    std::string displayName() const override { return "基础配置"; }
    std::string shortName() const override { return "基础配置"; }
    std::string iconGlyph() const override { return eui::utf8(0xF013); }

    void onStart() override {
        if (!store_) store_ = makeReleaseStore();
    }

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        if (!store_) store_ = makeReleaseStore();
        if (!store_) {
            composeEmptyPage(ui, screen, kReleaseConfigPageKey, "基础配置");
            return;
        }

        // dirty 读库 / 写回调置 dirty 的节奏全由页面入口内部管理，模块不插手。
        composeReleaseConfigPage(ui, screen, *store_, state_);
    }

private:
    std::unique_ptr<ReleaseStore> store_;
    // 页内 UI 状态（Tab / 表单 / 删除确认 / 列表快照）必须跨帧复用。
    ReleaseConfigUiState state_;
};

} // namespace

} // namespace module::release

// 分组父项不可导航：按契约返回 nullptr，装配层跳过注册（侧边栏树用登记表行承载父级）。
extern "C" ::core::Module* create_release_group_module() { return nullptr; }

extern "C" ::core::Module* create_release_tasks_module() {
    return new module::release::ReleaseTasksModule();
}

extern "C" ::core::Module* create_release_config_module() {
    return new module::release::ReleaseConfigModule();
}