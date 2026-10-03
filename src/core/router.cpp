#include "router.h"
#include "logging.h"

// 注：本文件不再包含任何 EUI-NEO 头（eui::Ui/Screen/KeyEvent 经
// module_interface.h 的 header-only 类型桥接进来，仅作签名透传）——
// 空态兜底的绘制归属 app 层，结构性断言见 spec §4.2.1 + 审计 CP-01。

#include <atomic>
#include <utility>

namespace core {

void Router::registerModule(Module* module) {
    if (module == nullptr) return;
    for (auto* m : modules_) {
        if (m == module) return;   // 活门控（live-toggle）：重复注册 = no-op，
    }                              // 侧边栏不再出现同页双入口
    // 表序插入（语义、单点真相与测试出处见 router.h 的 registerModule 声明）：
    // off→on 重注册回表序原位，不再 append 末尾
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
            return;                // 只移除首个匹配；重复调用幂等
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
    // 没有匹配模块：交给 app 层注入的兜底渲染（文案/样式归 app 层持有，
    // core 只调用不绘制——spec §4.2.1）；未注入则不画，保持静默降级不崩
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