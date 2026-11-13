// src/module_git/git_page_controller.h
// Git 页面控制器（need:7 批次二）：N1~N5 流转状态机 + worker 结果回投泵。
//  状态面：Empty（空态页/校验失败/记忆失效）→ Loading（校验中）→ Ready（三栏主界面）。
//  线程纪律：libgit2 / 目录枚举只在 worker；UI 线程 compose 每帧 composePump()
//  （回投 inbox 排空 + 懒加载投递），模型/快照字段只在 UI 线程读写。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <eui/types.h>

#include "git_changes_model.h"
#include "git_content_model.h"
#include "git_diff_model.h"
#include "git_search_model.h"
#include "git_tree_model.h"
#include "git_worker.h"
#include "platform/file_watcher.h"

namespace module::git {

enum class GitPagePhase { Empty, Ready };

// F-13 watcher 事件过滤（技术方案 §2.2 过滤表；纯函数，doctest 逐行覆盖）：
//   工作树路径 / .git/index、.git/HEAD、.git/refs/** → status 脏（节流队列）；
//   .git 其余（objects/logs/gc 洪泛）→ 忽略；
//   Unknown（缓冲溢出，relativePath 空）→ 全量重扫语义 = status 脏 + 无条件置过期条。
struct WatchVerdict {
    bool marksStatus;      // 投 1s 节流刷新队列
    bool marksOpenStale;   // 无条件置过期条（元数据跳变/溢出）；工作树路径由
                           // 控制器另行比对打开文件（onWatchEvents）
};
WatchVerdict classifyWatchEvent(platform::FileWatchAction action,
                                const std::string& relativePath);

// 右栏内容/diff 面状态（need:10 批次三）：None=未选择；Loading=后台计算中；
// Ready=行模型在位；Placeholder=四态占位（文案由 content().state 区分）；
// Error=读取/计算失败（errorMessage + 重试）。
enum class RightPaneState { None, Loading, Ready, Placeholder, Error };

class GitPageController {
public:
    // F-01 校验失败三文案（错误码区分）：
    //   ①非 git 仓（ValidationError）②无权限（FilePermissionDenied）③记忆仓失效
    struct EmptyError {
        std::string title;    // 错误条主文案
        std::string detail;   // 第二行说明（记忆失效时=「已清除该条记忆，请重新选择仓库。」）
        bool fromMemory = false;
    };

    explicit GitPageController(GitWorker* worker);

    // ---- 生命周期（模块 onStart/onStop 转发）----
    void onStart();   // N1：记忆仓自动恢复（异步校验，失效即清除）；会话存活时走 N10 会话恢复
    void onStop();    // 停 worker（模块侧已停）；状态保内存不落盘（N10 恢复范围=批次 #11）

    // ---- compose 每帧泵（UI 线程）----
    void composePump();

    // ---- watcher 接线（F-13；回调在 watcher 后台线程，只走 postToUi）----
    void onWatchEvents(std::vector<platform::FileWatchEvent> events);
    const std::string& watchRoot() const { return workdirUtf8_; }
    // 开仓/切仓后由模块重挂 watcher（换 watch 根）
    void setRepoChangeCallback(std::function<void(const std::string&)> cb) {
        repoChangeCb_ = std::move(cb);
    }
    // 模块侧回报 watcher 可用性：false → composePump 降级 2s 轮询 status（仅可见时）
    void setWatcherActive(bool active) { watcherActive_ = active; }

    // ---- 交互动作（UI 线程）----
    void beginPickRepo();   // I-10：空态页主按钮 → 系统对话框 → 异步校验
    void switchRepo();      // I-09：切换仓库 → N2 空态页（N10 三项保留，同仓再选即恢复）
    void setTreeMode(GitTreeModel::Mode mode);  // I-01（连点幂等）
    void toggleDir(const std::string& dirRel);  // I-02 展开/折叠（懒加载触发点）
    void selectTreeFile(const std::string& relPath);  // I-03 → 右栏内容视图
    void selectChange(const std::string& path);       // I-04 → 右栏 diff 视图
    void retryDir(const std::string& dirRel);         // I-08 树行内重试
    void retryContent();                              // I-08 内容错误态重试
    void retryDiff();                                 // I-08 diff 错误态重试
    void reloadStale();  // F-13③：过期条「点击重载」→ 重读打开内容/diff + status
    void refresh();  // I-07：手动刷新（1s 节流，F-09④；watcher 自动刷新同闸）

