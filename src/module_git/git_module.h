// src/module_git/git_module.h
// Git 管理器模块（只读 v1，need:7）：Router 页面模块生命周期持有者。
#pragma once

#include <memory>

#include "core/module_interface.h"
#include "core/theme.h"
#include "git_page_controller.h"
#include "git_worker.h"
#include "platform/file_watcher.h"
#include "platform/git/git_libgit2.h"

namespace module::git {

class GitModule : public ::core::Module {
public:
    GitModule();
    ~GitModule() override;

    ::core::PageId pageId() const override { return ::core::PageId::Git; }
    std::string displayName() const override { return "Git"; }
    std::string shortName() const override { return "Git"; }
    std::string iconGlyph() const override { return "\xEF\x84\xA6"; }  // U+F126 fa-code-branch

    void compose(eui::Ui& ui, const eui::Screen& screen) override;

    // onStart：Libgit2Scope（refcount init）+ worker 线程 + watcher 挂载 + 会话恢复
    void onStart() override;
    // onStop：watcher 停（join）→ worker join → scope 析构（shutdown -1）；顺序不可反
    void onStop() override;

private:
    void mountWatcher();  // 以 controller 当前 watch 根挂 watcher（300ms debounce）

    std::unique_ptr<::platform::git::Libgit2Scope> libgit2Scope_;
    std::unique_ptr<GitWorker> worker_;
    std::unique_ptr<GitPageController> controller_;
    // 声明在 controller_ 之后：先于 controller 析构（join 后台线程，回调不悬垂）
    std::unique_ptr<::platform::FileWatcher> watcher_;
};

} // namespace module::git
