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

    // 已注册模块的只读访问（侧边栏渲染 displayName/iconGlyph/pageId 用）
    const std::vector<Module*>& modules() const { return modules_; }

    // 取出并清空挂在某页上的跳转负载（一次性消费语义）：
    //   - 返回空串表示该页没有待消费负载
    //   - 取过一次后再次调用返回空（compose 每帧调用不会重复触发）
    //   - 页面隔离：别页取不走，也不清掉本页挂起的负载
    //   - 纯导航（payload 为空）不会清除已挂起的负载
    std::string consumePendingPayload(PageId page);

    void compose(eui::Ui& ui, const eui::Screen& screen);

    // 把未被聚焦元素消费的键盘事件转发给当前页模块（P7a 键盘移动）。
    // 返回当前模块是否消费了该事件。
    bool dispatchKeyEvent(const eui::KeyEvent& event);

    void startAll();
    void stopAll();

private:
    std::vector<Module*> modules_;
    PageId current_ = PageId::Home;

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