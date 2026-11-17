// src/module_git/git_change_list_model.h
// 变更列表行模型（need291 Git 管理器重设计；技术方案卡 doc25 §3 详设落地）：
// status 快照 → staged/unstaged 双组分桶 → 每桶变更路径集派生目录树（树形态，
// md_tree.cpp:34-99 同款祖先目录集+逐级 emit）或目录段聚合平铺（设计 §4.7）；
// 单链压缩（唯一子目录链合并显示 a/b/c/，VSCode Compact Folders 惯例，clue#199）；
// untracked 目录条目（尾 '/'）剥尾归一（git_tree_model.cpp:140-152 先例）特化为
// 带 U 徽标目录行，展开走 lazy 单层枚举（git_fs_walk::walkChildren）——数据面
// 「untracked 目录整体一条不递归」硬约束不破（git_repo.h:17）。
// 行为变更点（D6-A，PRD F-12 验收原文）：staged 双侧同文件两组各一行——
// 组内去重、跨组不去重（旧 GitChangesModel::build 双侧合并口径退役，卡 §3）。
// 渲染纪律（clue#22）：rows() 是模型状态的纯函数——compose measure 每帧两跑
// 幂等；行盒与命中矩形由视图按（行, 宽度）现算，模型不缓存任何矩形。
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "git_fs_walk.h"
#include "platform/git/git_repo.h"

namespace module::git {

enum class ChangeViewMode { Tree, Flat };

struct ChangeListRow {
    enum class Kind { GroupHeader, Dir, File, Loading, Truncate, Error };
    enum class Group { Staged, Unstaged };
    Kind kind = Kind::File;
    Group group = Group::Unstaged;
    int depth = 0;
    std::string name;     // 末段显示名（目录含'/'；压缩链整体一个节点如 a/b/c/）
    std::string relPath;  // 相对 workdir POSIX（File=文件路径；Dir=目录路径；
                          // GroupHeader/Loading/Truncate/Error 为空或父目录）
    std::string dirRel;   // Kind==Dir 有效；untracked 目录=status 条目剥尾斜杠；
                          // Loading/Truncate/Error=父目录 dirRel（视图定位用）
    std::optional<platform::git::StatusKind> status;  // File 行徽标；Dir 行 U 时为 Untracked
    std::string oldPath;     // R 行旧路径（tooltip「old → new」用）
    size_t childCount = 0;   // tracked 派生 Dir=子树变更条目数；untracked Dir=0
                             //（lazy 前不可得不显计数，R4 定稿）；GroupHeader=组内计数
    size_t remainCount = 0;  // Kind==Truncate 有效（>500 截断余量；尾行文案由视图组）
    std::string errorText;   // Kind==Error 原因
};

class GitChangeListModel {
public:
    // 单目录文件行截断上限（设计 §6.2：>500 显示前 500 + 尾行「还有 N 个文件·
    // 在搜索中查看全部」，PRD N4）
    static constexpr size_t kMaxUntrackedChildren = 500;
    // 组头默认折叠阈值（POC②：>200 两组头默认收起，200/199 默认展开；PRD N2）
    static constexpr size_t kGroupCollapseThreshold = 200;

    // 切仓/清空：数据面全清（viewMode 为会话级 UI 偏好，保留——设计 §6.3）
    void reset();

    // 派生入口：快照分桶入库 + 重建行序列；展开位图/组折叠/lazy 缓存全保留
    //（刷新=同款重建，F-09）。>200 阈值只对用户未手动切换过的组注入默认折叠。
    void rebuild(const platform::git::StatusSnapshot& snapshot, ChangeViewMode mode);
    // 树/平铺切换（快照保留重派生；两形态展开位图互不消费，设计 §6.3）
    void setViewMode(ChangeViewMode mode);
    ChangeViewMode viewMode() const { return viewMode_; }

    // 可见行序（派生结果；组头 childCount=组内计数，空组整组不出行——设计 §4.3）
    const std::vector<ChangeListRow>& rows() const { return rows_; }

    // 目录展开/折叠（当前形态位图；连点防抖由视图同值判幂等，这里只做集合置位）。
    // 默认态（设计 §7 N2「≤50 条全展开」）：tracked 派生目录默认展开，显式折叠
    // 记忆（collapsed 位图）；untracked lazy 目录默认收起，显式展开才投枚举。
    void setExpanded(const std::string& dirRel, bool expanded);
    bool isExpanded(const std::string& dirRel) const;
    // 全量成员判定（无视展开/折叠可见性——刷新保选中 F-09、jumpToHit ∈变更判定）：
    // 双桶直接命中 + untracked lazy 缓存递归
    bool hasFile(const std::string& relPath) const;

