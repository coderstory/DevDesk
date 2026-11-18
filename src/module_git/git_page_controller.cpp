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
#include <filesystem>
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

// LRU 去重键：lexically_normal + 折叠小写（Windows 路径大小写不敏感语义；
// 非对称盘符大小写写入同条历史，无平台 #ifdef）
std::string recentRepoKey(const std::string& pathUtf8) {
    std::string key = platform::git::pathToUtf8(
        std::filesystem::path(platform::git::utf8ToPath(pathUtf8)).lexically_normal());
    for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return key;
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
    // ②（need299）untracked 枚举双驱动：顶层条目=status 后自动（裁决④，
    // autoTriggerUntrackedEnum）；嵌套 lazy 目录=toggle 驱动。POC① 单层枚举
    // <20ms，无需预算限流。
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
        // 最近仓下拉行集（need299 §3.1）：下拉可见时每帧刷新（≤8 条 exists 探测
        // +当前仓标记；失效行置灰——§3.1 失效仓行）
        if (repoDropdownVisible_) {
            recentEntries_.clear();
            recentEntries_.reserve(recentRepos_.size());
            for (const auto& p : recentRepos_) {
                RecentRepoEntry e;
                e.path = p;
                e.name = repoNameFromPath(p);
                e.exists = std::filesystem::exists(platform::git::utf8ToPath(p));
                e.current = (p == workdirUtf8_);
                recentEntries_.push_back(std::move(e));
            }
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
    // 切换后选中行保持可见（PRD F-02 scrollIntoView 口径）
    scrollSelectedIntoView();
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

// ---- 列表过滤（need:299 §4.2b/§4.9）----

void GitPageController::setFilterDraft(std::string v) {
    filterDraft_ = std::move(v);
    listModel_.setFilter(filterDraft_);  // 即输即滤（无防抖；IME 组合中框架不回调）
    listRows_ = listModel_.rows();
}

void GitPageController::clearFilter() {
    if (filterDraft_.empty() && listModel_.filterQuery().empty()) return;
    filterDraft_.clear();
    listModel_.setFilter({});
    listRows_ = listModel_.rows();
    // §4.9：清空=恢复全量+原展开位图；对仍命中的选中行 scrollIntoView
    scrollSelectedIntoView();
}

bool GitPageController::filterActive() const {
    return !listModel_.filterQuery().empty();
}

// ---- 切换仓库下拉（need:299 §3.1/§6.10）----

void GitPageController::openRecentRepo(const std::string& repoPathUtf8) {
    repoDropdownVisible_ = false;  // 选中仓=收起后执行（§6.10）
    if (phase_ != GitPagePhase::Ready || repoPathUtf8 == workdirUtf8_) {
        return;  // 当前仓行点击=收起不重载（§6.10④ 幂等）
    }
    validateAsync(repoPathUtf8, /*fromMemory=*/false);
}

void GitPageController::onStart() {
    if (!recentLoaded_) {
        recentRepos_ = GitRepoStore::load().recentRepos;  // 最近仓历史一次性载入
        recentLoaded_ = true;
    }
    // N10 会话内切回（切其它模块再回来）：仓/右栏打开文件与视图类型保留；列表
    // 展开/折叠位图随模型保留（F-09 同口径）。worker/回投箱已随 onStop 重置，
    // status 与打开内容全部重取。
    if (phase_ == GitPagePhase::Ready && !workdirUtf8_.empty()) {
        resetRightPane();
        if (!selectedPath_.empty()) {
            if (rightTab_ == RightTab::Diff) {
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
    // 停机：清回投箱（停机期间不再有回投目标；在途 worker 任务随 stop 丢弃）
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
    repoDropdownVisible_ = false;
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
                    if (auto saved = GitRepoStore::save(GitRepoMemory{path, recentRepos_});
                        saved.isFail()) {
                        LOG_WARN("GitPage: 记忆仓刷新失败: " + saved.errorMessage());
                    }
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
                return;
            }
            // need299 §6.10②：下拉失效仓行/浏览目录校验失败 → 回空态页错误块
            //（Ready 期点击也落同一面，错误不静默）
            closeRepo();
            if (code == core::ErrorCode::FilePermissionDenied) {
                emptyError_ = EmptyError{kErrNoPermission, {}, false};
            } else {
                emptyError_ = EmptyError{kErrNotARepo, {}, false};
            }
        });
    });
}

void GitPageController::openRepo(const std::string& repoPathUtf8) {
    // N10 切回恢复：同仓再选（下拉重选当前仓不进此路径——openRecentRepo 幂等早退；
    // 空态页重选同仓恢复右栏打开文件与视图类型；列表展开位图与滚动位不保留）。
    const std::string prevWorkdir = workdirUtf8_;
    const std::string prevSelected = selectedPath_;
    const RightTab prevTab = rightTab_;
    const bool sameRepo = phase_ == GitPagePhase::Empty && !prevWorkdir.empty() &&
                          prevWorkdir == repoPathUtf8;
    closeRepo();
    workdirUtf8_ = repoPathUtf8;
    phase_ = GitPagePhase::Ready;
    emptyError_.reset();
    // 最近仓 LRU 前插去重 + 记忆仓写入（含历史；最佳努力，失败只记日志）
    recordRecentRepo(repoPathUtf8);
    auto saved = GitRepoStore::save(GitRepoMemory{repoPathUtf8, recentRepos_});
    if (saved.isFail()) {
        LOG_WARN("GitPage: 记忆仓写入失败: " + saved.errorMessage());
    }
    listModel_.reset();  // 切仓=过滤词清空（§4.9；model.reset 连带清查询）
    filterDraft_.clear();
    requestedUntrackedDirs_.clear();
    listScrollOffset_ = 0.0f;
    resetRightPane();
    if (sameRepo && !prevSelected.empty()) {
        selectedPath_ = prevSelected;
        rightTab_ = prevTab;
        if (prevTab == RightTab::Content) {
            requestContent(prevSelected, /*force=*/true);
        } else {
            requestDiff(prevSelected, /*force=*/true);
        }
    } else {
        selectedPath_.clear();
        rightTab_ = RightTab::Diff;
    }
    if (repoChangeCb_) repoChangeCb_(workdirUtf8_);
    requestStatus();
}

