// src/module_home/home_module.cpp
// Home 仪表盘模块 - MVP 实现
#include "core/logging.h"
#include "core/module_interface.h"

namespace module::home {

class HomeModule : public core::Module {
public:
    core::PageId pageId() const override { return core::PageId::Home; }
    std::string displayName() const override { return "首页"; }
    std::string iconGlyph() const override { return "🏠"; }

    void compose(core::eui::Ui& ui, const core::eui::Screen& screen) override {
        LOG_DEBUG("Home 模块 compose 被调用");
        (void)ui;
        (void)screen;
    }

    void onStart() override { LOG_INFO("Home 模块启动"); }
    void onStop() override { LOG_INFO("Home 模块停止"); }
};

} // namespace module::home

extern "C" core::Module* create_home_module() {
    return new module::home::HomeModule();
}