    // 组头折叠（设计 §4.3/§6.4；折叠态记忆，clue#14 口径）
    void setGroupCollapsed(ChangeListRow::Group group, bool collapsed);
    bool isGroupCollapsed(ChangeListRow::Group group) const;

    // untracked 目录 lazy 状态机（F-04；seq 过期丢弃由 controller 负责——
    // requestStatus 同款先例 git_page_controller.cpp:362）
    void markUntrackedLoading(const std::string& dirRel);
    void markUntrackedFailed(const std::string& dirRel, const std::string& message);
    // 重试：清错误回待加载（pendingUntrackedDirs() 下轮重投）
    void requestRetry(const std::string& dirRel);
    // walk 结果入库：枚举结果全量取用（非 ignored 即 untracked，POC①）——
    // 目录条目（Directory/Subrepo）续 lazy Dir 行、文件条目打 U；
    // 文件 >kMaxUntrackedChildren 截断 + Truncate 尾行
    void applyUntrackedChildren(const std::string& dirRel, const fs_walk::WalkResult& result);
    // 已展开且子层未加载且未在加载/错误中的 untracked 目录（controller 据此投
    // worker；浅路径在前——首屏根层先回，旧 pendingDirs 同款）
    std::vector<std::string> pendingUntrackedDirs() const;

private:
    struct ChangeEntry {
        platform::git::StatusKind kind;
        std::string oldPath;
    };

    void derive();
    void emitGroup(ChangeListRow::Group group,
                   const std::map<std::string, ChangeEntry>& files,
                   const std::set<std::string>& lazyDirs);
    void emitGroupTree(ChangeListRow::Group group,
                       const std::map<std::string, ChangeEntry>& files,
                       const std::set<std::string>& lazyDirs);
    void emitGroupFlat(ChangeListRow::Group group,
                       const std::map<std::string, ChangeEntry>& files,
                       const std::set<std::string>& lazyDirs);
    void emitFileRow(const std::string& path, const ChangeEntry& entry, int depth,
                     ChangeListRow::Group group);
    void emitLazyRows(const std::string& dirRel, int depth, ChangeListRow::Group group);
    size_t countLazyFiles(const std::string& dirRel) const;
    void collectLazyDirs(const std::vector<ChangeListRow>& rows,
                         std::set<std::string>& out) const;
    bool isLazyTarget(const std::string& dirRel) const;

    ChangeViewMode viewMode_ = ChangeViewMode::Tree;
    // 分桶（组内按路径字节序去重；跨组不去重——D6-A）
    std::map<std::string, ChangeEntry> staged_;
    std::map<std::string, ChangeEntry> unstaged_;
    std::set<std::string> untrackedDirs_;  // untracked 目录（剥尾'/'，恒归未暂存组——D6-A）
    // 展开/折叠位图（树/平铺各自一份，互不消费——设计 §6.3）。expanded 位图仅
    // untracked lazy 目录消费（默认收起，显式展开）；collapsed 位图仅 tracked
    // 派生目录消费（默认展开，显式折叠记忆——F-09 刷新保留）
    std::set<std::string> expandedTree_;
    std::set<std::string> expandedFlat_;
    std::set<std::string> collapsedTree_;
    std::set<std::string> collapsedFlat_;
    std::set<ChangeListRow::Group> groupCollapsed_;
    std::set<ChangeListRow::Group> groupTouched_;  // 用户手动切换过的组（>200 注入只对未触碰组生效）
    // untracked lazy 状态机（F-04；缓存跨刷新保留，loading/error 派生行见 emitLazyRows）
    std::map<std::string, std::vector<ChangeListRow>> untrackedChildren_;
    std::set<std::string> loadingDirs_;
    std::map<std::string, std::string> dirErrors_;  // dirRel → 失败原因
    std::vector<ChangeListRow> rows_;
};

// 徽标字母（F-12；自 git_tree_model.h 迁出——类退役随迁，need291 合并轮缺口 B）
char statusBadgeLetter(platform::git::StatusKind kind);

} // namespace module::git
