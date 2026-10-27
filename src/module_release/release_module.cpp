// 上线模块两个 Router 页面（骨架）：空态 = 页面标题 + 「暂无数据」，证明侧边栏 →
// Router → 模块 compose 整条链路可达。
#include "release_module.h"

#include "core/module_interface.h"
#include "core/theme.h"

#include <eui/dsl.h>

#include <string>

namespace module::release {

namespace {

// 空态页：标题 + 居中占位文案。两个页面共用，只有 pageKey/标题不同。
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

class ReleaseTasksModule final : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::ReleaseTasks; }
    std::string displayName() const override { return "任务"; }
    std::string shortName() const override { return "任务"; }
    std::string iconGlyph() const override { return eui::utf8(0xF0AE); }

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        composeEmptyPage(ui, screen, kReleaseTasksPageKey, "任务");
    }
};

class ReleaseConfigModule final : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::ReleaseConfig; }
    std::string displayName() const override { return "基础配置"; }
    std::string shortName() const override { return "基础配置"; }
    std::string iconGlyph() const override { return eui::utf8(0xF013); }

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        composeEmptyPage(ui, screen, kReleaseConfigPageKey, "基础配置");
    }
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