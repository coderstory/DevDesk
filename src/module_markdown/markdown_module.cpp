// src/module_markdown/markdown_module.cpp
// Markdown 笔记本模块 - MVP 实现
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/result.h"
#include "core/router.h"

#include <eui/dsl.h>

#include <filesystem>
#include <fstream>

namespace module::markdown {

// MVP：文件读取 + 简单保存
class MarkdownModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::Markdown; }
    std::string displayName() const override { return "Markdown 笔记本"; }
    std::string iconGlyph() const override { return "📝"; }

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        // 这里会在完整实现时用 EUI-NEO 的 DSL 渲染
        // MVP: 仅日志记录
        LOG_DEBUG("Markdown 模块 compose 被调用");
        (void)ui;
        (void)screen;
    }

    void onStart() override {
        core::paths::ensureDirectoriesExist();
        LOG_INFO("Markdown 模块启动");
    }

    void onStop() override {
        LOG_INFO("Markdown 模块停止");
    }
};

} // namespace module::markdown

extern "C" ::core::Module* create_markdown_module() {
    return new module::markdown::MarkdownModule();
}