void GitPageController::recordRecentRepo(const std::string& repoPathUtf8) {
    std::vector<std::string> updated;
    updated.reserve(GitRepoStore::kMaxRecentRepos);
    updated.push_back(repoPathUtf8);  // 新→旧，最近仓置顶
    const std::string key = recentRepoKey(repoPathUtf8);
    for (const auto& p : recentRepos_) {
        if (updated.size() >= GitRepoStore::kMaxRecentRepos) break;  // ≤8 条 LRU
        if (recentRepoKey(p) != key) updated.push_back(p);
    }
    recentRepos_ = std::move(updated);
}

void GitPageController::closeRepo() {
    phase_ = GitPagePhase::Empty;
    workdirUtf8_.clear();
    statusError_.clear();
    statusLoading_ = false;
    selectedPath_.clear();
    rightTab_ = RightTab::Diff;
    listScrollOffset_ = 0.0f;
    listModel_.reset();
    filterDraft_.clear();
    repoDropdownVisible_ = false;
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
    if (rightTab_ == RightTab::Diff) {
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
    // 裁决④：新快照落地的顶层 untracked 条目自动枚举首层（过滤词保留重应用）
    autoTriggerUntrackedEnum();
    clearSelectionIfMissing();
}

void GitPageController::autoTriggerUntrackedEnum() {
    // pendingUntrackedDirs：顶层条目（无视展开）+ 嵌套 lazy 已展开未缓存目录；
    // 在途/已缓存/错误待重试均不在列——requestedUntrackedDirs_ 防重入双保险
    for (const auto& dir : listModel_.pendingUntrackedDirs()) {
        if (requestedUntrackedDirs_.insert(dir).second) {
            requestUntrackedChildren(dir);
        }
    }
}

void GitPageController::clearSelectionIfMissing() {
    // N8（设计 §6.1 边界/§7）：选中文件被后台刷新删除 → 右栏回空态 + 列表去行
    if (selectedPath_.empty()) return;
    if (rowHasFile(selectedPath_)) return;
    selectedPath_.clear();
    rightTab_ = RightTab::Diff;
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
    // 模型全量判定（不依赖可见行集——选中行在折叠目录下同样可取徽标/判 U）
    return listModel_.fileStatus(selectedPath_);
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
        //（非 ignored 即 untracked），文件条目打 U 并入派生、子目录条目续 lazy。
        fs_walk::WalkResult walked = fs_walk::walkChildren(repo, workdir, dirRel);
        postToUi([this, path, dirRel, walked] {
            if (path != workdirUtf8_) return;  // 期间已切仓：过期结果丢弃
            if (!walked.ok) {
                listModel_.markUntrackedFailed(dirRel, walked.message);
                return;
            }
            const bool emptyDir = walked.entries.empty();
            listModel_.applyUntrackedChildren(dirRel, walked);
            // N4 竞态空目录（枚举期间文件被删）：行消失语义=触发一次刷新，
            // 快照更新后目录条目消失（设计 §7 N4 空格）
            if (emptyDir) requestStatus();
        });
    });
}

void GitPageController::selectChange(const std::string& path) {
    // F-05 主动线：文件行点击 → diff 即时切换；
    // rightTab_ 保持（设计 §6.1：已在「内容」tab 保持 tab 选择切文件）
    selectedPath_ = path;
    requestDiff(path, /*force=*/false);
}

void GitPageController::setRightTab(RightTab tab) {
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
    // 切仓/关仓（含同仓重开）：清右栏数据面 + 作废在途请求
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

void GitPageController::scrollSelectedIntoView() {
    if (selectedPath_.empty()) return;
    for (size_t i = 0; i < listRows_.size(); ++i) {
        if (listRows_[i].kind == ChangeListRow::Kind::File &&
            listRows_[i].relPath == selectedPath_) {
            // 32px 定高槽（视图 kRowSlotH 同值）；滚至行顶−半屏内可见
            listScrollOffset_ = std::max(0.0f, static_cast<float>(i) * 32.0f - 200.0f);
            break;
        }
    }
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
    // bug#296：未跟踪行（顶层枚举并入或嵌套 lazy 子文件）在 workdir↔HEAD diff 里
    // 无 delta 可匹配（F-09 不递归）→ 零匹配落「文件已无变更」误报。选中行状态
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
    // 全保留（模型内状态，applyStatusSnapshot 统一重建）；过滤词保留重应用（§4.9）
    requestStatus();
}

std::string GitPageController::repoDisplayName() const {
    return repoNameFromPath(workdirUtf8_);
}

} // namespace module::git
