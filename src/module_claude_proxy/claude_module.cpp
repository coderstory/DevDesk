// src/module_claude_proxy/claude_module.cpp
// Claude 配置切换器模块 - MVP 实现
#include "core/logging.h"
#include "core/module_interface.h"

#include <eui/dsl.h>

namespace module::claude {

class ClaudeModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::ClaudeProxy; }
    std::string displayName() const override { return "Claude 切换器"; }
    std::string iconGlyph() const override { return "🔄"; }

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        LOG_DEBUG("Claude 模块 compose 被调用");
        (void)ui;
        (void)screen;
    }

    void onStart() override { LOG_INFO("Claude 模块启动"); }
    void onStop() override { LOG_INFO("Claude 模块停止"); }
};

} // namespace module::claude

extern "C" ::core::Module* create_claude_module() {
    return new module::claude::ClaudeModule();
}