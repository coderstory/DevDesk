// src/module_git/git_page_controller.cpp
#include "git_page_controller.h"

#include "core/logging.h"
#include "core/theme.h"
#include "git_repo_store.h"
#include "platform/folder_picker.h"
#include "platform/git/git_libgit2.h"
#include "ui_kit/text_highlight/code_lexers.h"
#include "ui_kit/text_highlight/md_highlight.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace th = ui_kit::text_highlight;

namespace module::git {

namespace {

// F-01 校验失败三文案（错误条主文案按原因区分；spec §5.7 / PRD §5）
constexpr const char* kErrNotARepo = "该目录不是 git 仓库";
constexpr const char* kErrNoPermission = "没有权限访问该目录";
constexpr const char* kErrMemoryPrefix = "上次打开的仓库不可用：";
constexpr const char* kErrMemoryHint = "已清除该条记忆，请重新选择仓库。";

std::string repoNameFromPath(const std::string& pathUtf8) {
    if (pathUtf8.empty()) return {};
    size_t end = pathUtf8.size();
    while (end > 0 && (pathUtf8[end - 1] == '/' || pathUtf8[end - 1] == '\\')) {
        --end;
    }
    const size_t start = pathUtf8.find_last_of("/\\", end - 1);
    return start == std::string::npos ? pathUtf8.substr(0, end)
                                      : pathUtf8.substr(start + 1, end - start - 1);
}

} // namespace

// F-13 watcher 过滤表（技术方案 §2.2；relativePath 为相对 watch 根的 POSIX 分隔路径）
// marksOpenStale=「无条件置过期条」（元数据跳变/溢出 → diff 基准可能过期）；
// 工作树路径是否命中打开文件由控制器另行比对（onWatchEvents）
WatchVerdict classifyWatchEvent(platform::FileWatchAction action,
                                const std::string& relativePath) {
    if (action == platform::FileWatchAction::Unknown || relativePath.empty()) {
        return {true, true};  // 缓冲溢出：全量重扫兜底
    }
    const bool isDotGit = relativePath.rfind(".git", 0) == 0 &&
                          (relativePath.size() == 4 || relativePath[4] == '/');
    if (!isDotGit) {
        return {true, false};  // 工作树路径：status 脏
    }
    // .git 目录自身建删（仓结构变化）→ status 脏
    if (relativePath.size() == 4) {
        return {true, false};
    }
    // .git 内部：index / HEAD / refs/** = 外部 commit/stage/checkout → status 脏
    // 且 diff 基准跳变（打开内容过期）；objects/logs/pack 等 gc 洪泛 → 忽略
    if (relativePath == ".git/index" || relativePath == ".git/HEAD" ||
        relativePath.rfind(".git/refs/", 0) == 0) {
        return {true, true};
    }
    return {false, false};
}

GitPageController::GitPageController(GitWorker* worker) : worker_(worker) {}

void GitPageController::postToUi(std::function<void()> continuation) {
    std::lock_guard<std::mutex> lock(inboxMu_);
    inbox_.push_back(std::move(continuation));
}

void GitPageController::composePump() {
    // ① 排空 worker 回投（单帧内顺序执行，闭包只触 UI 线程字段）
    for (;;) {
        std::function<void()> job;
        {
            std::lock_guard<std::mutex> lock(inboxMu_);
            if (inbox_.empty()) break;
            job = std::move(inbox_.front());
            inbox_.pop_front();
        }
        job();
    }
    // ②（need291）untracked lazy 枚举改 toggle 驱动（toggleUntrackedDir 主动请求），
    //    无自动投递泵；POC① 单层枚举 <20ms，无需预算限流。
    // ③ F-13 刷新联动泵：watcher 置脏 → 1s 节流闸（trailing：到窗发一次）→
    // requestStatus；过期条评估；watcher 不可用降级 2s 轮询（仅模块可见=本泵在跑）
    if (phase_ == GitPagePhase::Ready) {
        const auto now = std::chrono::steady_clock::now();
        if (watchDirtyUi_ && !refreshThrottled()) {
            watchDirtyUi_ = false;
            requestStatus();
        }
        if (!watcherActive_ && !refreshThrottled() &&
            now - lastStatusAt_ > std::chrono::seconds(2)) {
            requestStatus();
        }
        // ⑤ 全局搜索防抖（need:14）：输入停 300ms 提交（Enter 走 submitSearchNow）
        if (searchEditPending_ && now - searchEditAt_ > std::chrono::milliseconds(300)) {
            searchEditPending_ = false;
            submitSearch(searchDraft_);
        }
    } else {
        watchDirtyUi_ = false;
        staleBar_ = false;
    }

    // ④ 行模型物化（视图行闭包持有的成员引用；每帧重算 = 状态派生，零缓存矩形）
    listRows_ = listModel_.rows();
}

void GitPageController::setViewMode(ChangeViewMode mode) {
    if (viewMode_ == mode) return;  // 连点幂等（设计 §6.3）
    viewMode_ = mode;
    if (phase_ != GitPagePhase::Ready) return;
    // 展开位图两形态互不消费（设计 §6.3：互不干扰）；快照重建行序列
    listModel_.rebuild(lastSnapshot_, viewMode_);
    listRows_ = listModel_.rows();
    // 切换后选中行保持可见（PRD F-02 scrollIntoView 口径）：滚到选中行槽位
    if (!selectedPath_.empty()) {
        for (size_t i = 0; i < listRows_.size(); ++i) {
            if (listRows_[i].kind == ChangeListRow::Kind::File &&
                listRows_[i].relPath == selectedPath_) {
                // 32px 定高槽（视图 kRowSlotH 同值）；滚至行顶−半屏内可见
                listScrollOffset_ = std::max(0.0f, static_cast<float>(i) * 32.0f - 200.0f);
                break;
            }
        }
    }
}

void GitPageController::toggleGroup(ChangeListRow::Group group) {
    listModel_.setGroupCollapsed(group, !listModel_.isGroupCollapsed(group));
}

void GitPageController::toggleDir(const std::string& dirRel) {
    // tracked 派生目录：纯模型展开位图（lazy 枚举只属 untracked，D3-A）
    listModel_.setExpanded(dirRel, !listModel_.isExpanded(dirRel));
}

void GitPageController::toggleUntrackedDir(const std::string& dirRel) {
    const bool expanded = listModel_.isExpanded(dirRel);
    listModel_.setExpanded(dirRel, !expanded);
    if (!expanded && requestedUntrackedDirs_.insert(dirRel).second) {
        // 首次展开：lazy 枚举（F-04；已缓存/已在途不重走——POC① 非递归约束下
        // 每层只枚举一次，刷新保留缓存同口径）
        requestUntrackedChildren(dirRel);
    }
}

void GitPageController::retryUntrackedDir(const std::string& dirRel) {
    // N4 错误态重试：目录保持展开，重走枚举
    requestedUntrackedDirs_.insert(dirRel);
    requestUntrackedChildren(dirRel);
}

void GitPageController::onStart() {
    // N10 会话内切回（切其它模块再回来）：仓/右栏打开文件与视图类型保留；列表
    // 展开/折叠位图随模型保留（F-09 同口径）。worker/回投箱已随 onStop 重置，
    // status 与打开内容全部重取。
    if (phase_ == GitPagePhase::Ready && !workdirUtf8_.empty()) {
        resetRightPane();
        if (!selectedPath_.empty()) {
            if (rightTab_ == RightTab::Diff && !searchContentView_) {
                requestDiff(selectedPath_, /*force=*/true);
            } else {
                requestContent(selectedPath_, /*force=*/true);
            }
        }
        requestStatus();
        return;
    }
    // N1：记忆仓自动恢复。有记忆 → 异步校验（Loading 面）；失效 → 清记忆 + 空态错误条
    const GitRepoMemory memory = GitRepoStore::load();
    if (memory.lastRepoPath.empty()) return;
    validateAsync(memory.lastRepoPath, /*fromMemory=*/true);
}

void GitPageController::onStop() {
    // 取消③切模块：seq++ 让 join 等待中的长扫描在下一文件边界立即退出
    //（git_worker stop 再兜底丢弃未执行任务）；然后清回投箱
    ++searchSeq_;
    // P2-5（need:14 审计）：切模块时扫描在途 → 复位扫描面。不复位则回模块永挂
    // 「正在扫描」+spinner，且同词幂等（submitSearch query 未清）堵死重扫。
    // 选 None（全清）而非 Done+truncated：truncated 语义=200 上限截断，挪用会给
    // 出「结果完整」假象；None 回模块草稿仍在（searchDraft_ 不清），Enter 即重查。
    if (searchState_ == GitSearchState::Scanning) {
        resetSearchFace();
    }
    // worker 生命周期归模块；这里只清回投箱（停机期间不再有回投目标）
    std::lock_guard<std::mutex> lock(inboxMu_);
    inbox_.clear();
}

bool GitPageController::validateSpinnerDue() const {
    if (!validating_) return false;
    const auto elapsed = std::chrono::steady_clock::now() - validateStart_;
    return elapsed > std::chrono::seconds(1);
}

void GitPageController::beginPickRepo() {
    if (validating_) return;  // 校验中按钮 disabled 语义双保险
    // 走注入缝（claude/wallpaper 同款）：生产 = 原生对话框，测试经 setPickerForTest 替换
    auto picked = ::platform::folderpicker::pickerSlot()("选择 Git 仓库目录", {});
    if (picked.isFail()) {
        return;  // 用户取消：停留空态页（spec I-10）
    }
    validateAsync(picked.value(), /*fromMemory=*/false);
}

void GitPageController::switchRepo() {
    // I-09：迁移 N2 空态页；仓/模式/右栏打开文件与视图类型按 N10 枚举保留在内存，
    // 同仓再选时 openRepo 恢复。树展开状态与滚动位不保留（openRepo 会 reset）。
    phase_ = GitPagePhase::Empty;
    emptyError_.reset();
    validating_ = false;
    statusLoading_ = false;
    watchDirtyUi_ = false;
    staleBar_ = false;
    resetRightPane();
}

void GitPageController::validateAsync(const std::string& repoPathUtf8,
                                      bool fromMemory) {
    validating_ = true;
    emptyError_.reset();
    validateStart_ = std::chrono::steady_clock::now();
    const std::string path = repoPathUtf8;
    worker_->post([this, path, fromMemory] {
        // 校验用一次性 open（不入缓存——目录可能不是仓，污染无意义）
        auto opened = ::platform::git::Repo::open(platform::git::utf8ToPath(path));
        // errorMessage() 只对 fail 态合法（ok 态取值 = bad_variant_access），
        // 捕获前按态取值，闭包只持拷贝
        const bool openOk = opened.isOk();
        postToUi([this, path, fromMemory, code = opened.errorCode(),
                  message = openOk ? std::string() : opened.errorMessage()] {
            validating_ = false;
            if (code == core::ErrorCode::Ok) {
                if (fromMemory) {
                    // 恢复成功：刷新记忆（路径可能已规范化）
                    GitRepoStore::save(GitRepoMemory{path});
                }
                openRepo(path);
                return;
            }
            if (fromMemory) {
                // N1 记忆仓失效：清除 + 空态页错误条（F-14③）
                auto cleared = GitRepoStore::clear();
                if (cleared.isFail()) {
                    LOG_WARN("GitPage: 失效记忆清除失败: " + cleared.errorMessage());
                }
                emptyError_ = EmptyError{kErrMemoryPrefix + message, kErrMemoryHint,
                                         /*fromMemory=*/true};
            } else if (code == core::ErrorCode::FilePermissionDenied) {
                emptyError_ = EmptyError{kErrNoPermission, {}, false};
            } else {
                emptyError_ = EmptyError{kErrNotARepo, {}, false};
            }
        });
    });
}

void GitPageController::openRepo(const std::string& repoPathUtf8) {
    // N10 切回恢复：同仓再选（切换仓库 → 空态页 → 重选同仓）恢复右栏打开文件与
    // 视图类型；列表展开位图与滚动位不保留（回列表首屏）。异仓 = 全新会话。
    const std::string prevWorkdir = workdirUtf8_;
    const std::string prevSelected = selectedPath_;
    const RightTab prevTab = rightTab_;
    const bool prevSearchView = searchContentView_;
    const bool sameRepo = phase_ == GitPagePhase::Empty && !prevWorkdir.empty() &&
                          prevWorkdir == repoPathUtf8;
    closeRepo();
    workdirUtf8_ = repoPathUtf8;
    phase_ = GitPagePhase::Ready;
    emptyError_.reset();
    // 记忆仓写入（最佳努力，失败只记日志——查看功能不依赖它）
    auto saved = GitRepoStore::save(GitRepoMemory{repoPathUtf8});
    if (saved.isFail()) {
        LOG_WARN("GitPage: 记忆仓写入失败: " + saved.errorMessage());
    }
    listModel_.reset();
    requestedUntrackedDirs_.clear();
    listScrollOffset_ = 0.0f;
    resetRightPane();
    if (sameRepo && !prevSelected.empty()) {
        selectedPath_ = prevSelected;
        rightTab_ = prevTab;
        searchContentView_ = prevSearchView;
        if (prevSearchView || prevTab == RightTab::Content) {
            requestContent(prevSelected, /*force=*/true);
        } else {
            requestDiff(prevSelected, /*force=*/true);
        }
    } else {
        selectedPath_.clear();
        rightTab_ = RightTab::Diff;
        searchContentView_ = false;
    }
    if (repoChangeCb_) repoChangeCb_(workdirUtf8_);
    requestStatus();
}

void GitPageController::closeRepo() {
    phase_ = GitPagePhase::Empty;
    workdirUtf8_.clear();
    statusError_.clear();
    statusLoading_ = false;
    selectedPath_.clear();
    rightTab_ = RightTab::Diff;
    searchContentView_ = false;
    listScrollOffset_ = 0.0f;
    listModel_.reset();
    requestedUntrackedDirs_.clear();
    listRows_.clear();
    lastSnapshot_ = platform::git::StatusSnapshot{};
    watchDirtyUi_ = false;
    staleBar_ = false;
    resetRightPane();
}

bool GitPageController::refreshThrottled() const {
    return std::chrono::steady_clock::now() - lastStatusAt_ <
           std::chrono::seconds(1);
}

void GitPageController::onWatchEvents(
    std::vector<platform::FileWatchEvent> events) {
    // watcher 后台线程：只跑纯过滤（技术方案 §2.2），判定经 postToUi 回 UI 线程
    bool anyStatus = false;
    bool metaOrUnknown = false;               // .git 元数据 / 溢出兜底
    std::vector<std::string> worktreePaths;   // 工作树事件（与打开文件比对）
    for (const auto& e : events) {
        const WatchVerdict v = classifyWatchEvent(e.action, e.relativePath);
        if (!v.marksStatus) continue;
        anyStatus = true;
        if (v.marksOpenStale) {
            metaOrUnknown = true;
        } else {
            worktreePaths.push_back(e.relativePath);
        }
    }
    if (!anyStatus) return;
    postToUi([this, metaOrUnknown, worktreePaths = std::move(worktreePaths)] {
        if (phase_ != GitPagePhase::Ready) return;
        watchDirtyUi_ = true;  // composePump 节流闸放行时投一次 status
        // F-13③：元数据跳变/溢出 → 打开 diff 基准可能过期；工作树事件命中打开
        // 文件 → 内容过期。过期条覆盖绘制，不进布局（零滚动跳动）。
        if (metaOrUnknown) {
            staleBar_ = true;
            return;
        }
        for (const auto& p : worktreePaths) {
            if (p == selectedPath_ || p == diffPath_) {
                staleBar_ = true;
                break;
            }
        }
    });
}

void GitPageController::reloadStale() {
    if (!staleBar_ || selectedPath_.empty()) return;
    staleBar_ = false;
    if (rightTab_ == RightTab::Diff && !searchContentView_) {
        requestDiff(selectedPath_, /*force=*/true);
    } else {
        requestContent(selectedPath_, /*force=*/true);
    }
}

void GitPageController::requestStatus() {
    if (workdirUtf8_.empty()) return;
    statusLoading_ = true;
    statusError_.clear();
    lastStatusAt_ = std::chrono::steady_clock::now();
    const std::string path = workdirUtf8_;
    worker_->postWithRepo(path, [this, path](platform::git::Repo* repo) {
        if (repo == nullptr) {
            postToUi([this] {
                statusLoading_ = false;
                statusError_ = "无法打开仓库";
            });
            return;
        }
        auto snapshot = repo->status();
        // errorMessage() 只对 fail 态合法（ok 态取值 = bad_variant_access）
        const bool statusOk = snapshot.isOk();
        postToUi([this, path, snapshot, ok = statusOk,
                  message = statusOk ? std::string() : snapshot.errorMessage()] {
            if (path != workdirUtf8_) return;  // 期间已切仓：过期结果丢弃
            statusLoading_ = false;
            if (!ok) {
                // F-13②（批次 #11 完善）：失败保留旧数据，只记错误面
                statusError_ = message;
                return;
            }
            applyStatusSnapshot(snapshot.value());
        });
    });
}

void GitPageController::applyStatusSnapshot(
    const platform::git::StatusSnapshot& snapshot) {
    lastSnapshot_ = snapshot;
    // F-09：刷新=行序列重建，展开位图/组折叠/untracked 缓存模型内保留（clue#14）
    listModel_.rebuild(snapshot, viewMode_);
    listRows_ = listModel_.rows();
    clearSelectionIfMissing();
}

void GitPageController::clearSelectionIfMissing() {
    // N8（设计 §6.1 边界/§7）：选中文件被后台刷新删除 → 右栏回空态 + 列表去行。
    // 搜索独立内容视图不受列表约束（命中文件本就 ∉ 变更列表）。
    if (searchContentView_ || selectedPath_.empty()) return;
    if (rowHasFile(selectedPath_)) return;
    selectedPath_.clear();
    rightTab_ = RightTab::Diff;
    pendingDiffLine_.reset();
    diffHighlightLine_ = 0;
    diffModel_.clear();
    diffPath_.clear();
    diffState_ = RightPaneState::None;
    ++diffReq_;
    content_ = FileContent{};
    contentPath_.clear();
    contentState_ = RightPaneState::None;
    contentRuns_.clear();
    ++contentReq_;
}

bool GitPageController::rowHasFile(const std::string& relPath) const {
    if (relPath.empty()) return false;
    // 全量成员判定（模型双桶+lazy 缓存），非可见行扫描——折叠组/未展开目录下
    // 的选中文件刷新后不丢（F-09：折叠组含选中行=选中保持前提）
    return listModel_.hasFile(relPath);
}

std::optional<platform::git::StatusKind> GitPageController::selectedRowStatus()
    const {
    if (selectedPath_.empty()) return std::nullopt;
    for (const auto& row : listRows_) {
        if (row.kind == ChangeListRow::Kind::File && row.relPath == selectedPath_ &&
            row.status.has_value()) {
            return row.status;
        }
    }
    return std::nullopt;
}

void GitPageController::requestUntrackedChildren(const std::string& dirRel) {
    listModel_.markUntrackedLoading(dirRel);
    const std::string path = workdirUtf8_;
    worker_->postWithRepo(path, [this, path, dirRel](platform::git::Repo* repo) {
        const std::filesystem::path workdir =
            repo != nullptr ? repo->workdir()
                            : platform::git::utf8ToPath(path);
        // repo == nullptr（open 失败）时 walk 无 ignore 判定——降级纯 fs 枚举。
        // POC①（need291）：单层枚举 ~1.6µs/条目；归并口径=枚举结果全量取用
        //（非 ignored 即 untracked），子目录条目续 lazy、文件条目打 U。
        fs_walk::WalkResult walked = fs_walk::walkChildren(repo, workdir, dirRel);
        postToUi([this, path, dirRel, walked] {
            if (path != workdirUtf8_) return;  // 期间已切仓：过期结果丢弃
            if (!walked.ok) {
                listModel_.markUntrackedFailed(dirRel, walked.message);
                return;
            }
            const bool emptyDir = walked.entries.empty();
            listModel_.applyUntrackedChildren(dirRel, walked);
            // N4 竞态空目录（展开期间文件被删）：行消失语义=触发一次刷新，
            // 快照更新后目录条目消失、行随之消失（设计 §7 N4 空格）
            if (emptyDir) requestStatus();
        });
    });
}

void GitPageController::selectChange(const std::string& path) {
    // F-05 主动线：文件行点击 → diff 即时切换；搜索独立内容视图退出；
    // rightTab_ 保持（设计 §6.1：已在「内容」tab 保持 tab 选择切文件）
    searchContentView_ = false;
    selectedPath_ = path;
    pendingContentLine_.reset();
    contentHighlightLine_ = 0;
    requestDiff(path, /*force=*/false);
}

void GitPageController::setRightTab(RightTab tab) {
    if (searchContentView_) return;  // §6.8 独立内容视图无 tab（「内容」恒激活）
    if (rightTab_ == tab) return;    // 连点幂等
    rightTab_ = tab;
    if (selectedPath_.empty()) return;
    // §6.6：切换不重发请求（数据已取）；错误态重进 = 重试语义
    if (tab == RightTab::Content) {
        requestContent(selectedPath_, /*force=*/contentState_ == RightPaneState::Error);
    } else {
        requestDiff(selectedPath_, /*force=*/diffState_ == RightPaneState::Error);
    }
}

void GitPageController::retryStatus() {
    // N2 扫描失败重试：绕 1s 节流闸（失败后立即重试是显式用户动作）
    if (phase_ != GitPagePhase::Ready) return;
    requestStatus();
}

void GitPageController::retryContent() {
    if (selectedPath_.empty()) return;
    requestContent(selectedPath_, /*force=*/true);
}

void GitPageController::retryDiff() {
    if (selectedPath_.empty()) return;
    requestDiff(selectedPath_, /*force=*/true);
}

void GitPageController::resetRightPane() {
    // 取消②切仓/关仓（含同仓重开）：在途扫描文件边界自灭 + 回无搜索态
    ++searchSeq_;
    resetSearchFace();
    searchContentView_ = false;
    content_ = FileContent{};
    contentPath_.clear();
    contentState_ = RightPaneState::None;
    contentError_.clear();
    contentRuns_.clear();
    ++contentReq_;  // 在途结果作废
    diffModel_.clear();
    diffPath_.clear();
    diffState_ = RightPaneState::None;
    diffError_.clear();
    ++diffReq_;
    contentScrollOffset_ = 0.0f;
    diffScrollOffset_ = 0.0f;
    contentHOffset_ = 0.0f;
    diffHOffset_ = 0.0f;
    pendingContentLine_.reset();
    contentHighlightLine_ = 0;
    pendingDiffLine_.reset();
    diffHighlightLine_ = 0;
    staleBar_ = false;
}

void GitPageController::requestContent(const std::string& relPath, bool force) {
    if (workdirUtf8_.empty() || relPath.empty()) return;
    if (!force && contentPath_ == relPath &&
        (contentState_ == RightPaneState::Loading ||
         contentState_ == RightPaneState::Ready)) {
        return;  // 已在加载/已加载：连点幂等（I-03/I-05）
    }
    if (contentPath_ != relPath) {
        // bug#280①：跨文件切换清滚动/横偏——残留偏移把新文件的短行平移出屏
        //（内容区空白）；同文件重载（stale 重载/retryContent）保留位置
        contentScrollOffset_ = 0.0f;
        contentHOffset_ = 0.0f;
    }
    contentPath_ = relPath;
    contentState_ = RightPaneState::Loading;
    contentError_.clear();
    const std::uint64_t req = ++contentReq_;
    const std::filesystem::path abs =
        platform::git::utf8ToPath(workdirUtf8_) /
        platform::git::utf8ToPath(relPath);
    worker_->post([this, relPath, req, abs] {
        const FileContent fc = FileContent::readFromDisk(abs);
        postToUi([this, relPath, req, fc] {
            if (req != contentReq_ || relPath != contentPath_) return;  // 过期丢弃
            if (fc.state == ContentState::Ok) {
                content_ = fc;
                contentState_ = RightPaneState::Ready;
                buildContentRuns();
                // F-08②：jumpToHit 锚在本文件 Ready 后常亮（滚动锚由视图消费）
                if (pendingContentLine_) contentHighlightLine_ = *pendingContentLine_;
            } else if (fc.state == ContentState::ReadError) {
                content_ = FileContent{};
                contentState_ = RightPaneState::Error;
                contentError_ = fc.errorMessage;
                contentRuns_.clear();
            } else {
                content_ = fc;  // 四态占位（Empty/Binary/Oversize/BadEncoding）
                contentRuns_.clear();
                contentState_ = RightPaneState::Placeholder;
            }
        });
    });
}

void GitPageController::requestDiff(const std::string& relPath, bool force) {
    if (workdirUtf8_.empty() || relPath.empty()) return;
    if (!force && diffPath_ == relPath &&
        (diffState_ == RightPaneState::Loading ||
         diffState_ == RightPaneState::Ready)) {
        return;  // 已选中再点不重载 diff（I-04 边界）
    }
    if (diffPath_ != relPath) {
        // bug#280①：同 requestContent——跨文件切换清滚动/横偏（diff 区空白根因）
        diffScrollOffset_ = 0.0f;
        diffHOffset_ = 0.0f;
    }
    diffPath_ = relPath;
    diffState_ = RightPaneState::Loading;
    diffError_.clear();
    const std::uint64_t req = ++diffReq_;
    const std::string path = workdirUtf8_;
    // bug#296：未跟踪行（顶层或 lazy 展开子文件）在 workdir↔HEAD diff 里无
    // delta 可匹配（F-09 不递归）→ 零匹配落「文件已无变更」误报。选中行状态
    // 为 U 时绕过 diff 管线直接合成全增（bug#18 档①同链，fs 读无需 repo）。
    const bool untrackedRow =
        selectedRowStatus() == platform::git::StatusKind::Untracked;
    worker_->postWithRepo(
        path, [this, relPath, req, path, untrackedRow](platform::git::Repo* repo) {
        if (untrackedRow) {
            platform::git::DiffPatch patch;
            patch.newPath = relPath;
            patch.isUntracked = true;
            std::string placeholder = synthesizeUntrackedDiff(
                patch,
                platform::git::utf8ToPath(path) / platform::git::utf8ToPath(relPath));
            postToUi([this, relPath, req, patch = std::move(patch),
                      placeholder = std::move(placeholder)]() mutable {
                if (req != diffReq_ || relPath != diffPath_) return;  // 过期丢弃
                diffModel_.setPatch(std::move(patch), std::move(placeholder));
                diffState_ = RightPaneState::Ready;
            });
            return;
        }
        if (repo == nullptr) {
            postToUi([this, relPath, req] {
                if (req != diffReq_ || relPath != diffPath_) return;
                diffState_ = RightPaneState::Error;
                diffError_ = "无法打开仓库";
            });
            return;
        }
        // per-task 后台计算（技术方案 §6）：diff 单文件 pathFilter，worker 线程内
        // 摘出目标 patch，只把 patch 拷回 UI 线程
        auto diff = repo->diff(platform::git::DiffBasis::WorkdirToHead, relPath);
        const bool ok = diff.isOk();
        platform::git::DiffPatch patch;
        std::string message;
        bool matched = false;
        if (ok) {
            for (auto& p : diff.value().patches) {
                if (p.newPath == relPath || p.oldPath == relPath) {
                    patch = std::move(p);
                    matched = true;
                    break;
                }
            }
            // bug#18：untracked 条目 libgit2 不带 UNTRACKED_CONTENT → patch 无 hunks
            // （F-09 内存约束下禁加该 flag）。模型层合成：≤1MiB 文本全增行 /
            // >1MiB·二进制占位（need291：目录占位文案分支随 F-05 目录行不进 diff
            // 交互根治而退役）
            synthesizeUntrackedDiff(
                patch,
                platform::git::utf8ToPath(path) / platform::git::utf8ToPath(relPath));
        } else {
            message = diff.errorMessage();
        }
        postToUi([this, relPath, req, patch = std::move(patch), ok, message,
                  matched]() mutable {
            if (req != diffReq_ || relPath != diffPath_) return;  // 过期丢弃
            if (!ok) {
                diffState_ = RightPaneState::Error;
                diffError_ = message;
                return;
            }
            // bug#290/PRD N5：零匹配（选中后文件被删等竞态）→ 显式文案 + 自动
            // 触发列表刷新（禁静默空/禁「无内容变更」误导）
            std::string placeholder;
            if (!matched) placeholder = "文件已无变更，列表将刷新";
            diffModel_.setPatch(std::move(patch), std::move(placeholder));
            diffState_ = RightPaneState::Ready;
            if (!matched) requestStatus();
        });
    });
}

void GitPageController::buildContentRuns() {
    contentRuns_.assign(content_.lines.size(), {});
    if (content_.lines.empty()) return;
    const core::theme::Tokens tokens = core::theme::current();

    // .md → md 行级高亮器（平移后的 ui_kit/text_highlight）：全文围栏扫描 +
    // 逐行 runs（全文档坐标）→ 平移回行内坐标
    const std::string lowerPath = [&] {
        std::string s = contentPath_;
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }();
    if (lowerPath.size() >= 3 && lowerPath.compare(lowerPath.size() - 3, 3, ".md") == 0) {
        std::string fullText;
        std::vector<int> lineStarts;
        lineStarts.reserve(content_.lines.size());
        for (const std::string& line : content_.lines) {
            lineStarts.push_back(static_cast<int>(fullText.size()));
            fullText += line;
            fullText += '\n';
        }
        module::markdown::MdHighlighter hl;
        hl.setColors(module::markdown::mdHighlightColors(tokens));
        for (size_t i = 0; i < content_.lines.size(); ++i) {
            const auto runs =
                hl.runsForLine(fullText, content_.lines[i], lineStarts[i]);
            auto& out = contentRuns_[i];
            out.reserve(runs.size());
            const int lineLen = static_cast<int>(content_.lines[i].size());
            for (const eui::TextRun& r : runs) {
                const int start = std::max(0, r.byteStart - lineStarts[i]);
                const int end = std::min(lineLen, r.byteEnd - lineStarts[i]);
                if (start < end) {
                    out.push_back({start, end, r.color});
                }
            }
        }
        return;
    }

    // 五类代码语言 → kind span → 当时主题色 runs
    th::CodeLang lang;
    if (th::codeLangFromExtension(lowerPath, lang)) {
        th::CodeHighlighter hl;
        hl.setLang(lang);
        const th::CodeTokenColors colors = th::codeTokenColors(tokens);
        for (size_t i = 0; i < content_.lines.size(); ++i) {
            const auto spans = hl.spansForLine(content_.lines[i]);
            auto& out = contentRuns_[i];
            out.reserve(spans.size());
            for (const th::HlSpan& s : spans) {
                out.push_back({s.start, s.end, th::colorOf(s.kind, colors)});
            }
        }
    }
}

void GitPageController::refresh() {
    if (phase_ != GitPagePhase::Ready) return;
    if (refreshThrottled()) return;  // F-09④：1s 内至多触发一次重算（手动/watcher 同闸）
    // F-09（need291）：刷新=快照重建行序列；展开位图/组折叠/选中/untracked 缓存
    // 全保留（模型内状态，applyStatusSnapshot 统一重建）
    requestStatus();
}

// ---- 全局搜索（need:14 F-08；技术方案 §2/§3/§6）----

namespace {

constexpr size_t kSearchFlushFiles = 32;   // 增量包：每 32 文件或 250ms 出一包
constexpr int kSearchFlushMs = 250;
constexpr size_t kMaxTotalHits = 200;      // 总量上限：达 200 即停（truncated）

std::string trimmed(const std::string& s) {
    const auto isSpace = [](unsigned char c) {
        return std::isspace(c) != 0;
    };
    size_t b = 0;
    while (b < s.size() && isSpace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && isSpace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

} // namespace

void GitPageController::markSearchEdited() {
    if (phase_ != GitPagePhase::Ready) return;
    searchEditPending_ = true;
    searchEditAt_ = std::chrono::steady_clock::now();
}

void GitPageController::submitSearchNow() {
    if (phase_ != GitPagePhase::Ready) return;
    searchEditPending_ = false;
    submitSearch(searchDraft_);
}

void GitPageController::submitSearch(const std::string& rawNeedle) {
    if (phase_ != GitPagePhase::Ready) return;
    const std::string needle = trimmed(rawNeedle);
    if (needle.empty()) {
        cancelSearch();  // 清空输入 = 显式取消（清除 × 同路径）
        return;
    }
    if (needle == searchQuery_ && searchState_ != GitSearchState::Error) {
        return;  // 同词幂等（Done/Scanning 间连 Enter 不重扫；Error 允许重试）
    }
    searchQuery_ = needle;
    requestSearch(needle);
}

void GitPageController::cancelSearch() {
    ++searchSeq_;  // 取消①ESC/清除：在途任务下一文件边界自灭
    resetSearchFace();
    searchDraft_.clear();
}

void GitPageController::resetSearchFace() {
    searchState_ = GitSearchState::None;
    searchQuery_.clear();
    searchResults_.clear();
    searchScanned_ = 0;
    searchTotalFiles_ = 0;
    searchTruncated_ = false;
    searchError_.clear();
    searchPopupVisible_ = false;
    searchPopupScroll_ = 0.0f;
    searchEditPending_ = false;
}

void GitPageController::requestSearch(const std::string& needle) {
    // 取消①新查询：先 ++ 再投——旧任务下一文件边界自灭，无需出队
    const std::uint64_t seq = ++searchSeq_;
    searchState_ = GitSearchState::Scanning;
    searchResults_.clear();
    searchScanned_ = 0;
    searchTotalFiles_ = 0;
    searchTruncated_ = false;
    searchError_.clear();
    searchPopupVisible_ = true;
    // untracked 目录快照（不重算 status）：lastSnapshot_ 中尾 '/' 条目。B0 核对
    // 落注：libgit2 status 不递归 untracked 时目录键形态 = "dir/"（尾斜杠），
    // kind=Untracked（WT_NEW→Untracked）——按尾 '/' 判定，不按 kind 过滤
    std::vector<std::string> skipDirs;
    for (const auto& entry : lastSnapshot_.entries) {
        if (!entry.path.empty() && entry.path.back() == '/') {
            skipDirs.push_back(entry.path);
        }
    }
    const std::string path = workdirUtf8_;
    worker_->postWithRepo(path,
                          [this, path, needle, seq,
                           skipDirs = std::move(skipDirs)](platform::git::Repo* repo) {
        if (repo == nullptr) {
            postToUi([this, seq] {
                if (seq != searchSeq_.load()) return;
                searchState_ = GitSearchState::Error;
                searchError_ = "无法打开仓库";
            });
            return;
        }
        const std::filesystem::path workdir = repo->workdir();
        // ② walk：从根逐目录复用 git_fs_walk::walkChildren 枚举全量文件清单
        //（.git/ignored/symlink 不跟随/Subrepo/NoAccess 语义零漂移）；untracked
        // 目录按快照跳过不递归（防 node_modules 式 hang，F-12 同源口径）
        std::vector<std::string> files;
        std::vector<std::string> dirs;
        dirs.push_back(std::string());
        while (!dirs.empty()) {
            if (seq != searchSeq_.load()) return;  // 目录边界过期自灭
            const std::string dir = std::move(dirs.back());
            dirs.pop_back();
            const fs_walk::WalkResult walked = fs_walk::walkChildren(repo, workdir, dir);
            if (!walked.ok) continue;  // 无权限/枚举失败：静默跳过（宽松语义）
            for (auto& row : walked.entries) {
                if (row.kind == fs_walk::EntryKind::Directory) {
                    const std::string withSlash = row.relPath + "/";
                    const bool skip =
                        std::find(skipDirs.begin(), skipDirs.end(), withSlash) !=
                        skipDirs.end();
                    if (!skip) dirs.push_back(std::move(row.relPath));
                } else if (row.kind == fs_walk::EntryKind::File) {
                    files.push_back(std::move(row.relPath));
                }  // Subrepo/NoAccess/派生行不入清单
            }
        }
        const size_t total = files.size();
        // ③ match：逐文件 FileContent::readFromDisk 四态判据（state≠Ok skip，
        // ReadError 静默计数）；Ok 态逐行 matchLines（单文件 5 条上限）
        std::vector<SearchHit> pending;
        size_t scanned = 0;
        size_t hitTotal = 0;
        bool truncated = false;
        auto lastFlush = std::chrono::steady_clock::now();
        auto flushIfDue = [&](bool force) {
            const bool countDue = pending.size() >= kSearchFlushFiles;
            const bool timeDue =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - lastFlush)
                    .count() >= kSearchFlushMs;
            if (!force && !countDue && !timeDue) return;
            lastFlush = std::chrono::steady_clock::now();
            postToUi([this, seq, b = std::move(pending), s = scanned, t = total] {
                if (seq != searchSeq_.load()) return;  // 过期增量丢弃（§3 取消）
                for (auto& h : b) searchResults_.push_back(std::move(h));
                if (searchState_ == GitSearchState::Scanning) {
                    searchScanned_ = s;
                    searchTotalFiles_ = t;
                }
            });
        };
        for (const auto& rel : files) {
            if (seq != searchSeq_.load()) return;  // 文件边界过期自灭
            if (truncated) break;  // 200 条即停：提前收不再计数（§5）
            const FileContent fc = FileContent::readFromDisk(
                workdir / platform::git::utf8ToPath(rel));
            ++scanned;
            if (fc.state != ContentState::Ok) continue;
            auto hits = matchLines(fc.lines, needle);
            for (auto& h : hits) {
                if (hitTotal >= kMaxTotalHits) {
                    truncated = true;
                    break;
                }
                h.relPath = rel;
                pending.push_back(std::move(h));
                ++hitTotal;
            }
            flushIfDue(false);  // 每 32 文件 / 250ms 渐进出果（F-08③）
        }
        if (seq != searchSeq_.load()) return;
        flushIfDue(true);
        postToUi([this, seq, s = scanned, t = total, trunc = truncated] {
            if (seq != searchSeq_.load()) return;
            searchScanned_ = s;
            searchTotalFiles_ = t;
            searchTruncated_ = trunc;
            if (searchState_ == GitSearchState::Scanning) {
                searchState_ = GitSearchState::Done;
            }
        });
    });
}

void GitPageController::jumpToHit(const SearchHit& hit) {
    // F-08②（need291 D7-A 双通道锚）：
    //   命中∈变更列表 → 选中该行 + diff 视图锚定（newNo 匹配滚动+高亮）；
    //   命中∉变更列表 → 独立内容视图（头部仅路径，「内容」恒激活）+ 内容锚。
    // clue#14：滚动锚=行号，Ready 后视图按行盒几何消费，禁偏移量直设。
    if (rowHasFile(hit.relPath)) {
        selectChange(hit.relPath);
        rightTab_ = RightTab::Diff;
        pendingDiffLine_ = hit.line;
        // P1-2（need:14 审计）同款：同文件已 Ready 时 requestDiff 幂等早退不经
        // Ready 落值点——高亮先行落值，Ready 到达后赋同值幂等。
        diffHighlightLine_ = hit.line;
        return;
    }
    searchContentView_ = true;
    selectedPath_ = hit.relPath;
    rightTab_ = RightTab::Content;
    pendingContentLine_ = hit.line;
    contentHighlightLine_ = hit.line;
    requestContent(hit.relPath, /*force=*/false);
}

size_t GitPageController::searchFileCount() const {
    std::vector<const std::string*> seen;  // ≤200 条，去重现算成本可忽略
    for (const auto& h : searchResults_) {
        bool found = false;
        for (const auto* p : seen) {
            if (*p == h.relPath) {
                found = true;
                break;
            }
        }
        if (!found) seen.push_back(&h.relPath);
    }
    return seen.size();
}

std::string GitPageController::repoDisplayName() const {
    return repoNameFromPath(workdirUtf8_);
}

} // namespace module::git
