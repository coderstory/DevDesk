// src/module_git/git_module.cpp
#include "git_module.h"

#include "core/logging.h"
#include "git_views.h"

namespace module::git {

GitModule::GitModule()
    : worker_(std::make_unique<GitWorker>()),
      controller_(std::make_unique<GitPageController>(worker_.get())) {
    // F-13：开仓/切仓后重挂 watcher（换 watch 根），并回报可用性（false →
    // 控制器降级 2s 轮询 status）
    controller_->setRepoChangeCallback(
        [this](const std::string&) { mountWatcher(); });
}

GitModule::~GitModule() = default;

void GitModule::mountWatcher() {
    if (watcher_ != nullptr) {
        watcher_->stop();  // 幂等；换根重挂
    }
    const std::string root = controller_->watchRoot();
    if (root.empty()) {
        controller_->setWatcherActive(false);
        return;
    }
    watcher_ = std::make_unique<::platform::FileWatcher>();
    // debounce 300ms（技术方案 §2.2：watcher 侧吸收突发，刷新侧另有 1s 节流）
    const auto started = watcher_->start(
        ::platform::git::utf8ToPath(root),
        [c = controller_.get()](std::vector<::platform::FileWatchEvent> events) {
            c->onWatchEvents(std::move(events));
        },
        std::chrono::milliseconds(300));
    const bool active = started.isOk() && watcher_->active();
    controller_->setWatcherActive(active);
    if (!active) {
        LOG_WARN("GitModule: FileWatcher 不可用，降级 2s 轮询 status（仅模块可见时）");
    }
}

void GitModule::onStart() {
    // 配对顺序（技术方案 §3）：先全局 init（scope），再起 worker——线程内 open
    // 依赖 libgit2 已初始化。Libgit2Scope 构造失败只 LOG（git_libgit2.h 契约），
    // 后续每次 open 走 Result 错误链上报。
    libgit2Scope_ = std::make_unique<::platform::git::Libgit2Scope>();
    worker_->start();
    controller_->onStart();
    mountWatcher();
}

void GitModule::onStop() {
    if (watcher_ != nullptr) {
        watcher_->stop();  // join：先于 controller 析构/停用，回调不悬垂
        watcher_.reset();
    }
    controller_->onStop();
    worker_->stop();  // join：保证闭包不触碰已析构的 controller
    libgit2Scope_.reset();
}

void GitModule::compose(eui::Ui& ui, const eui::Screen& screen) {
    composeGitPage(ui, screen, *controller_, core::theme::current());
}

} // namespace module::git

extern "C" ::core::Module* create_git_module() {
    return new module::git::GitModule();
}
