// src/core/router.h
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "module_interface.h"

namespace core {

class Router {
public:
    Router() = default;

    // 注册模块（活门控）：重复注册同一指针 = 幂等 no-op，nullptr = no-op。顺序语义（契约以本处为准）：
    // 插入位置按 Module::pageId() 升序，pageId 相等时稳定插在等值区间之后——故 modules() 遍历序 ==
    // 侧边栏渲染序 == 表序，与注册先后无关（off→on 重注册回原位）。单点真相：PageId 枚举序 ==
    // kModuleRegistry 表序，改侧边栏顺序须同步改枚举序 + 表序
    void registerModule(Module* module);

    // 注销模块（活门控）：移除首个匹配指针；nullptr/未注册 = 幂等 no-op。只动注册表，不负责生命周期
    //（onStop 由调用方先行调用）；当前页被注销后 currentPage 不自动改（重定向是 app 层职责）
    void unregisterModule(Module* module);

    // 导航到目标页；payload 为可选跨模块跳转负载（默认空 = 纯导航）。非空 payload 挂到目标页上，
    // 等目标页 compose 里 consumePendingPayload() 取走。格式："action:<指令>" / "open:<相对路径>"
    void navigateTo(PageId page, const std::string& payload = "");
    PageId currentPage() const;

    // 页面区原点 x（窗口坐标）：row(侧边栏, app.page) 使模块 compose 内元素/浮层的坐标原点是页面区
    // 左上而非窗口左上，而指针事件携带窗口坐标。模块换算指针位置时减去本值（y 方向原点恒 0）。
    // app 层每帧写入；默认 0 = 未接入时行为与旧版一致
    float pageOriginX() const { return pageOriginX_; }
    void setPageOriginX(float x) { pageOriginX_ = x; }

    // 已注册模块的只读访问（侧边栏渲染用）
    const std::vector<Module*>& modules() const { return modules_; }

    // 页面是否已注册（main 门控注册后，Home 降级渲染/判断导航目标可达都查这里）
    bool isRegistered(PageId page) const;

    // 取出并清空某页的跳转负载（一次性消费）：空串 = 无待消费负载；取过即空（compose 每帧调不重复
    // 触发）；页面隔离，别页取不走也不清本页负载；纯导航不清除已挂起负载
    std::string consumePendingPayload(PageId page);

    // 焦点元素没消费的按键派发到当前页模块（Module::onKeyEvent，默认 false = 不消费）。通道：
    // EUI-NEO app 级 onKeyEvent（只回调焦点元素未消费的键）→ main.cpp → 这里。返回模块是否消费
    bool dispatchKeyEvent(const eui::KeyEvent& key);

    // navigateTo 会回调目标模块的 Module::onPageEnter（进页通知，含同页重入），模块可借此重查数据

    void compose(eui::Ui& ui, const eui::Screen& screen);

    // 空态兜底渲染回调（app 层注入）：当前页没有对应模块时 Router 不再自行绘制（core 只派发不绘制——
    // eui::Ui/eui::Screen 仅可透传），改为调用本回调。未注入 = compose 静默返回不崩
    using FallbackCompose = std::function<void(eui::Ui&, const eui::Screen&)>;
    void setFallbackCompose(FallbackCompose fn);

    void startAll();
    void stopAll();

private:
    std::vector<Module*> modules_;
    PageId current_ = PageId::Home;
    float pageOriginX_ = 0.0f;  // 页面区原点 x（app_sidebar compose 每帧写入）

    // 跨模块跳转负载（单槽）：同一时刻最多一个挂起负载
    PageId pendingPayloadPage_ = PageId::Home;
    std::string pendingPayload_;

    // 空态兜底渲染回调：空 = 未注入 → compose 不绘制（见 setFallbackCompose）
    FallbackCompose fallbackCompose_;
};

// 进程级 Router 共享访问器（同 database.h 的 registerAppDatabase/appDatabase 模式，保持 module_*
// 互不依赖）。main.cpp 装配时注册；appRouter() 返回 nullptr = 尚未注册（调用方跳过导航只记日志）
void registerAppRouter(Router* router);
Router* appRouter();

} // namespace core