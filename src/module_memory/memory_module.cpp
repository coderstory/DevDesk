// src/module_memory/memory_module.cpp
#include "memory_module.h"

#include <algorithm>

#include "core/logging.h"
#include "core/theme.h"

#include <eui/dsl.h>

namespace module::memory {

::core::PageId MemoryModule::pageId() const { return ::core::PageId::Memory; }

std::string MemoryModule::displayName() const { return "记忆"; }

std::string MemoryModule::shortName() const { return "记忆"; }

// fa-brain (U+F5DC) 的 UTF-8 字节；须与 app_registry 表行 iconGlyph 列同值。
std::string MemoryModule::iconGlyph() const { return "\xEF\x97\x9C"; }

void MemoryModule::onStart() {
    LOG_INFO("记忆模块已启用：本版本为页面骨架，引擎托管后续批次接入");
}

void MemoryModule::onStop() {
    LOG_INFO("记忆模块已停止");
}

void MemoryModule::compose(eui::Ui& ui, const eui::Screen& screen) {
    const auto& t = core::theme::current();
    constexpr float kTitleH = 32.0f;
    constexpr float kHintH = 24.0f;
    // 页内边距与其余页面同一口径（spacingLarge × 2 = 32），内容宽度随窗口弹性收缩。
    const float pagePad = t.sizes.spacingLarge * 2.0f;
    const float contentW = std::max(0.0f, screen.width - pagePad * 2.0f);

    ui.text("memory.title")
        .position(pagePad, 0.0f)
        .size(contentW, kTitleH)
        .text("记忆")
        .fontSize(t.sizes.fontSizeTitle)
        .lineHeight(kTitleH)
        .fontWeight(700)
        .color(t.colors.textPrimary)
        .build();

    ui.text("memory.hint")
        .position(pagePad, kTitleH + t.sizes.spacingLarge)
        .size(contentW, kHintH)
        .text("记忆引擎（telemem）托管面板：主控台 / 记忆审核 / 配置三视图将在后续提交接入")
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(kHintH)
        .color(t.colors.textSecondary)
        .build();
}

} // namespace module::memory

// 默认开启的普通页面模块：工厂恒非空（无平台能力门槛）。
extern "C" ::core::Module* create_memory_module() {
    return new module::memory::MemoryModule();
}
