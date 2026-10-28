// src/module_memory/memory_module.h
// 记忆页模块：telemem 记忆引擎的托管面板（S2 骨架）。Router 页面契约实现，
// 插拔模式与 module_wallpaper 同族；引擎托管逻辑在 engine/ 子层，本类只做 UI 编排。
#pragma once

#include <memory>
#include <string>

#include "core/module_interface.h"

namespace core::theme {
struct Tokens;  // 前置声明：三视图 compose 按 const& 传 token，头不拖 theme 全量
}

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
    // 三视图 compose 拆分（x=页左边距，w=内容宽，y=视图体起始 y）
    void composeConsole(eui::Ui& ui, const core::theme::Tokens& t, float x,
                        float w, float y);
    void composeReview(eui::Ui& ui, const core::theme::Tokens& t, float x,
                       float w, float y);
    void composeConfig(eui::Ui& ui, const core::theme::Tokens& t, float x,
                       float w, float y);

    // 当前视图下标：0=主控台 1=记忆审核 2=配置（S2 三视图骨架）
    int activeView_ = 0;
};

} // namespace module::memory
