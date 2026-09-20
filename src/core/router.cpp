#include "router.h"
#include "logging.h"

#include <eui/screen.h>
#include <eui/ui.h>

namespace core {

void Router::registerModule(Module* module) {
    if (module) modules_.push_back(module);
}

void Router::navigateTo(PageId page) {
    LOG_INFO("导航到: " + std::string(pageIdToString(page)));
    current_ = page;
}

PageId Router::currentPage() const { return current_; }

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

} // namespace core