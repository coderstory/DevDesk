// src/module_schedule/schedule_module.h
// 定时调度模块（波 S7a）：Module 接口骨架 + 宿主装配。
// 持有 ScheduleStore（DB）、ChannelRegistry（内置四渠道）、
// ScheduleProviderRegistry（系统定时只读来源，S8 接）与列表页 UI。
// 调度引擎（S4 合入）后在本模块 onStart/onStop 接线唯一 ticker 泵。
#pragma once

#include "core/module_interface.h"

#include <memory>

namespace module::schedule {

class ScheduleModule : public ::core::Module {
public:
    ScheduleModule();
    ~ScheduleModule() override;

    ::core::PageId pageId() const override;
    std::string displayName() const override;
    std::string shortName() const override;
    std::string iconGlyph() const override;

    void compose(eui::Ui& ui, const eui::Screen& screen) override;

    void onStart() override;
    void onStop() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace module::schedule
