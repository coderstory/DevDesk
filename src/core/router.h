// src/core/router.h
#pragma once

#include <string>
#include <vector>

#include "module_interface.h"

namespace core {

class Router {
public:
    Router() = default;

    void registerModule(Module* module);

    // 导航到目标页；payload 为可选的跨模块跳转负载（默认空 = 纯导航，
    // 既有调用零改动兼容）。非空 payload 会挂到目标页上，等目标页在
    // compose 里通过 consumePendingPayload() 取走。
    //   约定格式："action:<指令>"（触发动作）/ "open:<相对路径>"（打开资源）
    void navigateTo(PageId page, const std::string& payload = "");
    PageId currentPage() const;

    // 页面区原点 x（窗口坐标）：app 层布局 row(侧边栏, app.page) 把页面子树
    // 放在侧边栏右侧，模块 compose 内元素/浮层的坐标原点因此是页面区左上而非
    // 窗口左上；而指针事件携带的是窗口坐标（runtime_input 只做 /dpiScale）。
    // 模块把指针位置换算成菜单/浮层坐标时减去本值（y 方向页面区纵跨整窗，
    // 原点恒 0，无需换算）。app 层每帧 compose 写入（app_sidebar.cpp）；
    // 默认 0 = 未接入时行为与旧版一致（无偏移）。
    float pageOriginX() const { return pageOriginX_; }
    void setPageOriginX(float x) { pageOriginX_ = x; }

    // 已注册模块的只读访问（侧边栏渲染 displayName/iconGlyph/pageId 用）
    const std::vector<Module*>& modules() const { return modules_; }

    // 页面是否已注册（模块插拔方案 A：main 按 enabledModules 门控注册后，
    // Home 降级渲染 / 调用方判断导航目标可达都查这里）
    bool isRegistered(PageId page) const;

    // 取出并清空挂在某页上的跳转负载（一次性消费语义）：
    //   - 返回空串表示该页没有待消费负载
    //   - 取过一次后再次调用返回空（compose 每帧调用不会重复触发）
    //   - 页面隔离：别页取不走，也不清掉本页挂起的负载
    //   - 纯导航（payload 为空）不会清除已挂起的负载
    std::string consumePendingPayload(PageId page);

    // 焦点元素没消费的按键派发到当前页模块（Module::onKeyEvent，默认 false
    // = 不消费）。通道：EUI-NEO app 级 onKeyEvent（runtime_input 只把焦点
    // 元素未消费的键回调出来）→ main.cpp → 这里。返回模块是否消费。
    bool dispatchKeyEvent(const eui::KeyEvent& key);

    // navigateTo 会回调目标模块的 Module::onPageEnter（进页通知，
    // 含同页重入），模块可借此重查数据。默认空实现零改动兼容。

    void compose(eui::Ui& ui, const eui::Screen& screen);

    void startAll();
    void stopAll();

private:
    std::vector<Module*> modules_;
    PageId current_ = PageId::Home;
    float pageOriginX_ = 0.0f;  // 页面区原点 x（app_sidebar compose 每帧写入）

    // 跨模块跳转负载（单槽）：navigateTo 带 payload 时记录目标页，
    // 目标页 compose 时取走。同一时刻最多一个挂起负载。
    PageId pendingPayloadPage_ = PageId::Home;
    std::string pendingPayload_;
};

// ============ 进程级 Router 共享访问器 ============
//
// main.cpp 装配时调用 registerAppRouter()，module_* 层通过 appRouter()
// 触发页面导航（与 database.h 的 registerAppDatabase/appDatabase 同一模式，
// 保持 module_* 之间互不依赖）。
//
// 返回 nullptr 表示尚未注册（调用方应跳过导航，只记日志）。
void registerAppRouter(Router* router);
Router* appRouter();

} // namespace core