    // ---- 全局搜索（need:14 F-08；技术方案 §6.2；取消语义 §3）----
    // 搜索框草稿（input value；onChange 持有，防抖到 300ms 由 composePump 提交）
    const std::string& searchDraft() const { return searchDraft_; }
    void setSearchDraft(std::string v) { searchDraft_ = std::move(v); }
    void markSearchEdited();      // 输入编辑时刻戳（防抖起点）
    void submitSearchNow();       // Enter 立即提交（跳过 debounce）
    void cancelSearch();          // ESC / 清除 ×：显式取消（seq++ + 回无搜索态）
    void jumpToHit(const SearchHit& hit);  // F-08②：selectTreeFile + 内容锚行号
    // 浮层显隐（失焦收起 ≠ 取消：扫描后台继续，spinner 指示——§3）
    bool searchPopupVisible() const { return searchPopupVisible_; }
    void setSearchPopupVisible(bool v) { searchPopupVisible_ = v; }
    // 定位锚：jumpToHit 记行号 → 内容 Ready 后视图按行盒几何消费（内容锚滚动）
    const std::optional<int>& pendingContentLine() const { return pendingContentLine_; }
    void clearPendingContentLine() { pendingContentLine_.reset(); }

    // ---- 视图读面（UI 线程，rows()/数据均为状态派生）----
    GitPagePhase phase() const { return phase_; }
    bool validating() const { return validating_; }
    // 校验等待是否已超过 1s（spec §5.7「>1s 才显旋转圈」）
    bool validateSpinnerDue() const;
    const std::optional<EmptyError>& emptyError() const { return emptyError_; }
    const std::string& repoPathUtf8() const { return workdirUtf8_; }
    std::string repoDisplayName() const;  // 仓名 = 路径末段
    GitTreeModel& tree() { return tree_; }
    const GitTreeModel& tree() const { return tree_; }
    // 树可见行（composePump 每帧物化；virtualList 行闭包可能在 compose 作用域外
    // 被布局期回调——必须指向成员而非栈上临时）
    const std::vector<TreeRow>& visibleRows() const { return visibleRows_; }
    // 变更/未跟踪两组行（build 已按路径去重排序）
    const std::vector<ChangeRow>& changeRows() const { return changeRows_; }
    // 中栏混排行模型（组头/条目交错；与 changeRows 同帧物化）
    struct ChangeListItem {
        bool header;
        size_t rowIndex;  // header 时=组号（0=变更 1=未跟踪），条目时=changeRows 下标
    };
    const std::vector<ChangeListItem>& changeListItems() const { return changeListItems_; }
    size_t changedCount() const;
    size_t untrackedCount() const;
    bool statusLoading() const { return statusLoading_; }
    const std::string& statusError() const { return statusError_; }
    // 选中与右栏动线
    const std::string& selectedPath() const { return selectedPath_; }
    bool rightShowsDiff() const { return rightShowsDiff_; }
    // 该选中文件是否有变更（决定 diff Tab 是否出现，PRD §6 契约）
    bool selectedHasChange() const;
    float treeScrollOffset() const { return treeScrollOffset_; }
    void setTreeScrollOffset(float v) { treeScrollOffset_ = v; }

    // ---- 右栏数据面（批次三；内容=行文本+高亮 runs，diff=GitDiffModel）----
    RightPaneState contentState() const { return contentState_; }
    const FileContent& content() const { return content_; }
    // 每行高亮 runs（行内字节偏移；load 完成时按当时主题一次性算好，主题切换
    // 到重新选择前保持——查看器口径，编辑器不做）
    const std::vector<std::vector<eui::TextRun>>& contentRuns() const { return contentRuns_; }
    const std::string& contentPath() const { return contentPath_; }
    const std::string& contentError() const { return contentError_; }
    RightPaneState diffState() const { return diffState_; }
    const std::string& diffPath() const { return diffPath_; }
    const std::string& diffError() const { return diffError_; }
    GitDiffModel& diffModel() { return diffModel_; }
    float contentScrollOffset() const { return contentScrollOffset_; }
    void setContentScrollOffset(float v) { contentScrollOffset_ = v; }
    float diffScrollOffset() const { return diffScrollOffset_; }
    void setDiffScrollOffset(float v) { diffScrollOffset_ = v; }
    // 横向偏移（F-07⑥ 真横滚；状态在控制器，virtualList onHChange 回写）
    float contentHOffset() const { return contentHOffset_; }
    void setContentHOffset(float v) { contentHOffset_ = v; }
    float diffHOffset() const { return diffHOffset_; }
    void setDiffHOffset(float v) { diffHOffset_ = v; }

    // ---- 全局搜索读面（UI 线程；结果为状态派生）----
    // None=无查询（浮层不显示）；Scanning=扫描中（渐进出果）；Done=终态；
    // Error=失败（searchError_ 带原因）
    enum class GitSearchState { None, Scanning, Done, Error };
    GitSearchState searchState() const { return searchState_; }
    const std::string& searchQuery() const { return searchQuery_; }
    const std::vector<SearchHit>& searchResults() const { return searchResults_; }
    size_t searchScanned() const { return searchScanned_; }
    size_t searchTotalFiles() const { return searchTotalFiles_; }
    bool searchTruncated() const { return searchTruncated_; }
    const std::string& searchError() const { return searchError_; }
    size_t searchFileCount() const;  // M 个文件 = results 去重 relPath（≤200，现算）
    // 命中行高亮（1 起；0=无；内容 Ready 后常亮底色——F-08②）
    int contentHighlightLine() const { return contentHighlightLine_; }
    // 结果浮层列表滚动位（virtualList offset 状态持有，与 content 同款）
    float searchPopupScroll() const { return searchPopupScroll_; }
    void setSearchPopupScroll(float v) { searchPopupScroll_ = v; }

