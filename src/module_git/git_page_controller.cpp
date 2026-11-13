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
    // ② 懒加载投递：已展开但未加载的目录（浅层优先），单帧最多 4 个防任务洪泛
    if (phase_ == GitPagePhase::Ready && tree_.mode() == GitTreeModel::Mode::AllFiles) {
        int budget = 4;
        for (const auto& dirRel : tree_.pendingDirs()) {
            if (budget-- <= 0) break;
            requestWalk(dirRel);
        }
    }
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
    visibleRows_ = tree_.rows();
    changeListItems_.clear();
    if (!changeRows_.empty()) {
        const size_t changedN = changedCount();
        const size_t untrackedN = untrackedCount();
        if (changedN > 0) {
            changeListItems_.push_back({true, 0});
            for (size_t i = 0; i < changeRows_.size(); ++i) {
                if (changeRows_[i].group == ChangeGroup::Changed) {
                    changeListItems_.push_back({false, i});
                }
            }
        }
        if (untrackedN > 0) {
            changeListItems_.push_back({true, 1});
            for (size_t i = 0; i < changeRows_.size(); ++i) {
                if (changeRows_[i].group == ChangeGroup::Untracked) {
                    changeListItems_.push_back({false, i});
                }
            }
        }
    }
}

void GitPageController::onStart() {
    // N10 会话内切回（切其它模块再回来）：仓/树模式/右栏打开文件与视图类型三项
    // 保留；树展开状态与滚动位不保留（PRD §4 N10：返回回根层首屏）。worker/回投
    // 箱已随 onStop 重置，status 与打开内容全部重取。
    if (phase_ == GitPagePhase::Ready && !workdirUtf8_.empty()) {
        tree_.reset();
        tree_.setMode(mode_);
        treeScrollOffset_ = 0.0f;
        resetRightPane();
        if (!selectedPath_.empty()) {
            if (rightShowsDiff_) {
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
    // N10 切回恢复：同仓再选（切换仓库 → 空态页 → 重选同仓）恢复树模式 + 右栏
    // 打开文件与视图类型；树展开与滚动位不保留。异仓 = 全新会话。
    const std::string prevWorkdir = workdirUtf8_;
    const std::string prevSelected = selectedPath_;
    const bool prevDiff = rightShowsDiff_;
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
    tree_.reset();
    tree_.setMode(mode_);
    if (mode_ == GitTreeModel::Mode::ChangesOnly && !lastSnapshot_.entries.empty()) {
        tree_.applyStatus(lastSnapshot_);
    }
    treeScrollOffset_ = 0.0f;
    resetRightPane();
    if (sameRepo && !prevSelected.empty()) {
        selectedPath_ = prevSelected;
        rightShowsDiff_ = prevDiff;
        if (prevDiff) {
            requestDiff(prevSelected, /*force=*/true);
        } else {
            requestContent(prevSelected, /*force=*/true);
        }
    } else {
        selectedPath_.clear();
        rightShowsDiff_ = false;
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
    rightShowsDiff_ = false;
    treeScrollOffset_ = 0.0f;
    changeRows_.clear();
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
    if (rightShowsDiff_) {
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
    changeRows_ = GitChangesModel::build(snapshot);
    tree_.applyStatus(snapshot);
}

void GitPageController::requestWalk(const std::string& dirRel) {
    tree_.markLoading(dirRel);
    const std::string path = workdirUtf8_;
    worker_->postWithRepo(path, [this, path, dirRel](platform::git::Repo* repo) {
        const std::filesystem::path workdir =
            repo != nullptr ? repo->workdir()
                            : platform::git::utf8ToPath(path);
        // repo == nullptr（open 失败）时 walk 无 ignore 判定——降级纯 fs 枚举
        WalkResult walked = GitTreeModel::walkChildren(repo, workdir, dirRel);
        postToUi([this, path, dirRel, walked] {
            if (path != workdirUtf8_) return;  // 期间已切仓：过期结果丢弃
            if (!walked.ok) {
                if (walked.permission) {
                    tree_.markNoAccess(dirRel);
                } else {
                    tree_.markLoadFailed(dirRel, walked.message);
                }
                return;
            }
            tree_.applyChildren(dirRel, std::move(walked.entries));
        });
    });
}

void GitPageController::setTreeMode(GitTreeModel::Mode mode) {
    if (mode_ == mode) return;  // I-01 连点幂等
    mode_ = mode;
    tree_.setMode(mode);
    treeScrollOffset_ = 0.0f;  // 切换后树滚动回顶（spec I-01）
    if (mode == GitTreeModel::Mode::AllFiles) {
        // 回全部文件：已展开目录缓存保留（不重走）；根层缺失则 composePump 补投
    }
}

void GitPageController::toggleDir(const std::string& dirRel) {
    if (tree_.mode() != GitTreeModel::Mode::AllFiles) return;
    const bool expanded = tree_.isExpanded(dirRel);
    tree_.setExpanded(dirRel, !expanded);
    if (expanded) {
        treeScrollOffset_ = treeScrollOffset_;  // 折叠不动滚动位（virtualList 自适应）
    }
}

void GitPageController::selectTreeFile(const std::string& relPath) {
    selectedPath_ = relPath;
    rightShowsDiff_ = false;  // I-03：右栏切内容视图
    pendingContentLine_.reset();   // 非搜索选中：清除定位锚（jumpToHit 随后重设）
    contentHighlightLine_ = 0;
    requestContent(relPath, /*force=*/false);
}

void GitPageController::selectChange(const std::string& path) {
    selectedPath_ = path;
    rightShowsDiff_ = true;  // I-04：右栏切 diff（N7→N8）
    pendingContentLine_.reset();
    contentHighlightLine_ = 0;
    requestDiff(path, /*force=*/false);
}

void GitPageController::retryDir(const std::string& dirRel) {
    tree_.requestRetry(dirRel);  // pendingDirs() 下帧重投（I-08）
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
    staleBar_ = false;
}

void GitPageController::requestContent(const std::string& relPath, bool force) {
    if (workdirUtf8_.empty() || relPath.empty()) return;
    if (!force && contentPath_ == relPath &&
        (contentState_ == RightPaneState::Loading ||
         contentState_ == RightPaneState::Ready)) {
        return;  // 已在加载/已加载：连点幂等（I-03/I-05）
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
    diffPath_ = relPath;
    diffState_ = RightPaneState::Loading;
    diffError_.clear();
    const std::uint64_t req = ++diffReq_;
    const std::string path = workdirUtf8_;
    worker_->postWithRepo(path, [this, relPath, req, path](platform::git::Repo* repo) {
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
        std::string placeholder;
        if (ok) {
            for (auto& p : diff.value().patches) {
                if (p.newPath == relPath || p.oldPath == relPath) {
                    patch = std::move(p);
                    break;
                }
            }
            // bug#18：untracked 条目 libgit2 不带 UNTRACKED_CONTENT → patch 无 hunks
            // （F-09 内存约束下禁加该 flag）。模型层合成：≤1MiB 文本全增行 /
            // >1MiB·二进制占位 / 空文件保持空态（复用 content_model 读路径判据）
            placeholder = synthesizeUntrackedDiff(
                patch,
                platform::git::utf8ToPath(path) / platform::git::utf8ToPath(relPath));
        } else {
            message = diff.errorMessage();
        }
        postToUi([this, relPath, req, patch = std::move(patch), ok, message,
                  placeholder = std::move(placeholder)]() mutable {
            if (req != diffReq_ || relPath != diffPath_) return;  // 过期丢弃
            if (!ok) {
                diffState_ = RightPaneState::Error;
                diffError_ = message;
                return;
            }
            diffModel_.setPatch(std::move(patch), std::move(placeholder));
            diffState_ = RightPaneState::Ready;
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
    tree_.reset();
    tree_.setMode(mode_);
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
        // ② walk：从根逐目录复用 GitTreeModel::walkChildren 枚举全量文件清单
        //（.git/ignored/symlink 不跟随/Subrepo/NoAccess 语义零漂移）；untracked
        // 目录按快照跳过不递归（防 node_modules 式 hang，F-12 同源口径）
        std::vector<std::string> files;
        std::vector<std::string> dirs;
        dirs.push_back(std::string());
        while (!dirs.empty()) {
            if (seq != searchSeq_.load()) return;  // 目录边界过期自灭
            const std::string dir = std::move(dirs.back());
            dirs.pop_back();
            const WalkResult walked = GitTreeModel::walkChildren(repo, workdir, dir);
            if (!walked.ok) continue;  // 无权限/枚举失败：静默跳过（宽松语义）
            for (auto& row : walked.entries) {
                if (row.kind == TreeRowKind::Directory) {
                    const std::string withSlash = row.relPath + "/";
                    const bool skip =
                        std::find(skipDirs.begin(), skipDirs.end(), withSlash) !=
                        skipDirs.end();
                    if (!skip) dirs.push_back(std::move(row.relPath));
                } else if (row.kind == TreeRowKind::File) {
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
    // F-08②：N6 复用（selectTreeFile：右栏切内容视图 + 树同步选中）+ 内容锚
    // 行号（内容 Ready 后视图按行盒几何滚动——clue#14 禁偏移量直设口径）
    selectTreeFile(hit.relPath);
    pendingContentLine_ = hit.line;
    // P1-2（need:14 审计）：同文件已 Ready 时 requestContent 幂等早退，不经过
    // Ready 落值点——重复点击/同文件第二命中只滚动不高亮。直接落值：未 Ready 时
    // 高亮先行无害（行渲染仅在 Ready 体），Ready 到达后落值点赋同值幂等。
    contentHighlightLine_ = hit.line;
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


size_t GitPageController::changedCount() const {
    size_t n = 0;
    for (const auto& row : changeRows_) {
        if (row.group == ChangeGroup::Changed) ++n;
    }
    return n;
}

size_t GitPageController::untrackedCount() const {
    size_t n = 0;
    for (const auto& row : changeRows_) {
        if (row.group == ChangeGroup::Untracked) ++n;
    }
    return n;
}

bool GitPageController::selectedHasChange() const {
    if (selectedPath_.empty()) return false;
    for (const auto& row : changeRows_) {
        if (row.path == selectedPath_) return true;
    }
    return false;
}

std::string GitPageController::repoDisplayName() const {
    return repoNameFromPath(workdirUtf8_);
}

} // namespace module::git
