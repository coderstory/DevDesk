// src/module_schedule/schedule_module.cpp
// 定时调度模块宿主：Module 接口 + onStart 装配（store / 渠道注册表 /
// ScheduleEngine）+ onStop 释放。引擎在 onStart 订阅全局唯一 1Hz ticker 泵
//（§2.4 反重构：禁逐规则重注册），onStop 先停引擎再释放其引用的装配。
#include "module_schedule/schedule_module.h"

#include "core/database.h"
#include "core/logging.h"
#include "core/theme.h"
#include "core/time.h"
#include "module_schedule/channel_registry.h"
#include "module_schedule/schedule_engine.h"
#include "module_schedule/schedule_provider.h"
#include "module_schedule/schedule_store.h"
#include "module_schedule/schedule_ui_list.h"

#include <memory>

namespace module::schedule {

struct ScheduleModule::Impl {
    bool started = false;
    std::unique_ptr<ScheduleStore> store;
    std::unique_ptr<ChannelRegistry> channels;
    std::unique_ptr<ScheduleProviderRegistry> providers;
    // 引擎持有 store 的 DB 连接与 channels 的引用，声明放最后 = 最先析构；
    // listUi 持有 engine->taskRegistry() 指针（编辑弹窗 run_task 下拉用），
    // 必须先于引擎析构 → 声明在 engine 之后。
    std::unique_ptr<ScheduleEngine> engine;
    std::unique_ptr<ScheduleListUi> listUi;
};

ScheduleModule::ScheduleModule() : impl_(std::make_unique<Impl>()) {
    // 注册表构造期即建：装配段（main.cpp）在 onStart 前注册系统来源
    impl_->providers = std::make_unique<ScheduleProviderRegistry>();
}

module::schedule::ScheduleProviderRegistry& ScheduleModule::providerRegistry() {
    return *impl_->providers;
}

ScheduleModule::~ScheduleModule() = default;

::core::PageId ScheduleModule::pageId() const { return ::core::PageId::Schedule; }
std::string ScheduleModule::displayName() const { return "定时调度"; }
std::string ScheduleModule::shortName() const { return "定时"; }
std::string ScheduleModule::iconGlyph() const { return eui::utf8(0xF017); }  // fa-clock

void ScheduleModule::onStart() {
    // 重入幂等：double-onStart = no-op
    if (impl_->started) return;
    auto* db = ::core::appDatabase();
    if (!db) {
        LOG_WARN("schedule 模块：数据库尚未注册，跳过装配");
        return;
    }
    impl_->store = std::make_unique<ScheduleStore>(*db);
    impl_->channels = std::make_unique<ChannelRegistry>();
    registerBuiltinChannels(*impl_->channels);
    // 引擎先于 listUi 创建：列表编辑弹窗的 run_task 载荷需要引擎暴露的
    // 任务注册表（taskKeys 下拉）；start() 仍在首次 reload 之后。
    impl_->engine = std::make_unique<ScheduleEngine>(*db, *impl_->channels);
    impl_->listUi =
        std::make_unique<ScheduleListUi>(*impl_->store, *impl_->providers,
                                         &impl_->engine->taskRegistry());
    impl_->listUi->reload(static_cast<int64_t>(core::time::nowUnix()));
    // start() 订阅唯一 1Hz 泵。
    impl_->engine->start();
    impl_->started = true;
    LOG_INFO("schedule 模块启动（渠道 5 / 系统来源 0，调度引擎 1Hz 已挂泵）");
}

void ScheduleModule::onStop() {
    // 重入幂等：未启动的 off = no-op
    if (!impl_->started) return;
    // 先停引擎（注销 ticker 订阅、清内存队列；落库 pending/running 行保留），
    // 再释放它引用的 registry/store——逆序释放防悬垂引用。
    impl_->engine->stop();
    impl_->engine.reset();
    impl_->listUi.reset();
    impl_->channels.reset();
    impl_->store.reset();
    impl_->started = false;
    LOG_INFO("schedule 模块停止");
}

void ScheduleModule::compose(eui::Ui& ui, const eui::Screen& screen) {
    if (!impl_->started || !impl_->listUi) {
        // 未启动（DB 缺失等）：空页兜底，不崩
        const auto& t = core::theme::current();
        ui.text("schedule.list.unstarted")
            .position(16.0f, 16.0f)
            .size(screen.width - 32.0f, 20.0f)
            .text("定时模块未启动（数据库不可用）")
            .fontSize(t.sizes.fontSizeSmall)
            .color(t.colors.textTertiary)
            .build();
        return;
    }
    impl_->listUi->compose(ui, screen);
}

} // namespace module::schedule

extern "C" ::core::Module* create_schedule_module() {
    return new module::schedule::ScheduleModule();
}
