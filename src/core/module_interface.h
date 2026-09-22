// src/core/module_interface.h
#pragma once

#include <string>

#include <eui/dsl.h>
#include <eui/types.h>

namespace core {

enum class PageId {
    Home,
    Markdown,
    Todo,
    ClaudeProxy,
    Settings,
};

inline const char* pageIdToString(PageId id) {
    switch (id) {
        case PageId::Home: return "Home";
        case PageId::Markdown: return "Markdown";
        case PageId::Todo: return "Todo";
        case PageId::ClaudeProxy: return "ClaudeProxy";
        case PageId::Settings: return "Settings";
    }
    return "?";
}

class Module {
public:
    virtual ~Module() = default;

    virtual PageId pageId() const = 0;
    virtual std::string displayName() const = 0;
    virtual std::string iconGlyph() const = 0;

    virtual void compose(eui::Ui& ui, const eui::Screen& screen) = 0;

    // 页面级键盘事件：焦点元素没消费的按键，经 app 级 onKeyEvent →
    // Router::dispatchKeyEvent 派发到当前页模块。返回 true = 已消费。
    // 背景：EUI-NEO InputBuilder 无逐组件 onKeyEvent passthrough（Phase 2
    // 上游 PR），模块级快捷键（Todo Ctrl+↑/↓ 移动、Markdown Ctrl+Shift+P
    // 三态切换，不涉及编辑器文本变换）走这条全局通道。
    virtual bool onKeyEvent(const eui::KeyEvent& event) {
        (void)event;
        return false;
    }

    virtual void onStart() {}
    virtual void onStop() {}
};

} // namespace core