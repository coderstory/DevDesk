#include "router.h"
#include "logging.h"

#include <eui/dsl.h>

#include <atomic>

namespace core {

void Router::registerModule(Module* module) {
    if (module) modules_.push_back(module);
}

bool Router::isRegistered(PageId page) const {
    for (auto* m : modules_) {
        if (m && m->pageId() == page) return true;
    }
    return false;
}

void Router::navigateTo(PageId page, const std::string& payload) {
    // 未注册页兜底（模块插拔方案 A，设计 §5.3）：目标模块被关闭后，任何
    // 残留跳转入口（Home 卡片、文件打开分发等）都不送进"无页面"空白页，
    // 回退 Home。Home 恒注册（kAlwaysOn），无递归风险；万一 Home 也没
    // 注册（防御），保持当前页不动。payload 面向已关闭的模块，随跳转作废
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
    // 进页通知（与 dispatchKeyEvent 同一扩展模式）：目标模块可借
    // onPageEnter 重查数据（Home 进页重查仪表盘）；含同页重入
    for (auto* m : modules_) {
        if (m && m->pageId() == page) {
            m->onPageEnter();
            break;
        }
    }
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
    // 调用方：main.cpp 的 atexit handler（getRouter 初始化尾部注册）——
    // 应用退出（关窗 / 托盘 Exit）时触发各模块 onStop（如 Claude 模块的
    // settings.json 接管还原），详见 main.cpp 注释
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