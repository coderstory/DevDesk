// src/module_git/git_page_controller.h
// Git 页面控制器（need:291 两栏重设计；need:299 搜索删除/列表过滤/最近仓下拉）：
// N1~N5 流转状态机 + worker 结果回投泵。
//  状态面：Empty（空态页/校验失败/记忆失效）→ Loading（校验中）→ Ready（两栏主界面）。
//  线程纪律：libgit2 / 目录枚举只在 worker；UI 线程 compose 每帧 composePump()
//  （回投 inbox 排空），模型/快照字段只在 UI 线程读写。
//  动线（设计 §6）：文件行点击=selectChange→diff 常驻主视图；「内容」=右卡头部
//  tab 次视图；目录行点击=展开/收起永不进 diff（bug#290 根治口径，F-05）；
//  untracked 目录自动枚举首层（裁决④）+ 嵌套 lazy 展开（F-09 不递归不破）。
//  全仓搜索已删除（need:300 老板令「全仓库搜索文本的功能删除了把，没意义」），
//  本页唯一查找动线=列表过滤输入框（need:299 §4.9）。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <eui/types.h>

#include "git_change_list_model.h"
#include "git_content_model.h"
#include "git_diff_model.h"
#include "git_fs_walk.h"
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
    // D5-A：右栏视图 tab（差异=常驻主视图默认；内容=次视图）
    enum class RightTab { Diff, Content };

    // F-01 校验失败三文案（错误码区分）：
    //   ①非 git 仓（ValidationError）②无权限（FilePermissionDenied）③记忆仓失效
    struct EmptyError {
        std::string title;    // 错误条主文案
        std::string detail;   // 第二行说明（记忆失效时=「已清除该条记忆，请重新选择仓库。」）
        bool fromMemory = false;
    };

    // 最近仓下拉行（need299 §3.1；每帧现算，≤kMaxRecentRepos 条）
    struct RecentRepoEntry {
        std::string path;   // UTF-8 绝对路径
        std::string name;   // 仓名 = 路径末段
        bool exists = false;   // 目录仍在（失效行置灰+「已失效」，仍可点击重开）
        bool current = false;  // 当前仓（右缘 ✓，点击=收起幂等不重载）
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
    void beginPickRepo();   // I-10：空态页主按钮/下拉「浏览目录…」→ 系统对话框 → 异步校验
    void switchRepo();      // I-09：迁移 N2 空态页（N10 右栏两项保留，同仓再选即恢复）
    // D1-C 树/平铺双态（连点幂等；展开位图两形态互不消费=设计 §6.3）
    void setViewMode(ChangeViewMode mode);
    ChangeViewMode viewMode() const { return viewMode_; }
    // D6-A 组头折叠（>200 默认折叠注入在模型侧）
    void toggleGroup(ChangeListRow::Group group);
    // 目录行展开/收起（D3-A：点击=toggle 永不进 diff）。tracked 目录纯模型位图；
    // untracked 嵌套 lazy 目录 = 懒枚举触发点（F-04：markLoading→worker walkChildren→
    // applyUntrackedChildren；seq 过期丢弃同 requestStatus 口径）。顶层 untracked
    // 条目无目录行（裁决④），由 applyStatusSnapshot 自动枚举
    void toggleDir(const std::string& dirRel);
    void toggleUntrackedDir(const std::string& dirRel);
    void retryUntrackedDir(const std::string& dirRel);  // N4 错误态重试
    void selectChange(const std::string& path);       // 文件行 → diff 主动线（F-05）
    void setRightTab(RightTab tab);                   // 差异/内容 tab（§6.6 切换不重发请求）
    RightTab rightTab() const { return rightTab_; }
    // D4-A 左栏折叠/展开（§6.5 连点幂等；折叠态右卡满宽）
    bool listCollapsed() const { return listCollapsed_; }
    void toggleListCollapsed() { listCollapsed_ = !listCollapsed_; }
    void retryContent();                              // I-08 内容错误态重试
    void retryDiff();                                 // I-08 diff 错误态重试
    void retryStatus();                               // N2 扫描失败重试（绕 1s 节流闸）
    void reloadStale();  // F-13③：过期条「点击重载」→ 重读打开内容/diff + status
    void refresh();  // I-07：手动刷新（1s 节流，F-09④；watcher 自动刷新同闸）

    // ---- 列表过滤（need:299 §4.2b/§4.9；纯输入框即输即滤，无按钮无防抖）----
    const std::string& filterDraft() const { return filterDraft_; }
    void setFilterDraft(std::string v);  // 存草稿 + 即时应用（IME 组合中框架不回调）
    void clearFilter();                  // ×/Esc/逐字删空：恢复全量 + 选中行 scrollIntoView
    bool filterActive() const;           // 生效查询词非空（空态判定用）

    // ---- 切换仓库历史下拉（need:299 §3.1/§6.10）----
    bool repoDropdownVisible() const { return repoDropdownVisible_; }
    void setRepoDropdownVisible(bool v) { repoDropdownVisible_ = v; }
    void toggleRepoDropdown() { repoDropdownVisible_ = !repoDropdownVisible_; }
    // 下拉行集（每帧现算：≤8 条 exists() 探测——失效行置灰+「已失效」）
    const std::vector<RecentRepoEntry>& recentRepoEntries() const { return recentEntries_; }
    // 仓行点击：当前仓=收起幂等不重载；失效仓=尝试重开（失败走 N1 错误块 §6.10②）
    void openRecentRepo(const std::string& repoPathUtf8);

    // ---- 视图读面（UI 线程，rows()/数据均为状态派生）----
    GitPagePhase phase() const { return phase_; }
    bool validating() const { return validating_; }
    // 校验等待是否已超过 1s（spec §5.7「>1s 才显旋转圈」）
    bool validateSpinnerDue() const;
    const std::optional<EmptyError>& emptyError() const { return emptyError_; }
    const std::string& repoPathUtf8() const { return workdirUtf8_; }
    std::string repoDisplayName() const;  // 仓名 = 路径末段
    GitChangeListModel& listModel() { return listModel_; }
    // 变更列表行序列（composePump 每帧物化；virtualList 行闭包可能在 compose
    // 作用域外被布局期回调——必须指向成员而非栈上临时）
    const std::vector<ChangeListRow>& listRows() const { return listRows_; }
    bool statusLoading() const { return statusLoading_; }
    const std::string& statusError() const { return statusError_; }
    // 选中与右栏动线
    const std::string& selectedPath() const { return selectedPath_; }
    // 选中行状态徽标（右卡头部用；未选中/已消失 = nullopt；模型全量判定，
    // 折叠目录下的选中行同样可取）
    std::optional<platform::git::StatusKind> selectedRowStatus() const;
    float listScrollOffset() const { return listScrollOffset_; }
    void setListScrollOffset(float v) { listScrollOffset_ = v; }

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
    void requestUntrackedChildren(const std::string& dirRel);
    // 裁决④：status 落地后自动枚举全部待枚举 untracked 目录（顶层条目；
    // 嵌套 lazy 目录仍走展开触发）
    void autoTriggerUntrackedEnum();
    void applyStatusSnapshot(const platform::git::StatusSnapshot& snapshot);
    // N8：刷新后选中文件已不在列表 → 清选中+右栏回空态（竞态删除口径）
    void clearSelectionIfMissing();
    // 行序列内是否含该文件行（选中保持判定；listRows_ 现算）
    bool rowHasFile(const std::string& relPath) const;
    void requestContent(const std::string& relPath, bool force);
    void requestDiff(const std::string& relPath, bool force);
    void buildContentRuns();  // Ok 态 load 后一次性算高亮 runs（O(n)，UI 线程）
    void resetRightPane();    // 切仓/关仓清右栏数据面 + 作废在途请求
    // 选中行滚至可视（树/平铺切换与清过滤共用；PRD F-02 scrollIntoView 口径）
    void scrollSelectedIntoView();
    // 最近仓 LRU 维护（openRepo 成功时：前插+lexically_normal 折叠小写键去重+截 8）
    void recordRecentRepo(const std::string& repoPathUtf8);

    GitWorker* worker_;
    GitChangeListModel listModel_;
    std::vector<ChangeListRow> listRows_;
    platform::git::StatusSnapshot lastSnapshot_;
    ChangeViewMode viewMode_ = ChangeViewMode::Tree;  // D1-C 默认树形态

    GitPagePhase phase_ = GitPagePhase::Empty;
    bool validating_ = false;
    std::chrono::steady_clock::time_point validateStart_;
    std::optional<EmptyError> emptyError_;
    std::string workdirUtf8_;
    std::string statusError_;
    bool statusLoading_ = false;
    std::string selectedPath_;
    RightTab rightTab_ = RightTab::Diff;      // D5-A：diff 常驻默认
    bool listCollapsed_ = false;              // D4-A 左栏折叠态
    float listScrollOffset_ = 0.0f;
    // F-04 lazy 已请求集（防重入：展开已缓存目录不再重走；刷新保留，切仓清）
    std::set<std::string> requestedUntrackedDirs_;

    // 列表过滤（need:299 §4.9；生效查询在模型，草稿在此回显）
    std::string filterDraft_;
    // 切换仓库下拉（need:299 §3.1）+ 最近仓历史（≤8 LRU，git_manager.json 持久化）
    bool repoDropdownVisible_ = false;
    std::vector<std::string> recentRepos_;    // 新→旧；onStart 一次性载入
    bool recentLoaded_ = false;
    mutable std::vector<RecentRepoEntry> recentEntries_;  // 下拉可见时 composePump 刷新

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
