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

    // 模块图标：返回 FontAwesome 7 Free-Solid 图标码点的 UTF-8 编码
    //（用 eui::utf8(0xF015) 生成），渲染侧经 TextBuilder::icon(const std::string&)
    // 自动切到 Icon 字体。码点必须存在于 EUI-NEO 内置的
    // assets/Font Awesome 7 Free-Solid-900.otf（main.cpp 侧边栏消费）。
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

    // 页面进入通知：Router::navigateTo 切到本模块页面时回调（含同页重入，
    // 如从别的页切回）。模块可借此重查数据（如 Home 仪表盘进页刷新）。
    // 启动首次进入不经过这里（走 onStart），默认空实现零改动兼容。
    virtual void onPageEnter() {}

    virtual void onStart() {}
    virtual void onStop() {}
};

} // namespace core