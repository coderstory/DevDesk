// src/module_memory/memory_module.h
// 记忆页模块：telemem 记忆引擎的托管面板（S2 骨架）。Router 页面契约实现，
// 插拔模式与 module_wallpaper 同族；引擎托管逻辑在 engine/ 子层，本类只做 UI 编排。
#pragma once

#include <memory>
#include <string>

#include "core/module_interface.h"

namespace module::memory {

class MemoryModule final : public ::core::Module {
public:
    ::core::PageId pageId() const override;
    std::string displayName() const override;
    std::string shortName() const override;
    std::string iconGlyph() const override;
    void compose(eui::Ui& ui, const eui::Screen& screen) override;
    void onStart() override;
    void onStop() override;

private:
    // 当前视图下标：0=主控台 1=记忆审核 2=配置（S2 三视图骨架）
    int activeView_ = 0;
};

} // namespace module::memory
