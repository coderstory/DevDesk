#include "router.h"
#include "logging.h"

#include <eui/dsl.h>

#include <atomic>

namespace core {

void Router::registerModule(Module* module) {
    if (module) modules_.push_back(module);
}

void Router::navigateTo(PageId page, const std::string& payload) {
    LOG_INFO(payload.empty()
                 ? "导航到: " + std::string(pageIdToString(page))
                 : "导航到: " + std::string(pageIdToString(page)) + "（负载 " +
                       payload + "）");
    current_ = page;
    // 仅非空 payload 才记录：纯导航（默认空参）不清除已挂起的负载，
    // 保证既有 navigateTo(page) 调用行为零变化
    if (!payload.empty()) {
        pendingPayloadPage_ = page;
        pendingPayload_ = payload;
    }
}

std::string Router::consumePendingPayload(PageId page) {
    if (pendingPayload_.empty() || pendingPayloadPage_ != page) return {};
    // swap 取出即清空 → 一次性消费
    std::string out;
    out.swap(pendingPayload_);
    return out;
}

PageId Router::currentPage() const { return current_; }

bool Router::dispatchKeyEvent(const eui::KeyEvent& key) {
    for (auto* m : modules_) {
        if (m && m->pageId() == current_) return m->onKeyEvent(key);
    }
    return false;
}

void Router::compose(eui::Ui& ui, const eui::Screen& screen) {
    for (auto* m : modules_) {
        if (m && m->pageId() == current_) {
            m->compose(ui, screen);
            return;
        }
    }
    // 没有匹配 → 显示空
    ui.text("empty").text("无页面").build();
}

void Router::startAll() {
    for (auto* m : modules_) {
        if (m) {
            LOG_INFO("启动模块: " + m->displayName());
            m->onStart();
        }
    }
}

void Router::stopAll() {
    for (auto* m : modules_) {
        if (m) {
            LOG_INFO("停止模块: " + m->displayName());
            m->onStop();
        }
    }
}

// ============ 进程级 Router 共享访问器 ============

namespace {
std::atomic<Router*> g_appRouter{nullptr};
} // namespace

void registerAppRouter(Router* router) {
    g_appRouter.store(router, std::memory_order_release);
}

Router* appRouter() {
    return g_appRouter.load(std::memory_order_acquire);
}

} // namespace core