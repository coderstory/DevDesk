#pragma once

#include <functional>
#include <string>
#include <vector>

#include "module_interface.h"

namespace core {

class Router {
public:
    Router() = default;

    // 注册模块（活门控）：重复指针/nullptr = 幂等 no-op。按 pageId 升序插入（相等时稳定插在等值区间
    // 后）→ modules() 遍历序 == 侧边栏渲染序 == 表序；PageId 枚举序须与 kModuleRegistry 表序一致
    void registerModule(Module* module);

    // 注销模块（活门控）：移除首个匹配指针，nullptr/未注册 = 幂等 no-op；onStop 由调用方先行，
    // 当前页被注销后 currentPage 不自动改
    void unregisterModule(Module* module);

    // 导航到目标页；payload 为可选跨模块跳转负载（默认空 = 纯导航）。非空 payload 挂到目标页上，
    // 等目标页 compose 里 consumePendingPayload() 取走。格式："action:<指令>" / "open:<相对路径>"
    void navigateTo(PageId page, const std::string& payload = "");
    PageId currentPage() const;

    // 页面区原点 x（窗口坐标）：row(侧边栏, app.page) 使模块内坐标原点是页面区左上而非窗口左上，
    // 模块换算指针位置时减去本值（y 方向原点恒 0）。app 层每帧写入
    float pageOriginX() const { return pageOriginX_; }
    void setPageOriginX(float x) { pageOriginX_ = x; }

    // 已注册模块的只读访问（侧边栏渲染用）
    const std::vector<Module*>& modules() const { return modules_; }

    // 页面是否已注册（main 门控注册后，Home 降级渲染/判断导航目标可达都查这里）
    bool isRegistered(PageId page) const;

    // 取出并清空某页的跳转负载（一次性消费）：空串 = 无待消费；页面隔离，别页取不走；纯导航不清除
    std::string consumePendingPayload(PageId page);

    // 焦点元素未消费的按键派发到当前页模块（EUI-NEO app 级 onKeyEvent → main.cpp → 这里）；返回是否消费
    bool dispatchKeyEvent(const eui::KeyEvent& key);

    // navigateTo 会回调目标模块的 Module::onPageEnter（进页通知，含同页重入），模块可借此重查数据

    void compose(eui::Ui& ui, const eui::Screen& screen);

    // 空态兜底渲染回调（app 层注入）：当前页无对应模块时调用本回调；未注入 = compose 静默返回
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

// 进程级 Router 共享访问器（与 registerAppDatabase 同模式，module_* 互不依赖）；
// appRouter() 返回 nullptr = 尚未注册（调用方跳过导航只记日志）
void registerAppRouter(Router* router);
Router* appRouter();

} // namespace core