    // F-13③ 过期条：watcher 判定已打开内容可能过期 → 覆盖条「内容已过期，点击重载」
    // （覆盖绘制不进布局，出现/消失零滚动跳动）；重载成功后消失。
    bool staleBarVisible() const { return staleBar_; }

private:
    void postToUi(std::function<void()> continuation);
    void openRepo(const std::string& repoPathUtf8);
    void closeRepo();
    void validateAsync(const std::string& repoPathUtf8, bool fromMemory);
    void requestStatus();
    // F-09④ 1s 节流闸：距上次 status 重算 ≥1s 才放行（watcher 自动与手动刷新同闸）
    bool refreshThrottled() const;
    void requestWalk(const std::string& dirRel);
    void applyStatusSnapshot(const platform::git::StatusSnapshot& snapshot);
    void requestContent(const std::string& relPath, bool force);
    void requestDiff(const std::string& relPath, bool force);
    void buildContentRuns();  // Ok 态 load 后一次性算高亮 runs（O(n)，UI 线程）
    void resetRightPane();    // 切仓/关仓清右栏数据面 + 作废在途请求
    void submitSearch(const std::string& rawNeedle);  // trim 后提交（空=取消语义）
    void requestSearch(const std::string& needle);    // 投 walk+match 后台任务（seq++）
    void resetSearchFace();                           // 清结果回无搜索态（seq 不动）

    GitWorker* worker_;
    GitTreeModel tree_;
    std::vector<TreeRow> visibleRows_;
    std::vector<ChangeRow> changeRows_;
    std::vector<ChangeListItem> changeListItems_;
    platform::git::StatusSnapshot lastSnapshot_;

    GitPagePhase phase_ = GitPagePhase::Empty;
    bool validating_ = false;
    std::chrono::steady_clock::time_point validateStart_;
    std::optional<EmptyError> emptyError_;
    std::string workdirUtf8_;
    std::string statusError_;
    bool statusLoading_ = false;
    std::string selectedPath_;
    bool rightShowsDiff_ = false;
    float treeScrollOffset_ = 0.0f;
    GitTreeModel::Mode mode_ = GitTreeModel::Mode::AllFiles;

    // 右栏数据面（批次三）：过期结果按请求序号丢弃（contentReq_/diffReq_）
    FileContent content_;
    std::string contentPath_;
    RightPaneState contentState_ = RightPaneState::None;
    std::string contentError_;
    std::uint64_t contentReq_ = 0;
    std::vector<std::vector<eui::TextRun>> contentRuns_;
    GitDiffModel diffModel_;
    std::string diffPath_;
    RightPaneState diffState_ = RightPaneState::None;
    std::string diffError_;
    std::uint64_t diffReq_ = 0;
    float contentScrollOffset_ = 0.0f;
    float diffScrollOffset_ = 0.0f;
    float contentHOffset_ = 0.0f;
    float diffHOffset_ = 0.0f;
    int contentHighlightLine_ = 0;  // 命中行（1 起；0=无）——jumpToHit 锚常亮底色

    // 全局搜索状态面（need:14；字段全 UI 线程读写，searchSeq_ 除外——
    // worker 每文件边界读，atomic）
    std::atomic<std::uint64_t> searchSeq_{0};
    GitSearchState searchState_ = GitSearchState::None;
    std::string searchQuery_;       // 当前生效查询词（空=无查询）
    std::string searchDraft_;       // 搜索框草稿（input value）
    std::vector<SearchHit> searchResults_;
    size_t searchScanned_ = 0;
    size_t searchTotalFiles_ = 0;
    bool searchTruncated_ = false;
    std::string searchError_;
    bool searchPopupVisible_ = false;
    float searchPopupScroll_ = 0.0f;
    std::optional<int> pendingContentLine_;  // jumpToHit 锚（Ready 后消费）
    // 防抖（composePump 帧驱动 300ms；Enter 跳过）
    bool searchEditPending_ = false;
    std::chrono::steady_clock::time_point searchEditAt_{};

    // F-13 刷新联动（watcher → 过滤 → 节流；onWatchEvents 只走 postToUi，
    // 这些字段全在 UI 线程读写）
    std::function<void(const std::string&)> repoChangeCb_;
    bool watcherActive_ = true;   // false → 2s 轮询降级
    bool watchDirtyUi_ = false;   // 1s 节流队列（trailing：到窗即发一次）
    bool staleBar_ = false;
    std::chrono::steady_clock::time_point lastStatusAt_{};

    // worker → UI 回投箱（互斥；只存闭包，composePump 排空执行）
    std::mutex inboxMu_;
    std::deque<std::function<void()>> inbox_;
};

} // namespace module::git
