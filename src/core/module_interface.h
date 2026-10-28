// src/core/module_interface.h
#pragma once

#include <string>

#include <eui/dsl.h>
#include <eui/types.h>

namespace core {

// 枚举序 = 侧边栏顺序 + Router 注册/分发序，是跨模块契约（三处必须同步：本枚举、
// kModuleRegistry 表序、Router::registerModule 的升序插入）。
// PageId 从不持久化（无落库/落配置路径，pageIdToString 只用于日志诊断），因此重排
// 枚举不会让老配置指向错误页面；但仍须同步登记表表序，否则侧边栏序静默漂移。
enum class PageId {
    Home,
    Markdown,
    Todo,
    Schedule,    // 位于 Todo 之后 = 侧边栏顺序（定时调度）
    ClaudeProxy,
    Agent,       // 位于 Settings 之前 = 侧边栏顺序
    Memory,      // 记忆页（telemem 托管面板）；插在 Wallpaper 之前 = 侧边栏「记忆」在「壁纸」上方
    Wallpaper,
    Mcp,         // 位于 Wallpaper 之后、Settings 之前 = 侧边栏顺序（work/system 分组界）
    // 上线模块（多页共用门控键 release）：ReleaseGroup 是不可导航的分组父项
    //（工厂返回 nullptr，装配期跳过注册），只承载侧边栏二级菜单的父级身份。
    // 整组排在 Settings 之前（设置/关于固定收尾在侧边栏最后）。
    ReleaseGroup,
    ReleaseTasks,
    ReleaseConfig,
    Settings,
    About,
};

inline const char* pageIdToString(PageId id) {
    switch (id) {
        case PageId::Home: return "Home";
        case PageId::Markdown: return "Markdown";
        case PageId::Todo: return "Todo";
        case PageId::Schedule: return "Schedule";
        case PageId::ClaudeProxy: return "ClaudeProxy";
        case PageId::Agent: return "Agent";
        case PageId::Memory: return "Memory";
        case PageId::Wallpaper: return "Wallpaper";
        case PageId::Mcp: return "Mcp";
        case PageId::ReleaseGroup: return "ReleaseGroup";
        case PageId::ReleaseTasks: return "ReleaseTasks";
        case PageId::ReleaseConfig: return "ReleaseConfig";
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