#include "router.h"
#include "logging.h"

// 本文件不含任何 EUI-NEO 头：eui::Ui/Screen/KeyEvent 经 module_interface.h
// 的 header-only 类型桥接透传；空态兜底的绘制归 app 层。

#include <atomic>
#include <utility>

namespace core {

void Router::registerModule(Module* module) {
    if (module == nullptr) return;
    for (auto* m : modules_) {
        if (m == module) return;   // 重复注册 = no-op（避免同页双入口）
    }
    // 按 pageId 升序插入原位：off→on 重注册回表序原位，不 append 末尾
    for (auto it = modules_.begin(); it != modules_.end(); ++it) {
        if ((*it)->pageId() > module->pageId()) {
            modules_.insert(it, module);
            return;
        }
    }
    modules_.push_back(module);
}

void Router::unregisterModule(Module* module) {
    if (module == nullptr) return;
    for (auto it = modules_.begin(); it != modules_.end(); ++it) {
        if (*it == module) {
            modules_.erase(it);
            return;                // 只移除首个匹配，重复调用幂等
        }
    }
}

bool Router::isRegistered(PageId page) const {
    for (auto* m : modules_) {
        if (m && m->pageId() == page) return true;
    }
    return false;
}

void Router::navigateTo(PageId page, const std::string& payload) {
    // 未注册页兜底：残留跳转入口（Home 卡片、文件打开分发等）不送进空白页，
    // 回退 Home（恒注册，无递归）；Home 也没注册则保持当前页不动
    if (!isRegistered(page)) {
        LOG_WARN("跳转到未注册页面 " + std::string(pageIdToString(page)) +
                 (payload.empty() ? std::string()
                                  : "（负载 " + payload + "）") +
                 "，回退 Home");
        if (page != PageId::Home && isRegistered(PageId::Home)) {
            navigateTo(PageId::Home);
        }
        return;
    }
    LOG_INFO(payload.empty()
                 ? "导航到: " + std::string(pageIdToString(page))
                 : "导航到: " + std::string(pageIdToString(page)) + "（负载 " +
                       payload + "）");
    current_ = page;
    // 进页通知：目标模块可借 onPageEnter 重查数据（含同页重入）
    for (auto* m : modules_) {
        if (m && m->pageId() == page) {
            m->onPageEnter();
            break;
        }
    }
    // 仅非空 payload 才记录：纯导航不清除已挂起的负载
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

void Router::setFallbackCompose(FallbackCompose fn) {
    fallbackCompose_ = std::move(fn);
}

void Router::compose(eui::Ui& ui, const eui::Screen& screen) {
    for (auto* m : modules_) {
        if (m && m->pageId() == current_) {
            m->compose(ui, screen);
            return;
        }
    }
    // 无匹配模块：交给 app 层注入的兜底渲染；未注入则不画（静默降级不崩）
    if (fallbackCompose_) {
        fallbackCompose_(ui, screen);
    }
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
    // 由 main.cpp 的 atexit handler 调用，退出时触发各模块 onStop
    for (auto* m : modules_) {
        if (m) {
            LOG_INFO("停止模块: " + m->displayName());
            m->onStop();
        }
    }
}

// 进程级 Router 共享访问器

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