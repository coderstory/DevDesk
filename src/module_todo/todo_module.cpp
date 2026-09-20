// src/module_todo/todo_module.cpp
// Todo 看板模块 - MVP 实现
#include "core/logging.h"
#include "core/module_interface.h"

namespace module::todo {

class TodoModule : public core::Module {
public:
    core::PageId pageId() const override { return core::PageId::Todo; }
    std::string displayName() const override { return "Todo 看板"; }
    std::string iconGlyph() const override { return "✅"; }

    void compose(core::eui::Ui& ui, const core::eui::Screen& screen) override {
        LOG_DEBUG("Todo 模块 compose 被调用");
        (void)ui;
        (void)screen;
    }

    void onStart() override { LOG_INFO("Todo 模块启动"); }
    void onStop() override { LOG_INFO("Todo 模块停止"); }
};

} // namespace module::todo

extern "C" core::Module* create_todo_module() {
    return new module::todo::TodoModule();
}