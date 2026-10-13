// src/core/module_interface.h
#pragma once

#include <string>

#include <eui/dsl.h>
#include <eui/types.h>

namespace core {

// 枚举序 = 侧边栏顺序 + Router 分发编号，是跨模块契约（重排会静默改变默认导航顺序）。
enum class PageId {
    Home,
    Markdown,
    Todo,
    ClaudeProxy,
    Agent,       // 位于 Settings 之前 = 侧边栏顺序
    Settings,
    About,
};

inline const char* pageIdToString(PageId id) {
    switch (id) {
        case PageId::Home: return "Home";
        case PageId::Markdown: return "Markdown";
        case PageId::Todo: return "Todo";
        case PageId::ClaudeProxy: return "ClaudeProxy";
        case PageId::Agent: return "Agent";
        case PageId::Settings: return "Settings";
        case PageId::About: return "About";
    }
    return "?";
}

class Module {
public:
    virtual ~Module() = default;

    virtual PageId pageId() const = 0;
    virtual std::string displayName() const = 0;

    // 显示短名（托盘/关闭提示等空间受限场景），默认回落 displayName()
    virtual std::string shortName() const { return displayName(); }

    // FontAwesome 7 Free-Solid 图标码点的 UTF-8 编码；码点须存在于 EUI-NEO 内置字体
    virtual std::string iconGlyph() const = 0;

    virtual void compose(eui::Ui& ui, const eui::Screen& screen) = 0;

    // 页面级键盘事件：焦点元素未消费的按键经 Router 派发（模块级快捷键走此通道）；返回 true = 已消费
    virtual bool onKeyEvent(const eui::KeyEvent& event) {
        (void)event;
        return false;
    }

    // 页面进入通知：navigateTo 切到本模块页面时回调（含同页重入）；启动首次进入走 onStart
    virtual void onPageEnter() {}

    virtual void onStart() {}
    virtual void onStop() {}
};

} // namespace core