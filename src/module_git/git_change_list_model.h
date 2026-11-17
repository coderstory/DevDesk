// src/module_git/git_change_list_model.h
// 变更列表行模型（need291 Git 管理器重设计；need299 老板裁决④+原则令修订）：
// status 快照 → staged/unstaged 双组分桶 → 每桶变更路径集派生目录树（树形态，
// md_tree.cpp:34-99 同款祖先目录集+逐级 emit）或目录段聚合平铺（设计 §4.7）；
// 单链压缩（唯一子目录链合并显示 a/b/c/，VSCode Compact Folders 惯例，clue#199）。
// 裁决④（2026-11-17「未跟踪的只有文件 没有文件夹」）：status 的 untracked 目录
// 条目不再产 Dir 行——控制器在 status 后自动枚举首层（复用 requestUntrackedChildren
// 的 walkChildren filesystem 链，F-09 不递归不破），枚举出的文件行带 U 徽标并入
// 树/平铺同源派生；嵌套子目录=派生目录行（纯结构装置）懒展开。
// 文件夹原则令（2026-11-17「文件夹只是描述文件层次结构的」）：Dir 行=纯结构装置，
// 仅树形态为组织而存在、可展开收起；不可选中、不进 diff、不带状态徽标、不计入
// 组头计数——组头「已暂存 (N)」计数只算文件行。
// 列表过滤（need299 §4.9）：全相对路径子串、ASCII 大小写不敏感（原 git_search_model
// 的 asciiFold 迁入）；过滤=派生期视图裁剪，不动展开位图（清空恢复原位图）。
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
    std::string dirRel;   // Kind==Dir 有效；untracked 嵌套派生目录=枚举条目路径；
                          // Loading/Truncate/Error=父目录 dirRel（视图定位用）
    std::optional<platform::git::StatusKind> status;  // 仅 File 行徽标（原则令：
                                                      // 文件夹行不带状态徽标）
    bool lazy = false;       // 仅 Kind==Dir：untracked lazy 派生目录（点击=懒枚举
                             // 展开，默认收起；其余 Dir=tracked 派生默认展开）
    std::string oldPath;     // R 行旧路径（tooltip「old → new」用）
    size_t childCount = 0;   // Dir 行=子树文件行数（纯结构信息，lazy 未枚举前=0）；
                             // GroupHeader=组内文件行数（原则令：只算文件行）
    size_t remainCount = 0;  // Kind==Truncate 有效（>500 截断余量；尾行文案由视图组）
    std::string errorText;   // Kind==Error 原因
};

class GitChangeListModel {
public:
    // 单目录文件行截断上限（设计 §6.2：>500 显示前 500 + 尾行「还有 N 个文件」）
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

    // 可见行序（派生结果；组头 childCount=组内文件行数，空组整组不出行——设计 §4.3）
    const std::vector<ChangeListRow>& rows() const { return rows_; }

    // 目录展开/折叠（当前形态位图；连点防抖由视图同值判幂等，这里只做集合置位）。
    // 双默认态：tracked 派生目录默认展开（折叠走 collapsed 位图）；untracked lazy
    // 派生目录默认收起（展开走 expanded 位图，首次展开由控制器投枚举）。
    void setExpanded(const std::string& dirRel, bool expanded);
    bool isExpanded(const std::string& dirRel) const;
    // 全量成员判定（无视展开/折叠可见性——刷新保选中 F-09）：双桶+lazy 缓存递归
    bool hasFile(const std::string& relPath) const;
    // 单文件状态（不依赖可见行集——选中行在折叠目录下仍可取徽标/判 U）
    std::optional<platform::git::StatusKind> fileStatus(
        const std::string& relPath) const;

    // 组头折叠（设计 §4.3/§6.4；折叠态记忆，clue#14 口径）
    void setGroupCollapsed(ChangeListRow::Group group, bool collapsed);
    bool isGroupCollapsed(ChangeListRow::Group group) const;

    // 列表过滤（need299 §4.2b/§4.9：全相对路径子串、ASCII 大小写不敏感；
    // 空串=无过滤。派生期裁剪：树=命中文件+祖先链自动临时展开（不写位图，清空
    // 恢复原展开）；平铺=行级匹配；组头计数过滤态=命中文件行数）
    void setFilter(const std::string& rawQuery);
    const std::string& filterQuery() const { return filterQuery_; }

    // untracked 目录 lazy 状态机（F-04；seq 过期丢弃由 controller 负责）：
    void markUntrackedLoading(const std::string& dirRel);
    void markUntrackedFailed(const std::string& dirRel, const std::string& message);
    // 重试：清错误回待加载（pendingUntrackedDirs() 下轮重投）
    void requestRetry(const std::string& dirRel);
    // walk 结果入库：枚举结果全量取用（非 ignored 即 untracked，POC①）——
    // 文件条目打 U 并入顶层派生（裁决④）、目录条目续 lazy 派生目录行；
    // 文件 >kMaxUntrackedChildren 截断 + Truncate 尾行
    void applyUntrackedChildren(const std::string& dirRel, const fs_walk::WalkResult& result);
    // 待枚举 untracked 目录（浅路径在前——首屏根层先回）：
    // ①顶层 status 条目=自动枚举（裁决④，无视展开）；②嵌套 lazy 派生目录=展开才枚举
    std::vector<std::string> pendingUntrackedDirs() const;

private:
    struct ChangeEntry {
        platform::git::StatusKind kind;
        std::string oldPath;
    };

    bool matchesFilter(const std::string& relPath) const;

    // 未暂存有效文件集 = unstaged_ ∪ 顶层 untracked 目录已枚举文件行（裁决④并入）
    std::map<std::string, ChangeEntry> mergedUnstagedFiles() const;
    // 嵌套 lazy 派生目录集（各顶层缓存内 Dir 行递归收集）——派生的 lazy 锚
    std::set<std::string> nestedLazyDirs() const;

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
    // 顶层 untracked 条目的枚举失败/截断尾行（无 Dir 行可挂，组尾 depth 0——裁决④）
    void emitTopLevelStateRows(ChangeListRow::Group group);
    size_t countLazyFiles(const std::string& dirRel) const;
    size_t countMatchedLazyFiles(const std::string& dirRel) const;
    bool cacheHasMatchedFile(const std::string& dirRel) const;
    void collectLazyDirs(const std::vector<ChangeListRow>& rows,
                         std::set<std::string>& out) const;
    bool isLazyTarget(const std::string& dirRel) const;

    ChangeViewMode viewMode_ = ChangeViewMode::Tree;
    // 分桶（组内按路径字节序去重；跨组不去重——D6-A）
    std::map<std::string, ChangeEntry> staged_;
    std::map<std::string, ChangeEntry> unstaged_;
    std::set<std::string> untrackedDirs_;  // 顶层 untracked 目录条目（剥尾'/'，恒归
                                           // 未暂存组——D6-A；裁决④自动枚举首层）
    // 列表过滤（need299 §4.9）：原词回显 + 折叠小写匹配词（空=无过滤）
    std::string filterQuery_;
    std::string filterFolded_;
    // 展开/折叠位图（树/平铺各自一份，互不消费——设计 §6.3）。expanded 位图仅
    // untracked lazy 派生目录消费（默认收起，显式展开）；collapsed 位图仅 tracked
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

// 徽标字母（F-12；仅文件行消费——原则令：文件夹行不带徽标）
char statusBadgeLetter(platform::git::StatusKind kind);

} // namespace module::git
