// src/module_schedule/schedule_module.cpp
// 定时调度模块宿主（§3「宿主/装配」行）：Module 接口 + onStart 装配 +
// onStop 释放。本波不接调度引擎——引擎（S4）合入后，onStart 追加唯一
// ticker 泵订阅（见下方接缝注释），onStop 注销。
#include "module_schedule/schedule_module.h"

#include "core/database.h"
#include "core/logging.h"
#include "core/theme.h"
#include "core/time.h"
#include "module_schedule/channel_registry.h"
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
    std::unique_ptr<ScheduleListUi> listUi;
};

ScheduleModule::ScheduleModule() : impl_(std::make_unique<Impl>()) {}

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
    // 系统定时来源注册表：本波空注册表，release 等来源 S8 起注册
    impl_->providers = std::make_unique<ScheduleProviderRegistry>();
    impl_->listUi = std::make_unique<ScheduleListUi>(*impl_->store,
                                                     *impl_->providers);
    impl_->listUi->reload(static_cast<int64_t>(core::time::nowUnix()));
    impl_->started = true;
    // S4 合入后此处构造并启动 schedule_engine（唯一 ticker 泵订阅）
    LOG_INFO("schedule 模块启动（渠道 4 / 系统来源 0）");
}

void ScheduleModule::onStop() {
    // 重入幂等：未启动的 off = no-op
    if (!impl_->started) return;
    // S4 合入后此处注销 ticker 泵并停止 schedule_engine
    // 释放装配（DB 引用随 store 一起丢弃；无后台线程持有本模块成员，
    // 引擎未接线前无 UAF 面）
    impl_->listUi.reset();
    impl_->providers.reset();
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
