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

    // 页面级键盘事件（P7a 键盘移动）：EUI-NEO 无聚焦元素消费时经
    // DslAppConfig::onKeyEvent → Router::dispatchKeyEvent 转发到当前页模块。
    // 返回 true 表示已消费（当前仅约定语义，EUI-NEO 回调无返回值）。
    virtual bool onKeyEvent(const eui::KeyEvent& event) { return false; }

    virtual void onStart() {}
    virtual void onStop() {}
};

} // namespace core