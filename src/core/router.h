// src/core/router.h
#pragma once

#include <vector>

#include "module_interface.h"

namespace core {

class Router {
public:
    Router() = default;

    void registerModule(Module* module);

    void navigateTo(PageId page);
    PageId currentPage() const;

    void compose(eui::Ui& ui, const eui::Screen& screen);

    void startAll();
    void stopAll();

private:
    std::vector<Module*> modules_;
    PageId current_ = PageId::Home;
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